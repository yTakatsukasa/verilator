// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Scheduling subgraph helpers
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2003-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SchedSubgraph.h"

#include "V3SubgraphSummary.h"

namespace V3Sched {

namespace {

using SubgraphCallUsageSummaryMap
    = std::unordered_map<const AstCFunc*, std::vector<SubgraphCallUsageSummary>>;
using SubgraphScopeContractSummaryMap
    = std::unordered_map<const AstScope*, SubgraphScopeContractSummary>;

struct SubgraphRegistry final {
    SubgraphCallUsageSummaryMap m_callUsageSummaries;
    SubgraphScopeContractSummaryMap m_scopeContractSummaries;
    std::unordered_set<const AstNodeProcedure*> m_snapshotProcedures;
};

SubgraphRegistry& subgraphRegistry() {
    static SubgraphRegistry s_registry;
    return s_registry;
}

}  // namespace

AstCFunc* cloneUnguardedFuncBody(AstCFunc* funcp, AstScope* scopep, const std::string& nameSuffix,
                                 bool slow) {
    AstNode* bodyp = funcp->stmtsp();
    if (AstIf* const ifp = VN_CAST(bodyp, If)) {
        if (!ifp->nextp() && !ifp->elsesp() && ifp->thensp()) bodyp = ifp->thensp();
    }
    const bool shareSubgraphHelper = scopep->modp()->subgraphBoundary() && !funcp->cname().empty();
    const string cloneName = shareSubgraphHelper ? funcp->name() + "__sgclone" + nameSuffix
                                                 : funcp->name() + nameSuffix;
    AstCFunc* const clonep = new AstCFunc{funcp->fileline(), cloneName, scopep, ""};
    clonep->dontCombine(true);
    clonep->isStatic(false);
    clonep->isLoose(true);
    clonep->slow(slow);
    clonep->isConst(false);
    clonep->declPrivate(true);
    if (shareSubgraphHelper) clonep->cname(funcp->cname() + nameSuffix);
    scopep->addBlocksp(clonep);
    if (bodyp) clonep->addStmtsp(bodyp->cloneTree(true));
    return clonep;
}

void registerSubgraphCallUsageSummary(const AstCFunc* funcp,
                                      std::vector<SubgraphCallUsageSummary>&& summary) {
    subgraphRegistry().m_callUsageSummaries[funcp] = std::move(summary);
}

const std::vector<SubgraphCallUsageSummary>* getSubgraphCallUsageSummary(const AstCFunc* funcp) {
    auto& callUsageSummaries = subgraphRegistry().m_callUsageSummaries;
    const auto it = callUsageSummaries.find(funcp);
    return it == callUsageSummaries.end() ? nullptr : &it->second;
}

const SubgraphScopeContractSummary* getSubgraphScopeContractSummary(const AstScope* scopep) {
    auto& scopeContractSummaries = subgraphRegistry().m_scopeContractSummaries;
    const auto it = scopeContractSummaries.find(scopep);
    if (it != scopeContractSummaries.end()) return &it->second;

    const V3SubgraphSummary::ScopeSummary* const summaryp
        = V3SubgraphSummary::getScopeSummary(scopep);
    if (!summaryp) return nullptr;

    SubgraphScopeContractSummary contract;
    contract.m_hasClockedState = summaryp->m_parentStub.m_hasClockedState;
    contract.m_hasPostPhase = summaryp->m_parentStub.m_hasPostPhase;
    contract.m_readsExternalVars = summaryp->m_parentStub.m_readsExternalVars;
    contract.m_boundaryReads.reserve(summaryp->m_parentStub.m_boundaryReads.size());
    contract.m_boundaryWrites.reserve(summaryp->m_parentStub.m_boundaryWrites.size());
    for (AstVarScope* const vscp : summaryp->m_parentStub.m_boundaryReads) {
        contract.m_boundaryReads.push_back(
            {vscp, V3SubgraphSummary::isDerivedBoundaryInput(vscp)});
    }
    for (AstVarScope* const vscp : summaryp->m_parentStub.m_boundaryWrites) {
        contract.m_boundaryWrites.push_back(vscp);
    }
    return &subgraphRegistry()
                .m_scopeContractSummaries.emplace(scopep, std::move(contract))
                .first->second;
}

void clearSubgraphCallUsageSummaries() {
    subgraphRegistry().m_callUsageSummaries.clear();
    subgraphRegistry().m_scopeContractSummaries.clear();
}

void rememberSubgraphSnapshotProcedure(const AstNodeProcedure* procp) {
    subgraphRegistry().m_snapshotProcedures.insert(procp);
}

void clearSubgraphSnapshotProcedures() { subgraphRegistry().m_snapshotProcedures.clear(); }

bool isSubgraphSnapshotProcedure(const AstNodeProcedure* procp) {
    return subgraphRegistry().m_snapshotProcedures.count(procp);
}

}  // namespace V3Sched
