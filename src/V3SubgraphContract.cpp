// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Immutable subgraph scheduling contract
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
//
// Build a phase-specific summary of every variable used by an ordered
// subgraph helper. Uses are classified relative to the boundary scope and
// frozen before the parent order graph is built.
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphContract.h"

#include <unordered_map>
#include <unordered_set>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

bool isUnderScope(const AstScope* scopep, const AstScope* basep) {
    for (const AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
        if (scanp == basep) return true;
    }
    return false;
}

class SubgraphContractBuilder final : public VNVisitorConst {
    using Uses = std::vector<V3SubgraphContract::Use>;
    using UseIndex = std::unordered_map<AstVarScope*, size_t>;

    AstScope* const m_boundaryScopep;
    Uses& m_boundaryUses;
    Uses& m_externalUses;
    Uses& m_internalUses;
    UseIndex m_boundaryUseIndex;
    UseIndex m_externalUseIndex;
    UseIndex m_internalUseIndex;
    std::unordered_set<const AstCFunc*> m_visitedFuncps;
    const bool m_cuttableBoundaryReads;
    const bool m_cuttableInternalReads;

    static void addUse(Uses& uses, UseIndex& index, AstNodeVarRef* refp, bool cuttableReads) {
        AstVarScope* const vscp = refp->varScopep();
        const auto inserted = index.emplace(vscp, uses.size());
        if (inserted.second) {
            uses.push_back(V3SubgraphContract::Use{vscp, refp->access().isReadOrRW(),
                                                   refp->access().isWriteOrRW(),
                                                   refp->access().isReadOrRW() && cuttableReads});
            return;
        }
        V3SubgraphContract::Use& use = uses[inserted.first->second];
        // An ordered helper can write a boundary input and consume the propagated value later.
        // Such a read is internal to the helper, not a dependency on the value that existed
        // before the coarse node ran. Preserve only reads seen before the first write.
        const bool read = refp->access().isReadOrRW() && !use.m_write;
        use.m_read |= read;
        use.m_cuttable |= read && cuttableReads;
        use.m_write |= refp->access().isWriteOrRW();
    }

    void visit(AstCFunc* nodep) override {
        if (!m_visitedFuncps.emplace(nodep).second) return;
        iterateChildrenConst(nodep);
    }
    void visit(AstCCall* nodep) override {
        iterateChildrenConst(nodep);
        AstCFunc* const funcp = nodep->funcp();
        UASSERT_OBJ(funcp, nodep, "Subgraph contract call has no function");
        if (!funcp->entryPoint()) iterateConst(funcp);
    }
    void visit(AstNodeVarRef* nodep) override {
        AstVarScope* const vscp = nodep->varScopep();
        UASSERT_OBJ(vscp, nodep, "Subgraph contract reference has no scope");
        if (!isUnderScope(vscp->scopep(), m_boundaryScopep)) {
            addUse(m_externalUses, m_externalUseIndex, nodep, m_cuttableBoundaryReads);
        } else if (vscp->scopep() == m_boundaryScopep && vscp->varp()->isIO()) {
            addUse(m_boundaryUses, m_boundaryUseIndex, nodep, m_cuttableBoundaryReads);
        } else {
            addUse(m_internalUses, m_internalUseIndex, nodep, m_cuttableInternalReads);
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    SubgraphContractBuilder(AstCFunc* funcp, AstScope* boundaryScopep, Uses& boundaryUses,
                            Uses& externalUses, Uses& internalUses, bool cuttableBoundaryReads,
                            bool cuttableInternalReads)
        : m_boundaryScopep{boundaryScopep}
        , m_boundaryUses{boundaryUses}
        , m_externalUses{externalUses}
        , m_internalUses{internalUses}
        , m_cuttableBoundaryReads{cuttableBoundaryReads}
        , m_cuttableInternalReads{cuttableInternalReads} {
        iterateConst(funcp);
    }
    ~SubgraphContractBuilder() override = default;
};

}  // namespace

V3SubgraphContract::V3SubgraphContract(AstScope* boundaryScopep, AstSenTree* domainp, bool post,
                                       std::vector<Use>&& boundaryUses,
                                       std::vector<Use>&& externalUses,
                                       std::vector<Use>&& internalUses)
    : m_boundaryScopep{boundaryScopep}
    , m_domainp{domainp}
    , m_post{post}
    , m_boundaryUses{std::move(boundaryUses)}
    , m_externalUses{std::move(externalUses)}
    , m_internalUses{std::move(internalUses)} {}

V3SubgraphContract V3SubgraphContract::make(AstCFunc* funcp, AstScope* boundaryScopep,
                                            AstSenTree* domainp, bool post, bool refresh) {
    std::vector<Use> boundaryUses;
    std::vector<Use> externalUses;
    std::vector<Use> internalUses;
    SubgraphContractBuilder{funcp,        boundaryScopep,  boundaryUses, externalUses,
                            internalUses, post || refresh, post};
    return V3SubgraphContract{
        boundaryScopep,         domainp, post, std::move(boundaryUses), std::move(externalUses),
        std::move(internalUses)};
}

V3SubgraphContract V3SubgraphContract::fromUses(AstScope* boundaryScopep, AstSenTree* domainp,
                                                bool post, std::vector<Use>&& boundaryUses,
                                                std::vector<Use>&& externalUses,
                                                std::vector<Use>&& internalUses) {
    return V3SubgraphContract{
        boundaryScopep,         domainp, post, std::move(boundaryUses), std::move(externalUses),
        std::move(internalUses)};
}

V3SubgraphContract
V3SubgraphContract::remap(const V3SubgraphContract& source, AstScope* boundaryScopep,
                          AstSenTree* domainp,
                          const std::unordered_map<AstVarScope*, AstVarScope*>& sourceToTarget) {
    const auto remapUses = [&](const std::vector<Use>& sourceUses) {
        std::vector<Use> result;
        result.reserve(sourceUses.size());
        for (const Use& use : sourceUses) {
            const auto it = sourceToTarget.find(use.m_varScopep);
            result.push_back(Use{it == sourceToTarget.end() ? use.m_varScopep : it->second,
                                 use.m_read, use.m_write, use.m_cuttable});
        }
        return result;
    };
    return V3SubgraphContract{boundaryScopep,
                              domainp,
                              source.post(),
                              remapUses(source.boundaryUses()),
                              remapUses(source.externalUses()),
                              remapUses(source.internalUses())};
}

std::vector<V3SubgraphContract::LogicalUse>
V3SubgraphContract::makeLogicalBoundaryUses(AstScope* boundaryScopep) {
    std::vector<LogicalUse> uses;
    for (AstNode* memberp = boundaryScopep->modp()->stmtsp(); memberp;
         memberp = memberp->nextp()) {
        AstVar* const varp = VN_CAST(memberp, Var);
        if (!varp) continue;
        if (!varp->isIO()) continue;
        const bool read = varp->direction().isNonOutput();
        const bool write = varp->direction().isWritable();
        if (read || write) uses.push_back(LogicalUse{varp->origName(), read, write});
    }
    return uses;
}

bool V3SubgraphContract::isDelayedState(const AstVarScope* vscp) {
    return 0 == vscp->varp()->name().rfind("__Vdly", 0);
}
