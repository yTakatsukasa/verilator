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

#include "V3Stats.h"

#include <unordered_map>
#include <unordered_set>

namespace {

using ModuleSummaryMap
    = std::unordered_map<const AstNodeModule*, V3SubgraphSummary::ModuleSummary>;
using ScopeSummaryMap = std::unordered_map<const AstScope*, V3SubgraphSummary::ScopeSummary>;

ModuleSummaryMap s_moduleSummaries;
ScopeSummaryMap s_scopeSummaries;
std::unordered_set<const AstVarScope*> s_derivedBoundaryInputs;
std::unordered_set<std::string> s_externallyConsumedBoundaryVars;
uint64_t s_boundaryWriteCandidates = 0;
uint64_t s_boundaryWritesPruned = 0;
bool s_externalConsumersCaptured = false;

bool isCompileTimeConstant(const AstVar* varp) { return varp->isParam() || varp->isGenVar(); }

std::string boundaryVarKey(const AstVarScope* vscp) {
    const std::string scopeName = vscp->scopep()->name();
    return cvtToStr(scopeName.size()) + ":" + scopeName + vscp->varp()->name();
}

const AstScope* subgraphBoundaryScope(const AstScope* scopep) {
    for (const AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
        if (scanp->modp()->subgraphBoundary()) return scanp;
    }
    return nullptr;
}

bool isUnderScope(const AstScope* scopep, const AstScope* basep) {
    for (const AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
        if (scanp == basep) return true;
    }
    return false;
}

class SubgraphExternalConsumptionVisitor final : public VNVisitorConst {
    const AstScope* m_scopep = nullptr;

    void visit(AstScope* nodep) override {
        VL_RESTORER(m_scopep);
        m_scopep = nodep;
        iterateChildrenConst(nodep);
    }
    void visit(AstNodeVarRef* nodep) override {
        if (!nodep->access().isReadOrRW()) return;
        AstVarScope* const vscp = nodep->varScopep();
        const AstScope* const boundaryp = subgraphBoundaryScope(vscp->scopep());
        if (boundaryp && !isUnderScope(m_scopep, boundaryp)) {
            s_externallyConsumedBoundaryVars.insert(boundaryVarKey(vscp));
        }
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    explicit SubgraphExternalConsumptionVisitor(AstNetlist* nodep) { iterateConst(nodep); }
    ~SubgraphExternalConsumptionVisitor() override = default;
};

class SubgraphModuleSummaryVisitor final : public VNVisitorConst {
    const AstNodeModule* m_modp = nullptr;

