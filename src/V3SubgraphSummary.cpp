// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Subgraph summary builder
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphSummary.h"

#include <unordered_map>
#include <unordered_set>

namespace {

struct ModuleSummary final {
    std::vector<std::string> m_nonOutputPortNames;
    std::vector<std::string> m_writablePortNames;
    std::unordered_set<std::string> m_derivedBoundaryInputNames;
};

using ModuleSummaryMap = std::unordered_map<const AstNodeModule*, ModuleSummary>;
using ScopeSummaryMap = std::unordered_map<const AstScope*, V3SubgraphSummary::ScopeSummary>;

ModuleSummaryMap s_moduleSummaries;
ScopeSummaryMap s_scopeSummaries;
std::unordered_set<const AstVarScope*> s_derivedBoundaryInputs;

bool isCompileTimeConstant(const AstVar* varp) { return varp->isParam() || varp->isGenVar(); }

class SubgraphModuleSummaryVisitor final : public VNVisitorConst {
    const AstNodeModule* m_modp = nullptr;

    static void analyzeNode(const AstNode* nodep, ModuleSummary& summary) {
        bool readsBoundaryValue = false;
        nodep->foreach([&](const AstVarRef* refp) {
            if (readsBoundaryValue || !refp->access().isReadOrRW()) return;
            if (isCompileTimeConstant(refp->varp())) return;
            readsBoundaryValue = true;
        });
        if (!readsBoundaryValue) return;

        nodep->foreach([&](const AstVarRef* refp) {
            if (!refp->access().isWriteOrRW()) return;
            AstVar* const varp = refp->varp();
            if (!varp->isIO() || !varp->direction().isNonOutput()) return;
            summary.m_derivedBoundaryInputNames.insert(varp->name());
        });
    }

    void visit(AstNodeModule* nodep) override {
        VL_RESTORER(m_modp);
        m_modp = nodep;
        if (nodep->subgraphBoundary()) {
            ModuleSummary& summary = s_moduleSummaries[nodep];
            for (AstNode* memberp = nodep->stmtsp(); memberp; memberp = memberp->nextp()) {
                AstVar* const varp = VN_CAST(memberp, Var);
                if (!varp || !varp->isIO()) continue;
                if (varp->direction().isNonOutput()) {
                    summary.m_nonOutputPortNames.push_back(varp->name());
                }
                if (varp->direction().isWritable()) {
                    summary.m_writablePortNames.push_back(varp->name());
                }
            }
        }
        iterateChildrenConst(nodep);
    }

    void visit(AstAssign* nodep) override {
        if (m_modp && m_modp->subgraphBoundary()) analyzeNode(nodep, s_moduleSummaries[m_modp]);
    }
    void visit(AstAssignW* nodep) override {
        if (m_modp && m_modp->subgraphBoundary()) analyzeNode(nodep, s_moduleSummaries[m_modp]);
    }
    void visit(AstNodeProcedure* nodep) override {
        if (m_modp && m_modp->subgraphBoundary()) analyzeNode(nodep, s_moduleSummaries[m_modp]);
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    explicit SubgraphModuleSummaryVisitor(AstNetlist* nodep) { iterateConst(nodep); }
    ~SubgraphModuleSummaryVisitor() override = default;
};

class SubgraphScopeSummaryBinder final : public VNVisitorConst {
    void visit(AstScope* nodep) override {
        if (!nodep->modp()->subgraphBoundary()) {
            iterateChildrenConst(nodep);
            return;
        }

        const auto it = s_moduleSummaries.find(nodep->modp());
        UASSERT_OBJ(it != s_moduleSummaries.end(), nodep, "Missing subgraph module summary");
        const ModuleSummary& modSummary = it->second;

        std::unordered_map<std::string, AstVarScope*> varsByName;
        for (AstVarScope* vscp = nodep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
            varsByName.emplace(vscp->varp()->name(), vscp);
        }

        V3SubgraphSummary::ScopeSummary& scopeSummary = s_scopeSummaries[nodep];
        scopeSummary.m_nonOutputPorts.reserve(modSummary.m_nonOutputPortNames.size());
        scopeSummary.m_writablePorts.reserve(modSummary.m_writablePortNames.size());

        for (const std::string& name : modSummary.m_nonOutputPortNames) {
            const auto varsIt = varsByName.find(name);
            if (varsIt == varsByName.end()) continue;
            scopeSummary.m_nonOutputPorts.push_back(varsIt->second);
            if (modSummary.m_derivedBoundaryInputNames.count(name)) {
                s_derivedBoundaryInputs.insert(varsIt->second);
            }
        }
        for (const std::string& name : modSummary.m_writablePortNames) {
            const auto varsIt = varsByName.find(name);
            if (varsIt == varsByName.end()) continue;
            scopeSummary.m_writablePorts.push_back(varsIt->second);
        }

        iterateChildrenConst(nodep);
    }

    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    explicit SubgraphScopeSummaryBinder(AstNetlist* nodep) { iterateConst(nodep); }
    ~SubgraphScopeSummaryBinder() override = default;
};

}  // namespace

void V3SubgraphSummary::buildModules(AstNetlist* nodep) {
    clear();
    if (!v3Global.opt.subgraphSchedule()) return;
    SubgraphModuleSummaryVisitor{nodep};
}

void V3SubgraphSummary::bindScopes(AstNetlist* nodep) {
    s_scopeSummaries.clear();
    s_derivedBoundaryInputs.clear();
    if (!v3Global.opt.subgraphSchedule()) return;
    if (s_moduleSummaries.empty()) buildModules(nodep);
    SubgraphScopeSummaryBinder{nodep};
}

void V3SubgraphSummary::clear() {
    s_moduleSummaries.clear();
    s_scopeSummaries.clear();
    s_derivedBoundaryInputs.clear();
}

const V3SubgraphSummary::ScopeSummary* V3SubgraphSummary::getScopeSummary(const AstScope* scopep) {
    const auto it = s_scopeSummaries.find(scopep);
    return it == s_scopeSummaries.end() ? nullptr : &it->second;
}

bool V3SubgraphSummary::isDerivedBoundaryInput(const AstVarScope* vscp) {
    return s_derivedBoundaryInputs.count(vscp);
}
