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

using ScopeSummaryMap = std::unordered_map<const AstScope*, V3SubgraphSummary::ScopeSummary>;
ScopeSummaryMap s_scopeSummaries;
std::unordered_set<const AstVarScope*> s_derivedBoundaryInputs;

const AstScope* boundaryScopeFor(const AstScope* scopep) {
    for (const AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
        if (scanp->modp()->subgraphBoundary()) return scanp;
    }
    return nullptr;
}

class SubgraphSummaryVisitor final : public VNVisitorConst {
    void analyzeStmt(const AstNode* stmtp) {
        bool readsBoundaryValue = false;
        stmtp->foreach([&](const AstVarRef* refp) {
            if (readsBoundaryValue || !refp->access().isReadOrRW()) return;
            if (boundaryScopeFor(refp->varScopep()->scopep())) readsBoundaryValue = true;
        });
        if (!readsBoundaryValue) return;

        stmtp->foreach([&](const AstVarRef* refp) {
            if (!refp->access().isWriteOrRW()) return;
            AstVarScope* const vscp = refp->varScopep();
            if (!vscp->varp()->isIO() || !vscp->varp()->direction().isNonOutput()) return;
            if (!vscp->scopep()->modp()->subgraphBoundary()) return;
            s_derivedBoundaryInputs.insert(vscp);
        });
    }

    void visit(AstScope* nodep) override {
        if (nodep->modp()->subgraphBoundary()) {
            V3SubgraphSummary::ScopeSummary& summary = s_scopeSummaries[nodep];
            for (AstVarScope* vscp = nodep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
                AstVar* const varp = vscp->varp();
                if (!varp->isIO()) continue;
                if (varp->direction().isNonOutput()) summary.m_nonOutputPorts.push_back(vscp);
                if (varp->direction().isWritable()) summary.m_writablePorts.push_back(vscp);
            }
        }
        iterateChildrenConst(nodep);
    }

    void visit(AstActive* nodep) override {
        for (const AstNode* stmtp = nodep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            analyzeStmt(stmtp);
        }
    }

    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    explicit SubgraphSummaryVisitor(AstNetlist* nodep) { iterateConst(nodep); }
    ~SubgraphSummaryVisitor() override = default;
};

}  // namespace

void V3SubgraphSummary::build(AstNetlist* nodep) {
    clear();
    if (!v3Global.opt.subgraphSchedule()) return;
    SubgraphSummaryVisitor{nodep};
}

void V3SubgraphSummary::clear() {
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