    static void analyzeNode(const AstNode* nodep, V3SubgraphSummary::ModuleSummary& summary) {
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
            V3SubgraphSummary::ModuleSummary& summary = s_moduleSummaries[nodep];
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

    void visit(AstAlways* nodep) override {
        if (m_modp && m_modp->subgraphBoundary() && nodep->sentreep()
            && nodep->sentreep()->hasClocked()) {
            s_moduleSummaries[m_modp].m_hasClockedState = true;
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstAlwaysPost* nodep) override {
        if (m_modp && m_modp->subgraphBoundary()) {
            s_moduleSummaries[m_modp].m_hasPostPhase = true;
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
    AstScope* m_boundaryScopep = nullptr;

    void visit(AstScope* nodep) override {
        if (!nodep->modp()->subgraphBoundary() || m_boundaryScopep) {
            iterateChildrenConst(nodep);
            return;
        }

        const auto it = s_moduleSummaries.find(nodep->modp());
        UASSERT_OBJ(it != s_moduleSummaries.end(), nodep, "Missing subgraph module summary");
        const V3SubgraphSummary::ModuleSummary& modSummary = it->second;

        std::unordered_map<std::string, AstVarScope*> varsByName;
        for (AstVarScope* vscp = nodep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
            varsByName.emplace(vscp->varp()->name(), vscp);
        }

        VL_RESTORER(m_boundaryScopep);
        m_boundaryScopep = nodep;
        V3SubgraphSummary::ScopeSummary& scopeSummary = s_scopeSummaries[nodep];
        scopeSummary.m_parentStub.m_hasClockedState = modSummary.m_hasClockedState;
        scopeSummary.m_parentStub.m_hasPostPhase = modSummary.m_hasPostPhase;
        scopeSummary.m_parentStub.m_boundaryReads.reserve(modSummary.m_nonOutputPortNames.size());
        scopeSummary.m_parentStub.m_boundaryWrites.reserve(modSummary.m_writablePortNames.size());

        for (const std::string& name : modSummary.m_nonOutputPortNames) {
            const auto varsIt = varsByName.find(name);
            if (varsIt == varsByName.end()) continue;
            scopeSummary.m_parentStub.m_boundaryReads.push_back(varsIt->second);
            if (modSummary.m_derivedBoundaryInputNames.count(name)) {
                s_derivedBoundaryInputs.insert(varsIt->second);
            }
        }
        for (const std::string& name : modSummary.m_writablePortNames) {
            const auto varsIt = varsByName.find(name);
            if (varsIt == varsByName.end()) continue;
            AstVarScope* const vscp = varsIt->second;
            ++s_boundaryWriteCandidates;
            AstVar* const varp = vscp->varp();
            if (s_externallyConsumedBoundaryVars.count(boundaryVarKey(vscp)) || varp->isPrimaryIO()
                || varp->isSigPublic() || varp->isTrace()) {
                scopeSummary.m_parentStub.m_boundaryWrites.push_back(vscp);
            } else {
                ++s_boundaryWritesPruned;
            }
        }

        iterateChildrenConst(nodep);
    }

    void visit(AstNodeVarRef* nodep) override {
        if (m_boundaryScopep && (nodep->access().isReadOrRW() || nodep->access().isWriteOrRW())
            && !isCompileTimeConstant(nodep->varp())
            && !isUnderScope(nodep->varScopep()->scopep(), m_boundaryScopep)) {
            s_scopeSummaries[m_boundaryScopep].m_parentStub.m_readsExternalVars = true;
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
    s_moduleSummaries.clear();
    s_scopeSummaries.clear();
    s_derivedBoundaryInputs.clear();
    s_boundaryWriteCandidates = 0;
    s_boundaryWritesPruned = 0;
    if (!v3Global.opt.subgraphSchedule()) return;
    SubgraphModuleSummaryVisitor{nodep};
    if (!s_externalConsumersCaptured) SubgraphExternalConsumptionVisitor{nodep};
}

void V3SubgraphSummary::captureExternalConsumers(AstNetlist* nodep) {
    s_externallyConsumedBoundaryVars.clear();
    s_externalConsumersCaptured = false;
    if (!v3Global.opt.subgraphSchedule()) return;
    SubgraphExternalConsumptionVisitor{nodep};
    s_externalConsumersCaptured = true;
}

void V3SubgraphSummary::bindScopes(AstNetlist* nodep) {
    s_scopeSummaries.clear();
    s_derivedBoundaryInputs.clear();
    s_boundaryWriteCandidates = 0;
    s_boundaryWritesPruned = 0;
    if (!v3Global.opt.subgraphSchedule()) return;
    if (s_moduleSummaries.empty()) buildModules(nodep);
    SubgraphScopeSummaryBinder{nodep};
    if (v3Global.opt.stats()) {
        const string prefix = "Scheduling, Subgraph parent contracts, ";
        V3Stats::addStat(prefix + "boundary write candidates", s_boundaryWriteCandidates);
        V3Stats::addStat(prefix + "boundary writes pruned", s_boundaryWritesPruned);
    }
}

void V3SubgraphSummary::clear() {
    s_moduleSummaries.clear();
    s_scopeSummaries.clear();
    s_derivedBoundaryInputs.clear();
    s_externallyConsumedBoundaryVars.clear();
    s_boundaryWriteCandidates = 0;
    s_boundaryWritesPruned = 0;
    s_externalConsumersCaptured = false;
}

const V3SubgraphSummary::ScopeSummary* V3SubgraphSummary::getScopeSummary(const AstScope* scopep) {
    const auto it = s_scopeSummaries.find(scopep);
    return it == s_scopeSummaries.end() ? nullptr : &it->second;
}

bool V3SubgraphSummary::isDerivedBoundaryInput(const AstVarScope* vscp) {
    return s_derivedBoundaryInputs.count(vscp);
}
