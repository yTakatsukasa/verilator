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

#include "V3Hasher.h"
#include "V3Os.h"
#include "V3Stats.h"
#include "V3SubgraphSummary.h"

#include <limits>

namespace V3Sched {

namespace {

struct SubgraphInstanceContract final {
    std::vector<AstSubgraphInstance::BoundaryReadContract> m_boundaryReads;
    std::vector<AstVarScope*> m_boundaryWrites;
    std::vector<AstVarScope*> m_coarseWrites;
    std::vector<AstSubgraphInstance::ExternalUseContract> m_externalUses;
    bool m_hasClockedState = false;
    bool m_hasPostPhase = false;

    void addBoundaryRead(AstVarScope* vscp, bool derived) {
        for (AstSubgraphInstance::BoundaryReadContract& read : m_boundaryReads) {
            if (read.m_varscp != vscp) continue;
            read.m_derived |= derived;
            return;
        }
        m_boundaryReads.push_back({vscp, derived});
    }
    bool addBoundaryWrite(AstVarScope* vscp) {
        for (AstVarScope* const scanp : m_boundaryWrites) {
            if (scanp == vscp) return false;
        }
        m_boundaryWrites.push_back(vscp);
        return true;
    }
    bool addCoarseWrite(AstVarScope* vscp) {
        for (AstVarScope* const scanp : m_coarseWrites) {
            if (scanp == vscp) return false;
        }
        m_coarseWrites.push_back(vscp);
        return true;
    }
    void addExternalUse(AstVarScope* vscp, bool read, bool write) {
        for (AstSubgraphInstance::ExternalUseContract& use : m_externalUses) {
            if (use.m_varscp != vscp) continue;
            use.m_read |= read;
            use.m_write |= write;
            return;
        }
        m_externalUses.push_back({vscp, read, write});
    }
    void mergeFrom(const SubgraphInstanceContract& other) {
        m_hasClockedState |= other.m_hasClockedState;
        m_hasPostPhase |= other.m_hasPostPhase;
        for (const AstSubgraphInstance::BoundaryReadContract& read : other.m_boundaryReads) {
            addBoundaryRead(read.m_varscp, read.m_derived);
        }
        for (AstVarScope* const vscp : other.m_boundaryWrites) addBoundaryWrite(vscp);
        for (AstVarScope* const vscp : other.m_coarseWrites) addCoarseWrite(vscp);
        for (const AstSubgraphInstance::ExternalUseContract& use : other.m_externalUses) {
            addExternalUse(use.m_varscp, use.m_read, use.m_write);
        }
    }
};

using SubgraphInstanceContractMap = std::unordered_map<const AstScope*, SubgraphInstanceContract>;

struct SubgraphRegistry final {
    std::unordered_set<const AstScope*> m_inputRefreshScopes;
    SubgraphInstanceContractMap m_scopeContracts;
    std::unordered_set<const AstNodeProcedure*> m_snapshotProcedures;
    std::unordered_map<const AstScope*, std::vector<AstCFunc*>> m_stlSubgraphFuncs;
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

void appendSubgraphInputRefreshCalls(AstCFunc* funcp) {
    SubgraphRegistry& registry = subgraphRegistry();
    std::vector<const AstScope*> scopes{registry.m_inputRefreshScopes.begin(),
                                        registry.m_inputRefreshScopes.end()};
    std::sort(scopes.begin(), scopes.end(), [](const AstScope* lhsp, const AstScope* rhsp) {
        return lhsp->name() < rhsp->name();
    });
    uint64_t calls = 0;
    for (const AstScope* const scopep : scopes) {
        const auto it = registry.m_stlSubgraphFuncs.find(scopep);
        if (it == registry.m_stlSubgraphFuncs.end()) continue;
        for (AstCFunc* const tailFuncp : it->second) {
            funcp->addStmtsp(util::callVoidFunc(tailFuncp));
            ++calls;
        }
    }
    V3Stats::addStat("Scheduling, Subgraph input refresh calls", calls);
    V3Stats::addStat("Scheduling, Subgraph input refresh scopes", scopes.size());
}

const SubgraphInstanceContract* getSubgraphScopeContract(const AstScope* scopep) {
    auto& scopeContracts = subgraphRegistry().m_scopeContracts;
    const auto it = scopeContracts.find(scopep);
    if (it != scopeContracts.end()) return &it->second;

    const V3SubgraphSummary::ScopeSummary* const summaryp
        = V3SubgraphSummary::getScopeSummary(scopep);
    if (!summaryp) return nullptr;

    SubgraphInstanceContract contract;
    contract.m_hasClockedState = summaryp->m_parentStub.m_hasClockedState;
    contract.m_hasPostPhase = summaryp->m_parentStub.m_hasPostPhase;
    contract.m_boundaryReads.reserve(summaryp->m_parentStub.m_boundaryReads.size());
    contract.m_boundaryWrites.reserve(summaryp->m_parentStub.m_boundaryWrites.size());
    for (AstVarScope* const vscp : summaryp->m_parentStub.m_boundaryReads) {
        contract.addBoundaryRead(vscp, V3SubgraphSummary::isDerivedBoundaryInput(vscp));
    }
    for (AstVarScope* const vscp : summaryp->m_parentStub.m_boundaryWrites) {
        contract.addBoundaryWrite(vscp);
    }
    return &scopeContracts.emplace(scopep, std::move(contract)).first->second;
}

void clearSubgraphScopeContracts() { subgraphRegistry().m_scopeContracts.clear(); }

void clearSubgraphInputRefreshScopes() { subgraphRegistry().m_inputRefreshScopes.clear(); }

void rememberSubgraphInputRefreshScope(const AstScope* scopep) {
    subgraphRegistry().m_inputRefreshScopes.insert(scopep);
}

void rememberSubgraphSnapshotProcedure(const AstNodeProcedure* procp) {
    subgraphRegistry().m_snapshotProcedures.insert(procp);
}

void clearSubgraphSnapshotProcedures() { subgraphRegistry().m_snapshotProcedures.clear(); }

bool isSubgraphSnapshotProcedure(const AstNodeProcedure* procp) {
    return subgraphRegistry().m_snapshotProcedures.count(procp);
}

namespace {

using SubgraphWrapperKind = AstSubgraphInstance::WrapperKind;

struct SubgraphWrapper final {
    SubgraphWrapperKind m_kind = SubgraphWrapperKind::STMT;
    VAlwaysKwd m_keyword = VAlwaysKwd::ALWAYS;
};

bool operator==(const SubgraphWrapper& lhs, const SubgraphWrapper& rhs) {
    return lhs.m_kind == rhs.m_kind && lhs.m_keyword == rhs.m_keyword;
}

SubgraphWrapper wrapperFromLogic(AstNode* nodep) {
    SubgraphWrapper result;
    AstNodeProcedure* const origp = VN_CAST(nodep, NodeProcedure);
    if (const AstAlways* const alwaysp = VN_CAST(origp, Always)) {
        result.m_kind = SubgraphWrapperKind::ALWAYS;
        result.m_keyword = alwaysp->keyword();
    } else if (VN_IS(origp, AlwaysObserved)) {
        result.m_kind = SubgraphWrapperKind::ALWAYS_OBSERVED;
    } else if (VN_IS(origp, AlwaysPost)) {
        result.m_kind = SubgraphWrapperKind::ALWAYS_POST;
    } else if (VN_IS(origp, AlwaysPre)) {
        result.m_kind = SubgraphWrapperKind::ALWAYS_PRE;
    } else if (VN_IS(origp, AlwaysReactive)) {
        result.m_kind = SubgraphWrapperKind::ALWAYS_REACTIVE;
    } else if (VN_IS(origp, InitialAutomatic)) {
        result.m_kind = SubgraphWrapperKind::INITIAL_AUTOMATIC;
    }
    return result;
}

void disableLifePostForExternalReads(const LogicByScope& subgraphLogic, AstScope* subgraphScopep) {
    subgraphLogic.foreachLogic([&](AstNode* logicp) {
        logicp->foreach([&](AstVarRef* refp) {
            if (refp->access().isWriteOnly()) return;
            AstVarScope* const vscp = refp->varScopep();
            if (vscp->scopep() == subgraphScopep) return;
            vscp->optimizeLifePost(false);
        });
    });
}

struct SubgraphGroup final {
    AstScope* m_scopep = nullptr;
    AstSenTree* m_senTreep = nullptr;
    FileLine* m_flp = nullptr;
    LogicByScope m_earlyLogic;
    bool m_hasPost = false;
    bool m_hasNonPostLate = false;
    SubgraphWrapper m_lateWrapper;
    LogicByScope m_lateLogic;
    LogicByScope* m_ownerp = nullptr;
};

struct SubgraphActiveKey final {
    LogicByScope* m_ownerp = nullptr;
    AstSenTree* m_senTreep = nullptr;

    bool operator==(const SubgraphActiveKey& other) const {
        return m_ownerp == other.m_ownerp && m_senTreep == other.m_senTreep;
    }
};

struct SubgraphActiveKeyHash final {
    size_t operator()(const SubgraphActiveKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_ownerp);
        hash ^= std::hash<const void*>{}(key.m_senTreep) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                + (hash >> 2);
        return hash;
    }
};

struct SubgraphActiveEntry final {
    AstScope* m_scopep = nullptr;
    AstActive* m_activep = nullptr;
};

struct SubgraphBatchKey final {
    LogicByScope* m_ownerp = nullptr;
    AstScope* m_scopep = nullptr;
    AstSenTree* m_senTreep = nullptr;
    SubgraphWrapper m_wrapper;
    AstSubgraphInstance::Phase m_phase = AstSubgraphInstance::Phase::NONE;

    bool operator==(const SubgraphBatchKey& other) const {
        return m_ownerp == other.m_ownerp && m_scopep == other.m_scopep
               && m_senTreep == other.m_senTreep && m_wrapper == other.m_wrapper
               && m_phase == other.m_phase;
    }
};

struct SubgraphBatchKeyHash final {
    size_t operator()(const SubgraphBatchKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_ownerp);
        hash ^= std::hash<const void*>{}(key.m_scopep) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                + (hash >> 2);
        hash ^= std::hash<const void*>{}(key.m_senTreep) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                + (hash >> 2);
        hash ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.m_wrapper.m_kind))
                + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        hash ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.m_wrapper.m_keyword))
                + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        hash ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.m_phase)) + 0x9e3779b97f4a7c15ULL
                + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct SubgraphLogicRefSig final {
    uintptr_t m_access = 0;
    const AstVarScope* m_vscp = nullptr;
};

// Only value constants can become helper arguments. Constants below these skipped nodes define
// expression shape or scheduling and must remain compile-time constants.
class SubgraphParameterizableConstVisitor final : public VNVisitorConst {
    std::unordered_set<const AstConst*> m_constps;

    void visit(AstArraySel* nodep) override { iterateConstNull(nodep->fromp()); }
    void visit(AstConst* nodep) override {
        const V3Number& num = nodep->num();
        if (!num.is1Step() && !num.isAnyXZ() && !num.isNull() && !num.isOpaque()
            && nodep->dtypep()->basicp()) {
            m_constps.insert(nodep);
        }
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }
    void visit(AstNodeDType*) override {}
    void visit(AstNodeStream* nodep) override { iterateConstNull(nodep->lhsp()); }
    void visit(AstReplicate* nodep) override { iterateConstNull(nodep->srcp()); }
    void visit(AstSenItem*) override {}
    void visit(AstSel* nodep) override { iterateConstNull(nodep->fromp()); }
    void visit(AstSliceSel* nodep) override { iterateConstNull(nodep->fromp()); }

public:
    explicit SubgraphParameterizableConstVisitor(AstNode* nodep) { iterateConstNull(nodep); }

    bool contains(const AstConst* constp) const { return m_constps.count(constp); }
};

struct SubgraphLogicNodeSig final {
    std::vector<bool> m_constParameterizable;
    std::vector<const AstConst*> m_consts;
    std::vector<std::string> m_constValues;
    std::vector<uintptr_t> m_nodeTypes;
    uintptr_t m_type = 0;
    std::vector<SubgraphLogicRefSig> m_refs;
};

using SubgraphLogicSig = std::vector<SubgraphLogicNodeSig>;

struct SubgraphDomainShapes final {
    std::vector<uintptr_t> m_canonical;
    std::vector<uintptr_t> m_exact;
};

struct SubgraphLogicShape final {
    size_t m_constValues = 0;
    size_t m_nodeTypes = 0;
    size_t m_refAccesses = 0;

    bool operator==(const SubgraphLogicShape& other) const {
        return m_constValues == other.m_constValues && m_nodeTypes == other.m_nodeTypes
               && m_refAccesses == other.m_refAccesses;
    }
};

struct SubgraphScheduleArtifact;

enum class SubgraphTemplateMapFailReason : uint8_t {
    NONE,
    CONST_VALUE,
    NODE_COUNT,
    NODE_TOPOLOGY,
    NODE_TYPE,
    REF_ACCESS,
    REF_CONFLICT,
    REF_COUNT,
    REF_DTYPE,
};

enum class SubgraphSharedHelperApplyFailReason : uint8_t {
    NONE,
    ARGUMENTS,
    CALL_FUNCTION,
    CONSTANTS,
};

struct SubgraphOrderCacheEntry final {
    SubgraphScheduleArtifact* m_artifactp = nullptr;
    std::vector<uintptr_t> m_exactDomainShape;
    AstCFunc* m_funcp = nullptr;
    SubgraphLogicSig m_logicSig;
    std::shared_ptr<const V3Order::OrderRecipe> m_recipep;
    bool m_cloneable = true;
    bool m_triggeredShareable = false;
    bool m_triggeredWritesDelayedShadow = false;
    bool m_triggeredWritesInstanceLocal = false;
    bool m_triggeredWritesLocalTemp = false;
    bool m_triggeredWritesNonLocal = false;
    bool m_triggeredWritesTriggerTemp = false;
    bool m_triggeredWritesVlemTemp = false;
};

struct SubgraphOrderCacheBucket final {
    std::vector<SubgraphOrderCacheEntry> m_entries;
    size_t m_recipeIndex = std::numeric_limits<size_t>::max();
};

struct SubgraphOrderCacheKey final {
    std::vector<uintptr_t> m_domainShape;
    SubgraphLogicShape m_logicShape;
    AstNodeModule* m_modp = nullptr;
    AstSubgraphInstance::Phase m_phase = AstSubgraphInstance::Phase::NONE;
    SubgraphWrapper m_wrapper;

    bool operator==(const SubgraphOrderCacheKey& other) const {
        return m_modp == other.m_modp && m_phase == other.m_phase && m_wrapper == other.m_wrapper
               && m_domainShape == other.m_domainShape
               && m_logicShape.m_nodeTypes == other.m_logicShape.m_nodeTypes
               && m_logicShape.m_refAccesses == other.m_logicShape.m_refAccesses;
    }
};

struct SubgraphOrderCacheKeyHash final {
    size_t operator()(const SubgraphOrderCacheKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_modp);
        hash ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.m_phase)) + 0x9e3779b97f4a7c15ULL
                + (hash << 6) + (hash >> 2);
        hash ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.m_wrapper.m_kind))
                + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        hash ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.m_wrapper.m_keyword))
                + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        for (const uintptr_t value : key.m_domainShape) {
            hash ^= std::hash<uintptr_t>{}(value) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                    + (hash >> 2);
        }
        hash ^= key.m_logicShape.m_nodeTypes + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        hash ^= key.m_logicShape.m_refAccesses + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct SubgraphScheduleArtifactKey final {
    std::vector<uintptr_t> m_domainShape;
    SubgraphLogicShape m_logicShape;
    AstNodeModule* m_modp = nullptr;
    AstSubgraphInstance::Phase m_phase = AstSubgraphInstance::Phase::NONE;

    bool operator==(const SubgraphScheduleArtifactKey& other) const {
        return m_modp == other.m_modp && m_phase == other.m_phase
               && m_domainShape == other.m_domainShape && m_logicShape == other.m_logicShape;
    }
};

struct SubgraphScheduleArtifactKeyHash final {
    size_t operator()(const SubgraphScheduleArtifactKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_modp);
        hash ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.m_phase)) + 0x9e3779b97f4a7c15ULL
                + (hash << 6) + (hash >> 2);
        for (const uintptr_t value : key.m_domainShape) {
            hash ^= std::hash<uintptr_t>{}(value) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                    + (hash >> 2);
        }
        hash ^= key.m_logicShape.m_constValues + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        hash ^= key.m_logicShape.m_nodeTypes + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        hash ^= key.m_logicShape.m_refAccesses + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct SubgraphScheduleArtifactCoarseKey final {
    std::vector<uintptr_t> m_domainShape;
    AstNodeModule* m_modp = nullptr;
    AstSubgraphInstance::Phase m_phase = AstSubgraphInstance::Phase::NONE;

    bool operator==(const SubgraphScheduleArtifactCoarseKey& other) const {
        return m_modp == other.m_modp && m_phase == other.m_phase
               && m_domainShape == other.m_domainShape;
    }
};

struct SubgraphScheduleArtifactCoarseKeyHash final {
    size_t operator()(const SubgraphScheduleArtifactCoarseKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_modp);
        hash ^= std::hash<uint8_t>{}(static_cast<uint8_t>(key.m_phase)) + 0x9e3779b97f4a7c15ULL
                + (hash << 6) + (hash >> 2);
        for (const uintptr_t value : key.m_domainShape) {
            hash ^= std::hash<uintptr_t>{}(value) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                    + (hash >> 2);
        }
        return hash;
    }
};

size_t hashDomainShape(const std::vector<uintptr_t>& domainShape) {
    size_t hash = 0;
    for (const uintptr_t value : domainShape) {
        hash ^= std::hash<uintptr_t>{}(value) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
    }
    return hash;
}

uint64_t statStartUsecs() {
    if (!v3Global.opt.stats()) return 0;
    return V3Os::timeUsecs();
}

void addElapsedUsecs(uint64_t& accum, uint64_t startUsecs) {
    if (!startUsecs) return;
    accum += V3Os::timeUsecs() - startUsecs;
}

enum class SubgraphArtifactUncloneableReason : uint8_t {
    NONE,
    TRIGGERED,
    CLONE_FAIL,
};

enum class SubgraphSharedHelperSkipReason : uint8_t {
    NONE,
    CLONE_FAIL,
    MODULE_MISMATCH,
    NON_LOOSE,
    OTHER,
    PHASE,
    TRIGGERED,
    TRIGGERED_INPUT_TAIL,
    TRIGGERED_NOT_SHAREABLE,
};

struct SubgraphTailContract final {
    bool m_readsBoundaryInput = false;
    bool m_readsBoundaryState = false;
    bool m_readsExternal = false;
    bool m_writesBoundaryInput = false;
    bool m_writesBoundaryState = false;
    bool m_writesExternal = false;
};

enum class SubgraphInstanceLocalVarKind : uint8_t {
    NONE,
    DELAYED_SHADOW,
    LOCAL_TEMP,
    TRIGGER_CURR,
    TRIGGER_PREV,
    TRIGGER_SCHED,
    TRIGGER_STATE,
    TRIGGER_STATE_ACC,
    VLEM_TEMP,
};

struct SubgraphScheduleBundleContext final {
    AstScope* m_currentScopep = nullptr;
    SubgraphTailContract m_currentScopeTailContract;
    bool m_currentScopeHasDerivedBoundaryReads = false;
};

struct SubgraphSharedHelperContext final {
    const SubgraphScheduleBundleContext* m_bundlep = nullptr;
    AstSubgraphInstance::Phase m_phase = AstSubgraphInstance::Phase::NONE;
};

struct SubgraphSharedHelperArg final {
    AstVarScope* m_vscp = nullptr;
    bool m_reads = false;
    bool m_writes = false;
};

struct SubgraphSharedHelperHiddenUseKey final {
    const AstCFunc* m_funcp = nullptr;
    const AstScope* m_sourceBoundaryScopep = nullptr;
    const AstScope* m_targetBoundaryScopep = nullptr;

    bool operator==(const SubgraphSharedHelperHiddenUseKey& other) const {
        return m_funcp == other.m_funcp && m_sourceBoundaryScopep == other.m_sourceBoundaryScopep
               && m_targetBoundaryScopep == other.m_targetBoundaryScopep;
    }
};

struct SubgraphSharedHelperHiddenUseKeyHash final {
    size_t operator()(const SubgraphSharedHelperHiddenUseKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_funcp);
        hash ^= std::hash<const void*>{}(key.m_sourceBoundaryScopep) + 0x9e3779b97f4a7c15ULL
                + (hash << 6) + (hash >> 2);
        hash ^= std::hash<const void*>{}(key.m_targetBoundaryScopep) + 0x9e3779b97f4a7c15ULL
                + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct SubgraphSharedHelperRemapVariant final {
    AstCFunc* m_callFuncp = nullptr;
    std::vector<std::string> m_constValues;
    std::vector<SubgraphSharedHelperArg> m_helperArgs;
    std::vector<const AstVarScope*> m_implicitVscps;
};

enum class SubgraphSharedHelperVarRole : uint8_t {
    ARGUMENT,
    HELPER_LOCAL,
    IDENTITY,
    IMPLICIT_CONTEXT,
};

struct SubgraphScheduleArtifact final {
    AstCFunc* m_callFuncp = nullptr;
    std::vector<SubgraphSharedHelperArg> m_helperArgs;
    SubgraphScheduleArtifactKey m_key;
    SubgraphLogicSig m_logicSig;
    std::vector<SubgraphSharedHelperRemapVariant> m_remapVariants;
    std::unordered_map<AstScope*, AstCFunc*> m_scopeCloneFuncps;
    AstScope* m_scopep = nullptr;
    SubgraphTailContract m_tailContract;
    std::unordered_map<const AstVarScope*, SubgraphSharedHelperVarRole> m_varMapContract;
    bool m_cloneable = true;
    bool m_hasTriggered = false;
    bool m_triggeredShareable = false;
    SubgraphArtifactUncloneableReason m_uncloneableReason
        = SubgraphArtifactUncloneableReason::NONE;
};

struct SubgraphScheduleRemap final {
    std::unordered_map<const AstVarScope*, AstVarScope*> m_templateVarMap;
};

struct SubgraphScheduleArtifactReuse final {
    SubgraphScheduleArtifact* m_artifactp = nullptr;
    SubgraphScheduleRemap m_remap;
};

struct SubgraphTriggeredRefInfo final {
    bool m_hasTriggered = false;
    bool m_shareable = true;
    bool m_writesDelayedShadow = false;
    bool m_writesInstanceLocal = false;
    bool m_writesLocalTemp = false;
    bool m_writesNonLocal = false;
    bool m_writesTriggerTemp = false;
    bool m_writesVlemTemp = false;

    bool writesOnlySharedHelperSafeInstanceLocal() const {
        return m_writesInstanceLocal && m_writesDelayedShadow && !m_writesLocalTemp
               && !m_writesTriggerTemp && !m_writesVlemTemp;
    }
};

struct SubgraphScheduleInstance final {
    AstCFunc* m_callFuncp = nullptr;
    SubgraphInstanceContract m_contract;
    std::vector<SubgraphSharedHelperArg> m_helperArgs;
    AstScope* m_scopep = nullptr;
    bool m_sharedCall = false;
    std::vector<AstCFunc*> m_tailFuncps;
    AstSenTree* m_triggerDomainp = nullptr;
};

struct SubgraphSchedulePlan final {
    SubgraphScheduleArtifact* m_artifactp = nullptr;
    SubgraphScheduleInstance m_instance;
    AstSubgraphInstance::Phase m_phase = AstSubgraphInstance::Phase::NONE;
    SubgraphWrapper m_wrapper;
};

struct SubgraphScheduleBundle final {
    std::vector<SubgraphSchedulePlan> m_plans;

    bool empty() const { return m_plans.empty(); }
};

struct SubgraphScheduledGroup final {
    SubgraphScheduleBundle m_bundle;
    const SubgraphGroup* m_groupp = nullptr;
    AstActive* m_subgraphActivep = nullptr;
};

struct SubgraphLogicInputStats final {
    uint64_t m_actives = 0;
    uint64_t m_directOtherStatements = 0;
    uint64_t m_directSnapshotInstances = 0;
    uint64_t m_directSnapshotProcedures = 0;
    uint64_t m_directStatements = 0;
    uint64_t m_directSubgraphInstances = 0;
    uint64_t m_nodes = 0;
    uint64_t m_parentVisibleNodes = 0;
    uint64_t m_snapshotInstanceBodyNodes = 0;
    uint64_t m_snapshotProcedureBodyNodes = 0;
    uint64_t m_subgraphInstanceBodyNodes = 0;
    uint64_t m_subgraphInstances = 0;
};

struct SnapshotRef final {
    AstVarScope* m_snapshotVscp = nullptr;
    uint32_t m_elemIndex = 0;
    bool m_isBundle = false;
};

struct SnapshotBucket final {
    LogicByScope* m_ownerp = nullptr;
    AstSenTree* m_senTreep = nullptr;
    std::vector<AstVarScope*> m_sourceVars;
    std::unordered_set<AstVarScope*> m_seen;
    std::unordered_map<AstVarScope*, SnapshotRef> m_snapshotRefs;
};

struct SnapshotBucketKey final {
    LogicByScope* m_ownerp = nullptr;
    AstSenTree* m_senTreep = nullptr;

    bool operator==(const SnapshotBucketKey& other) const {
        return m_ownerp == other.m_ownerp && m_senTreep == other.m_senTreep;
    }
};

struct SnapshotBucketKeyHash final {
    size_t operator()(const SnapshotBucketKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_ownerp);
        hash ^= std::hash<const void*>{}(key.m_senTreep) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                + (hash >> 2);
        return hash;
    }
};

struct SnapshotSourceSetKey final {
    std::vector<AstVarScope*> m_sourceVars;

    bool operator==(const SnapshotSourceSetKey& other) const {
        return m_sourceVars == other.m_sourceVars;
    }
};

struct SnapshotSourceSetKeyHash final {
    size_t operator()(const SnapshotSourceSetKey& key) const {
        size_t hash = 0;
        for (const AstVarScope* const vscp : key.m_sourceVars) {
            hash ^= std::hash<const void*>{}(vscp) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                    + (hash >> 2);
        }
        return hash;
    }
};

struct SnapshotHelperKey final {
    AstScope* m_scopep = nullptr;
    bool m_slow = false;
    std::vector<AstVarScope*> m_sourceVars;

    bool operator==(const SnapshotHelperKey& other) const {
        return m_scopep == other.m_scopep && m_slow == other.m_slow
               && m_sourceVars == other.m_sourceVars;
    }
};

struct SnapshotHelperKeyHash final {
    size_t operator()(const SnapshotHelperKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_scopep);
        hash ^= std::hash<bool>{}(key.m_slow) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        for (const AstVarScope* const vscp : key.m_sourceVars) {
            hash ^= std::hash<const void*>{}(vscp) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                    + (hash >> 2);
        }
        return hash;
    }
};

struct SnapshotHelperEntry final {
    AstCFunc* m_funcp = nullptr;
};

struct TailCloneRefSig final {
    uintptr_t m_access = 0;
    const AstVarScope* m_vscp = nullptr;

    bool operator==(const TailCloneRefSig& other) const {
        return m_access == other.m_access && m_vscp == other.m_vscp;
    }
};

struct TailCloneSig final {
    V3Hash m_bodyHash;
    std::vector<const AstCFunc*> m_calls;
    std::vector<TailCloneRefSig> m_refs;

    bool operator==(const TailCloneSig& other) const {
        return m_bodyHash == other.m_bodyHash && m_calls == other.m_calls
               && m_refs == other.m_refs;
    }
};

struct TailCloneKey final {
    AstScope* m_boundaryScopep = nullptr;
    LogicByScope* m_ownerp = nullptr;
    AstSenTree* m_senTreep = nullptr;
    bool m_slow = false;
    TailCloneSig m_tailSig;

    bool operator==(const TailCloneKey& other) const {
        return m_boundaryScopep == other.m_boundaryScopep && m_ownerp == other.m_ownerp
               && m_senTreep == other.m_senTreep && m_slow == other.m_slow
               && m_tailSig == other.m_tailSig;
    }
};

struct TailCloneKeyHash final {
    size_t operator()(const TailCloneKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_boundaryScopep);
        hash ^= std::hash<const void*>{}(key.m_ownerp) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                + (hash >> 2);
        hash ^= std::hash<const void*>{}(key.m_senTreep) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                + (hash >> 2);
        hash ^= std::hash<bool>{}(key.m_slow) + 0x9e3779b97f4a7c15ULL + (hash << 6) + (hash >> 2);
        hash ^= std::hash<V3Hash>{}(key.m_tailSig.m_bodyHash) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                + (hash >> 2);
        for (const AstCFunc* const funcp : key.m_tailSig.m_calls) {
            hash ^= std::hash<const void*>{}(funcp) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                    + (hash >> 2);
        }
        for (const TailCloneRefSig& ref : key.m_tailSig.m_refs) {
            hash ^= std::hash<uintptr_t>{}(ref.m_access) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                    + (hash >> 2);
            hash ^= std::hash<const void*>{}(ref.m_vscp) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                    + (hash >> 2);
        }
        return hash;
    }
};

class SnapshotNameAllocator final {
    std::unordered_map<AstScope*, std::unordered_set<std::string>> m_usedNames;

    std::unordered_set<std::string>& usedNamesFor(AstScope* scopep) {
        std::unordered_set<std::string>& usedNames = m_usedNames[scopep];
        if (!usedNames.empty()) return usedNames;
        for (AstVarScope* vscp = scopep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
            usedNames.insert(vscp->varp()->name());
        }
        return usedNames;
    }

public:
    std::string get(AstScope* scopep, const std::string& base) {
        std::unordered_set<std::string>& usedNames = usedNamesFor(scopep);
        if (usedNames.insert(base).second) return base;
        for (unsigned index = 1;; ++index) {
            const std::string name = base + "__" + cvtToStr(index);
            if (usedNames.insert(name).second) return name;
        }
    }
};

struct SubgraphInternalOrderAggregate final {
    uint64_t m_calls = 0;
    uint64_t m_constants = 0;
    uint64_t m_maxNodes = 0;
    uint64_t m_nodes = 0;
    uint64_t m_refs = 0;
    uint64_t m_usecs = 0;
};

struct SubgraphLoweringStats final {
    uint64_t m_artifactKeyMaxEntriesPerFullKey = 0;
    uint64_t m_artifactKeyUniqueDomainShapes = 0;
    uint64_t m_artifactKeyUniqueFull = 0;
    uint64_t m_artifactKeyUniqueModules = 0;
    uint64_t m_artifactMisses = 0;
    uint64_t m_artifactReuseLookups = 0;
    uint64_t m_artifactReuseMissLogicMismatch = 0;
    uint64_t m_artifactReuseMissNoEntry = 0;
    uint64_t m_artifactReuseMissNoEntryConstValue = 0;
    uint64_t m_artifactReuseMissNoEntryNodeTopology = 0;
    uint64_t m_artifactReuseMissNoEntryRefAccess = 0;
    uint64_t m_artifactReuseMissNoEntrySameDomainShape = 0;
    uint64_t m_artifactReuseMissNoEntrySameModule = 0;
    uint64_t m_artifactReuseMissNoEntrySameModuleDomainShape = 0;
    uint64_t m_artifactReuseMissNoEntrySameModulePhase = 0;
    uint64_t m_artifactReuseFullKeyCandidates = 0;
    uint64_t m_artifactReuseFullKeyHits = 0;
    uint64_t m_artifactReuseScopeCloneHits = 0;
    uint64_t m_artifactReuseScopeClones = 0;
    uint64_t m_artifactReuseSharedCalls = 0;
    uint64_t m_artifactReuseSharedSkipCloneFail = 0;
    uint64_t m_artifactReuseSharedSkipModuleMismatch = 0;
    uint64_t m_artifactReuseSharedSkipNonLoose = 0;
    uint64_t m_artifactReuseSharedSkipOther = 0;
    uint64_t m_artifactReuseSharedSkipPhase = 0;
    uint64_t m_artifactReuseSharedSkipTriggered = 0;
    uint64_t m_artifactReuseSharedSkipTriggeredInputTail = 0;
    uint64_t m_artifactReuseSharedSkipTriggeredNotShareable = 0;
    uint64_t m_artifactReuseSharedSkipTriggeredOther = 0;
    uint64_t m_artifactReuseSkipCloneFail = 0;
    uint64_t m_artifactReuseSkipOther = 0;
    uint64_t m_artifactReuseSkipTriggered = 0;
    uint64_t m_artifactReuseTemplateMapFailConstValue = 0;
    uint64_t m_artifactReuseTemplateMapFailNodeCount = 0;
    uint64_t m_artifactReuseTemplateMapFailNodeTopology = 0;
    uint64_t m_artifactReuseTemplateMapFailNodeType = 0;
    uint64_t m_artifactReuseTemplateMapFailRefAccess = 0;
    uint64_t m_artifactReuseTemplateMapFailRefConflict = 0;
    uint64_t m_artifactReuseTemplateMapFailRefCount = 0;
    uint64_t m_artifactReuseTemplateMapFailRefDType = 0;
    uint64_t m_artifactReuseTemplateMapFails = 0;
    uint64_t m_artifactReuses = 0;
    uint64_t m_artifactReuseCloneFails = 0;
    uint64_t m_artifactReuseCoarseHits = 0;
    uint64_t m_artifactReuseCoarseLookups = 0;
    uint64_t m_artifactReuseCoarseMisses = 0;
    uint64_t m_artifactTailCloneFails = 0;
    uint64_t m_artifactTailReuseCandidates = 0;
    uint64_t m_artifactTailReuses = 0;
    uint64_t m_artifacts = 0;
    uint64_t m_bundleBuilds = 0;
    uint64_t m_bundleEmpty = 0;
    uint64_t m_bundleMaterialized = 0;
    uint64_t m_bundleMaterializedPlans = 0;
    uint64_t m_bundlePlans = 0;
    uint64_t m_contractExternalUseScans = 0;
    uint64_t m_contractExternalUseSnapshotSkips = 0;
    uint64_t m_groups = 0;
    uint64_t m_inputActivesAfter = 0;
    uint64_t m_inputActivesBefore = 0;
    uint64_t m_inputDirectOtherStatementsAfter = 0;
    uint64_t m_inputDirectOtherStatementsBefore = 0;
    uint64_t m_inputDirectSnapshotInstancesAfter = 0;
    uint64_t m_inputDirectSnapshotInstancesBefore = 0;
    uint64_t m_inputDirectSnapshotProceduresAfter = 0;
    uint64_t m_inputDirectSnapshotProceduresBefore = 0;
    uint64_t m_inputDirectStatementsAfter = 0;
    uint64_t m_inputDirectStatementsBefore = 0;
    uint64_t m_inputDirectSubgraphInstancesAfter = 0;
    uint64_t m_inputDirectSubgraphInstancesBefore = 0;
    uint64_t m_inputNodesAfter = 0;
    uint64_t m_inputNodesBefore = 0;
    uint64_t m_inputParentVisibleNodesAfter = 0;
    uint64_t m_inputParentVisibleNodesBefore = 0;
    uint64_t m_inputSnapshotInstanceBodyNodesAfter = 0;
    uint64_t m_inputSnapshotInstanceBodyNodesBefore = 0;
    uint64_t m_inputSnapshotProcedureBodyNodesAfter = 0;
    uint64_t m_inputSnapshotProcedureBodyNodesBefore = 0;
    uint64_t m_inputSubgraphInstanceBodyNodesAfter = 0;
    uint64_t m_inputSubgraphInstanceBodyNodesBefore = 0;
    uint64_t m_inputSubgraphInstancesAfter = 0;
    uint64_t m_inputSubgraphInstancesBefore = 0;
    uint64_t m_logicShapeBuilds = 0;
    uint64_t m_logicSigBuilds = 0;
    uint64_t m_logicSigBuildsAvoided = 0;
    uint64_t m_orderCacheCloneApplyFailArguments = 0;
    uint64_t m_orderCacheCloneApplyFailConstants = 0;
    uint64_t m_orderCacheCloneFailOther = 0;
    uint64_t m_orderCacheCloneFailState = 0;
    uint64_t m_orderCacheCloneFailShadow = 0;
    uint64_t m_orderCacheCloneFailTemp = 0;
    uint64_t m_orderCacheCloneFailVlem = 0;
    uint64_t m_orderCacheCloneNull = 0;
    std::map<string, uint64_t> m_orderCacheCloneFailNames;
    uint64_t m_orderCacheCloneGeneratedVarRemapDelayed = 0;
    uint64_t m_orderCacheCloneGeneratedVarRemapTemp = 0;
    uint64_t m_orderCacheCloneGeneratedVarRemapTrigger = 0;
    uint64_t m_orderCacheCloneGeneratedVarRemapVlem = 0;
    uint64_t m_orderCacheCloneGeneratedVarRemaps = 0;
    uint64_t m_orderCacheDirectIndexFallbacks = 0;
    uint64_t m_orderCacheDirectIndexHits = 0;
    uint64_t m_orderCacheDirectIndexLookups = 0;
    uint64_t m_orderCacheEntries = 0;
    uint64_t m_orderCacheEntryHits = 0;
    uint64_t m_orderCacheHits = 0;
    uint64_t m_orderCacheLookups = 0;
    uint64_t m_orderCacheMisses = 0;
    uint64_t m_orderCacheMissNoEntryConstValue = 0;
    uint64_t m_orderCacheMissNoEntryNodeTopology = 0;
    uint64_t m_orderCacheMissNoEntryRefAccess = 0;
    uint64_t m_orderCacheRecipeClones = 0;
    uint64_t m_orderCacheRecipeConstantRemaps = 0;
    uint64_t m_orderCacheRecipeHits = 0;
    uint64_t m_orderCacheRecipeReplays = 0;
    uint64_t m_orderCacheRecipeSharedHits = 0;
    uint64_t m_orderCacheSharedHits = 0;
    uint64_t m_orderCacheSharedSkipArguments = 0;
    uint64_t m_orderCacheSharedSkipCallFunction = 0;
    uint64_t m_orderCacheSharedSkipCloneFail = 0;
    uint64_t m_orderCacheSharedSkipConstants = 0;
    uint64_t m_orderCacheSharedSkipModuleMismatch = 0;
    uint64_t m_orderCacheSharedSkipNonLoose = 0;
    uint64_t m_orderCacheSharedSkipOther = 0;
    uint64_t m_orderCacheSharedSkipPhase = 0;
    uint64_t m_orderCacheSharedSkipTriggered = 0;
    uint64_t m_orderCacheSharedSkipTriggeredInputTail = 0;
    uint64_t m_orderCacheSharedSkipTriggeredNotShareable = 0;
    uint64_t m_orderCacheSharedSkipTriggeredOther = 0;
    uint64_t m_orderCacheSharedSkipVarMap = 0;
    uint64_t m_orderCacheSkipTriggered = 0;
    uint64_t m_orderCacheSkipTriggeredInstanceLocal = 0;
    uint64_t m_orderCacheSkipTriggeredNoArtifact = 0;
    uint64_t m_orderCacheSkipTriggeredNotShareable = 0;
    uint64_t m_orderCacheSkipTriggeredStl = 0;
    uint64_t m_orderCacheTemplateMapFailConstValue = 0;
    uint64_t m_orderCacheTemplateMapFailNodeCount = 0;
    uint64_t m_orderCacheTemplateMapFailNodeTopology = 0;
    uint64_t m_orderCacheTemplateMapFailNodeType = 0;
    uint64_t m_orderCacheTemplateMapFailRefAccess = 0;
    uint64_t m_orderCacheTemplateMapFailRefConflict = 0;
    uint64_t m_orderCacheTemplateMapFailRefCount = 0;
    uint64_t m_orderCacheTemplateMapFailRefDType = 0;
    uint64_t m_orderCacheTemplateMapFails = 0;
    uint64_t m_orderCacheVariantBuckets = 0;
    uint64_t m_orderCacheVariantCandidates = 0;
    uint64_t m_orderCacheVariantMax = 0;
    uint64_t m_orderedFuncClones = 0;
    uint64_t m_parentConsumedContractWrites = 0;
    uint64_t m_parentConsumedSubgraphVars = 0;
    uint64_t m_schedulePlans = 0;
    uint64_t m_sharedHelperCallArgs = 0;
    uint64_t m_sharedHelperCallArgsMax = 0;
    uint64_t m_sharedHelperConstantArgs = 0;
    uint64_t m_sharedHelperContractArgumentVars = 0;
    uint64_t m_sharedHelperContractHelperLocalVars = 0;
    uint64_t m_sharedHelperContractIdentityVars = 0;
    uint64_t m_sharedHelperContractImplicitContextVars = 0;
    uint64_t m_sharedHelperExternalArgs = 0;
    uint64_t m_sharedHelperFormalArgsAfter = 0;
    uint64_t m_sharedHelperFormalArgsBefore = 0;
    uint64_t m_sharedHelperFormalArgsMax = 0;
    uint64_t m_sharedHelperHiddenUseCacheHits = 0;
    uint64_t m_sharedHelperHiddenUseCalls = 0;
    uint64_t m_sharedHelperHiddenUseScans = 0;
    uint64_t m_sharedHelperHiddenUses = 0;
    uint64_t m_sharedHelperImplicitContextVars = 0;
    uint64_t m_sharedHelperInstanceLocalArgs = 0;
    uint64_t m_sharedHelperParameterizationFails = 0;
    uint64_t m_sharedHelperParameterizations = 0;
    uint64_t m_sharedHelperParameterizedFuncs = 0;
    uint64_t m_sharedHelperRemapVariantBuilds = 0;
    uint64_t m_sharedHelperRemapVariantCandidateVars = 0;
    uint64_t m_sharedHelperRemapVariantCandidateVarsMax = 0;
    uint64_t m_sharedHelperRemapVariantConstantRemaps = 0;
    uint64_t m_sharedHelperRemapVariantHits = 0;
    uint64_t m_sharedHelperRemapVariantOversizeSkips = 0;
    uint64_t m_sharedHelperRemapVariantVars = 0;
    uint64_t m_sharedHelperScopeIndexBuilds = 0;
    uint64_t m_sharedHelperScopeIndexHits = 0;
    uint64_t m_sharedHelperStlArgumentSkips = 0;
    uint64_t m_sharedHelperVarIndexBuilds = 0;
    uint64_t m_sharedHelperVarIndexHits = 0;
    uint64_t m_snapshotBuckets = 0;
    uint64_t m_snapshotBundleElems = 0;
    uint64_t m_snapshotBundles = 0;
    uint64_t m_snapshotHelpers = 0;
    uint64_t m_snapshotHelperReuses = 0;
    uint64_t m_snapshotProcedures = 0;
    uint64_t m_snapshotScalars = 0;
    uint64_t m_snapshotSourceSetDuplicates = 0;
    uint64_t m_snapshotSourceSets = 0;
    uint64_t m_snapshotSources = 0;
    uint64_t m_tailCloneReuses = 0;
    uint64_t m_tailClones = 0;
    uint64_t m_tailWrappers = 0;
    uint64_t m_timeBuildContractUsecs = 0;
    uint64_t m_timeBuildLogicShapeUsecs = 0;
    uint64_t m_timeBuildLogicSigUsecs = 0;
    uint64_t m_timeBuildPlansUsecs = 0;
    uint64_t m_timeCloneOrderedFuncsUsecs = 0;
    uint64_t m_timeCollectGroupsUsecs = 0;
    uint64_t m_timeCollectInputStatsUsecs = 0;
    uint64_t m_timeCollectRegionWrittenVarsUsecs = 0;
    uint64_t m_timeComputeDomainShapeUsecs = 0;
    uint64_t m_timeDiscardLogicUsecs = 0;
    uint64_t m_timeEmitSnapshotsUsecs = 0;
    uint64_t m_timeInternalOrderUsecs = 0;
    uint64_t m_timeLookupArtifactsUsecs = 0;
    uint64_t m_timeLowerGroupsUsecs = 0;
    uint64_t m_timeMakeArtifactsUsecs = 0;
    uint64_t m_timeMarkHiddenUsesUsecs = 0;
    uint64_t m_timeMaterializeUsecs = 0;
    uint64_t m_timeParameterizeHelpersUsecs = 0;
    uint64_t m_timeParameterizeRemapVariantsUsecs = 0;
    uint64_t m_timePrepareSnapshotsUsecs = 0;
    uint64_t m_timePopulateHelperArgsUsecs = 0;
    uint64_t m_timeRecipeReplayUsecs = 0;
    uint64_t m_timeSplitOrderedFuncsUsecs = 0;
    uint64_t m_timeTemplateMapUsecs = 0;
    uint64_t m_timeTotalUsecs = 0;
    uint64_t m_timeTriggeredAnalysisUsecs = 0;
    uint64_t m_triggeredArtifactCandidates = 0;
    uint64_t m_triggeredArtifactInputTailShareable = 0;
    uint64_t m_triggeredArtifactInputTailWrites = 0;
    uint64_t m_triggeredArtifactNoNonLocalInstanceLocalWrites = 0;
    uint64_t m_triggeredArtifactNoNonLocalWrites = 0;
    uint64_t m_triggeredArtifactShareable = 0;
    uint64_t m_triggeredArtifactUnshareable = 0;
    uint64_t m_triggeredArtifactWritesDelayedShadow = 0;
    uint64_t m_triggeredArtifactWritesInstanceLocal = 0;
    uint64_t m_triggeredArtifactWritesLocalTemp = 0;
    uint64_t m_triggeredArtifactWritesNonLocal = 0;
    uint64_t m_triggeredArtifactWritesTriggerTemp = 0;
    uint64_t m_triggeredArtifactWritesVlemTemp = 0;
    uint64_t m_triggeredRefCurr = 0;
    uint64_t m_triggeredRefOther = 0;
    uint64_t m_triggeredRefPrev = 0;
    uint64_t m_triggeredRefSched = 0;
    uint64_t m_triggeredRefState = 0;
    uint64_t m_triggeredRefStateAcc = 0;
    uint64_t m_instances = 0;
    std::map<std::string, SubgraphInternalOrderAggregate> m_internalOrderAggregates;
    std::vector<std::pair<std::string, uint64_t>> m_internalOrderTimes;

    static uint64_t ratioPermille(uint64_t numerator, uint64_t denominator) {
        if (!denominator) return 0;
        return numerator * 1000 / denominator;
    }

    static double seconds(uint64_t usecs) { return usecs / 1.0e6; }

    static void addTemplateMapFailStats(const string& prefix, const string& path,
                                        uint64_t constValue, uint64_t nodeCount,
                                        uint64_t nodeTopology, uint64_t nodeType,
                                        uint64_t refAccess, uint64_t refConflict,
                                        uint64_t refCount, uint64_t refDType, uint64_t total) {
        V3Stats::addStat(prefix + path + " template map fail constant value", constValue);
        V3Stats::addStat(prefix + path + " template map fail node count", nodeCount);
        V3Stats::addStat(prefix + path + " template map fail node topology", nodeTopology);
        V3Stats::addStat(prefix + path + " template map fail node type", nodeType);
        V3Stats::addStat(prefix + path + " template map fail ref access", refAccess);
        V3Stats::addStat(prefix + path + " template map fail ref conflict", refConflict);
        V3Stats::addStat(prefix + path + " template map fail ref count", refCount);
        V3Stats::addStat(prefix + path + " template map fail ref dtype", refDType);
        V3Stats::addStat(prefix + path + " template map fails", total);
    }

    void report(const string& tag) const {
        if (!v3Global.opt.stats()) return;
        const string prefix = "Scheduling, Subgraph " + tag + ", ";
        const uint64_t artifactLookups = m_artifactMisses + m_artifactReuses;
        const uint64_t inputParentVisibleNodesReduced
            = m_inputParentVisibleNodesBefore > m_inputParentVisibleNodesAfter
                  ? m_inputParentVisibleNodesBefore - m_inputParentVisibleNodesAfter
                  : 0;
        const uint64_t inputHiddenBodyNodesAfter
            = m_inputSubgraphInstanceBodyNodesAfter + m_inputSnapshotProcedureBodyNodesAfter;
        const uint64_t orderCacheLookups = m_orderCacheHits + m_orderCacheMisses;
        const uint64_t artifactReuseScopeCloneCalls
            = m_artifactReuseScopeCloneHits + m_artifactReuseScopeClones;
        const uint64_t artifactReuseHelperCalls
            = m_artifactReuseSharedCalls + artifactReuseScopeCloneCalls;
        V3Stats::addStat(prefix + "artifact key max entries per full key",
                         m_artifactKeyMaxEntriesPerFullKey);
        V3Stats::addStat(prefix + "artifact key unique domain shapes",
                         m_artifactKeyUniqueDomainShapes);
        V3Stats::addStat(prefix + "artifact key unique full", m_artifactKeyUniqueFull);
        V3Stats::addStat(prefix + "artifact key unique modules", m_artifactKeyUniqueModules);
        V3Stats::addStat(prefix + "artifact misses", m_artifactMisses);
        V3Stats::addStat(prefix + "artifact reuse coarse hits", m_artifactReuseCoarseHits);
        V3Stats::addStat(prefix + "artifact reuse coarse lookups", m_artifactReuseCoarseLookups);
        V3Stats::addStat(prefix + "artifact reuse coarse misses", m_artifactReuseCoarseMisses);
        V3Stats::addStat(prefix + "artifact reuse lookups", m_artifactReuseLookups);
        V3Stats::addStat(prefix + "artifact reuse miss logic mismatch",
                         m_artifactReuseMissLogicMismatch);
        V3Stats::addStat(prefix + "artifact reuse miss no entry", m_artifactReuseMissNoEntry);
        V3Stats::addStat(prefix + "artifact reuse miss no entry constant value",
                         m_artifactReuseMissNoEntryConstValue);
        V3Stats::addStat(prefix + "artifact reuse miss no entry node topology",
                         m_artifactReuseMissNoEntryNodeTopology);
        V3Stats::addStat(prefix + "artifact reuse miss no entry ref access",
                         m_artifactReuseMissNoEntryRefAccess);
        V3Stats::addStat(prefix + "artifact reuse miss no entry same domain shape",
                         m_artifactReuseMissNoEntrySameDomainShape);
        V3Stats::addStat(prefix + "artifact reuse miss no entry same module",
                         m_artifactReuseMissNoEntrySameModule);
        V3Stats::addStat(prefix + "artifact reuse miss no entry same module domain shape",
                         m_artifactReuseMissNoEntrySameModuleDomainShape);
        V3Stats::addStat(prefix + "artifact reuse miss no entry same module phase",
                         m_artifactReuseMissNoEntrySameModulePhase);
        V3Stats::addStat(prefix + "artifact reuse full key candidates",
                         m_artifactReuseFullKeyCandidates);
        V3Stats::addStat(prefix + "artifact reuse full key hits", m_artifactReuseFullKeyHits);
        V3Stats::addStat(prefix + "artifact reuse scope clone hits",
                         m_artifactReuseScopeCloneHits);
        V3Stats::addStat(prefix + "artifact reuse scope clone calls",
                         artifactReuseScopeCloneCalls);
        V3Stats::addStat(prefix + "artifact reuse scope clones", m_artifactReuseScopeClones);
        V3Stats::addStat(prefix + "artifact reuse shared calls", m_artifactReuseSharedCalls);
        V3Stats::addStat(prefix + "artifact reuse shared clone avoids",
                         m_artifactReuseSharedCalls);
        V3Stats::addStat(prefix + "artifact reuse shared call permille",
                         ratioPermille(m_artifactReuseSharedCalls, artifactReuseHelperCalls));
        V3Stats::addStat(prefix + "artifact reuse shared skip clone fail",
                         m_artifactReuseSharedSkipCloneFail);
        V3Stats::addStat(prefix + "artifact reuse shared skip module mismatch",
                         m_artifactReuseSharedSkipModuleMismatch);
        V3Stats::addStat(prefix + "artifact reuse shared skip non loose",
                         m_artifactReuseSharedSkipNonLoose);
        V3Stats::addStat(prefix + "artifact reuse shared skip other",
                         m_artifactReuseSharedSkipOther);
        V3Stats::addStat(prefix + "artifact reuse shared skip phase",
                         m_artifactReuseSharedSkipPhase);
        V3Stats::addStat(prefix + "artifact reuse shared skip triggered",
                         m_artifactReuseSharedSkipTriggered);
        V3Stats::addStat(prefix + "artifact reuse shared skip triggered input tail",
                         m_artifactReuseSharedSkipTriggeredInputTail);
        V3Stats::addStat(prefix + "artifact reuse shared skip triggered not shareable",
                         m_artifactReuseSharedSkipTriggeredNotShareable);
        V3Stats::addStat(prefix + "artifact reuse shared skip triggered other",
                         m_artifactReuseSharedSkipTriggeredOther);
        V3Stats::addStat(prefix + "artifact reuse skip clone fail", m_artifactReuseSkipCloneFail);
        V3Stats::addStat(prefix + "artifact reuse skip other", m_artifactReuseSkipOther);
        V3Stats::addStat(prefix + "artifact reuse skip triggered", m_artifactReuseSkipTriggered);
        addTemplateMapFailStats(
            prefix, "artifact reuse", m_artifactReuseTemplateMapFailConstValue,
            m_artifactReuseTemplateMapFailNodeCount, m_artifactReuseTemplateMapFailNodeTopology,
            m_artifactReuseTemplateMapFailNodeType, m_artifactReuseTemplateMapFailRefAccess,
            m_artifactReuseTemplateMapFailRefConflict, m_artifactReuseTemplateMapFailRefCount,
            m_artifactReuseTemplateMapFailRefDType, m_artifactReuseTemplateMapFails);
        V3Stats::addStat(prefix + "artifact reuses", m_artifactReuses);
        V3Stats::addStat(prefix + "artifact reuse permille",
                         ratioPermille(m_artifactReuses, artifactLookups));
        V3Stats::addStat(prefix + "artifact reuse clone fails", m_artifactReuseCloneFails);
        V3Stats::addStat(prefix + "artifact tail clone fails", m_artifactTailCloneFails);
        V3Stats::addStat(prefix + "artifact tail reuse candidates", m_artifactTailReuseCandidates);
        V3Stats::addStat(prefix + "artifact tail reuses", m_artifactTailReuses);
        V3Stats::addStat(prefix + "artifacts", m_artifacts);
        V3Stats::addStat(prefix + "bundle builds", m_bundleBuilds);
        V3Stats::addStat(prefix + "bundle empty", m_bundleEmpty);
        V3Stats::addStat(prefix + "bundle materialized", m_bundleMaterialized);
        V3Stats::addStat(prefix + "bundle materialized plans", m_bundleMaterializedPlans);
        V3Stats::addStat(prefix + "bundle materialized plans per bundle permille",
                         ratioPermille(m_bundleMaterializedPlans, m_bundleMaterialized));
        V3Stats::addStat(prefix + "bundle plans", m_bundlePlans);
        V3Stats::addStat(prefix + "bundle plans per build permille",
                         ratioPermille(m_bundlePlans, m_bundleBuilds));
        V3Stats::addStat(prefix + "contract external use scans", m_contractExternalUseScans);
        V3Stats::addStat(prefix + "contract external use snapshot skips",
                         m_contractExternalUseSnapshotSkips);
        V3Stats::addStat(prefix + "groups", m_groups);
        V3Stats::addStat(prefix + "input actives after", m_inputActivesAfter);
        V3Stats::addStat(prefix + "input actives before", m_inputActivesBefore);
        V3Stats::addStat(prefix + "input direct other statements after",
                         m_inputDirectOtherStatementsAfter);
        V3Stats::addStat(prefix + "input direct other statements before",
                         m_inputDirectOtherStatementsBefore);
        V3Stats::addStat(prefix + "input direct snapshot instances after",
                         m_inputDirectSnapshotInstancesAfter);
        V3Stats::addStat(prefix + "input direct snapshot instances before",
                         m_inputDirectSnapshotInstancesBefore);
        V3Stats::addStat(prefix + "input direct snapshot procedures after",
                         m_inputDirectSnapshotProceduresAfter);
        V3Stats::addStat(prefix + "input direct snapshot procedures before",
                         m_inputDirectSnapshotProceduresBefore);
        V3Stats::addStat(prefix + "input direct statements after", m_inputDirectStatementsAfter);
        V3Stats::addStat(prefix + "input direct statements before", m_inputDirectStatementsBefore);
        V3Stats::addStat(prefix + "input direct subgraph instances after",
                         m_inputDirectSubgraphInstancesAfter);
        V3Stats::addStat(prefix + "input direct subgraph instances before",
                         m_inputDirectSubgraphInstancesBefore);
        V3Stats::addStat(prefix + "input nodes after", m_inputNodesAfter);
        V3Stats::addStat(prefix + "input nodes before", m_inputNodesBefore);
        V3Stats::addStat(prefix + "input hidden body nodes after", inputHiddenBodyNodesAfter);
        V3Stats::addStat(prefix + "input parent visible nodes after",
                         m_inputParentVisibleNodesAfter);
        V3Stats::addStat(prefix + "input parent visible nodes before",
                         m_inputParentVisibleNodesBefore);
        V3Stats::addStat(prefix + "input parent visible nodes reduced",
                         inputParentVisibleNodesReduced);
        V3Stats::addStat(
            prefix + "input parent visible reduction permille",
            ratioPermille(inputParentVisibleNodesReduced, m_inputParentVisibleNodesBefore));
        V3Stats::addStat(prefix + "input snapshot instance body nodes after",
                         m_inputSnapshotInstanceBodyNodesAfter);
        V3Stats::addStat(prefix + "input snapshot instance body nodes before",
                         m_inputSnapshotInstanceBodyNodesBefore);
        V3Stats::addStat(prefix + "input snapshot procedure body nodes after",
                         m_inputSnapshotProcedureBodyNodesAfter);
        V3Stats::addStat(prefix + "input snapshot procedure body nodes before",
                         m_inputSnapshotProcedureBodyNodesBefore);
        V3Stats::addStat(prefix + "input subgraph instance body nodes after",
                         m_inputSubgraphInstanceBodyNodesAfter);
        V3Stats::addStat(prefix + "input subgraph instance body nodes before",
                         m_inputSubgraphInstanceBodyNodesBefore);
        V3Stats::addStat(prefix + "input subgraph instances after", m_inputSubgraphInstancesAfter);
        V3Stats::addStat(prefix + "input subgraph instances before",
                         m_inputSubgraphInstancesBefore);
        uint64_t internalOrderAggregateCalls = 0;
        uint64_t internalOrderAggregateConstants = 0;
        uint64_t internalOrderAggregateNodes = 0;
        uint64_t internalOrderAggregateRefs = 0;
        for (const auto& pair : m_internalOrderAggregates) {
            const SubgraphInternalOrderAggregate& aggregate = pair.second;
            internalOrderAggregateCalls += aggregate.m_calls;
            internalOrderAggregateConstants += aggregate.m_constants;
            internalOrderAggregateNodes += aggregate.m_nodes;
            internalOrderAggregateRefs += aggregate.m_refs;
        }
        V3Stats::addStat(prefix + "internal order aggregate calls", internalOrderAggregateCalls);
        V3Stats::addStat(prefix + "internal order aggregate constants",
                         internalOrderAggregateConstants);
        V3Stats::addStat(prefix + "internal order aggregate groups",
                         m_internalOrderAggregates.size());
        V3Stats::addStat(prefix + "internal order aggregate nodes", internalOrderAggregateNodes);
        V3Stats::addStat(prefix + "internal order aggregate refs", internalOrderAggregateRefs);
        V3Stats::addStat(prefix + "order cache clone apply fail arguments",
                         m_orderCacheCloneApplyFailArguments);
        V3Stats::addStat(prefix + "order cache clone apply fail constants",
                         m_orderCacheCloneApplyFailConstants);
        V3Stats::addStat(prefix + "order cache clone fail other", m_orderCacheCloneFailOther);
        V3Stats::addStat(prefix + "order cache clone fail state", m_orderCacheCloneFailState);
        V3Stats::addStat(prefix + "order cache clone fail shadow", m_orderCacheCloneFailShadow);
        V3Stats::addStat(prefix + "order cache clone fail temp", m_orderCacheCloneFailTemp);
        V3Stats::addStat(prefix + "order cache clone fail vlem", m_orderCacheCloneFailVlem);
        V3Stats::addStat(prefix + "order cache clone null", m_orderCacheCloneNull);
        for (const auto& itr : m_orderCacheCloneFailNames) {
            V3Stats::addStat(prefix + "order cache clone fail name " + itr.first, itr.second);
        }
        V3Stats::addStat(prefix + "order cache clone generated var remap delayed",
                         m_orderCacheCloneGeneratedVarRemapDelayed);
        V3Stats::addStat(prefix + "order cache clone generated var remap temp",
                         m_orderCacheCloneGeneratedVarRemapTemp);
        V3Stats::addStat(prefix + "order cache clone generated var remap trigger",
                         m_orderCacheCloneGeneratedVarRemapTrigger);
        V3Stats::addStat(prefix + "order cache clone generated var remap vlem",
                         m_orderCacheCloneGeneratedVarRemapVlem);
        V3Stats::addStat(prefix + "order cache clone generated var remaps",
                         m_orderCacheCloneGeneratedVarRemaps);
        V3Stats::addStat(prefix + "order cache direct index fallbacks",
                         m_orderCacheDirectIndexFallbacks);
        V3Stats::addStat(prefix + "order cache direct index hits", m_orderCacheDirectIndexHits);
        V3Stats::addStat(prefix + "order cache direct index lookups",
                         m_orderCacheDirectIndexLookups);
        V3Stats::addStat(prefix + "order cache entries", m_orderCacheEntries);
        V3Stats::addStat(prefix + "order cache entry hits", m_orderCacheEntryHits);
        V3Stats::addStat(prefix + "order cache hits", m_orderCacheHits);
        V3Stats::addStat(prefix + "order cache lookups", m_orderCacheLookups);
        V3Stats::addStat(prefix + "order cache hit permille",
                         ratioPermille(m_orderCacheHits, orderCacheLookups));
        V3Stats::addStat(prefix + "order cache misses", m_orderCacheMisses);
        V3Stats::addStat(prefix + "order cache miss no entry constant value",
                         m_orderCacheMissNoEntryConstValue);
        V3Stats::addStat(prefix + "order cache miss no entry node topology",
                         m_orderCacheMissNoEntryNodeTopology);
        V3Stats::addStat(prefix + "order cache miss no entry ref access",
                         m_orderCacheMissNoEntryRefAccess);
        V3Stats::addStat(prefix + "order cache recipe clones", m_orderCacheRecipeClones);
        V3Stats::addStat(prefix + "order cache recipe constant remaps",
                         m_orderCacheRecipeConstantRemaps);
        V3Stats::addStat(prefix + "order cache recipe hits", m_orderCacheRecipeHits);
        V3Stats::addStat(prefix + "order cache recipe replays", m_orderCacheRecipeReplays);
        V3Stats::addStat(prefix + "order cache recipe shared hits", m_orderCacheRecipeSharedHits);
        V3Stats::addStat(prefix + "order cache shared hits", m_orderCacheSharedHits);
        V3Stats::addStat(prefix + "order cache shared skip arguments",
                         m_orderCacheSharedSkipArguments);
        V3Stats::addStat(prefix + "order cache shared skip call function",
                         m_orderCacheSharedSkipCallFunction);
        V3Stats::addStat(prefix + "order cache shared skip clone fail",
                         m_orderCacheSharedSkipCloneFail);
        V3Stats::addStat(prefix + "order cache shared skip constants",
                         m_orderCacheSharedSkipConstants);
        V3Stats::addStat(prefix + "order cache shared skip module mismatch",
                         m_orderCacheSharedSkipModuleMismatch);
        V3Stats::addStat(prefix + "order cache shared skip non loose",
                         m_orderCacheSharedSkipNonLoose);
        V3Stats::addStat(prefix + "order cache shared skip other", m_orderCacheSharedSkipOther);
        V3Stats::addStat(prefix + "order cache shared skip phase", m_orderCacheSharedSkipPhase);
        V3Stats::addStat(prefix + "order cache shared skip triggered",
                         m_orderCacheSharedSkipTriggered);
        V3Stats::addStat(prefix + "order cache shared skip triggered input tail",
                         m_orderCacheSharedSkipTriggeredInputTail);
        V3Stats::addStat(prefix + "order cache shared skip triggered not shareable",
                         m_orderCacheSharedSkipTriggeredNotShareable);
        V3Stats::addStat(prefix + "order cache shared skip triggered other",
                         m_orderCacheSharedSkipTriggeredOther);
        V3Stats::addStat(prefix + "order cache shared skip var map", m_orderCacheSharedSkipVarMap);
        V3Stats::addStat(prefix + "order cache skip triggered", m_orderCacheSkipTriggered);
        V3Stats::addStat(prefix + "order cache skip triggered instance local",
                         m_orderCacheSkipTriggeredInstanceLocal);
        V3Stats::addStat(prefix + "order cache skip triggered no artifact",
                         m_orderCacheSkipTriggeredNoArtifact);
        V3Stats::addStat(prefix + "order cache skip triggered not shareable",
                         m_orderCacheSkipTriggeredNotShareable);
        V3Stats::addStat(prefix + "order cache skip triggered stl", m_orderCacheSkipTriggeredStl);
        addTemplateMapFailStats(
            prefix, "order cache", m_orderCacheTemplateMapFailConstValue,
            m_orderCacheTemplateMapFailNodeCount, m_orderCacheTemplateMapFailNodeTopology,
            m_orderCacheTemplateMapFailNodeType, m_orderCacheTemplateMapFailRefAccess,
            m_orderCacheTemplateMapFailRefConflict, m_orderCacheTemplateMapFailRefCount,
            m_orderCacheTemplateMapFailRefDType, m_orderCacheTemplateMapFails);
        V3Stats::addStat(prefix + "order cache variant buckets", m_orderCacheVariantBuckets);
        V3Stats::addStat(prefix + "order cache variant candidates", m_orderCacheVariantCandidates);
        V3Stats::addStat(prefix + "order cache variant max", m_orderCacheVariantMax);
        V3Stats::addStat(prefix + "ordered function clones", m_orderedFuncClones);
        V3Stats::addStat(prefix + "parent consumed contract writes",
                         m_parentConsumedContractWrites);
        V3Stats::addStat(prefix + "parent consumed subgraph vars", m_parentConsumedSubgraphVars);
        V3Stats::addStat(prefix + "schedule plans", m_schedulePlans);
        V3Stats::addStat(prefix + "shared helper call args", m_sharedHelperCallArgs);
        V3Stats::addStat(prefix + "shared helper call args max", m_sharedHelperCallArgsMax);
        V3Stats::addStat(prefix + "shared helper constant args", m_sharedHelperConstantArgs);
        V3Stats::addStat(prefix + "shared helper contract argument vars",
                         m_sharedHelperContractArgumentVars);
        V3Stats::addStat(prefix + "shared helper contract helper local vars",
                         m_sharedHelperContractHelperLocalVars);
        V3Stats::addStat(prefix + "shared helper contract identity vars",
                         m_sharedHelperContractIdentityVars);
        V3Stats::addStat(prefix + "shared helper contract implicit context vars",
                         m_sharedHelperContractImplicitContextVars);
        V3Stats::addStat(prefix + "shared helper external args", m_sharedHelperExternalArgs);
        V3Stats::addStat(prefix + "shared helper formal args after",
                         m_sharedHelperFormalArgsAfter);
        V3Stats::addStat(prefix + "shared helper formal args before",
                         m_sharedHelperFormalArgsBefore);
        V3Stats::addStat(prefix + "shared helper formal args max", m_sharedHelperFormalArgsMax);
        V3Stats::addStat(prefix + "shared helper hidden use cache hits",
                         m_sharedHelperHiddenUseCacheHits);
        V3Stats::addStat(prefix + "shared helper hidden use calls", m_sharedHelperHiddenUseCalls);
        V3Stats::addStat(prefix + "shared helper hidden use scans", m_sharedHelperHiddenUseScans);
        V3Stats::addStat(prefix + "shared helper hidden uses", m_sharedHelperHiddenUses);
        V3Stats::addStat(prefix + "shared helper implicit context vars",
                         m_sharedHelperImplicitContextVars);
        V3Stats::addStat(prefix + "shared helper instance local args",
                         m_sharedHelperInstanceLocalArgs);
        V3Stats::addStat(prefix + "shared helper parameterization fails",
                         m_sharedHelperParameterizationFails);
        V3Stats::addStat(prefix + "shared helper parameterizations",
                         m_sharedHelperParameterizations);
        V3Stats::addStat(prefix + "shared helper parameterized funcs",
                         m_sharedHelperParameterizedFuncs);
        V3Stats::addStat(prefix + "shared helper remap variant builds",
                         m_sharedHelperRemapVariantBuilds);
        V3Stats::addStat(prefix + "shared helper remap variant candidate vars",
                         m_sharedHelperRemapVariantCandidateVars);
        V3Stats::addStat(prefix + "shared helper remap variant candidate vars max",
                         m_sharedHelperRemapVariantCandidateVarsMax);
        V3Stats::addStat(prefix + "shared helper remap variant constant remaps",
                         m_sharedHelperRemapVariantConstantRemaps);
        V3Stats::addStat(prefix + "shared helper remap variant hits",
                         m_sharedHelperRemapVariantHits);
        V3Stats::addStat(prefix + "shared helper remap variant oversize skips",
                         m_sharedHelperRemapVariantOversizeSkips);
        V3Stats::addStat(prefix + "shared helper remap variant vars",
                         m_sharedHelperRemapVariantVars);
        V3Stats::addStat(prefix + "shared helper scope index builds",
                         m_sharedHelperScopeIndexBuilds);
        V3Stats::addStat(prefix + "shared helper scope index hits", m_sharedHelperScopeIndexHits);
        V3Stats::addStat(prefix + "shared helper stl argument skips",
                         m_sharedHelperStlArgumentSkips);
        V3Stats::addStat(prefix + "shared helper var index builds", m_sharedHelperVarIndexBuilds);
        V3Stats::addStat(prefix + "shared helper var index hits", m_sharedHelperVarIndexHits);
        V3Stats::addStat(prefix + "snapshot buckets", m_snapshotBuckets);
        V3Stats::addStat(prefix + "snapshot bundle elements", m_snapshotBundleElems);
        V3Stats::addStat(prefix + "snapshot bundles", m_snapshotBundles);
        V3Stats::addStat(prefix + "snapshot helpers", m_snapshotHelpers);
        V3Stats::addStat(prefix + "snapshot helper reuses", m_snapshotHelperReuses);
        V3Stats::addStat(prefix + "snapshot procedures", m_snapshotProcedures);
        V3Stats::addStat(prefix + "snapshot scalars", m_snapshotScalars);
        V3Stats::addStat(prefix + "snapshot source set duplicates", m_snapshotSourceSetDuplicates);
        V3Stats::addStat(prefix + "snapshot source sets", m_snapshotSourceSets);
        V3Stats::addStat(prefix + "snapshot sources", m_snapshotSources);
        V3Stats::addStat(prefix + "tail clone reuses", m_tailCloneReuses);
        V3Stats::addStat(prefix + "tail clones", m_tailClones);
        V3Stats::addStat(prefix + "tail wrappers", m_tailWrappers);
        V3Stats::addStat(prefix + "time build contract sec", seconds(m_timeBuildContractUsecs), 6);
        V3Stats::addStat(prefix + "time build logic shape sec",
                         seconds(m_timeBuildLogicShapeUsecs), 6);
        V3Stats::addStat(prefix + "time build logic signature sec",
                         seconds(m_timeBuildLogicSigUsecs), 6);
        V3Stats::addStat(prefix + "time build plans sec", seconds(m_timeBuildPlansUsecs), 6);
        V3Stats::addStat(prefix + "time clone ordered funcs sec",
                         seconds(m_timeCloneOrderedFuncsUsecs), 6);
        V3Stats::addStat(prefix + "time collect groups sec", seconds(m_timeCollectGroupsUsecs), 6);
        V3Stats::addStat(prefix + "time collect input stats sec",
                         seconds(m_timeCollectInputStatsUsecs), 6);
        V3Stats::addStat(prefix + "time collect region written vars sec",
                         seconds(m_timeCollectRegionWrittenVarsUsecs), 6);
        V3Stats::addStat(prefix + "time compute domain shape sec",
                         seconds(m_timeComputeDomainShapeUsecs), 6);
        V3Stats::addStat(prefix + "time discard logic sec", seconds(m_timeDiscardLogicUsecs), 6);
        V3Stats::addStat(prefix + "time emit snapshots sec", seconds(m_timeEmitSnapshotsUsecs), 6);
        V3Stats::addStat(prefix + "time internal order sec", seconds(m_timeInternalOrderUsecs), 6);
        V3Stats::addStat(prefix + "time lookup artifacts sec", seconds(m_timeLookupArtifactsUsecs),
                         6);
        V3Stats::addStat(prefix + "time lower groups sec", seconds(m_timeLowerGroupsUsecs), 6);
        V3Stats::addStat(prefix + "time make artifacts sec", seconds(m_timeMakeArtifactsUsecs), 6);
        V3Stats::addStat(prefix + "time mark hidden uses sec", seconds(m_timeMarkHiddenUsesUsecs),
                         6);
        V3Stats::addStat(prefix + "time materialize sec", seconds(m_timeMaterializeUsecs), 6);
        V3Stats::addStat(prefix + "time parameterize helpers sec",
                         seconds(m_timeParameterizeHelpersUsecs), 6);
        V3Stats::addStat(prefix + "time parameterize remap variants sec",
                         seconds(m_timeParameterizeRemapVariantsUsecs), 6);
        V3Stats::addStat(prefix + "time prepare snapshots sec",
                         seconds(m_timePrepareSnapshotsUsecs), 6);
        V3Stats::addStat(prefix + "time populate helper args sec",
                         seconds(m_timePopulateHelperArgsUsecs), 6);
        V3Stats::addStat(prefix + "time recipe replay sec", seconds(m_timeRecipeReplayUsecs), 6);
        V3Stats::addStat(prefix + "time split ordered funcs sec",
                         seconds(m_timeSplitOrderedFuncsUsecs), 6);
        V3Stats::addStat(prefix + "time template map sec", seconds(m_timeTemplateMapUsecs), 6);
        V3Stats::addStat(prefix + "time total sec", seconds(m_timeTotalUsecs), 6);
        V3Stats::addStat(prefix + "time triggered analysis sec",
                         seconds(m_timeTriggeredAnalysisUsecs), 6);
        std::vector<std::pair<std::string, SubgraphInternalOrderAggregate>> orderedAggregates{
            m_internalOrderAggregates.begin(), m_internalOrderAggregates.end()};
        std::sort(orderedAggregates.begin(), orderedAggregates.end(),
                  [](const auto& lhs, const auto& rhs) {
                      if (lhs.second.m_usecs != rhs.second.m_usecs) {
                          return lhs.second.m_usecs > rhs.second.m_usecs;
                      }
                      return lhs.first < rhs.first;
                  });
        const size_t aggregateTopCount = std::min<size_t>(orderedAggregates.size(), 16);
        for (size_t i = 0; i < aggregateTopCount; ++i) {
            const std::string topPrefix = prefix + "internal order aggregate top " + cvtToStr(i)
                                          + " " + orderedAggregates[i].first + " ";
            const SubgraphInternalOrderAggregate& aggregate = orderedAggregates[i].second;
            V3Stats::addStat(topPrefix + "calls", aggregate.m_calls);
            V3Stats::addStat(topPrefix + "constants", aggregate.m_constants);
            V3Stats::addStat(topPrefix + "max nodes", aggregate.m_maxNodes);
            V3Stats::addStat(topPrefix + "nodes", aggregate.m_nodes);
            V3Stats::addStat(topPrefix + "refs", aggregate.m_refs);
            V3Stats::addStat(topPrefix + "sec", seconds(aggregate.m_usecs), 6);
        }
        std::vector<std::pair<std::string, uint64_t>> orderedTimes = m_internalOrderTimes;
        std::sort(orderedTimes.begin(), orderedTimes.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.second > rhs.second; });
        const size_t topCount = std::min<size_t>(orderedTimes.size(), 16);
        for (size_t i = 0; i < topCount; ++i) {
            V3Stats::addStat(prefix + "time internal order top " + cvtToStr(i) + " "
                                 + orderedTimes[i].first + " sec",
                             seconds(orderedTimes[i].second), 6);
        }
        V3Stats::addStat(prefix + "triggered artifact candidates", m_triggeredArtifactCandidates);
        V3Stats::addStat(prefix + "triggered artifact input tail shareable",
                         m_triggeredArtifactInputTailShareable);
        V3Stats::addStat(prefix + "triggered artifact input tail writes",
                         m_triggeredArtifactInputTailWrites);
        V3Stats::addStat(prefix + "triggered artifact no nonlocal instance local writes",
                         m_triggeredArtifactNoNonLocalInstanceLocalWrites);
        V3Stats::addStat(prefix + "triggered artifact no nonlocal writes",
                         m_triggeredArtifactNoNonLocalWrites);
        V3Stats::addStat(prefix + "triggered artifact shareable", m_triggeredArtifactShareable);
        V3Stats::addStat(prefix + "triggered artifact unshareable",
                         m_triggeredArtifactUnshareable);
        V3Stats::addStat(prefix + "triggered artifact writes delayed shadow",
                         m_triggeredArtifactWritesDelayedShadow);
        V3Stats::addStat(prefix + "triggered artifact writes instance local",
                         m_triggeredArtifactWritesInstanceLocal);
        V3Stats::addStat(prefix + "triggered artifact writes local temp",
                         m_triggeredArtifactWritesLocalTemp);
        V3Stats::addStat(prefix + "triggered artifact writes nonlocal",
                         m_triggeredArtifactWritesNonLocal);
        V3Stats::addStat(prefix + "triggered artifact writes trigger temp",
                         m_triggeredArtifactWritesTriggerTemp);
        V3Stats::addStat(prefix + "triggered artifact writes vlem temp",
                         m_triggeredArtifactWritesVlemTemp);
        V3Stats::addStat(prefix + "triggered ref curr", m_triggeredRefCurr);
        V3Stats::addStat(prefix + "triggered ref other", m_triggeredRefOther);
        V3Stats::addStat(prefix + "triggered ref prev", m_triggeredRefPrev);
        V3Stats::addStat(prefix + "triggered ref sched", m_triggeredRefSched);
        V3Stats::addStat(prefix + "triggered ref state", m_triggeredRefState);
        V3Stats::addStat(prefix + "triggered ref state acc", m_triggeredRefStateAcc);
        V3Stats::addStat(prefix + "instances", m_instances);
        V3Stats::addStat(prefix + "logic shape builds", m_logicShapeBuilds);
        V3Stats::addStat(prefix + "logic signature builds", m_logicSigBuilds);
        V3Stats::addStat(prefix + "logic signature builds avoided", m_logicSigBuildsAvoided);
    }

    void noteOrderCacheCloneFailName(const string& name) {
        if (m_orderCacheCloneFailNames.size() >= 16 && !m_orderCacheCloneFailNames.count(name))
            return;
        ++m_orderCacheCloneFailNames[name];
    }

    void noteArtifactTemplateMapFail(SubgraphTemplateMapFailReason reason) {
        ++m_artifactReuseTemplateMapFails;
        switch (reason) {
        case SubgraphTemplateMapFailReason::CONST_VALUE:
            ++m_artifactReuseTemplateMapFailConstValue;
            return;
        case SubgraphTemplateMapFailReason::NODE_COUNT:
            ++m_artifactReuseTemplateMapFailNodeCount;
            return;
        case SubgraphTemplateMapFailReason::NODE_TOPOLOGY:
            ++m_artifactReuseTemplateMapFailNodeTopology;
            return;
        case SubgraphTemplateMapFailReason::NODE_TYPE:
            ++m_artifactReuseTemplateMapFailNodeType;
            return;
        case SubgraphTemplateMapFailReason::REF_ACCESS:
            ++m_artifactReuseTemplateMapFailRefAccess;
            return;
        case SubgraphTemplateMapFailReason::REF_CONFLICT:
            ++m_artifactReuseTemplateMapFailRefConflict;
            return;
        case SubgraphTemplateMapFailReason::REF_COUNT:
            ++m_artifactReuseTemplateMapFailRefCount;
            return;
        case SubgraphTemplateMapFailReason::REF_DTYPE:
            ++m_artifactReuseTemplateMapFailRefDType;
            return;
        case SubgraphTemplateMapFailReason::NONE: return;
        }
    }

    void noteInternalOrder(const std::string& name, uint64_t usecs, AstNodeModule* modp,
                           AstSubgraphInstance::Phase phase, const SubgraphWrapper& wrapper,
                           const SubgraphOrderCacheKey& cacheKey,
                           const SubgraphLogicSig& logicSig) {
        if (!v3Global.opt.stats()) return;
        uint64_t constants = 0;
        uint64_t nodes = 0;
        uint64_t refs = 0;
        for (const SubgraphLogicNodeSig& node : logicSig) {
            constants += node.m_constValues.size();
            nodes += node.m_nodeTypes.size() + 1;
            refs += node.m_refs.size();
        }
        size_t domainHash = 0;
        for (const uintptr_t value : cacheKey.m_domainShape) {
            domainHash ^= std::hash<uintptr_t>{}(value) + 0x9e3779b97f4a7c15ULL + (domainHash << 6)
                          + (domainHash >> 2);
        }
        const std::string aggregateKey
            = modp->name() + " p" + cvtToStr(static_cast<unsigned>(phase)) + " w"
              + cvtToStr(static_cast<unsigned>(wrapper.m_kind)) + " k"
              + cvtToStr(static_cast<unsigned>(wrapper.m_keyword)) + " d" + cvtToStr(domainHash)
              + " n" + cvtToStr(cacheKey.m_logicShape.m_nodeTypes) + " r"
              + cvtToStr(cacheKey.m_logicShape.m_refAccesses) + " size" + cvtToStr(nodes);
        SubgraphInternalOrderAggregate& aggregate = m_internalOrderAggregates[aggregateKey];
        ++aggregate.m_calls;
        aggregate.m_constants += constants;
        aggregate.m_maxNodes = std::max(aggregate.m_maxNodes, nodes);
        aggregate.m_nodes += nodes;
        aggregate.m_refs += refs;
        aggregate.m_usecs += usecs;
        if (usecs) m_internalOrderTimes.emplace_back(name, usecs);
    }

    void noteOrderCacheTemplateMapFail(SubgraphTemplateMapFailReason reason) {
        ++m_orderCacheTemplateMapFails;
        switch (reason) {
        case SubgraphTemplateMapFailReason::CONST_VALUE:
            ++m_orderCacheTemplateMapFailConstValue;
            return;
        case SubgraphTemplateMapFailReason::NODE_COUNT:
            ++m_orderCacheTemplateMapFailNodeCount;
            return;
        case SubgraphTemplateMapFailReason::NODE_TOPOLOGY:
            ++m_orderCacheTemplateMapFailNodeTopology;
            return;
        case SubgraphTemplateMapFailReason::NODE_TYPE:
            ++m_orderCacheTemplateMapFailNodeType;
            return;
        case SubgraphTemplateMapFailReason::REF_ACCESS:
            ++m_orderCacheTemplateMapFailRefAccess;
            return;
        case SubgraphTemplateMapFailReason::REF_CONFLICT:
            ++m_orderCacheTemplateMapFailRefConflict;
            return;
        case SubgraphTemplateMapFailReason::REF_COUNT:
            ++m_orderCacheTemplateMapFailRefCount;
            return;
        case SubgraphTemplateMapFailReason::REF_DTYPE:
            ++m_orderCacheTemplateMapFailRefDType;
            return;
        case SubgraphTemplateMapFailReason::NONE: return;
        }
    }
};

class SubgraphLoweringState final {
    static constexpr size_t MAX_SHARED_HELPER_REMAP_VARS = 64;

public:
    explicit SubgraphLoweringState(const string& tag)
        : m_snapshotCrossBoundaryReads{tag == "nba"}
        , m_stlSubgraphFuncs{subgraphRegistry().m_stlSubgraphFuncs} {
        if (tag == "stl") m_stlSubgraphFuncs.clear();
    }

    static AstScope* boundaryScopeFor(AstScope* scopep) {
        for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
            if (scanp->modp()->subgraphBoundary()) return scanp;
        }
        return nullptr;
    }

    static bool isUnderBoundaryScope(AstScope* scopep, AstScope* boundaryScopep) {
        for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
            if (scanp == boundaryScopep) return true;
        }
        return false;
    }

    void collectParentConsumedSubgraphVars(const std::vector<LogicByScope*>& logic) {
        for (const LogicByScope* const lbsp : logic) {
            for (const auto& pair : *lbsp) {
                AstScope* const logicBoundaryp = boundaryScopeFor(pair.first);
                pair.second->foreach([&](AstNodeVarRef* refp) {
                    if (!refp->access().isReadOrRW()) return;
                    AstVarScope* const vscp = refp->varScopep();
                    AstScope* const varBoundaryp = boundaryScopeFor(vscp->scopep());
                    if (!varBoundaryp || varBoundaryp == logicBoundaryp) return;
                    m_parentConsumedSubgraphVars.insert(vscp);
                });
            }
        }
        m_stats.m_parentConsumedSubgraphVars = m_parentConsumedSubgraphVars.size();
    }

    void discardLogic(LogicByScope& logic) {
        const uint64_t startUsecs = statStartUsecs();
        for (const auto& pair : logic) {
            AstActive* const activep = pair.second;
            if (activep->backp()) activep->unlinkFrBack();
            activep->deleteTree();
        }
        logic.clear();
        addElapsedUsecs(m_stats.m_timeDiscardLogicUsecs, startUsecs);
    }

    static SubgraphDomainShapes
    computeDomainShapes(const LogicByScope& logic, AstScope* boundaryScopep,
                        const V3Order::ExternalDomainsProvider& externalDomains) {
        SubgraphDomainShapes result;
        std::unordered_map<const AstSenTree*, uintptr_t> canonicalDomains;
        logic.foreachLogic([&](AstNode* logicp) {
            result.m_canonical.push_back(static_cast<uintptr_t>(logicp->type()));
            result.m_exact.push_back(static_cast<uintptr_t>(logicp->type()));
            logicp->foreach([&](AstVarRef* refp) {
                result.m_canonical.push_back(static_cast<uintptr_t>(refp->access()));
                result.m_exact.push_back(static_cast<uintptr_t>(refp->access()));
                const AstVarScope* const vscp = refp->varScopep();
                const uintptr_t underBoundary
                    = isUnderBoundaryScope(vscp->scopep(), boundaryScopep);
                result.m_canonical.push_back(underBoundary);
                result.m_exact.push_back(underBoundary);
                std::vector<AstSenTree*> domains;
                externalDomains(vscp, domains);
                result.m_canonical.push_back(domains.size());
                result.m_exact.push_back(domains.size());
                for (AstSenTree* const domainp : domains) {
                    const auto emplaced
                        = canonicalDomains.emplace(domainp, canonicalDomains.size() + 1);
                    result.m_canonical.push_back(emplaced.first->second);
                    result.m_exact.push_back(reinterpret_cast<uintptr_t>(domainp));
                }
            });
        });
        return result;
    }

    static SubgraphLogicSig buildLogicSig(const LogicByScope& logic) {
        SubgraphLogicSig result;
        logic.foreachLogic([&](AstNode* logicp) {
            const SubgraphParameterizableConstVisitor parameterizableConsts{logicp};
            result.push_back(SubgraphLogicNodeSig{});
            SubgraphLogicNodeSig& nodeSig = result.back();
            nodeSig.m_type = static_cast<uintptr_t>(logicp->type());
            logicp->foreach([&](AstNode* nodep) {
                nodeSig.m_nodeTypes.push_back(static_cast<uintptr_t>(nodep->type()));
                if (AstConst* const constp = VN_CAST(nodep, Const)) {
                    nodeSig.m_consts.push_back(constp);
                    nodeSig.m_constParameterizable.push_back(
                        parameterizableConsts.contains(constp));
                    nodeSig.m_constValues.push_back(constp->num().toString());
                }
            });
            logicp->foreach([&](AstVarRef* refp) {
                nodeSig.m_refs.push_back(
                    {static_cast<uintptr_t>(refp->access()), refp->varScopep()});
            });
        });
        return result;
    }

    static SubgraphLogicShape buildLogicShape(const SubgraphLogicSig& logicSig) {
        SubgraphLogicShape result;
        for (const SubgraphLogicNodeSig& node : logicSig) {
            result.m_nodeTypes ^= std::hash<uintptr_t>{}(node.m_type) + 0x9e3779b97f4a7c15ULL
                                  + (result.m_nodeTypes << 6) + (result.m_nodeTypes >> 2);
            for (const uintptr_t type : node.m_nodeTypes) {
                result.m_nodeTypes ^= std::hash<uintptr_t>{}(type) + 0x9e3779b97f4a7c15ULL
                                      + (result.m_nodeTypes << 6) + (result.m_nodeTypes >> 2);
            }
            for (const string& value : node.m_constValues) {
                result.m_constValues ^= std::hash<string>{}(value) + 0x9e3779b97f4a7c15ULL
                                        + (result.m_constValues << 6)
                                        + (result.m_constValues >> 2);
            }
            for (const SubgraphLogicRefSig& ref : node.m_refs) {
                result.m_refAccesses ^= std::hash<uintptr_t>{}(ref.m_access)
                                        + 0x9e3779b97f4a7c15ULL + (result.m_refAccesses << 6)
                                        + (result.m_refAccesses >> 2);
            }
        }
        return result;
    }

    static SubgraphLogicShape buildLogicShape(const LogicByScope& logic) {
        SubgraphLogicShape result;
        logic.foreachLogic([&](AstNode* logicp) {
            result.m_nodeTypes ^= std::hash<uintptr_t>{}(static_cast<uintptr_t>(logicp->type()))
                                  + 0x9e3779b97f4a7c15ULL + (result.m_nodeTypes << 6)
                                  + (result.m_nodeTypes >> 2);
            logicp->foreach([&](AstNode* nodep) {
                result.m_nodeTypes ^= std::hash<uintptr_t>{}(static_cast<uintptr_t>(nodep->type()))
                                      + 0x9e3779b97f4a7c15ULL + (result.m_nodeTypes << 6)
                                      + (result.m_nodeTypes >> 2);
                if (AstConst* const constp = VN_CAST(nodep, Const)) {
                    result.m_constValues ^= std::hash<string>{}(constp->num().toString())
                                            + 0x9e3779b97f4a7c15ULL + (result.m_constValues << 6)
                                            + (result.m_constValues >> 2);
                }
            });
            logicp->foreach([&](AstVarRef* refp) {
                result.m_refAccesses
                    ^= std::hash<uintptr_t>{}(static_cast<uintptr_t>(refp->access()))
                       + 0x9e3779b97f4a7c15ULL + (result.m_refAccesses << 6)
                       + (result.m_refAccesses >> 2);
            });
        });
        return result;
    }

    static SubgraphTemplateMapFailReason
    buildTemplateVarScopeMap(const SubgraphLogicSig& templateSig, const LogicByScope& currentLogic,
                             std::unordered_map<const AstVarScope*, AstVarScope*>& result,
                             std::unordered_map<const AstConst*, const AstConst*>* constRemapp
                             = nullptr,
                             bool* constantsDifferp = nullptr) {
        std::unordered_map<const AstVarScope*, const AstVarScope*> reverseVarMap;
        std::vector<AstNode*> currentNodes;
        currentLogic.foreachLogic([&](AstNode* logicp) { currentNodes.push_back(logicp); });
        if (templateSig.size() != currentNodes.size()) {
            return SubgraphTemplateMapFailReason::NODE_COUNT;
        }

        for (size_t i = 0; i < templateSig.size(); ++i) {
            const SubgraphLogicNodeSig& templateNode = templateSig[i];
            AstNode* const currentNodep = currentNodes[i];
            if (templateNode.m_type != static_cast<uintptr_t>(currentNodep->type())) {
                return SubgraphTemplateMapFailReason::NODE_TYPE;
            }
            SubgraphLogicNodeSig currentNodeSig;
            currentNodeSig.m_type = static_cast<uintptr_t>(currentNodep->type());
            currentNodep->foreach([&](AstNode* nodep) {
                currentNodeSig.m_nodeTypes.push_back(static_cast<uintptr_t>(nodep->type()));
                if (AstConst* const constp = VN_CAST(nodep, Const)) {
                    currentNodeSig.m_consts.push_back(constp);
                    currentNodeSig.m_constParameterizable.push_back(false);
                    currentNodeSig.m_constValues.push_back(constp->num().toString());
                }
            });
            if (templateNode.m_nodeTypes != currentNodeSig.m_nodeTypes) {
                return SubgraphTemplateMapFailReason::NODE_TOPOLOGY;
            }
            if (!constRemapp && templateNode.m_constValues != currentNodeSig.m_constValues) {
                return SubgraphTemplateMapFailReason::CONST_VALUE;
            }
            if (constRemapp) {
                UASSERT_OBJ(templateNode.m_consts.size() == currentNodeSig.m_consts.size(),
                            currentNodep, "Mismatched constant signature sizes");
                for (size_t j = 0; j < templateNode.m_consts.size(); ++j) {
                    if (templateNode.m_consts[j]) {
                        constRemapp->emplace(templateNode.m_consts[j], currentNodeSig.m_consts[j]);
                    }
                }
                if (constantsDifferp
                    && templateNode.m_constValues != currentNodeSig.m_constValues) {
                    *constantsDifferp = true;
                }
            }

            std::vector<AstVarRef*> currentRefs;
            currentNodep->foreach([&](AstVarRef* refp) { currentRefs.push_back(refp); });
            if (templateNode.m_refs.size() != currentRefs.size()) {
                return SubgraphTemplateMapFailReason::REF_COUNT;
            }

            for (size_t j = 0; j < templateNode.m_refs.size(); ++j) {
                const SubgraphLogicRefSig& templateRef = templateNode.m_refs[j];
                AstVarRef* const currentRefp = currentRefs[j];
                if (templateRef.m_access != static_cast<uintptr_t>(currentRefp->access())) {
                    return SubgraphTemplateMapFailReason::REF_ACCESS;
                }
                AstVarScope* const currentVscp = currentRefp->varScopep();
                if (!templateRef.m_vscp->dtypep()->skipRefp()->sameTree(
                        currentVscp->dtypep()->skipRefp())) {
                    return SubgraphTemplateMapFailReason::REF_DTYPE;
                }
                const auto it = result.find(templateRef.m_vscp);
                if (it != result.end()) {
                    if (it->second != currentVscp) {
                        return SubgraphTemplateMapFailReason::REF_CONFLICT;
                    }
                } else {
                    const auto reverseIt = reverseVarMap.find(currentVscp);
                    if (reverseIt != reverseVarMap.end()
                        && reverseIt->second != templateRef.m_vscp) {
                        return SubgraphTemplateMapFailReason::REF_CONFLICT;
                    }
                    result.emplace(templateRef.m_vscp, currentVscp);
                    reverseVarMap.emplace(currentVscp, templateRef.m_vscp);
                }
            }
        }
        return SubgraphTemplateMapFailReason::NONE;
    }

    static bool canShareSubgraphLogic(const LogicByScope& logic, AstScope*) {
        bool hasUnresolvedRef = false;
        logic.foreachLogic([&](AstNode* logicp) {
            logicp->foreach([&](AstVarRef* refp) {
                if (!refp->varScopep()) hasUnresolvedRef = true;
            });
        });
        return !hasUnresolvedRef;
    }

    static bool nameEndsWith(const string& name, const string& suffix) {
        return name.size() >= suffix.size()
               && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    static string instanceLocalLeafName(const string& name) {
        const string::size_type pos = name.rfind("__DOT__");
        return pos == string::npos ? name : name.substr(pos + 7);
    }

    static SubgraphInstanceLocalVarKind instanceLocalVarKind(const string& name) {
        const string leafName = instanceLocalLeafName(name);
        if (leafName.rfind("__Vdly", 0) == 0) {
            return SubgraphInstanceLocalVarKind::DELAYED_SHADOW;
        }
        if (leafName.rfind("__Vtrigcurr", 0) == 0) {
            return SubgraphInstanceLocalVarKind::TRIGGER_CURR;
        }
        if (leafName.rfind("__Vtrigprev", 0) == 0) {
            return SubgraphInstanceLocalVarKind::TRIGGER_PREV;
        }
        if (leafName.rfind("__VtrigSched", 0) == 0) {
            return SubgraphInstanceLocalVarKind::TRIGGER_SCHED;
        }
        if (leafName.rfind("__V", 0) == 0 && nameEndsWith(leafName, "Triggered")) {
            return SubgraphInstanceLocalVarKind::TRIGGER_STATE;
        }
        if (leafName.rfind("__V", 0) == 0 && nameEndsWith(leafName, "TriggeredAcc")) {
            return SubgraphInstanceLocalVarKind::TRIGGER_STATE_ACC;
        }
        if (leafName.rfind("__Vcell", 0) == 0 || leafName.rfind("__Vfunc", 0) == 0
            || leafName.rfind("__Vtemp", 0) == 0) {
            return SubgraphInstanceLocalVarKind::LOCAL_TEMP;
        }
        if (leafName.rfind("__Vlem", 0) == 0) { return SubgraphInstanceLocalVarKind::VLEM_TEMP; }
        return SubgraphInstanceLocalVarKind::NONE;
    }

    static SubgraphInstanceLocalVarKind instanceLocalVarKind(const AstVarScope* vscp) {
        return instanceLocalVarKind(vscp->varp()->name());
    }

    static bool isRemappableInstanceLocalVar(const AstVarScope* vscp) {
        switch (instanceLocalVarKind(vscp)) {
        case SubgraphInstanceLocalVarKind::DELAYED_SHADOW:
        case SubgraphInstanceLocalVarKind::LOCAL_TEMP:
        case SubgraphInstanceLocalVarKind::TRIGGER_CURR:
        case SubgraphInstanceLocalVarKind::TRIGGER_PREV:
        case SubgraphInstanceLocalVarKind::TRIGGER_SCHED:
        case SubgraphInstanceLocalVarKind::VLEM_TEMP: return true;
        case SubgraphInstanceLocalVarKind::NONE:
        case SubgraphInstanceLocalVarKind::TRIGGER_STATE:
        case SubgraphInstanceLocalVarKind::TRIGGER_STATE_ACC: return false;
        }
        return false;
    }

    static bool isTriggeredStateVar(const AstVarScope* vscp) {
        const SubgraphInstanceLocalVarKind kind = instanceLocalVarKind(vscp);
        return kind == SubgraphInstanceLocalVarKind::TRIGGER_STATE
               || kind == SubgraphInstanceLocalVarKind::TRIGGER_STATE_ACC;
    }

    static void noteTriggeredRefKind(const AstVarScope* vscp, SubgraphLoweringStats& stats) {
        switch (instanceLocalVarKind(vscp)) {
        case SubgraphInstanceLocalVarKind::TRIGGER_CURR: ++stats.m_triggeredRefCurr; return;
        case SubgraphInstanceLocalVarKind::TRIGGER_PREV: ++stats.m_triggeredRefPrev; return;
        case SubgraphInstanceLocalVarKind::TRIGGER_SCHED: ++stats.m_triggeredRefSched; return;
        case SubgraphInstanceLocalVarKind::TRIGGER_STATE: ++stats.m_triggeredRefState; return;
        case SubgraphInstanceLocalVarKind::TRIGGER_STATE_ACC:
            ++stats.m_triggeredRefStateAcc;
            return;
        case SubgraphInstanceLocalVarKind::DELAYED_SHADOW:
        case SubgraphInstanceLocalVarKind::LOCAL_TEMP:
        case SubgraphInstanceLocalVarKind::NONE:
        case SubgraphInstanceLocalVarKind::VLEM_TEMP:
            if (0 == vscp->varp()->name().rfind("__Vtrig", 0)) ++stats.m_triggeredRefOther;
            return;
        }
    }

    static void noteTriggeredLocalWriteKind(SubgraphTriggeredRefInfo& info,
                                            SubgraphInstanceLocalVarKind kind) {
        info.m_writesInstanceLocal = true;
        switch (kind) {
        case SubgraphInstanceLocalVarKind::DELAYED_SHADOW:
            info.m_writesDelayedShadow = true;
            return;
        case SubgraphInstanceLocalVarKind::LOCAL_TEMP: info.m_writesLocalTemp = true; return;
        case SubgraphInstanceLocalVarKind::TRIGGER_CURR:
        case SubgraphInstanceLocalVarKind::TRIGGER_PREV:
        case SubgraphInstanceLocalVarKind::TRIGGER_SCHED: info.m_writesTriggerTemp = true; return;
        case SubgraphInstanceLocalVarKind::VLEM_TEMP: info.m_writesVlemTemp = true; return;
        case SubgraphInstanceLocalVarKind::NONE:
        case SubgraphInstanceLocalVarKind::TRIGGER_STATE:
        case SubgraphInstanceLocalVarKind::TRIGGER_STATE_ACC: return;
        }
    }

    static SubgraphTriggeredRefInfo analyzeOrderedFuncTriggeredRefs(AstCFunc* funcp,
                                                                    AstScope* boundaryScopep,
                                                                    SubgraphLoweringStats& stats,
                                                                    bool noteStats = true) {
        const uint64_t startUsecs = statStartUsecs();
        std::unordered_set<AstCFunc*> seenFuncs;
        SubgraphTriggeredRefInfo result;
        std::function<void(AstCFunc*)> gather = [&](AstCFunc* scanFuncp) {
            if (!seenFuncs.insert(scanFuncp).second) return;
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const vscp = refp->varScopep();
                if (noteStats) noteTriggeredRefKind(vscp, stats);
                if (refp->access().isWriteOrRW()) {
                    const SubgraphInstanceLocalVarKind kind = instanceLocalVarKind(vscp);
                    if (isRemappableInstanceLocalVar(vscp)) {
                        noteTriggeredLocalWriteKind(result, kind);
                    } else {
                        result.m_writesNonLocal = true;
                    }
                }
                if (!isTriggeredStateVar(vscp)) return;
                result.m_hasTriggered = true;
                if (instanceLocalVarKind(vscp)
                    == SubgraphInstanceLocalVarKind::TRIGGER_STATE_ACC) {
                    result.m_shareable = false;
                }
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint()) return;
                gather(calledFuncp);
            });
        };
        gather(funcp);
        addElapsedUsecs(stats.m_timeTriggeredAnalysisUsecs, startUsecs);
        return result;
    }

    static bool canShareTriggeredArtifact(const SubgraphTriggeredRefInfo& info) {
        if (!info.m_hasTriggered || !info.m_shareable) return false;
        if (info.m_writesNonLocal) return true;
        return !info.m_writesInstanceLocal || info.writesOnlySharedHelperSafeInstanceLocal();
    }

    static bool canCloneTriggeredOrderCacheEntry(const SubgraphTriggeredRefInfo& info) {
        return !info.m_hasTriggered || info.m_shareable;
    }

    static VAccess sharedHelperArgAccess(const SubgraphSharedHelperArg& arg) {
        if (arg.m_reads && arg.m_writes) return VAccess::READWRITE;
        return arg.m_writes ? VAccess::WRITE : VAccess::READ;
    }

    static AstVarScope* newSharedHelperArg(AstCFunc* funcp, const SubgraphSharedHelperArg& arg,
                                           size_t index) {
        FileLine* const flp = funcp->fileline();
        AstScope* const scopep = funcp->scopep();
        AstVar* const varp = new AstVar{flp, VVarType::BLOCKTEMP,
                                        "__VsubgraphArg" + cvtToStr(index), arg.m_vscp->dtypep()};
        varp->direction(arg.m_writes ? (arg.m_reads ? VDirection::INOUT : VDirection::OUTPUT)
                                     : VDirection::CONSTREF);
        varp->funcLocal(true);
        funcp->addArgsp(varp);
        AstVarScope* const vscp = new AstVarScope{flp, scopep, varp};
        scopep->addVarsp(vscp);
        return vscp;
    }

    AstScope* findSharedHelperTargetScope(AstScope* sourceBoundaryScopep,
                                          AstScope* targetBoundaryScopep, AstScope* sourceScopep) {
        if (sourceScopep == sourceBoundaryScopep) return targetBoundaryScopep;
        const string& sourceBoundaryName = sourceBoundaryScopep->name();
        const string& sourceName = sourceScopep->name();
        if (sourceName.size() <= sourceBoundaryName.size()
            || sourceName.compare(0, sourceBoundaryName.size(), sourceBoundaryName) != 0) {
            return nullptr;
        }
        const string targetName
            = targetBoundaryScopep->name() + sourceName.substr(sourceBoundaryName.size());
        AstNodeModule* const modp = sourceScopep->modp();
        auto inserted
            = m_sharedHelperScopesByModule.emplace(modp, std::unordered_map<string, AstScope*>{});
        std::unordered_map<string, AstScope*>& scopes = inserted.first->second;
        if (inserted.second) {
            ++m_stats.m_sharedHelperScopeIndexBuilds;
            modp->foreach([&](AstScope* scopep) { scopes.emplace(scopep->name(), scopep); });
        } else {
            ++m_stats.m_sharedHelperScopeIndexHits;
        }
        const auto it = scopes.find(targetName);
        return it == scopes.end() ? nullptr : it->second;
    }

    AstVarScope* findSharedHelperTargetVar(AstScope* targetScopep, const AstVarScope* sourceVscp) {
        auto inserted = m_sharedHelperVarsByScope.emplace(
            targetScopep, std::unordered_map<string, std::vector<AstVarScope*>>{});
        std::unordered_map<string, std::vector<AstVarScope*>>& vars = inserted.first->second;
        if (inserted.second) {
            ++m_stats.m_sharedHelperVarIndexBuilds;
            for (AstVarScope* vscp = targetScopep->varsp(); vscp;
                 vscp = VN_AS(vscp->nextp(), VarScope)) {
                if (vscp->scopep() == targetScopep) vars[vscp->varp()->name()].push_back(vscp);
            }
        } else {
            ++m_stats.m_sharedHelperVarIndexHits;
        }
        const auto it = vars.find(sourceVscp->varp()->name());
        if (it == vars.end()) return nullptr;
        for (AstVarScope* const vscp : it->second) {
            if (vscp->dtypep()->similarDType(sourceVscp->dtypep())) { return vscp; }
        }
        return nullptr;
    }

    void markSharedHelperHiddenUses(AstCFunc* funcp, AstScope* sourceBoundaryScopep,
                                    AstScope* targetBoundaryScopep) {
        const uint64_t startUsecs = statStartUsecs();
        ++m_stats.m_sharedHelperHiddenUseCalls;
        const SubgraphSharedHelperHiddenUseKey key{funcp, sourceBoundaryScopep,
                                                   targetBoundaryScopep};
        if (!m_sharedHelperHiddenUseCache.emplace(key).second) {
            ++m_stats.m_sharedHelperHiddenUseCacheHits;
            addElapsedUsecs(m_stats.m_timeMarkHiddenUsesUsecs, startUsecs);
            return;
        }
        ++m_stats.m_sharedHelperHiddenUseScans;
        std::unordered_set<AstCFunc*> seenFuncs;
        std::unordered_set<AstVarScope*> hiddenVscps;
        std::function<void(AstCFunc*)> gather = [&](AstCFunc* scanFuncp) {
            if (!seenFuncs.insert(scanFuncp).second) return;
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const sourceVscp = refp->varScopep();
                if (sourceVscp->varp()->isFuncLocal()
                    || !isUnderBoundaryScope(sourceVscp->scopep(), sourceBoundaryScopep)) {
                    return;
                }
                AstScope* const targetScopep = findSharedHelperTargetScope(
                    sourceBoundaryScopep, targetBoundaryScopep, sourceVscp->scopep());
                if (!targetScopep) return;
                AstVarScope* const targetVscp
                    = findSharedHelperTargetVar(targetScopep, sourceVscp);
                if (!targetVscp || !hiddenVscps.insert(targetVscp).second) return;
                targetVscp->optimizeLifePost(false);
                targetVscp->subgraphSharedUse(true);
                ++m_stats.m_sharedHelperHiddenUses;
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint() || calledFuncp->dpiImportPrototype()
                    || calledFuncp->scopep() != funcp->scopep()) {
                    return;
                }
                gather(calledFuncp);
            });
        };
        gather(funcp);
        addElapsedUsecs(m_stats.m_timeMarkHiddenUsesUsecs, startUsecs);
    }

    static bool parameterizeSharedHelper(AstCFunc* funcp, AstScope* boundaryScopep,
                                         SubgraphLogicSig& logicSig,
                                         bool parameterizeInstanceLocal,
                                         std::vector<SubgraphSharedHelperArg>& helperArgs,
                                         SubgraphLoweringStats& stats) {
        std::unordered_set<const AstVarScope*> eligibleVscps;
        for (const SubgraphLogicNodeSig& node : logicSig) {
            for (const SubgraphLogicRefSig& ref : node.m_refs) {
                const AstVarScope* const vscp = ref.m_vscp;
                const bool remappableInstanceLocal
                    = parameterizeInstanceLocal && isRemappableInstanceLocalVar(vscp)
                      && (!vscp->varp()->isFuncLocal()
                          || instanceLocalVarKind(vscp)
                                 == SubgraphInstanceLocalVarKind::DELAYED_SHADOW);
                if (!vscp->varp()->isFuncLocal() || remappableInstanceLocal) {
                    eligibleVscps.insert(vscp);
                }
            }
        }

        std::vector<AstCFunc*> funcs;
        std::unordered_set<AstCFunc*> seenFuncs;
        std::function<void(AstCFunc*)> gatherFuncs = [&](AstCFunc* scanFuncp) {
            if (!seenFuncs.insert(scanFuncp).second) return;
            funcs.push_back(scanFuncp);
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint() || calledFuncp->dpiImportPrototype()
                    || calledFuncp->scopep() != funcp->scopep()) {
                    return;
                }
                gatherFuncs(calledFuncp);
            });
        };
        gatherFuncs(funcp);
        for (AstCFunc* const scanFuncp : funcs) scanFuncp->noLife(true);

        std::unordered_set<const AstConst*> orderedConstps;
        for (AstCFunc* const scanFuncp : funcs) {
            scanFuncp->foreach([&](AstConst* constp) { orderedConstps.insert(constp); });
        }
        size_t parameterizableConstCount = 0;
        for (size_t nodeIndex = 0; nodeIndex < logicSig.size(); ++nodeIndex) {
            SubgraphLogicNodeSig& node = logicSig[nodeIndex];
            for (size_t constIndex = 0; constIndex < node.m_consts.size(); ++constIndex) {
                AstConst* const constp = const_cast<AstConst*>(node.m_consts[constIndex]);
                if (!node.m_constParameterizable[constIndex] || !orderedConstps.count(constp)) {
                    node.m_constParameterizable[constIndex] = false;
                    continue;
                }
                ++parameterizableConstCount;
            }
        }

        std::unordered_map<AstVarScope*, size_t> argIndex;
        std::unordered_set<AstVarScope*> baselineArgs;
        std::unordered_set<AstVarScope*> implicitContextVscps;
        std::unordered_set<AstVarScope*> instanceLocalArgs;
        for (AstCFunc* const scanFuncp : funcs) {
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const vscp = refp->varScopep();
                const bool remappable
                    = isRemappableInstanceLocalVar(vscp)
                      && (!vscp->varp()->isFuncLocal()
                          || instanceLocalVarKind(vscp)
                                 == SubgraphInstanceLocalVarKind::DELAYED_SHADOW);
                if (isUnderBoundaryScope(vscp->scopep(), boundaryScopep)
                    && !eligibleVscps.count(vscp) && (!parameterizeInstanceLocal || !remappable)) {
                    return;
                }
                if (!eligibleVscps.count(vscp)) {
                    if (!remappable) return;
                    eligibleVscps.insert(vscp);
                }
                baselineArgs.insert(vscp);
                if (vscp->scopep() == boundaryScopep) {
                    implicitContextVscps.insert(vscp);
                    refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
                    return;
                }
                if (isUnderBoundaryScope(vscp->scopep(), boundaryScopep)) {
                    instanceLocalArgs.insert(vscp);
                }
                const auto inserted = argIndex.emplace(vscp, helperArgs.size());
                if (inserted.second) helperArgs.push_back(SubgraphSharedHelperArg{vscp});
                SubgraphSharedHelperArg& arg = helperArgs[inserted.first->second];
                arg.m_reads |= refp->access().isReadOrRW();
                arg.m_writes |= refp->access().isWriteOrRW();
            });
        }
        stats.m_sharedHelperFormalArgsBefore
            += funcs.size() * (baselineArgs.size() + parameterizableConstCount);
        stats.m_sharedHelperImplicitContextVars += implicitContextVscps.size();
        if (helperArgs.empty()) {
            stats.m_sharedHelperParameterizedFuncs += funcs.size();
            ++stats.m_sharedHelperParameterizations;
            return true;
        }

        std::unordered_map<AstCFunc*, std::vector<AstCFunc*>> calleesByFunc;
        std::unordered_map<AstCFunc*, std::unordered_set<size_t>> requiredArgsByFunc;
        for (AstCFunc* const scanFuncp : funcs) {
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                const auto it = argIndex.find(refp->varScopep());
                if (it != argIndex.end()) requiredArgsByFunc[scanFuncp].insert(it->second);
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                if (seenFuncs.count(callp->funcp())) {
                    calleesByFunc[scanFuncp].push_back(callp->funcp());
                }
            });
        }

        // Propagate callee requirements to callers without making every split helper carry the
        // complete artifact contract.
        bool changed = false;
        do {
            changed = false;
            for (auto funcIt = funcs.rbegin(); funcIt != funcs.rend(); ++funcIt) {
                AstCFunc* const scanFuncp = *funcIt;
                std::unordered_set<size_t>& requiredArgs = requiredArgsByFunc[scanFuncp];
                for (AstCFunc* const calledFuncp : calleesByFunc[scanFuncp]) {
                    for (const size_t index : requiredArgsByFunc[calledFuncp]) {
                        changed |= requiredArgs.insert(index).second;
                    }
                }
            }
        } while (changed);

        std::unordered_map<AstCFunc*, std::vector<AstVarScope*>> argsByFunc;
        for (AstCFunc* const scanFuncp : funcs) {
            std::vector<AstVarScope*>& args = argsByFunc[scanFuncp];
            args.resize(helperArgs.size());
            uint64_t formalArgs = 0;
            for (size_t i = 0; i < helperArgs.size(); ++i) {
                if (!requiredArgsByFunc[scanFuncp].count(i)) continue;
                args[i] = newSharedHelperArg(scanFuncp, helperArgs[i], i);
                ++formalArgs;
                ++stats.m_sharedHelperFormalArgsAfter;
            }
            stats.m_sharedHelperFormalArgsMax
                = std::max(stats.m_sharedHelperFormalArgsMax, formalArgs);
        }
        for (AstCFunc* const scanFuncp : funcs) {
            const std::vector<AstVarScope*>& args = argsByFunc.at(scanFuncp);
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                const auto it = argIndex.find(refp->varScopep());
                if (it == argIndex.end()) return;
                AstVarScope* const argVscp = args[it->second];
                UASSERT_OBJ(argVscp, refp, "Missing shared helper function argument");
                refp->varp(argVscp->varp());
                refp->varScopep(argVscp);
                refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (!seenFuncs.count(calledFuncp)) return;
                uint64_t callArgs = 0;
                for (size_t i = 0; i < helperArgs.size(); ++i) {
                    if (!requiredArgsByFunc[calledFuncp].count(i)) continue;
                    UASSERT_OBJ(args[i], callp, "Missing caller shared helper argument");
                    callp->addArgsp(new AstVarRef{callp->fileline(), args[i],
                                                  sharedHelperArgAccess(helperArgs[i])});
                    ++callArgs;
                }
                stats.m_sharedHelperCallArgs += callArgs;
                stats.m_sharedHelperCallArgsMax
                    = std::max(stats.m_sharedHelperCallArgsMax, callArgs);
            });
        }
        stats.m_sharedHelperExternalArgs += helperArgs.size() - instanceLocalArgs.size();
        stats.m_sharedHelperInstanceLocalArgs += instanceLocalArgs.size();
        stats.m_sharedHelperParameterizedFuncs += funcs.size();
        ++stats.m_sharedHelperParameterizations;
        return true;
    }

    static bool sharedHelperNeedsArguments(AstCFunc* funcp, AstScope* boundaryScopep,
                                           const SubgraphLogicSig& logicSig,
                                           bool parameterizeInstanceLocal) {
        std::unordered_set<const AstVarScope*> eligibleVscps;
        for (const SubgraphLogicNodeSig& node : logicSig) {
            for (const SubgraphLogicRefSig& ref : node.m_refs) {
                const AstVarScope* const vscp = ref.m_vscp;
                const bool remappableInstanceLocal
                    = parameterizeInstanceLocal && isRemappableInstanceLocalVar(vscp)
                      && (!vscp->varp()->isFuncLocal()
                          || instanceLocalVarKind(vscp)
                                 == SubgraphInstanceLocalVarKind::DELAYED_SHADOW);
                if (!vscp->varp()->isFuncLocal() || remappableInstanceLocal) {
                    eligibleVscps.insert(vscp);
                }
            }
        }

        std::vector<AstCFunc*> funcs;
        std::unordered_set<AstCFunc*> seenFuncs;
        std::function<void(AstCFunc*)> gatherFuncs = [&](AstCFunc* scanFuncp) {
            if (!seenFuncs.insert(scanFuncp).second) return;
            funcs.push_back(scanFuncp);
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint() || calledFuncp->dpiImportPrototype()
                    || calledFuncp->scopep() != funcp->scopep()) {
                    return;
                }
                gatherFuncs(calledFuncp);
            });
        };
        gatherFuncs(funcp);

        bool needsArguments = false;
        for (AstCFunc* const scanFuncp : funcs) {
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                if (needsArguments) return;
                AstVarScope* const vscp = refp->varScopep();
                const bool remappable
                    = isRemappableInstanceLocalVar(vscp)
                      && (!vscp->varp()->isFuncLocal()
                          || instanceLocalVarKind(vscp)
                                 == SubgraphInstanceLocalVarKind::DELAYED_SHADOW);
                if (isUnderBoundaryScope(vscp->scopep(), boundaryScopep)
                    && !eligibleVscps.count(vscp) && (!parameterizeInstanceLocal || !remappable)) {
                    return;
                }
                if (!eligibleVscps.count(vscp) && !remappable) return;
                needsArguments = vscp->scopep() != boundaryScopep;
            });
            if (needsArguments) return true;
        }
        return false;
    }

    static bool parameterizeSharedHelperImplicitVars(
        AstCFunc* funcp, const std::vector<const AstVarScope*>& implicitVscps,
        std::vector<SubgraphSharedHelperArg>& helperArgs, SubgraphLoweringStats& stats) {
        std::unordered_map<const AstVarScope*, size_t> argIndex;
        for (const AstVarScope* const vscp : implicitVscps) {
            argIndex.emplace(vscp, helperArgs.size());
            helperArgs.push_back(
                SubgraphSharedHelperArg{const_cast<AstVarScope*>(vscp), false, false});
        }

        std::vector<AstCFunc*> funcs;
        std::unordered_set<AstCFunc*> seenFuncs;
        std::function<void(AstCFunc*)> gatherFuncs = [&](AstCFunc* scanFuncp) {
            if (!seenFuncs.insert(scanFuncp).second) return;
            funcs.push_back(scanFuncp);
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint() || calledFuncp->dpiImportPrototype()
                    || calledFuncp->scopep() != funcp->scopep()) {
                    return;
                }
                gatherFuncs(calledFuncp);
            });
        };
        gatherFuncs(funcp);

        for (AstCFunc* const scanFuncp : funcs) {
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                const auto it = argIndex.find(refp->varScopep());
                if (it == argIndex.end()) return;
                SubgraphSharedHelperArg& arg = helperArgs[it->second];
                arg.m_reads |= refp->access().isReadOrRW();
                arg.m_writes |= refp->access().isWriteOrRW();
            });
        }

        std::unordered_map<AstCFunc*, std::vector<AstCFunc*>> calleesByFunc;
        std::unordered_map<AstCFunc*, std::unordered_set<size_t>> requiredArgsByFunc;
        for (AstCFunc* const scanFuncp : funcs) {
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                const auto it = argIndex.find(refp->varScopep());
                if (it != argIndex.end()) requiredArgsByFunc[scanFuncp].insert(it->second);
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                if (seenFuncs.count(callp->funcp())) {
                    calleesByFunc[scanFuncp].push_back(callp->funcp());
                }
            });
        }

        bool changed = false;
        do {
            changed = false;
            for (auto funcIt = funcs.rbegin(); funcIt != funcs.rend(); ++funcIt) {
                AstCFunc* const scanFuncp = *funcIt;
                std::unordered_set<size_t>& requiredArgs = requiredArgsByFunc[scanFuncp];
                for (AstCFunc* const calledFuncp : calleesByFunc[scanFuncp]) {
                    for (const size_t index : requiredArgsByFunc[calledFuncp]) {
                        changed |= requiredArgs.insert(index).second;
                    }
                }
            }
        } while (changed);

        std::unordered_map<AstCFunc*, std::vector<AstVarScope*>> argsByFunc;
        for (AstCFunc* const scanFuncp : funcs) {
            std::vector<AstVarScope*>& args = argsByFunc[scanFuncp];
            args.resize(helperArgs.size());
            for (size_t i = 0; i < helperArgs.size(); ++i) {
                if (!requiredArgsByFunc[scanFuncp].count(i)) continue;
                args[i] = newSharedHelperArg(scanFuncp, helperArgs[i], i);
                ++stats.m_sharedHelperFormalArgsAfter;
            }
        }
        for (AstCFunc* const scanFuncp : funcs) {
            const std::vector<AstVarScope*>& args = argsByFunc.at(scanFuncp);
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                const auto it = argIndex.find(refp->varScopep());
                if (it == argIndex.end()) return;
                AstVarScope* const argVscp = args[it->second];
                UASSERT_OBJ(argVscp, refp, "Missing remapped shared helper function argument");
                refp->varp(argVscp->varp());
                refp->varScopep(argVscp);
                refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (!seenFuncs.count(calledFuncp)) return;
                for (size_t i = 0; i < helperArgs.size(); ++i) {
                    if (!requiredArgsByFunc[calledFuncp].count(i)) continue;
                    UASSERT_OBJ(args[i], callp, "Missing remapped caller shared helper argument");
                    callp->addArgsp(new AstVarRef{callp->fileline(), args[i],
                                                  sharedHelperArgAccess(helperArgs[i])});
                }
            });
        }
        stats.m_sharedHelperParameterizedFuncs += funcs.size();
        ++stats.m_sharedHelperParameterizations;
        return true;
    }

    bool populateSharedHelperArgs(
        SubgraphScheduleInstance& instance, const SubgraphScheduleArtifact& artifact,
        const std::vector<SubgraphSharedHelperArg>& helperArgs, AstScope* currentScopep,
        const std::unordered_map<const AstVarScope*, AstVarScope*>& templateVarMap,
        SubgraphLoweringStats& stats) {
        const uint64_t startUsecs = statStartUsecs();
        instance.m_helperArgs.clear();
        instance.m_helperArgs.reserve(helperArgs.size());
        if (artifact.m_scopep != currentScopep) {
            for (AstVarScope* vscp = currentScopep->varsp(); vscp;
                 vscp = VN_AS(vscp->nextp(), VarScope)) {
                if (instanceLocalVarKind(vscp) == SubgraphInstanceLocalVarKind::DELAYED_SHADOW) {
                    vscp->optimizeLifePost(false);
                }
            }
        }
        for (const SubgraphSharedHelperArg& arg : helperArgs) {
            AstVarScope* currentVscp = arg.m_vscp;
            const auto directIt = templateVarMap.find(arg.m_vscp);
            if (directIt != templateVarMap.end()) {
                currentVscp = directIt->second;
            } else if (isRemappableInstanceLocalVar(arg.m_vscp)) {
                string targetLeafName = instanceLocalLeafName(arg.m_vscp->varp()->name());
                for (const auto& pair : templateVarMap) {
                    if (instanceLocalLeafName(pair.first->varp()->name()) != targetLeafName) {
                        continue;
                    }
                    targetLeafName = instanceLocalLeafName(pair.second->varp()->name());
                    break;
                }
                if (artifact.m_scopep != currentScopep
                    || targetLeafName != instanceLocalLeafName(arg.m_vscp->varp()->name())) {
                    currentVscp = nullptr;
                    for (AstVarScope* scanp = currentScopep->varsp(); scanp;
                         scanp = VN_AS(scanp->nextp(), VarScope)) {
                        if (scanp->scopep() != currentScopep) continue;
                        if (instanceLocalLeafName(scanp->varp()->name()) != targetLeafName) {
                            continue;
                        }
                        if (instanceLocalVarKind(scanp) != instanceLocalVarKind(arg.m_vscp)) {
                            continue;
                        }
                        if (!scanp->dtypep()->similarDType(arg.m_vscp->dtypep())) continue;
                        currentVscp = scanp;
                        break;
                    }
                }
                if (!currentVscp) {
                    instance.m_helperArgs.clear();
                    addElapsedUsecs(stats.m_timePopulateHelperArgsUsecs, startUsecs);
                    return false;
                }
            } else if (artifact.m_scopep != currentScopep) {
                instance.m_helperArgs.clear();
                addElapsedUsecs(stats.m_timePopulateHelperArgsUsecs, startUsecs);
                return false;
            }
            if (arg.m_writes) currentVscp->optimizeLifePost(false);
            instance.m_helperArgs.push_back(
                SubgraphSharedHelperArg{currentVscp, arg.m_reads, arg.m_writes});
        }
        if (instance.m_sharedCall) {
            markSharedHelperHiddenUses(artifact.m_callFuncp, artifact.m_scopep, currentScopep);
        }
        addElapsedUsecs(stats.m_timePopulateHelperArgsUsecs, startUsecs);
        return true;
    }

    bool populateSharedHelperArgs(
        SubgraphScheduleInstance& instance, const SubgraphScheduleArtifact& artifact,
        AstScope* currentScopep,
        const std::unordered_map<const AstVarScope*, AstVarScope*>& templateVarMap,
        SubgraphLoweringStats& stats) {
        return populateSharedHelperArgs(instance, artifact, artifact.m_helperArgs, currentScopep,
                                        templateVarMap, stats);
    }

    static bool sharedHelperConstantsMatch(const SubgraphScheduleArtifact& artifact,
                                           const LogicByScope& currentLogic,
                                           bool requireExactMatch) {
        if (!requireExactMatch) return true;
        std::vector<AstNode*> currentNodes;
        currentLogic.foreachLogic([&](AstNode* logicp) { currentNodes.push_back(logicp); });
        if (artifact.m_logicSig.size() != currentNodes.size()) return false;

        for (size_t nodeIndex = 0; nodeIndex < currentNodes.size(); ++nodeIndex) {
            std::vector<AstConst*> consts;
            currentNodes[nodeIndex]->foreach([&](AstConst* constp) { consts.push_back(constp); });
            const SubgraphLogicNodeSig& templateNode = artifact.m_logicSig[nodeIndex];
            if (templateNode.m_constValues.size() != consts.size()) return false;
            for (size_t constIndex = 0; constIndex < consts.size(); ++constIndex) {
                if (templateNode.m_constValues[constIndex]
                    != consts[constIndex]->num().toString()) {
                    return false;
                }
            }
        }
        return true;
    }

    SubgraphSharedHelperApplyFailReason populateSharedHelperInstance(
        SubgraphScheduleInstance& instance, const SubgraphScheduleArtifact& artifact,
        AstScope* currentScopep,
        const std::unordered_map<const AstVarScope*, AstVarScope*>& templateVarMap,
        const LogicByScope& currentLogic, bool requireParameterizedDifferences,
        SubgraphLoweringStats& stats) {
        if (!instance.m_callFuncp) return SubgraphSharedHelperApplyFailReason::CALL_FUNCTION;
        if (!populateSharedHelperArgs(instance, artifact, currentScopep, templateVarMap, stats)) {
            return SubgraphSharedHelperApplyFailReason::ARGUMENTS;
        }
        if (!sharedHelperConstantsMatch(artifact, currentLogic, requireParameterizedDifferences)) {
            return SubgraphSharedHelperApplyFailReason::CONSTANTS;
        }
        return SubgraphSharedHelperApplyFailReason::NONE;
    }

    static bool sharedHelperCoversVarMap(
        const SubgraphScheduleArtifact& artifact,
        const std::unordered_map<const AstVarScope*, AstVarScope*>& templateVarMap) {
        for (const auto& pair : templateVarMap) {
            const auto contractIt = artifact.m_varMapContract.find(pair.first);
            if (contractIt == artifact.m_varMapContract.end()) return false;
            switch (contractIt->second) {
            case SubgraphSharedHelperVarRole::ARGUMENT: continue;
            case SubgraphSharedHelperVarRole::HELPER_LOCAL:
                if (!pair.second->varp()->isFuncLocal()) return false;
                if (!pair.second->dtypep()->similarDType(pair.first->dtypep())) return false;
                continue;
            case SubgraphSharedHelperVarRole::IDENTITY:
            case SubgraphSharedHelperVarRole::IMPLICIT_CONTEXT:
                if (pair.first == pair.second || pair.first->varp() == pair.second->varp()) {
                    continue;
                }
                return false;
            }
        }
        return true;
    }

    static SubgraphSharedHelperRemapVariant* findOrMakeSharedHelperRemapVariant(
        SubgraphScheduleArtifact& artifact,
        const std::unordered_map<const AstVarScope*, AstVarScope*>& templateVarMap,
        const std::unordered_map<const AstConst*, const AstConst*>& templateConstMap,
        SubgraphLoweringStats& stats) {
        std::vector<const AstVarScope*> implicitVscps;
        std::unordered_set<const AstVarScope*> seenImplicitVscps;
        for (const SubgraphLogicNodeSig& node : artifact.m_logicSig) {
            for (const SubgraphLogicRefSig& ref : node.m_refs) {
                const auto mapIt = templateVarMap.find(ref.m_vscp);
                if (mapIt == templateVarMap.end()) continue;
                const auto contractIt = artifact.m_varMapContract.find(ref.m_vscp);
                if (contractIt == artifact.m_varMapContract.end()) return nullptr;
                AstVarScope* const mappedVscp = mapIt->second;
                switch (contractIt->second) {
                case SubgraphSharedHelperVarRole::ARGUMENT: continue;
                case SubgraphSharedHelperVarRole::HELPER_LOCAL:
                    if (!mappedVscp->varp()->isFuncLocal()
                        || !mappedVscp->dtypep()->similarDType(ref.m_vscp->dtypep())) {
                        return nullptr;
                    }
                    continue;
                case SubgraphSharedHelperVarRole::IDENTITY:
                    if (ref.m_vscp != mappedVscp && ref.m_vscp->varp() != mappedVscp->varp()) {
                        return nullptr;
                    }
                    continue;
                case SubgraphSharedHelperVarRole::IMPLICIT_CONTEXT:
                    if (ref.m_vscp == mappedVscp || ref.m_vscp->varp() == mappedVscp->varp()) {
                        continue;
                    }
                    if (mappedVscp->varp()->isFuncLocal()
                        || !mappedVscp->dtypep()->similarDType(ref.m_vscp->dtypep())) {
                        return nullptr;
                    }
                    if (seenImplicitVscps.insert(ref.m_vscp).second) {
                        implicitVscps.push_back(ref.m_vscp);
                    }
                    continue;
                }
            }
        }
        if (implicitVscps.empty()) return nullptr;
        stats.m_sharedHelperRemapVariantCandidateVars += implicitVscps.size();
        stats.m_sharedHelperRemapVariantCandidateVarsMax = std::max<uint64_t>(
            stats.m_sharedHelperRemapVariantCandidateVarsMax, implicitVscps.size());
        if (implicitVscps.size() > MAX_SHARED_HELPER_REMAP_VARS) {
            ++stats.m_sharedHelperRemapVariantOversizeSkips;
            return nullptr;
        }

        std::vector<string> constValues;
        for (const SubgraphLogicNodeSig& node : artifact.m_logicSig) {
            for (const AstConst* const constp : node.m_consts) {
                const auto mapIt = templateConstMap.find(constp);
                const AstConst* const mappedConstp
                    = mapIt == templateConstMap.end() ? constp : mapIt->second;
                constValues.push_back(mappedConstp->num().toString());
            }
        }
        for (SubgraphSharedHelperRemapVariant& variant : artifact.m_remapVariants) {
            if (variant.m_implicitVscps != implicitVscps || variant.m_constValues != constValues) {
                continue;
            }
            ++stats.m_sharedHelperRemapVariantHits;
            return &variant;
        }

        AstCFunc* const variantFuncp = cloneOrderedFuncGraph(
            artifact.m_callFuncp, artifact.m_scopep, {}, templateConstMap, stats, nullptr, true);
        if (!variantFuncp) return nullptr;
        SubgraphSharedHelperRemapVariant variant;
        variant.m_callFuncp = variantFuncp;
        variant.m_constValues = std::move(constValues);
        variant.m_helperArgs = artifact.m_helperArgs;
        variant.m_implicitVscps = std::move(implicitVscps);
        const uint64_t parameterizeStartUsecs = statStartUsecs();
        const bool parameterized = parameterizeSharedHelperImplicitVars(
            variant.m_callFuncp, variant.m_implicitVscps, variant.m_helperArgs, stats);
        addElapsedUsecs(stats.m_timeParameterizeRemapVariantsUsecs, parameterizeStartUsecs);
        if (!parameterized) { return nullptr; }
        stats.m_sharedHelperRemapVariantVars += variant.m_implicitVscps.size();
        ++stats.m_sharedHelperRemapVariantBuilds;
        artifact.m_remapVariants.push_back(std::move(variant));
        return &artifact.m_remapVariants.back();
    }

    static std::unordered_map<const AstVarScope*, SubgraphSharedHelperVarRole>
    buildSharedHelperVarMapContract(const SubgraphLogicSig& logicSig,
                                    const std::vector<SubgraphSharedHelperArg>& helperArgs,
                                    AstScope* boundaryScopep, SubgraphLoweringStats& stats) {
        std::unordered_set<const AstVarScope*> helperArgVscps;
        helperArgVscps.reserve(helperArgs.size());
        for (const SubgraphSharedHelperArg& arg : helperArgs) {
            helperArgVscps.insert(arg.m_vscp);
        }

        std::unordered_map<const AstVarScope*, SubgraphSharedHelperVarRole> result;
        for (const SubgraphLogicNodeSig& node : logicSig) {
            for (const SubgraphLogicRefSig& ref : node.m_refs) {
                const AstVarScope* const vscp = ref.m_vscp;
                SubgraphSharedHelperVarRole role = SubgraphSharedHelperVarRole::IDENTITY;
                if (helperArgVscps.count(vscp)) {
                    role = SubgraphSharedHelperVarRole::ARGUMENT;
                } else if (vscp->varp()->isFuncLocal()) {
                    role = SubgraphSharedHelperVarRole::HELPER_LOCAL;
                } else if (isUnderBoundaryScope(vscp->scopep(), boundaryScopep)) {
                    role = SubgraphSharedHelperVarRole::IMPLICIT_CONTEXT;
                }
                switch (role) {
                case SubgraphSharedHelperVarRole::ARGUMENT:
                    ++stats.m_sharedHelperContractArgumentVars;
                    break;
                case SubgraphSharedHelperVarRole::HELPER_LOCAL:
                    ++stats.m_sharedHelperContractHelperLocalVars;
                    break;
                case SubgraphSharedHelperVarRole::IDENTITY:
                    ++stats.m_sharedHelperContractIdentityVars;
                    break;
                case SubgraphSharedHelperVarRole::IMPLICIT_CONTEXT:
                    ++stats.m_sharedHelperContractImplicitContextVars;
                    break;
                }
                result.emplace(vscp, role);
            }
        }
        return result;
    }

    static TailCloneSig buildTailCloneSig(AstCFunc* funcp) {
        TailCloneSig result;
        if (funcp->stmtsp()) result.m_bodyHash = V3Hasher::uncachedHash(funcp->stmtsp());
        funcp->foreach([&](AstCCall* callp) { result.m_calls.push_back(callp->funcp()); });
        funcp->foreach([&](AstVarRef* refp) {
            result.m_refs.push_back({static_cast<uintptr_t>(refp->access()), refp->varScopep()});
        });
        return result;
    }

    static bool canRemapGeneratedCloneVarByName(const AstVarScope* vscp) {
        return isRemappableInstanceLocalVar(vscp);
    }

    static AstVarScope* findGeneratedCloneVarByName(AstScope* destScopep,
                                                    const AstVarScope* sourceVscp) {
        if (!canRemapGeneratedCloneVarByName(sourceVscp)) return nullptr;
        return findScopeCloneVarByName(destScopep, sourceVscp);
    }

    static AstVarScope* findScopeCloneVarByName(AstScope* destScopep,
                                                const AstVarScope* sourceVscp) {
        const string& name = sourceVscp->varp()->name();
        AstNodeDType* const dtypep = sourceVscp->dtypep();
        for (AstVarScope* scanp = destScopep->varsp(); scanp;
             scanp = VN_AS(scanp->nextp(), VarScope)) {
            if (scanp->scopep() != destScopep) continue;
            if (scanp->varp()->name() != name) continue;
            if (!scanp->dtypep()->similarDType(dtypep)) continue;
            return scanp;
        }
        return nullptr;
    }

    static AstVarScope* findScopeCloneVarByName(AstScope* destScopep, const string& name,
                                                AstNodeDType* dtypep) {
        for (AstVarScope* scanp = destScopep->varsp(); scanp;
             scanp = VN_AS(scanp->nextp(), VarScope)) {
            if (scanp->scopep() != destScopep) continue;
            if (scanp->varp()->name() != name) continue;
            if (!scanp->dtypep()->similarDType(dtypep)) continue;
            return scanp;
        }
        return nullptr;
    }

    static AstVarScope* findScopeCloneVarByName(AstScope* destScopep, const string& name) {
        for (AstVarScope* scanp = destScopep->varsp(); scanp;
             scanp = VN_AS(scanp->nextp(), VarScope)) {
            if (scanp->scopep() != destScopep) continue;
            if (scanp->varp()->name() != name) continue;
            return scanp;
        }
        return nullptr;
    }

    static AstVarScope* findScopeCloneVarByVar(AstScope* destScopep, const AstVar* sourceVarp) {
        if (sourceVarp->isFuncLocal()) return nullptr;
        const string& name = sourceVarp->name();
        for (AstVarScope* scanp = destScopep->varsp(); scanp;
             scanp = VN_AS(scanp->nextp(), VarScope)) {
            if (scanp->scopep() != destScopep) continue;
            if (scanp->varp()->name() != name) continue;
            if (!scanp->dtypep()->similarDType(sourceVarp->dtypep())) continue;
            return scanp;
        }
        return nullptr;
    }

    static bool mapsToOtherSubgraphBoundary(AstVarScope* mappedVscp,
                                            AstScope* destBoundaryScopep) {
        AstScope* const mappedBoundaryScopep = boundaryScopeFor(mappedVscp->scopep());
        return mappedBoundaryScopep && mappedBoundaryScopep != destBoundaryScopep;
    }

    static bool canUseStructuralGeneratedVarMap(const AstVarScope* sourceVscp,
                                                AstVarScope* mappedVscp,
                                                AstScope* destBoundaryScopep) {
        if (!canRemapGeneratedCloneVarByName(sourceVscp)) return true;
        if (instanceLocalVarKind(sourceVscp) != instanceLocalVarKind(mappedVscp)) return false;
        if (!mappedVscp->dtypep()->similarDType(sourceVscp->dtypep())) return false;
        if (mapsToOtherSubgraphBoundary(mappedVscp, destBoundaryScopep)) return false;
        return isUnderBoundaryScope(mappedVscp->scopep(), destBoundaryScopep);
    }

    static bool mustBeDestScopedInClone(const AstVarScope* vscp) {
        return isRemappableInstanceLocalVar(vscp) || vscp->varp()->name().rfind("__PVT__", 0) == 0;
    }

    static AstVarScope* findDestScopedCloneVar(AstScope* destBoundaryScopep,
                                               const AstVarScope* sourceVscp) {
        AstVarScope* const mappedVscp = findScopeCloneVarByName(destBoundaryScopep, sourceVscp);
        if (mappedVscp == sourceVscp) return nullptr;
        return mappedVscp;
    }

    static AstVarScope* findDestScopedCloneVar(AstScope* destBoundaryScopep,
                                               const AstVarXRef* sourceRefp) {
        if (AstVarScope* const mappedVscp = findScopeCloneVarByName(
                destBoundaryScopep, sourceRefp->name(), sourceRefp->dtypep())) {
            return mappedVscp;
        }
        return findScopeCloneVarByName(destBoundaryScopep, sourceRefp->name());
    }

    static AstVarScope* findDestScopedCloneVar(AstScope* destBoundaryScopep,
                                               const AstNodeVarRef* sourceRefp) {
        if (AstVarScope* const mappedVscp = findScopeCloneVarByName(
                destBoundaryScopep, sourceRefp->name(), sourceRefp->dtypep())) {
            if (mappedVscp != sourceRefp->varScopep()) return mappedVscp;
        }
        AstVarScope* const mappedVscp
            = findScopeCloneVarByName(destBoundaryScopep, sourceRefp->name());
        if (mappedVscp == sourceRefp->varScopep()) return nullptr;
        return mappedVscp;
    }

    static void remapCloneRefTo(AstNodeVarRef* refp, AstVarScope* mappedVscp) {
        AstVarRef* const newp = new AstVarRef{refp->fileline(), mappedVscp, refp->access()};
        refp->replaceWith(newp);
        VL_DO_DANGLING(refp->deleteTree(), refp);
    }

    static bool hasUnsafeCloneSelfPointer(const AstNodeVarRef* refp) {
        if (refp->selfPointer().isEmpty()) return false;
        const string& name = refp->varp()->name();
        if (refp->access().isReadOnly() && name == "__VnbaTriggered") return false;
        return true;
    }

    static void noteGeneratedVarRemap(const AstVarScope* sourceVscp,
                                      SubgraphLoweringStats& stats) {
        switch (instanceLocalVarKind(sourceVscp)) {
        case SubgraphInstanceLocalVarKind::DELAYED_SHADOW:
            ++stats.m_orderCacheCloneGeneratedVarRemapDelayed;
            return;
        case SubgraphInstanceLocalVarKind::LOCAL_TEMP:
            ++stats.m_orderCacheCloneGeneratedVarRemapTemp;
            return;
        case SubgraphInstanceLocalVarKind::TRIGGER_CURR:
        case SubgraphInstanceLocalVarKind::TRIGGER_PREV:
        case SubgraphInstanceLocalVarKind::TRIGGER_SCHED:
            ++stats.m_orderCacheCloneGeneratedVarRemapTrigger;
            return;
        case SubgraphInstanceLocalVarKind::VLEM_TEMP:
            ++stats.m_orderCacheCloneGeneratedVarRemapVlem;
            return;
        case SubgraphInstanceLocalVarKind::NONE:
        case SubgraphInstanceLocalVarKind::TRIGGER_STATE:
        case SubgraphInstanceLocalVarKind::TRIGGER_STATE_ACC: return;
        }
    }

    static AstCFunc* cloneOrderedFuncGraphImpl(
        AstCFunc* funcp, AstScope* destBoundaryScopep,
        const std::unordered_map<const AstVarScope*, AstVarScope*>& templateVarMap,
        const std::unordered_map<const AstConst*, const AstConst*>& templateConstMap,
        SubgraphLoweringStats& stats, AstSenTree* triggerDomainp = nullptr,
        bool remapVariant = false) {
        static unsigned s_cloneIndex = 0;

        if (triggerDomainp) {
            AstIf* triggerIfp = nullptr;
            for (AstNode* stmtp = funcp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
                AstIf* const ifp = VN_CAST(stmtp, If);
                if (!ifp || triggerIfp) return nullptr;
                triggerIfp = ifp;
            }
            if (!triggerIfp) return nullptr;
        }

        std::vector<AstCFunc*> orderedFuncs;
        std::unordered_set<AstCFunc*> seenFuncs;
        std::function<void(AstCFunc*)> gatherFuncs = [&](AstCFunc* scanFuncp) {
            if (!seenFuncs.insert(scanFuncp).second) return;
            orderedFuncs.push_back(scanFuncp);
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint() || calledFuncp->dpiImportPrototype()
                    || calledFuncp->scopep() != funcp->scopep()) {
                    return;
                }
                gatherFuncs(calledFuncp);
            });
        };
        gatherFuncs(funcp);

        const auto noteUnmappedVar = [&](const AstVarScope* vscp) {
            const string& name = vscp->varp()->name();
            stats.noteOrderCacheCloneFailName(name);
            switch (instanceLocalVarKind(vscp)) {
            case SubgraphInstanceLocalVarKind::DELAYED_SHADOW:
            case SubgraphInstanceLocalVarKind::TRIGGER_CURR:
            case SubgraphInstanceLocalVarKind::TRIGGER_PREV:
            case SubgraphInstanceLocalVarKind::TRIGGER_SCHED:
            case SubgraphInstanceLocalVarKind::TRIGGER_STATE:
            case SubgraphInstanceLocalVarKind::TRIGGER_STATE_ACC:
                ++stats.m_orderCacheCloneFailShadow;
                return;
            case SubgraphInstanceLocalVarKind::LOCAL_TEMP:
                ++stats.m_orderCacheCloneFailTemp;
                return;
            case SubgraphInstanceLocalVarKind::VLEM_TEMP:
                ++stats.m_orderCacheCloneFailVlem;
                return;
            case SubgraphInstanceLocalVarKind::NONE: break;
            }
            if (name.rfind("__PVT__", 0) == 0) {
                ++stats.m_orderCacheCloneFailState;
            } else {
                ++stats.m_orderCacheCloneFailOther;
            }
        };

        std::unordered_map<const AstVarScope*, AstVarScope*> resolvedVarMap;
        for (const auto& pair : templateVarMap) {
            if (mapsToOtherSubgraphBoundary(pair.second, destBoundaryScopep)) continue;
            if (!canUseStructuralGeneratedVarMap(pair.first, pair.second, destBoundaryScopep)) {
                continue;
            }
            resolvedVarMap.emplace(pair);
            if (canRemapGeneratedCloneVarByName(pair.first) && pair.first != pair.second) {
                noteGeneratedVarRemap(pair.first, stats);
                ++stats.m_orderCacheCloneGeneratedVarRemaps;
            }
        }
        std::unordered_map<const AstCFunc*, std::unordered_set<const AstVar*>> argVarsByFunc;
        for (AstCFunc* const origFuncp : orderedFuncs) {
            std::unordered_set<const AstVar*>& argVars = argVarsByFunc[origFuncp];
            for (AstVar* argp = origFuncp->argsp(); argp; argp = VN_AS(argp->nextp(), Var)) {
                argVars.insert(argp);
            }
            bool failed = false;
            origFuncp->foreach([&](AstVarXRef* refp) {
                if (failed) return;
                noteUnmappedVar(refp->varScopep());
                failed = true;
            });
            origFuncp->foreach([&](AstNodeVarRef* refp) {
                if (failed) return;
                if (hasUnsafeCloneSelfPointer(refp)) {
                    noteUnmappedVar(refp->varScopep());
                    failed = true;
                    return;
                }
                if (argVars.count(refp->varp())) return;
                if (findScopeCloneVarByVar(destBoundaryScopep, refp->varp())) return;
                const AstVarScope* const sourceVscp = refp->varScopep();
                if (AstVarScope* const mappedVscp
                    = findDestScopedCloneVar(destBoundaryScopep, refp)) {
                    resolvedVarMap.emplace(sourceVscp, mappedVscp);
                    if (canRemapGeneratedCloneVarByName(sourceVscp)) {
                        noteGeneratedVarRemap(sourceVscp, stats);
                        ++stats.m_orderCacheCloneGeneratedVarRemaps;
                    }
                    return;
                }
                if (AstVarScope* const mappedVscp
                    = findDestScopedCloneVar(destBoundaryScopep, sourceVscp)) {
                    resolvedVarMap.emplace(sourceVscp, mappedVscp);
                    if (canRemapGeneratedCloneVarByName(sourceVscp)) {
                        noteGeneratedVarRemap(sourceVscp, stats);
                        ++stats.m_orderCacheCloneGeneratedVarRemaps;
                    }
                    return;
                }
                if (resolvedVarMap.find(sourceVscp) == resolvedVarMap.end()) {
                    AstScope* const sourceBoundaryScopep = boundaryScopeFor(sourceVscp->scopep());
                    if (sourceBoundaryScopep && sourceBoundaryScopep != destBoundaryScopep) {
                        if (AstVarScope* const mappedVscp
                            = findScopeCloneVarByName(destBoundaryScopep, sourceVscp)) {
                            resolvedVarMap.emplace(sourceVscp, mappedVscp);
                            if (canRemapGeneratedCloneVarByName(sourceVscp)) {
                                noteGeneratedVarRemap(sourceVscp, stats);
                                ++stats.m_orderCacheCloneGeneratedVarRemaps;
                            }
                            return;
                        }
                        noteUnmappedVar(sourceVscp);
                        failed = true;
                        return;
                    }
                    if (isTriggeredStateVar(sourceVscp) && refp->access().isReadOnly()) {
                        resolvedVarMap.emplace(sourceVscp, const_cast<AstVarScope*>(sourceVscp));
                        return;
                    }
                    if (AstVarScope* const mappedVscp
                        = findGeneratedCloneVarByName(destBoundaryScopep, sourceVscp)) {
                        resolvedVarMap.emplace(sourceVscp, mappedVscp);
                        noteGeneratedVarRemap(sourceVscp, stats);
                        ++stats.m_orderCacheCloneGeneratedVarRemaps;
                        return;
                    }
                    noteUnmappedVar(sourceVscp);
                    failed = true;
                }
            });
            if (failed) return nullptr;
        }

        std::unordered_map<const AstCFunc*, AstCFunc*> clonedFuncs;
        std::unordered_map<const AstVar*, AstVarScope*> clonedArgVscps;
        const auto cloneFuncShell = [&](AstCFunc* origFuncp) {
            AstCFunc* const clonep = new AstCFunc{
                origFuncp->fileline(), origFuncp->name() + "__sgclone_" + cvtToStr(s_cloneIndex++),
                destBoundaryScopep, origFuncp->rtnTypeVoid()};
            clonep->argTypes(origFuncp->argTypes());
            clonep->cname(clonep->name());
            clonep->declPrivate(origFuncp->declPrivate());
            clonep->dontCombine(origFuncp->dontCombine());
            clonep->dpiContext(origFuncp->dpiContext());
            clonep->dpiExportDispatcher(origFuncp->dpiExportDispatcher());
            clonep->dpiExportImpl(origFuncp->dpiExportImpl());
            clonep->dpiImportPrototype(origFuncp->dpiImportPrototype());
            clonep->dpiImportWrapper(origFuncp->dpiImportWrapper());
            clonep->dpiPure(origFuncp->dpiPure());
            clonep->entryPoint(origFuncp->entryPoint());
            clonep->funcPublic(origFuncp->funcPublic());
            clonep->ifdef(origFuncp->ifdef());
            clonep->isConst(origFuncp->isConst());
            clonep->isConstructor(origFuncp->isConstructor());
            clonep->isDestructor(origFuncp->isDestructor());
            clonep->isLoose(origFuncp->isLoose());
            clonep->isMethod(origFuncp->isMethod());
            clonep->isStatic(origFuncp->isStatic());
            clonep->isTrace(origFuncp->isTrace());
            clonep->isVirtual(origFuncp->isVirtual());
            clonep->keepIfEmpty(origFuncp->keepIfEmpty());
            if (origFuncp->needProcess()) clonep->setNeedProcess();
            clonep->scopep(destBoundaryScopep);
            clonep->slow(origFuncp->slow());
            destBoundaryScopep->addBlocksp(clonep);

            for (AstVar* argp = origFuncp->argsp(); argp; argp = VN_AS(argp->nextp(), Var)) {
                AstVar* const clonedArgp = argp->cloneTree(false);
                clonep->addArgsp(clonedArgp);
                AstVarScope* const clonedVscp
                    = new AstVarScope{clonedArgp->fileline(), destBoundaryScopep, clonedArgp};
                destBoundaryScopep->addVarsp(clonedVscp);
                clonedArgVscps.emplace(argp, clonedVscp);
            }
            clonedFuncs.emplace(origFuncp, clonep);
        };
        for (AstCFunc* const origFuncp : orderedFuncs) cloneFuncShell(origFuncp);

        for (AstCFunc* const origFuncp : orderedFuncs) {
            AstCFunc* const clonedFuncp = clonedFuncs.at(origFuncp);
            if (!origFuncp->stmtsp()) continue;
            AstNode* const bodyp = origFuncp->stmtsp()->cloneTree(true);
            if (origFuncp == funcp && triggerDomainp) {
                AstIf* triggerIfp = nullptr;
                for (AstNode* stmtp = bodyp; stmtp; stmtp = stmtp->nextp()) {
                    triggerIfp = VN_AS(stmtp, If);
                }
                AstIf* const currentIfp = util::createIfFromSenTree(triggerDomainp);
                AstNodeExpr* const currentCondp = currentIfp->condp()->unlinkFrBack();
                AstNodeExpr* const oldCondp = triggerIfp->condp()->unlinkFrBack();
                triggerIfp->condp(currentCondp);
                VL_DO_DANGLING(oldCondp->deleteTree(), oldCondp);
                VL_DO_DANGLING(currentIfp->deleteTree(), currentIfp);
            }
            std::vector<const AstConst*> originalConstps;
            std::vector<AstConst*> clonedConstps;
            origFuncp->stmtsp()->foreachAndNext(
                [&](AstConst* constp) { originalConstps.push_back(constp); });
            bodyp->foreachAndNext([&](AstConst* constp) { clonedConstps.push_back(constp); });
            UASSERT_OBJ(originalConstps.size() == clonedConstps.size(), origFuncp,
                        "Mismatched constants in ordered function clone");
            for (size_t i = 0; i < originalConstps.size(); ++i) {
                const auto it = templateConstMap.find(originalConstps[i]);
                if (it == templateConstMap.end()) continue;
                if (originalConstps[i]->num().toString() == it->second->num().toString()) continue;
                AstConst* const replacementp = const_cast<AstConst*>(it->second)->cloneTree(false);
                clonedConstps[i]->replaceWith(replacementp);
                VL_DO_DANGLING(clonedConstps[i]->deleteTree(), clonedConstps[i]);
                if (remapVariant) {
                    ++stats.m_sharedHelperRemapVariantConstantRemaps;
                } else {
                    ++stats.m_orderCacheRecipeConstantRemaps;
                }
            }
            bool failed = false;
            bodyp->foreachAndNext([&](AstCCall* callp) {
                if (failed) return;
                const auto it = clonedFuncs.find(callp->funcp());
                if (it == clonedFuncs.end()) return;
                callp->funcp(it->second);
                callp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
            });
            bodyp->foreachAndNext([&](AstNodeVarRef* refp) {
                if (failed) return;
                if (AstVarXRef* const xrefp = VN_CAST(refp, VarXRef)) {
                    if (AstVarScope* const mappedVscp
                        = findDestScopedCloneVar(destBoundaryScopep, xrefp)) {
                        remapCloneRefTo(refp, mappedVscp);
                        return;
                    }
                }
                if (VN_IS(refp, VarXRef) && mustBeDestScopedInClone(refp->varScopep())) {
                    noteUnmappedVar(refp->varScopep());
                    failed = true;
                    return;
                }
                if (hasUnsafeCloneSelfPointer(refp)) {
                    noteUnmappedVar(refp->varScopep());
                    failed = true;
                    return;
                }
                const auto argIt = clonedArgVscps.find(refp->varp());
                if (argIt != clonedArgVscps.end()) {
                    refp->varp(argIt->second->varp());
                    refp->varScopep(argIt->second);
                    refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
                    return;
                }
                const auto varIt = resolvedVarMap.find(refp->varScopep());
                if (varIt != resolvedVarMap.end()) {
                    refp->varp(varIt->second->varp());
                    refp->varScopep(varIt->second);
                    if (varIt->second->scopep() == destBoundaryScopep) {
                        refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
                    }
                    if (mustBeDestScopedInClone(varIt->second)
                        && varIt->second->scopep() != destBoundaryScopep) {
                        noteUnmappedVar(varIt->second);
                        failed = true;
                    }
                    return;
                }
                if (AstVarScope* const mappedVscp
                    = findDestScopedCloneVar(destBoundaryScopep, refp)) {
                    refp->varp(mappedVscp->varp());
                    refp->varScopep(mappedVscp);
                    refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
                    return;
                }
                if (AstVarScope* const mappedVscp
                    = findDestScopedCloneVar(destBoundaryScopep, refp->varScopep())) {
                    refp->varp(mappedVscp->varp());
                    refp->varScopep(mappedVscp);
                    refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
                    return;
                }
                if (AstVarScope* const mappedVscp
                    = findScopeCloneVarByVar(destBoundaryScopep, refp->varp())) {
                    refp->varp(mappedVscp->varp());
                    refp->varScopep(mappedVscp);
                    refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
                    return;
                }
                noteUnmappedVar(refp->varScopep());
                failed = true;
            });
            if (failed) {
                VL_DO_DANGLING(bodyp->deleteTree(), bodyp);
                return nullptr;
            }
            clonedFuncp->addStmtsp(bodyp);
        }

        return clonedFuncs.at(funcp);
    }

    static AstCFunc* cloneOrderedFuncGraph(
        AstCFunc* funcp, AstScope* destBoundaryScopep,
        const std::unordered_map<const AstVarScope*, AstVarScope*>& templateVarMap,
        const std::unordered_map<const AstConst*, const AstConst*>& templateConstMap,
        SubgraphLoweringStats& stats, AstSenTree* triggerDomainp = nullptr,
        bool remapVariant = false) {
        const uint64_t startUsecs = statStartUsecs();
        AstCFunc* const resultp
            = cloneOrderedFuncGraphImpl(funcp, destBoundaryScopep, templateVarMap,
                                        templateConstMap, stats, triggerDomainp, remapVariant);
        addElapsedUsecs(stats.m_timeCloneOrderedFuncsUsecs, startUsecs);
        return resultp;
    }

    static bool appendContractBoundaryUse(SubgraphInstanceContract& contract, AstVarScope* vscp,
                                          AstScope* boundaryScopep, VAccess access) {
        if (vscp->scopep() != boundaryScopep || !vscp->varp()->isIO()) return false;
        if (access.isReadOrRW() && vscp->varp()->direction().isNonOutput()) {
            contract.addBoundaryRead(vscp, V3SubgraphSummary::isDerivedBoundaryInput(vscp));
        }
        if (access.isWriteOrRW()) {
            const SubgraphInstanceContract* const summaryp
                = getSubgraphScopeContract(boundaryScopep);
            if (summaryp
                && std::find(summaryp->m_boundaryWrites.begin(), summaryp->m_boundaryWrites.end(),
                             vscp)
                       != summaryp->m_boundaryWrites.end()) {
                contract.addBoundaryWrite(vscp);
            }
        }
        return true;
    }

    void appendContractExternalUses(SubgraphInstanceContract& contract, AstCFunc* funcp,
                                    AstScope* boundaryScopep) {
        std::unordered_set<AstCFunc*> seen;
        std::function<void(AstCFunc*)> gather = [&](AstCFunc* scanFuncp) {
            if (!seen.insert(scanFuncp).second) return;
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const vscp = refp->varScopep();
                if (vscp->varp()->isFuncLocal()) return;
                if (0 == vscp->varp()->name().rfind("__VsubgraphSnapshot__", 0)) {
                    ++m_stats.m_contractExternalUseSnapshotSkips;
                    return;
                }
                if (appendContractBoundaryUse(contract, vscp, boundaryScopep, refp->access())) {
                    return;
                }
                const bool externalToSubgraph
                    = !isUnderBoundaryScope(vscp->scopep(), boundaryScopep);
                if (!externalToSubgraph) {
                    if (refp->access().isWriteOrRW() && m_parentConsumedSubgraphVars.count(vscp)) {
                        if (contract.addCoarseWrite(vscp)) {
                            ++m_stats.m_parentConsumedContractWrites;
                        }
                    }
                    return;
                }
                contract.addExternalUse(vscp, refp->access().isReadOrRW(),
                                        refp->access().isWriteOrRW());
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint()) return;
                gather(calledFuncp);
            });
        };
        gather(funcp);
        ++m_stats.m_contractExternalUseScans;
    }

    void appendContractHelperArgUses(SubgraphInstanceContract& contract,
                                     const std::vector<SubgraphSharedHelperArg>& helperArgs) {
        for (const SubgraphSharedHelperArg& arg : helperArgs) {
            if (!arg.m_writes || !m_parentConsumedSubgraphVars.count(arg.m_vscp)) continue;
            if (contract.addCoarseWrite(arg.m_vscp)) { ++m_stats.m_parentConsumedContractWrites; }
        }
    }

    void appendContractTailUses(SubgraphInstanceContract& contract, AstCFunc* funcp,
                                AstScope* boundaryScopep) {
        std::unordered_set<AstCFunc*> seen;
        std::function<void(AstCFunc*)> gather = [&](AstCFunc* scanFuncp) {
            if (!seen.insert(scanFuncp).second) return;
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const vscp = refp->varScopep();
                if (vscp->varp()->isFuncLocal()) return;
                if (appendContractBoundaryUse(contract, vscp, boundaryScopep, refp->access())) {
                    return;
                }
                if (isUnderBoundaryScope(vscp->scopep(), boundaryScopep)) return;
                contract.addExternalUse(vscp, refp->access().isReadOrRW(),
                                        refp->access().isWriteOrRW());
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint()) return;
                gather(calledFuncp);
            });
        };
        gather(funcp);
        ++m_stats.m_contractExternalUseScans;
    }

    void appendContractExternalUses(SubgraphInstanceContract& contract, const LogicByScope& logic,
                                    AstScope* boundaryScopep) {
        logic.foreachLogic([&](AstNode* logicp) {
            logicp->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const vscp = refp->varScopep();
                if (0 == vscp->varp()->name().rfind("__VsubgraphSnapshot__", 0)) {
                    ++m_stats.m_contractExternalUseSnapshotSkips;
                    return;
                }
                if (appendContractBoundaryUse(contract, vscp, boundaryScopep, refp->access())) {
                    return;
                }
                const bool externalToSubgraph
                    = !isUnderBoundaryScope(vscp->scopep(), boundaryScopep);
                if (!externalToSubgraph) {
                    if (refp->access().isWriteOrRW() && m_parentConsumedSubgraphVars.count(vscp)) {
                        if (contract.addCoarseWrite(vscp)) {
                            ++m_stats.m_parentConsumedContractWrites;
                        }
                    }
                    return;
                }
                contract.addExternalUse(vscp, refp->access().isReadOrRW(),
                                        refp->access().isWriteOrRW());
            });
        });
        ++m_stats.m_contractExternalUseScans;
    }

    static bool sharedHelperSupportsPhase(AstSubgraphInstance::Phase phase) {
        return phase == AstSubgraphInstance::Phase::PRE
               || phase == AstSubgraphInstance::Phase::POST;
    }

    static bool scopeHasDerivedBoundaryReads(AstScope* scopep) {
        const SubgraphInstanceContract* const contractp = getSubgraphScopeContract(scopep);
        if (!contractp) return false;
        for (const AstSubgraphInstance::BoundaryReadContract& read : contractp->m_boundaryReads) {
            if (read.m_derived) return true;
        }
        return false;
    }

    static SubgraphSharedHelperSkipReason
    classifySharedHelperUse(SubgraphScheduleArtifact* artifactp,
                            const SubgraphSharedHelperContext& context) {
        if (!sharedHelperSupportsPhase(context.m_phase))
            return SubgraphSharedHelperSkipReason::PHASE;
        const SubgraphScheduleBundleContext& bundleContext = *context.m_bundlep;
        if (!artifactp->m_callFuncp->isLoose()) return SubgraphSharedHelperSkipReason::NON_LOOSE;
        if (artifactp->m_callFuncp->scopep()->modp() != bundleContext.m_currentScopep->modp()) {
            return SubgraphSharedHelperSkipReason::MODULE_MISMATCH;
        }
        if (scopeHasDerivedBoundaryReads(artifactp->m_scopep)
            || bundleContext.m_currentScopeHasDerivedBoundaryReads) {
            return SubgraphSharedHelperSkipReason::OTHER;
        }
        if (artifactp->m_hasTriggered) {
            if (!artifactp->m_triggeredShareable) {
                return SubgraphSharedHelperSkipReason::TRIGGERED_NOT_SHAREABLE;
            }
            return SubgraphSharedHelperSkipReason::NONE;
        }
        if (artifactp->m_cloneable) return SubgraphSharedHelperSkipReason::NONE;
        switch (artifactp->m_uncloneableReason) {
        case SubgraphArtifactUncloneableReason::TRIGGERED:
            return SubgraphSharedHelperSkipReason::TRIGGERED;
        case SubgraphArtifactUncloneableReason::CLONE_FAIL:
            return SubgraphSharedHelperSkipReason::CLONE_FAIL;
        case SubgraphArtifactUncloneableReason::NONE: return SubgraphSharedHelperSkipReason::OTHER;
        }
        return SubgraphSharedHelperSkipReason::OTHER;
    }

    static bool canUseSharedHelper(SubgraphScheduleArtifact* artifactp,
                                   const SubgraphSharedHelperContext& context) {
        return classifySharedHelperUse(artifactp, context) == SubgraphSharedHelperSkipReason::NONE;
    }

    static AstCFunc* sharedHelperCallFunc(AstCFunc* helperFuncp, bool hasTriggered,
                                          AstSenTree* currentDomainp, AstSenTree*& callDomainp) {
        callDomainp = nullptr;
        if (!hasTriggered) return helperFuncp;
        AstIf* const guardp = VN_CAST(helperFuncp->stmtsp(), If);
        if (!guardp || guardp->nextp() || guardp->elsesp() || !guardp->thensp()) return nullptr;
        AstCFunc* bodyFuncp = nullptr;
        bool multipleCalls = false;
        guardp->thensp()->foreach([&](AstCCall* callp) {
            if (bodyFuncp) {
                multipleCalls = true;
            } else {
                bodyFuncp = callp->funcp();
            }
        });
        if (!bodyFuncp || multipleCalls || bodyFuncp->entryPoint()
            || bodyFuncp->dpiImportPrototype() || bodyFuncp->scopep() != helperFuncp->scopep()) {
            return nullptr;
        }
        callDomainp = currentDomainp;
        return bodyFuncp;
    }

    static AstCFunc* sharedHelperCallFunc(const SubgraphScheduleArtifact& artifact,
                                          AstSenTree* currentDomainp, AstSenTree*& callDomainp) {
        return sharedHelperCallFunc(artifact.m_callFuncp, artifact.m_hasTriggered, currentDomainp,
                                    callDomainp);
    }

    void noteSharedReuseSkip(SubgraphSharedHelperSkipReason reason) {
        switch (reason) {
        case SubgraphSharedHelperSkipReason::NONE: return;
        case SubgraphSharedHelperSkipReason::CLONE_FAIL:
            ++m_stats.m_artifactReuseSharedSkipCloneFail;
            return;
        case SubgraphSharedHelperSkipReason::MODULE_MISMATCH:
            ++m_stats.m_artifactReuseSharedSkipModuleMismatch;
            return;
        case SubgraphSharedHelperSkipReason::NON_LOOSE:
            ++m_stats.m_artifactReuseSharedSkipNonLoose;
            return;
        case SubgraphSharedHelperSkipReason::OTHER:
            ++m_stats.m_artifactReuseSharedSkipOther;
            return;
        case SubgraphSharedHelperSkipReason::PHASE:
            ++m_stats.m_artifactReuseSharedSkipPhase;
            return;
        case SubgraphSharedHelperSkipReason::TRIGGERED:
            ++m_stats.m_artifactReuseSharedSkipTriggered;
            ++m_stats.m_artifactReuseSharedSkipTriggeredOther;
            return;
        case SubgraphSharedHelperSkipReason::TRIGGERED_INPUT_TAIL:
            ++m_stats.m_artifactReuseSharedSkipTriggered;
            ++m_stats.m_artifactReuseSharedSkipTriggeredInputTail;
            return;
        case SubgraphSharedHelperSkipReason::TRIGGERED_NOT_SHAREABLE:
            ++m_stats.m_artifactReuseSharedSkipTriggered;
            ++m_stats.m_artifactReuseSharedSkipTriggeredNotShareable;
            return;
        }
    }

    void noteOrderCacheSharedSkip(SubgraphSharedHelperSkipReason reason) {
        switch (reason) {
        case SubgraphSharedHelperSkipReason::NONE: return;
        case SubgraphSharedHelperSkipReason::CLONE_FAIL:
            ++m_stats.m_orderCacheSharedSkipCloneFail;
            return;
        case SubgraphSharedHelperSkipReason::MODULE_MISMATCH:
            ++m_stats.m_orderCacheSharedSkipModuleMismatch;
            return;
        case SubgraphSharedHelperSkipReason::NON_LOOSE:
            ++m_stats.m_orderCacheSharedSkipNonLoose;
            return;
        case SubgraphSharedHelperSkipReason::OTHER: ++m_stats.m_orderCacheSharedSkipOther; return;
        case SubgraphSharedHelperSkipReason::PHASE: ++m_stats.m_orderCacheSharedSkipPhase; return;
        case SubgraphSharedHelperSkipReason::TRIGGERED:
            ++m_stats.m_orderCacheSharedSkipTriggered;
            ++m_stats.m_orderCacheSharedSkipTriggeredOther;
            return;
        case SubgraphSharedHelperSkipReason::TRIGGERED_INPUT_TAIL:
            ++m_stats.m_orderCacheSharedSkipTriggered;
            ++m_stats.m_orderCacheSharedSkipTriggeredInputTail;
            return;
        case SubgraphSharedHelperSkipReason::TRIGGERED_NOT_SHAREABLE:
            ++m_stats.m_orderCacheSharedSkipTriggered;
            ++m_stats.m_orderCacheSharedSkipTriggeredNotShareable;
            return;
        }
    }

    void noteOrderCacheSharedSkip(SubgraphSharedHelperApplyFailReason reason) {
        switch (reason) {
        case SubgraphSharedHelperApplyFailReason::NONE: return;
        case SubgraphSharedHelperApplyFailReason::ARGUMENTS:
            ++m_stats.m_orderCacheSharedSkipArguments;
            return;
        case SubgraphSharedHelperApplyFailReason::CALL_FUNCTION:
            ++m_stats.m_orderCacheSharedSkipCallFunction;
            return;
        case SubgraphSharedHelperApplyFailReason::CONSTANTS:
            ++m_stats.m_orderCacheSharedSkipConstants;
            return;
        }
    }

    SubgraphScheduleArtifactReuse
    findReusableSubgraphScheduleArtifact(const SubgraphScheduleArtifactKey& key,
                                         const LogicByScope& currentLogic,
                                         const SubgraphSharedHelperContext& sharedContext) {
        SubgraphScheduleArtifactReuse reuse;
        AstScope* const currentScopep = sharedContext.m_bundlep->m_currentScopep;
        ++m_stats.m_artifactReuseLookups;
        const auto it = m_subgraphArtifactCache.find(key);
        if (it == m_subgraphArtifactCache.end()) {
            ++m_stats.m_artifactReuseMissNoEntry;
            noteArtifactNoEntry(key);
            return reuse;
        }
        ++m_stats.m_artifactReuseFullKeyHits;
        m_stats.m_artifactReuseFullKeyCandidates += it->second.size();
        bool sawCloneFail = false;
        bool sawLogicMismatch = false;
        bool sawOther = false;
        bool sawTriggered = false;
        for (SubgraphScheduleArtifact* const artifactp : it->second) {
            reuse.m_remap.m_templateVarMap.clear();
            const uint64_t templateMapStartUsecs = statStartUsecs();
            const SubgraphTemplateMapFailReason templateMapFail = buildTemplateVarScopeMap(
                artifactp->m_logicSig, currentLogic, reuse.m_remap.m_templateVarMap);
            addElapsedUsecs(m_stats.m_timeTemplateMapUsecs, templateMapStartUsecs);
            if (templateMapFail != SubgraphTemplateMapFailReason::NONE) {
                m_stats.noteArtifactTemplateMapFail(templateMapFail);
                sawLogicMismatch = true;
                continue;
            }
            if (!artifactp->m_cloneable && artifactp->m_scopep != currentScopep
                && !canUseSharedHelper(artifactp, sharedContext)) {
                noteSharedReuseSkip(classifySharedHelperUse(artifactp, sharedContext));
                switch (artifactp->m_uncloneableReason) {
                case SubgraphArtifactUncloneableReason::TRIGGERED: sawTriggered = true; break;
                case SubgraphArtifactUncloneableReason::CLONE_FAIL: sawCloneFail = true; break;
                case SubgraphArtifactUncloneableReason::NONE: sawOther = true; break;
                }
                continue;
            }
            reuse.m_artifactp = artifactp;
            return reuse;
        }
        if (sawLogicMismatch) ++m_stats.m_artifactReuseMissLogicMismatch;
        if (sawTriggered) ++m_stats.m_artifactReuseSkipTriggered;
        if (sawCloneFail) ++m_stats.m_artifactReuseSkipCloneFail;
        if (sawOther) ++m_stats.m_artifactReuseSkipOther;
        reuse.m_remap.m_templateVarMap.clear();
        return reuse;
    }

    bool hasReusableSubgraphScheduleArtifactCoarse(const SubgraphScheduleArtifactCoarseKey& key) {
        ++m_stats.m_artifactReuseCoarseLookups;
        if (m_subgraphArtifactCoarseCache.count(key)) {
            ++m_stats.m_artifactReuseCoarseHits;
            return true;
        }
        ++m_stats.m_artifactReuseCoarseMisses;
        return false;
    }

    void noteSubgraphScheduleArtifactCoarseMiss(const SubgraphScheduleArtifactKey& key) {
        ++m_stats.m_artifactReuseLookups;
        ++m_stats.m_artifactReuseMissNoEntry;
        noteArtifactNoEntry(key);
    }

    void noteArtifactNoEntry(const SubgraphScheduleArtifactKey& key) {
        if (!v3Global.opt.stats()) return;
        bool constValueDiffers = false;
        bool nodeTopologyDiffers = false;
        bool refAccessDiffers = false;
        bool sameDomainShape = false;
        bool sameModule = false;
        bool sameModuleDomainShape = false;
        bool sameModulePhase = false;
        for (const auto& pair : m_subgraphArtifactCache) {
            const SubgraphScheduleArtifactKey& cached = pair.first;
            const bool moduleMatches = cached.m_modp == key.m_modp;
            const bool phaseMatches = cached.m_phase == key.m_phase;
            const bool domainShapeMatches = cached.m_domainShape == key.m_domainShape;
            sameDomainShape |= domainShapeMatches;
            sameModule |= moduleMatches;
            sameModuleDomainShape |= moduleMatches && domainShapeMatches;
            sameModulePhase |= moduleMatches && phaseMatches;
            if (!moduleMatches || !phaseMatches || !domainShapeMatches) continue;
            const SubgraphLogicShape& cachedShape = cached.m_logicShape;
            if (cachedShape.m_nodeTypes != key.m_logicShape.m_nodeTypes) {
                nodeTopologyDiffers = true;
            } else if (cachedShape.m_constValues != key.m_logicShape.m_constValues) {
                constValueDiffers = true;
            } else if (cachedShape.m_refAccesses != key.m_logicShape.m_refAccesses) {
                refAccessDiffers = true;
            }
        }
        if (constValueDiffers) ++m_stats.m_artifactReuseMissNoEntryConstValue;
        if (nodeTopologyDiffers) ++m_stats.m_artifactReuseMissNoEntryNodeTopology;
        if (refAccessDiffers) ++m_stats.m_artifactReuseMissNoEntryRefAccess;
        if (sameDomainShape) ++m_stats.m_artifactReuseMissNoEntrySameDomainShape;
        if (sameModule) ++m_stats.m_artifactReuseMissNoEntrySameModule;
        if (sameModuleDomainShape) ++m_stats.m_artifactReuseMissNoEntrySameModuleDomainShape;
        if (sameModulePhase) ++m_stats.m_artifactReuseMissNoEntrySameModulePhase;
    }

    void noteOrderCacheNoEntry(const SubgraphOrderCacheKey& key) {
        if (!v3Global.opt.stats()) return;
        bool constValueDiffers = false;
        bool nodeTopologyDiffers = false;
        bool refAccessDiffers = false;
        for (const auto& pair : m_subgraphOrderCache) {
            const SubgraphOrderCacheKey& cached = pair.first;
            if (cached.m_modp != key.m_modp || cached.m_phase != key.m_phase
                || !(cached.m_wrapper == key.m_wrapper)
                || cached.m_domainShape != key.m_domainShape) {
                continue;
            }
            const SubgraphLogicShape& cachedShape = cached.m_logicShape;
            if (cachedShape.m_nodeTypes != key.m_logicShape.m_nodeTypes) {
                nodeTopologyDiffers = true;
            } else if (cachedShape.m_constValues != key.m_logicShape.m_constValues) {
                constValueDiffers = true;
            } else if (cachedShape.m_refAccesses != key.m_logicShape.m_refAccesses) {
                refAccessDiffers = true;
            }
        }
        if (constValueDiffers) ++m_stats.m_orderCacheMissNoEntryConstValue;
        if (nodeTopologyDiffers) ++m_stats.m_orderCacheMissNoEntryNodeTopology;
        if (refAccessDiffers) ++m_stats.m_orderCacheMissNoEntryRefAccess;
    }

    SubgraphScheduleArtifact* makeSubgraphScheduleArtifact(
        const SubgraphScheduleArtifactKey& key, AstScope* scopep, AstCFunc* callFuncp,
        std::vector<SubgraphSharedHelperArg>&& helperArgs, SubgraphLogicSig&& logicSig,
        const SubgraphTailContract& tailContract, bool cloneable, bool hasTriggered,
        bool triggeredShareable, bool cacheable) {
        const uint64_t startUsecs = statStartUsecs();
        std::unique_ptr<SubgraphScheduleArtifact> artifactp{new SubgraphScheduleArtifact};
        artifactp->m_callFuncp = callFuncp;
        artifactp->m_helperArgs = std::move(helperArgs);
        artifactp->m_key = key;
        artifactp->m_logicSig = std::move(logicSig);
        artifactp->m_scopep = scopep;
        artifactp->m_tailContract = tailContract;
        artifactp->m_varMapContract = buildSharedHelperVarMapContract(
            artifactp->m_logicSig, artifactp->m_helperArgs, scopep, m_stats);
        artifactp->m_cloneable = cloneable;
        artifactp->m_hasTriggered = hasTriggered;
        artifactp->m_triggeredShareable = triggeredShareable;
        if (!cloneable)
            artifactp->m_uncloneableReason = SubgraphArtifactUncloneableReason::TRIGGERED;
        SubgraphScheduleArtifact* const resultp = artifactp.get();
        m_subgraphArtifacts.push_back(std::move(artifactp));
        if (cacheable) {
            m_subgraphArtifactCache[key].push_back(resultp);
            m_subgraphArtifactCoarseCache.insert(
                SubgraphScheduleArtifactCoarseKey{key.m_domainShape, key.m_modp, key.m_phase});
        }
        ++m_stats.m_artifactMisses;
        ++m_stats.m_artifacts;
        addElapsedUsecs(m_stats.m_timeMakeArtifactsUsecs, startUsecs);
        return resultp;
    }

    void updateArtifactKeyStats() {
        std::unordered_set<AstNodeModule*> modules;
        std::unordered_set<size_t> domainShapes;
        uint64_t maxEntries = 0;
        for (const auto& pair : m_subgraphArtifactCache) {
            modules.insert(pair.first.m_modp);
            domainShapes.insert(hashDomainShape(pair.first.m_domainShape));
            maxEntries = std::max<uint64_t>(maxEntries, pair.second.size());
        }
        m_stats.m_artifactKeyMaxEntriesPerFullKey = maxEntries;
        m_stats.m_artifactKeyUniqueDomainShapes = domainShapes.size();
        m_stats.m_artifactKeyUniqueFull = m_subgraphArtifactCache.size();
        m_stats.m_artifactKeyUniqueModules = modules.size();
    }

    SnapshotBucket& getSnapshotBucket(LogicByScope* ownerp, AstSenTree* senTreep) {
        const SnapshotBucketKey key{ownerp, senTreep};
        const auto it = m_snapshotBucketIndex.find(key);
        if (it != m_snapshotBucketIndex.end()) return m_snapshotBuckets[it->second];
        m_snapshotBuckets.emplace_back();
        SnapshotBucket& bucket = m_snapshotBuckets.back();
        bucket.m_ownerp = ownerp;
        bucket.m_senTreep = senTreep;
        m_snapshotBucketIndex.emplace(key, m_snapshotBuckets.size() - 1);
        return bucket;
    }

    void addSnapshotRequirement(LogicByScope* ownerp, AstSenTree* senTreep,
                                AstVarScope* sourceVscp) {
        SnapshotBucket& bucket = getSnapshotBucket(ownerp, senTreep);
        if (!bucket.m_seen.insert(sourceVscp).second) return;
        bucket.m_sourceVars.push_back(sourceVscp);
    }

    const SnapshotRef& getSnapshotRef(LogicByScope* ownerp, AstSenTree* senTreep,
                                      AstVarScope* sourceVscp) {
        SnapshotBucket& bucket = getSnapshotBucket(ownerp, senTreep);
        const auto it = bucket.m_snapshotRefs.find(sourceVscp);
        UASSERT_OBJ(it != bucket.m_snapshotRefs.end(), sourceVscp,
                    "Missing subgraph snapshot reference");
        return it->second;
    }

    static AstNodeExpr* makeSnapshotExpr(const SnapshotRef& snapshotRef, FileLine* flp,
                                         VAccess access) {
        AstNodeExpr* const refp = new AstVarRef{flp, snapshotRef.m_snapshotVscp, access};
        if (!snapshotRef.m_isBundle) return refp;
        return new AstArraySel{flp, refp, static_cast<int>(snapshotRef.m_elemIndex)};
    }

    bool needsCrossBoundarySnapshot(AstScope* boundaryScopep, AstVarScope* sourceVscp) const {
        AstScope* const sourceScopep = sourceVscp->scopep();
        const bool boundaryInput = sourceScopep == boundaryScopep && sourceVscp->varp()->isIO()
                                   && sourceVscp->varp()->direction().isNonOutput();
        const bool externalRead = !isUnderBoundaryScope(sourceScopep, boundaryScopep);
        if (!boundaryInput && !externalRead) return false;

        if (sourceScopep != boundaryScopep && boundaryScopeFor(sourceScopep)) {
            const bool otherBoundaryInput
                = sourceVscp->varp()->isIO() && sourceVscp->varp()->direction().isNonOutput();
            if (otherBoundaryInput) return m_regionWrittenVars.count(sourceVscp);
            return true;
        }

        return m_regionWrittenVars.count(sourceVscp);
    }

    void collectCrossBoundaryReads(AstNode* nodep, AstScope* boundaryScopep, LogicByScope* ownerp,
                                   AstSenTree* senTreep) {
        nodep->foreach([&](AstVarRef* refp) {
            if (refp->access() != VAccess::READ) return;
            AstVarScope* const sourceVscp = refp->varScopep();
            if (!needsCrossBoundarySnapshot(boundaryScopep, sourceVscp)) return;
            addSnapshotRequirement(ownerp, senTreep, sourceVscp);
        });
    }

    void collectCrossBoundaryReadsInLogic(LogicByScope& subgraphLogic, AstScope* boundaryScopep,
                                          LogicByScope* ownerp, AstSenTree* senTreep) {
        subgraphLogic.foreachLogic([&](AstNode* logicp) {
            collectCrossBoundaryReads(logicp, boundaryScopep, ownerp, senTreep);
        });
    }

    void rewriteCrossBoundaryReads(AstNode* nodep, AstScope* boundaryScopep, LogicByScope* ownerp,
                                   AstSenTree* senTreep) {
        if (!m_snapshotCrossBoundaryReads) return;
        nodep->foreach([&](AstVarRef* refp) {
            if (refp->access() != VAccess::READ) return;
            AstVarScope* const sourceVscp = refp->varScopep();
            if (!needsCrossBoundarySnapshot(boundaryScopep, sourceVscp)) return;
            const SnapshotRef& snapshotRef = getSnapshotRef(ownerp, senTreep, sourceVscp);
            refp->replaceWith(makeSnapshotExpr(snapshotRef, refp->fileline(), VAccess::READ));
            VL_DO_DANGLING(refp->deleteTree(), refp);
        });
    }

    void rewriteCrossBoundaryReadsInLogic(LogicByScope& subgraphLogic, AstScope* boundaryScopep,
                                          LogicByScope* ownerp, AstSenTree* senTreep) {
        subgraphLogic.foreachLogic([&](AstNode* logicp) {
            rewriteCrossBoundaryReads(logicp, boundaryScopep, ownerp, senTreep);
        });
    }

    AstCFunc* makeStlTailWrapper(const SubgraphScheduleInstance& instance, bool slow) {
        AstCFunc* const helperp = instance.m_callFuncp;
        AstScope* const scopep = instance.m_scopep;
        FileLine* const flp = helperp->fileline();
        AstCFunc* const wrapperp = new AstCFunc{
            flp, "__VsubgraphStlTailWrapper_" + cvtToStr(m_stlTailWrapperIndex++), scopep, ""};
        wrapperp->declPrivate(true);
        wrapperp->dontCombine(true);
        wrapperp->isConst(false);
        wrapperp->isLoose(true);
        wrapperp->isStatic(false);
        wrapperp->slow(slow);
        scopep->addBlocksp(wrapperp);

        AstCCall* const callp = new AstCCall{flp, helperp};
        if (instance.m_sharedCall) {
            callp->selfPointer(
                VSelfPointerText{VSelfPointerText::VlSyms{}, instance.m_scopep->nameDotless()});
        }
        for (const SubgraphSharedHelperArg& arg : instance.m_helperArgs) {
            callp->addArgsp(new AstVarRef{flp, arg.m_vscp, sharedHelperArgAccess(arg)});
        }
        callp->dtypeSetVoid();
        wrapperp->addStmtsp(callp->makeStmt());
        ++m_stats.m_tailWrappers;
        return wrapperp;
    }

    AstCFunc* cloneTailFuncForNba(AstCFunc* tailFuncp, AstScope* boundaryScopep,
                                  LogicByScope* ownerp, AstSenTree* senTreep, bool slow) {
        const TailCloneKey key{boundaryScopep, ownerp, senTreep, slow,
                               buildTailCloneSig(tailFuncp)};
        const auto it = m_tailCloneCache.find(key);
        if (it != m_tailCloneCache.end()) {
            ++m_stats.m_tailCloneReuses;
            return it->second;
        }

        static unsigned s_tailCloneIndex = 0;
        const bool shareSubgraphHelper
            = boundaryScopep->modp()->subgraphBoundary() && !tailFuncp->cname().empty();
        const string cloneName
            = shareSubgraphHelper
                  ? tailFuncp->name() + "__sgclone__nba_" + cvtToStr(s_tailCloneIndex++)
                  : tailFuncp->name() + "__nba_" + cvtToStr(s_tailCloneIndex++);
        AstCFunc* const clonep
            = new AstCFunc{tailFuncp->fileline(), cloneName, boundaryScopep, ""};
        clonep->dontCombine(true);
        clonep->isStatic(false);
        clonep->isLoose(true);
        clonep->slow(slow);
        clonep->isConst(false);
        clonep->declPrivate(true);
        if (shareSubgraphHelper) clonep->cname(tailFuncp->cname() + "__nba");
        boundaryScopep->addBlocksp(clonep);
        if (tailFuncp->stmtsp()) {
            AstNode* const bodyp = tailFuncp->stmtsp()->cloneTree(true);
            rewriteCrossBoundaryReads(bodyp, boundaryScopep, ownerp, senTreep);
            clonep->addStmtsp(bodyp);
        }
        ++m_stats.m_tailClones;
        m_tailCloneCache.emplace(key, clonep);
        return clonep;
    }

    bool tailNeedsNbaClone(AstCFunc* tailFuncp, AstScope* boundaryScopep) const {
        bool needClone = false;
        tailFuncp->foreach([&](AstVarRef* refp) {
            if (needClone || refp->access() != VAccess::READ) return;
            if (needsCrossBoundarySnapshot(boundaryScopep, refp->varScopep())) needClone = true;
        });
        return needClone;
    }

    bool m_snapshotCrossBoundaryReads = false;
    unsigned m_stlTailWrapperIndex = 0;
    std::unordered_set<SubgraphScheduleArtifactCoarseKey, SubgraphScheduleArtifactCoarseKeyHash>
        m_subgraphArtifactCoarseCache;
    std::unordered_map<SubgraphScheduleArtifactKey, std::vector<SubgraphScheduleArtifact*>,
                       SubgraphScheduleArtifactKeyHash>
        m_subgraphArtifactCache;
    std::vector<std::unique_ptr<SubgraphScheduleArtifact>> m_subgraphArtifacts;
    std::unordered_map<SubgraphOrderCacheKey, SubgraphOrderCacheBucket, SubgraphOrderCacheKeyHash>
        m_subgraphOrderCache;
    std::unordered_map<SnapshotBucketKey, size_t, SnapshotBucketKeyHash> m_snapshotBucketIndex;
    std::vector<SnapshotBucket> m_snapshotBuckets;
    std::unordered_map<SnapshotHelperKey, SnapshotHelperEntry, SnapshotHelperKeyHash>
        m_snapshotHelpers;
    std::unordered_set<SnapshotSourceSetKey, SnapshotSourceSetKeyHash> m_snapshotSourceSets;
    std::unordered_set<SubgraphSharedHelperHiddenUseKey, SubgraphSharedHelperHiddenUseKeyHash>
        m_sharedHelperHiddenUseCache;
    std::unordered_map<AstNodeModule*, std::unordered_map<string, AstScope*>>
        m_sharedHelperScopesByModule;
    std::unordered_map<AstScope*, std::unordered_map<string, std::vector<AstVarScope*>>>
        m_sharedHelperVarsByScope;
    std::unordered_map<TailCloneKey, AstCFunc*, TailCloneKeyHash> m_tailCloneCache;
    std::unordered_set<AstVarScope*> m_parentConsumedSubgraphVars;
    std::unordered_set<AstVarScope*> m_regionWrittenVars;
    std::unordered_map<const AstScope*, std::vector<AstCFunc*>>& m_stlSubgraphFuncs;
    SubgraphLoweringStats m_stats;
};

SubgraphGroup& findOrCreateSubgraphGroup(std::vector<SubgraphGroup>& groups, LogicByScope* ownerp,
                                         AstScope* scopep, AstSenTree* senTreep) {
    for (SubgraphGroup& group : groups) {
        if (group.m_scopep == scopep && group.m_senTreep == senTreep) return group;
    }
    groups.emplace_back();
    SubgraphGroup& group = groups.back();
    group.m_ownerp = ownerp;
    group.m_scopep = scopep;
    group.m_senTreep = senTreep;
    return group;
}

void collectSubgraphGroups(const std::vector<LogicByScope*>& logic,
                           std::vector<SubgraphGroup>& groups) {
    for (LogicByScope* const lbsp : logic) {
        LogicByScope lowered;

        for (const auto& pair : *lbsp) {
            AstScope* const scopep = pair.first;
            AstScope* const boundaryScopep = SubgraphLoweringState::boundaryScopeFor(scopep);
            if (!boundaryScopep) {
                lowered.emplace_back(pair);
                continue;
            }

            AstActive* const activep = pair.second;
            AstSenTree* const senTreep = activep->sentreep();
            SubgraphGroup& group
                = findOrCreateSubgraphGroup(groups, lbsp, boundaryScopep, senTreep);
            if (!group.m_flp) group.m_flp = activep->fileline();

            for (AstNode* stmtp = activep->stmtsp(); stmtp;) {
                AstNode* const nextp = stmtp->nextp();
                stmtp->unlinkFrBack();
                const bool isPost = VN_IS(stmtp, AlwaysPost);
                if (isPost) {
                    group.m_hasPost = true;
                } else if (!VN_IS(stmtp, AlwaysPre)) {
                    if (!group.m_hasNonPostLate) group.m_lateWrapper = wrapperFromLogic(stmtp);
                    group.m_hasNonPostLate = true;
                }
                LogicByScope& groupLogic
                    = VN_IS(stmtp, AlwaysPre) ? group.m_earlyLogic : group.m_lateLogic;
                groupLogic.add(scopep, senTreep, stmtp);
                stmtp = nextp;
            }
            activep->deleteTree();
        }
        *lbsp = std::move(lowered);
    }
}

SubgraphLogicInputStats collectSubgraphLogicInputStats(const std::vector<LogicByScope*>& logic) {
    SubgraphLogicInputStats stats;
    for (const LogicByScope* const lbsp : logic) {
        for (const auto& pair : *lbsp) {
            AstActive* const activep = pair.second;
            ++stats.m_actives;
            const uint64_t activeNodes = activep->nodeCount();
            uint64_t hiddenBodyNodes = 0;
            stats.m_nodes += activeNodes;
            for (AstNode* stmtp = activep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
                ++stats.m_directStatements;
                if (AstSubgraphInstance* const subgraphp = VN_CAST(stmtp, SubgraphInstance)) {
                    ++stats.m_directSubgraphInstances;
                    const uint64_t bodyNodes = subgraphp->nodeCount() - 1;
                    hiddenBodyNodes += bodyNodes;
                    stats.m_subgraphInstanceBodyNodes += bodyNodes;
                    if (subgraphp->phase() == AstSubgraphInstance::Phase::SNAPSHOT) {
                        ++stats.m_directSnapshotInstances;
                        stats.m_snapshotInstanceBodyNodes += bodyNodes;
                    }
                } else {
                    AstNodeProcedure* const procp = VN_CAST(stmtp, NodeProcedure);
                    if (procp && V3Sched::isSubgraphSnapshotProcedure(procp)) {
                        ++stats.m_directSnapshotProcedures;
                        const uint64_t bodyNodes = procp->nodeCount() - 1;
                        hiddenBodyNodes += bodyNodes;
                        stats.m_snapshotProcedureBodyNodes += bodyNodes;
                    } else {
                        ++stats.m_directOtherStatements;
                    }
                }
            }
            stats.m_parentVisibleNodes += activeNodes - hiddenBodyNodes;
            activep->foreach([&](AstSubgraphInstance*) { ++stats.m_subgraphInstances; });
        }
    }
    return stats;
}

void setSubgraphInputStatsBefore(SubgraphLoweringStats& stats,
                                 const SubgraphLogicInputStats& input) {
    stats.m_inputActivesBefore = input.m_actives;
    stats.m_inputDirectOtherStatementsBefore = input.m_directOtherStatements;
    stats.m_inputDirectSnapshotInstancesBefore = input.m_directSnapshotInstances;
    stats.m_inputDirectSnapshotProceduresBefore = input.m_directSnapshotProcedures;
    stats.m_inputDirectStatementsBefore = input.m_directStatements;
    stats.m_inputDirectSubgraphInstancesBefore = input.m_directSubgraphInstances;
    stats.m_inputNodesBefore = input.m_nodes;
    stats.m_inputParentVisibleNodesBefore = input.m_parentVisibleNodes;
    stats.m_inputSnapshotInstanceBodyNodesBefore = input.m_snapshotInstanceBodyNodes;
    stats.m_inputSnapshotProcedureBodyNodesBefore = input.m_snapshotProcedureBodyNodes;
    stats.m_inputSubgraphInstanceBodyNodesBefore = input.m_subgraphInstanceBodyNodes;
    stats.m_inputSubgraphInstancesBefore = input.m_subgraphInstances;
}

void setSubgraphInputStatsAfter(SubgraphLoweringStats& stats,
                                const SubgraphLogicInputStats& input) {
    stats.m_inputActivesAfter = input.m_actives;
    stats.m_inputDirectOtherStatementsAfter = input.m_directOtherStatements;
    stats.m_inputDirectSnapshotInstancesAfter = input.m_directSnapshotInstances;
    stats.m_inputDirectSnapshotProceduresAfter = input.m_directSnapshotProcedures;
    stats.m_inputDirectStatementsAfter = input.m_directStatements;
    stats.m_inputDirectSubgraphInstancesAfter = input.m_directSubgraphInstances;
    stats.m_inputNodesAfter = input.m_nodes;
    stats.m_inputParentVisibleNodesAfter = input.m_parentVisibleNodes;
    stats.m_inputSnapshotInstanceBodyNodesAfter = input.m_snapshotInstanceBodyNodes;
    stats.m_inputSnapshotProcedureBodyNodesAfter = input.m_snapshotProcedureBodyNodes;
    stats.m_inputSubgraphInstanceBodyNodesAfter = input.m_subgraphInstanceBodyNodes;
    stats.m_inputSubgraphInstancesAfter = input.m_subgraphInstances;
}

SubgraphWrapper lateWrapperForGroup(const SubgraphGroup& group) {
    if (group.m_hasNonPostLate) return group.m_lateWrapper;
    if (group.m_hasPost) return SubgraphWrapper{SubgraphWrapperKind::ALWAYS_POST};
    return wrapperFromLogic(group.m_lateLogic.front().second->stmtsp());
}

SubgraphInstanceContract buildSubgraphSchedulePlanContract(AstScope* scopep) {
    SubgraphInstanceContract contract;
    const SubgraphInstanceContract* const summaryp = getSubgraphScopeContract(scopep);
    if (summaryp) {
        contract.m_hasClockedState = summaryp->m_hasClockedState;
        contract.m_hasPostPhase = summaryp->m_hasPostPhase;
    }
    return contract;
}

void populateSubgraphInstanceContract(AstSubgraphInstance* subgraphp,
                                      const SubgraphInstanceContract& contract) {
    subgraphp->hasClockedState(contract.m_hasClockedState);
    subgraphp->hasPostPhase(contract.m_hasPostPhase);
    for (const auto& read : contract.m_boundaryReads) {
        subgraphp->addBoundaryRead(read.m_varscp, read.m_derived);
    }
    for (AstVarScope* const vscp : contract.m_boundaryWrites) {
        subgraphp->addBoundaryWrite(vscp);
    }
    for (AstVarScope* const vscp : contract.m_coarseWrites) { subgraphp->addCoarseWrite(vscp); }
    for (const auto& use : contract.m_externalUses) {
        subgraphp->addExternalUse(use.m_varscp, use.m_read, use.m_write);
    }
}

AstSubgraphInstance::Phase subgraphPhaseFor(const SubgraphWrapper& wrapper, bool isEarly) {
    if (wrapper.m_kind == SubgraphWrapperKind::ALWAYS_PRE || isEarly) {
        return AstSubgraphInstance::Phase::PRE;
    }
    return AstSubgraphInstance::Phase::POST;
}

void populateSubgraphScheduleInstanceContract(SubgraphScheduleInstance& instance,
                                              SubgraphLoweringState& state) {
    const uint64_t startUsecs = statStartUsecs();
    instance.m_contract = buildSubgraphSchedulePlanContract(instance.m_scopep);
    state.appendContractExternalUses(instance.m_contract, instance.m_callFuncp, instance.m_scopep);
    state.appendContractHelperArgUses(instance.m_contract, instance.m_helperArgs);
    for (AstCFunc* const tailFuncp : instance.m_tailFuncps) {
        state.appendContractTailUses(instance.m_contract, tailFuncp, instance.m_scopep);
    }
    addElapsedUsecs(state.m_stats.m_timeBuildContractUsecs, startUsecs);
}

void populateSubgraphScheduleInstanceContract(SubgraphScheduleInstance& instance,
                                              SubgraphLoweringState& state,
                                              const LogicByScope& logic) {
    const uint64_t startUsecs = statStartUsecs();
    instance.m_contract = buildSubgraphSchedulePlanContract(instance.m_scopep);
    state.appendContractExternalUses(instance.m_contract, logic, instance.m_scopep);
    state.appendContractHelperArgUses(instance.m_contract, instance.m_helperArgs);
    for (AstCFunc* const tailFuncp : instance.m_tailFuncps) {
        state.appendContractTailUses(instance.m_contract, tailFuncp, instance.m_scopep);
    }
    addElapsedUsecs(state.m_stats.m_timeBuildContractUsecs, startUsecs);
}

SubgraphTailContract buildSubgraphTailContract(const std::vector<AstCFunc*>& tailFuncps,
                                               AstScope* scopep) {
    SubgraphTailContract contract;
    std::unordered_set<AstCFunc*> seenFuncps;
    std::function<void(AstCFunc*)> gather = [&](AstCFunc* funcp) {
        if (!seenFuncps.insert(funcp).second) return;
        funcp->foreach([&](AstNodeVarRef* refp) {
            AstVarScope* const vscp = refp->varScopep();
            if (vscp->varp()->isFuncLocal()) return;
            bool* readp = nullptr;
            bool* writep = nullptr;
            if (vscp->scopep() != scopep) {
                readp = &contract.m_readsExternal;
                writep = &contract.m_writesExternal;
            } else if (vscp->varp()->isIO() && vscp->varp()->direction().isNonOutput()) {
                readp = &contract.m_readsBoundaryInput;
                writep = &contract.m_writesBoundaryInput;
            } else {
                readp = &contract.m_readsBoundaryState;
                writep = &contract.m_writesBoundaryState;
            }
            if (refp->access().isReadOrRW()) *readp = true;
            if (refp->access().isWriteOrRW()) *writep = true;
        });
        funcp->foreach([&](AstCCall* callp) {
            if (!callp->funcp()->entryPoint()) gather(callp->funcp());
        });
    };
    for (AstCFunc* const tailFuncp : tailFuncps) gather(tailFuncp);
    return contract;
}

void refreshSubgraphScheduleBundleContext(SubgraphScheduleBundleContext& context,
                                          const SubgraphLoweringState& state) {
    context.m_currentScopeTailContract = SubgraphTailContract{};
    const auto stlFuncsIt = state.m_stlSubgraphFuncs.find(context.m_currentScopep);
    if (stlFuncsIt == state.m_stlSubgraphFuncs.end()) return;
    context.m_currentScopeTailContract
        = buildSubgraphTailContract(stlFuncsIt->second, context.m_currentScopep);
}

AstActive* getOrCreateSubgraphActive(const SubgraphGroup& group,
                                     std::unordered_map<SubgraphActiveKey, SubgraphActiveEntry,
                                                        SubgraphActiveKeyHash>& subgraphActives) {
    const SubgraphActiveKey key{group.m_ownerp, group.m_senTreep};
    const auto it = subgraphActives.find(key);
    if (it != subgraphActives.end()) return it->second.m_activep;
    AstActive* const activep = new AstActive{group.m_flp, "subgraph", group.m_senTreep};
    subgraphActives.emplace(key, SubgraphActiveEntry{group.m_scopep, activep});
    return activep;
}

AstSubgraphInstance* getOrCreateSubgraphBatch(
    const SubgraphGroup& group, const SubgraphWrapper& wrapper, bool isEarly, AstActive* activep,
    std::unordered_map<SubgraphBatchKey, AstSubgraphInstance*, SubgraphBatchKeyHash>& batches,
    SubgraphLoweringState& state) {
    const AstSubgraphInstance::Phase phase = subgraphPhaseFor(wrapper, isEarly);
    const SubgraphBatchKey key{group.m_ownerp, group.m_scopep, group.m_senTreep, wrapper, phase};
    const auto it = batches.find(key);
    if (it != batches.end()) return it->second;

    AstSubgraphInstance* const subgraphp
        = new AstSubgraphInstance{group.m_flp, group.m_scopep, nullptr};
    subgraphp->phase(phase);
    subgraphp->wrapperKind(wrapper.m_kind);
    subgraphp->keyword(wrapper.m_keyword);
    activep->addStmtsp(subgraphp);
    batches.emplace(key, subgraphp);
    ++state.m_stats.m_instances;
    return subgraphp;
}

SubgraphSchedulePlan buildSubgraphSchedulePlan(
    AstNetlist* netlistp, LogicByScope& subgraphLogic, const SubgraphWrapper& wrapper,
    bool isEarly, const std::vector<AstCFunc*>* tailFuncps, const SubgraphGroup& group,
    const SubgraphScheduleBundleContext& bundleContext, SubgraphLoweringState& state,
    const V3Order::TrigToSenMap& trigToSen, const std::string& tag, bool slow,
    const V3Order::ExternalDomainsProvider& externalDomains, unsigned& subgraphIndex) {
    SubgraphSchedulePlan plan;
    if (subgraphLogic.empty()) return plan;
    const bool canShare
        = SubgraphLoweringState::canShareSubgraphLogic(subgraphLogic, group.m_scopep);
    SubgraphOrderCacheKey cacheKey;
    const uint64_t domainShapeStartUsecs = statStartUsecs();
    const SubgraphDomainShapes domainShapes = SubgraphLoweringState::computeDomainShapes(
        subgraphLogic, group.m_scopep, externalDomains);
    cacheKey.m_domainShape = domainShapes.m_canonical;
    addElapsedUsecs(state.m_stats.m_timeComputeDomainShapeUsecs, domainShapeStartUsecs);
    cacheKey.m_modp = group.m_scopep->modp();
    SubgraphOrderCacheEntry* insertedOrderCacheEntryp = nullptr;
    SubgraphLogicSig logicSig;
    bool logicShapeBuilt = false;
    const AstSubgraphInstance::Phase phase = subgraphPhaseFor(wrapper, isEarly);
    cacheKey.m_phase = phase;
    cacheKey.m_wrapper = wrapper;
    SubgraphScheduleArtifactKey artifactKey;
    artifactKey.m_domainShape = domainShapes.m_exact;
    artifactKey.m_modp = cacheKey.m_modp;
    artifactKey.m_phase = phase;
    const auto ensureLogicShape = [&]() {
        if (!canShare || logicShapeBuilt) return;
        const uint64_t logicShapeStartUsecs = statStartUsecs();
        cacheKey.m_logicShape = SubgraphLoweringState::buildLogicShape(subgraphLogic);
        addElapsedUsecs(state.m_stats.m_timeBuildLogicShapeUsecs, logicShapeStartUsecs);
        artifactKey.m_logicShape = cacheKey.m_logicShape;
        logicShapeBuilt = true;
        ++state.m_stats.m_logicShapeBuilds;
    };
    bool cacheableArtifact = canShare;
    const SubgraphSharedHelperContext sharedContext{&bundleContext, phase};
    if (cacheableArtifact) {
        if (tailFuncps) ++state.m_stats.m_artifactTailReuseCandidates;
        const uint64_t lookupStartUsecs = statStartUsecs();
        const SubgraphScheduleArtifactCoarseKey coarseKey{artifactKey.m_domainShape,
                                                          artifactKey.m_modp, artifactKey.m_phase};
        SubgraphScheduleArtifactReuse reuse;
        if (state.hasReusableSubgraphScheduleArtifactCoarse(coarseKey)) {
            ensureLogicShape();
            reuse = state.findReusableSubgraphScheduleArtifact(artifactKey, subgraphLogic,
                                                               sharedContext);
        } else {
            ensureLogicShape();
            state.noteSubgraphScheduleArtifactCoarseMiss(artifactKey);
        }
        addElapsedUsecs(state.m_stats.m_timeLookupArtifactsUsecs, lookupStartUsecs);
        SubgraphScheduleArtifact* const artifactp = reuse.m_artifactp;
        if (artifactp) {
            AstCFunc* callFuncp = nullptr;
            bool sharedCall = false;
            bool identityVarMap = true;
            for (const auto& pair : reuse.m_remap.m_templateVarMap) {
                if (pair.first == pair.second) continue;
                identityVarMap = false;
                break;
            }
            AstSenTree* sharedTriggerDomainp = nullptr;
            AstCFunc* sharedFuncp = nullptr;
            if (!(artifactp->m_scopep == group.m_scopep && identityVarMap)
                && SubgraphLoweringState::canUseSharedHelper(artifactp, sharedContext)
                && SubgraphLoweringState::sharedHelperCoversVarMap(
                    *artifactp, reuse.m_remap.m_templateVarMap)) {
                sharedFuncp = SubgraphLoweringState::sharedHelperCallFunc(
                    *artifactp, group.m_senTreep, sharedTriggerDomainp);
            }
            if (artifactp->m_scopep == group.m_scopep && identityVarMap) {
                callFuncp = artifactp->m_callFuncp;
            } else if (sharedFuncp) {
                callFuncp = sharedFuncp;
                sharedCall = sharedFuncp->scopep() != group.m_scopep;
                ++state.m_stats.m_artifactReuseSharedCalls;
            } else if (artifactp->m_scopep == group.m_scopep) {
                callFuncp = SubgraphLoweringState::cloneOrderedFuncGraph(
                    artifactp->m_callFuncp, group.m_scopep, reuse.m_remap.m_templateVarMap, {},
                    state.m_stats, group.m_senTreep);
                if (callFuncp) {
                    ++state.m_stats.m_artifactReuseScopeClones;
                    ++state.m_stats.m_orderedFuncClones;
                }
            } else {
                state.noteSharedReuseSkip(
                    SubgraphLoweringState::classifySharedHelperUse(artifactp, sharedContext));
                const auto cloneIt = artifactp->m_scopeCloneFuncps.find(group.m_scopep);
                if (cloneIt != artifactp->m_scopeCloneFuncps.end()) {
                    callFuncp = cloneIt->second;
                    ++state.m_stats.m_artifactReuseScopeCloneHits;
                } else {
                    callFuncp = SubgraphLoweringState::cloneOrderedFuncGraph(
                        artifactp->m_callFuncp, group.m_scopep, reuse.m_remap.m_templateVarMap, {},
                        state.m_stats);
                    if (callFuncp) {
                        artifactp->m_scopeCloneFuncps.emplace(group.m_scopep, callFuncp);
                        ++state.m_stats.m_artifactReuseScopeClones;
                        ++state.m_stats.m_orderedFuncClones;
                    }
                }
            }
            plan.m_instance.m_callFuncp = callFuncp;
            plan.m_instance.m_scopep = group.m_scopep;
            plan.m_instance.m_sharedCall = sharedCall;
            plan.m_instance.m_triggerDomainp = sharedTriggerDomainp;
            if (callFuncp
                && state.populateSharedHelperArgs(plan.m_instance, *artifactp, group.m_scopep,
                                                  reuse.m_remap.m_templateVarMap, state.m_stats)
                && SubgraphLoweringState::sharedHelperConstantsMatch(*artifactp, subgraphLogic,
                                                                     false)) {
                plan.m_artifactp = artifactp;
                if (tailFuncps) {
                    for (AstCFunc* const tailFuncp : *tailFuncps) {
                        plan.m_instance.m_tailFuncps.push_back(tailFuncp);
                    }
                }
                if (sharedCall) {
                    populateSubgraphScheduleInstanceContract(plan.m_instance, state,
                                                             subgraphLogic);
                } else {
                    populateSubgraphScheduleInstanceContract(plan.m_instance, state);
                }
                state.discardLogic(subgraphLogic);
                plan.m_phase = phase;
                plan.m_wrapper = wrapper;
                ++state.m_stats.m_artifactReuses;
                if (tailFuncps) ++state.m_stats.m_artifactTailReuses;
                ++state.m_stats.m_logicSigBuildsAvoided;
                ++state.m_stats.m_schedulePlans;
                return plan;
            }
            artifactp->m_cloneable = false;
            artifactp->m_uncloneableReason = SubgraphArtifactUncloneableReason::CLONE_FAIL;
            ++state.m_stats.m_artifactReuseCloneFails;
            if (tailFuncps) ++state.m_stats.m_artifactTailCloneFails;
        }
    }
    AstCFunc* funcp = nullptr;
    SubgraphTriggeredRefInfo originalTriggeredInfo;
    SubgraphTriggeredRefInfo scheduledTriggeredInfo;
    AstCFunc* stlTailFuncp = nullptr;
    std::vector<SubgraphSharedHelperArg> helperArgs;
    SubgraphOrderCacheEntry* matchedOrderCacheEntryp = nullptr;
    std::unordered_map<const AstConst*, const AstConst*> orderCacheTemplateConstMap;
    std::unordered_map<const AstVarScope*, AstVarScope*> orderCacheTemplateVarMap;
    bool orderCacheConstantsDiffer = false;
    bool orderCacheDomainsDiffer = false;
    const auto replayCachedOrder = [&](const SubgraphOrderCacheEntry& cacheEntry) {
        if (!cacheEntry.m_recipep || !cacheEntry.m_artifactp) return false;
        const std::string replayTag = tag + "_subgraph_recipe_" + cvtToStr(subgraphIndex++);
        const uint64_t replayStartUsecs = statStartUsecs();
        AstCFunc* const replayedFuncp = V3Order::replay(
            netlistp, {&subgraphLogic}, *cacheEntry.m_recipep, replayTag, slow, group.m_scopep);
        addElapsedUsecs(state.m_stats.m_timeRecipeReplayUsecs, replayStartUsecs);
        if (!replayedFuncp) return false;
        if (tailFuncps) {
            for (AstCFunc* const tailFuncp : *tailFuncps) {
                plan.m_instance.m_tailFuncps.push_back(tailFuncp);
            }
        }
        plan.m_artifactp = cacheEntry.m_artifactp;
        plan.m_instance.m_callFuncp = replayedFuncp;
        plan.m_instance.m_scopep = group.m_scopep;
        populateSubgraphScheduleInstanceContract(plan.m_instance, state);
        plan.m_phase = phase;
        plan.m_wrapper = wrapper;
        ++state.m_stats.m_logicSigBuildsAvoided;
        ++state.m_stats.m_orderCacheHits;
        ++state.m_stats.m_orderCacheRecipeHits;
        ++state.m_stats.m_orderCacheRecipeReplays;
        ++state.m_stats.m_schedulePlans;
        if (tailFuncps) ++state.m_stats.m_artifactTailReuses;
        return true;
    };
    if (canShare) {
        ensureLogicShape();
        ++state.m_stats.m_orderCacheLookups;
        const auto cacheIt = state.m_subgraphOrderCache.find(cacheKey);
        if (cacheIt != state.m_subgraphOrderCache.end()) {
            ++state.m_stats.m_orderCacheEntryHits;
            SubgraphOrderCacheBucket& bucket = cacheIt->second;
            state.m_stats.m_orderCacheVariantCandidates += bucket.m_entries.size();
            const auto tryCandidate = [&](SubgraphOrderCacheEntry& cacheEntry) {
                orderCacheConstantsDiffer = false;
                orderCacheDomainsDiffer = false;
                orderCacheTemplateConstMap.clear();
                orderCacheTemplateVarMap.clear();
                const uint64_t templateMapStartUsecs = statStartUsecs();
                const SubgraphTemplateMapFailReason templateMapFail
                    = SubgraphLoweringState::buildTemplateVarScopeMap(
                        cacheEntry.m_logicSig, subgraphLogic, orderCacheTemplateVarMap,
                        &orderCacheTemplateConstMap, &orderCacheConstantsDiffer);
                addElapsedUsecs(state.m_stats.m_timeTemplateMapUsecs, templateMapStartUsecs);
                if (templateMapFail == SubgraphTemplateMapFailReason::NONE) {
                    orderCacheDomainsDiffer
                        = cacheEntry.m_exactDomainShape != domainShapes.m_exact;
                    matchedOrderCacheEntryp = &cacheEntry;
                    return true;
                }
                state.m_stats.noteOrderCacheTemplateMapFail(templateMapFail);
                return false;
            };
            if (bucket.m_recipeIndex < bucket.m_entries.size()) {
                ++state.m_stats.m_orderCacheDirectIndexLookups;
                if (tryCandidate(bucket.m_entries[bucket.m_recipeIndex])) {
                    ++state.m_stats.m_orderCacheDirectIndexHits;
                } else {
                    ++state.m_stats.m_orderCacheDirectIndexFallbacks;
                }
            }
            if (!matchedOrderCacheEntryp) {
                for (size_t index = 0; index < bucket.m_entries.size(); ++index) {
                    if (index == bucket.m_recipeIndex) continue;
                    if (tryCandidate(bucket.m_entries[index])) break;
                }
            }
        } else {
            state.noteOrderCacheNoEntry(cacheKey);
        }
        if (matchedOrderCacheEntryp) {
            SubgraphOrderCacheEntry& cacheEntry = *matchedOrderCacheEntryp;
            bool orderCacheSharedHelperCovers = false;
            SubgraphSharedHelperRemapVariant* orderCacheRemapVariantp = nullptr;
            if (cacheEntry.m_artifactp
                && SubgraphLoweringState::canUseSharedHelper(cacheEntry.m_artifactp,
                                                             sharedContext)) {
                orderCacheSharedHelperCovers = SubgraphLoweringState::sharedHelperCoversVarMap(
                    *cacheEntry.m_artifactp, orderCacheTemplateVarMap);
                if (!orderCacheSharedHelperCovers) {
                    orderCacheRemapVariantp
                        = SubgraphLoweringState::findOrMakeSharedHelperRemapVariant(
                            *cacheEntry.m_artifactp, orderCacheTemplateVarMap,
                            orderCacheTemplateConstMap, state.m_stats);
                }
            }
            const auto populateOrderCacheSharedHelper = [&](bool requireExactConstants) {
                SubgraphScheduleArtifact& artifact = *cacheEntry.m_artifactp;
                AstSenTree* sharedTriggerDomainp = nullptr;
                AstCFunc* const sharedFuncp
                    = orderCacheRemapVariantp
                          ? SubgraphLoweringState::sharedHelperCallFunc(
                                orderCacheRemapVariantp->m_callFuncp, artifact.m_hasTriggered,
                                group.m_senTreep, sharedTriggerDomainp)
                          : SubgraphLoweringState::sharedHelperCallFunc(artifact, group.m_senTreep,
                                                                        sharedTriggerDomainp);
                plan.m_artifactp = &artifact;
                plan.m_instance.m_callFuncp = sharedFuncp;
                plan.m_instance.m_scopep = group.m_scopep;
                plan.m_instance.m_sharedCall
                    = sharedFuncp && sharedFuncp->scopep() != group.m_scopep;
                plan.m_instance.m_triggerDomainp = sharedTriggerDomainp;
                if (!sharedFuncp) return SubgraphSharedHelperApplyFailReason::CALL_FUNCTION;
                if (orderCacheRemapVariantp) {
                    if (!state.populateSharedHelperArgs(
                            plan.m_instance, artifact, orderCacheRemapVariantp->m_helperArgs,
                            group.m_scopep, orderCacheTemplateVarMap, state.m_stats)) {
                        return SubgraphSharedHelperApplyFailReason::ARGUMENTS;
                    }
                    return SubgraphSharedHelperApplyFailReason::NONE;
                }
                return state.populateSharedHelperInstance(
                    plan.m_instance, artifact, group.m_scopep, orderCacheTemplateVarMap,
                    subgraphLogic, requireExactConstants, state.m_stats);
            };
            if (orderCacheConstantsDiffer || orderCacheDomainsDiffer) {
                if (cacheEntry.m_artifactp
                    && SubgraphLoweringState::canUseSharedHelper(cacheEntry.m_artifactp,
                                                                 sharedContext)) {
                    SubgraphScheduleArtifact& artifact = *cacheEntry.m_artifactp;
                    if (orderCacheSharedHelperCovers || orderCacheRemapVariantp) {
                        if (tailFuncps) {
                            for (AstCFunc* const tailFuncp : *tailFuncps) {
                                plan.m_instance.m_tailFuncps.push_back(tailFuncp);
                            }
                        }
                        AstSenTree* sharedTriggerDomainp = nullptr;
                        AstCFunc* const sharedFuncp
                            = orderCacheRemapVariantp
                                  ? SubgraphLoweringState::sharedHelperCallFunc(
                                        orderCacheRemapVariantp->m_callFuncp,
                                        artifact.m_hasTriggered, group.m_senTreep,
                                        sharedTriggerDomainp)
                                  : SubgraphLoweringState::sharedHelperCallFunc(
                                        artifact, group.m_senTreep, sharedTriggerDomainp);
                        plan.m_artifactp = &artifact;
                        plan.m_instance.m_callFuncp = sharedFuncp;
                        plan.m_instance.m_scopep = group.m_scopep;
                        plan.m_instance.m_sharedCall
                            = sharedFuncp && sharedFuncp->scopep() != group.m_scopep;
                        plan.m_instance.m_triggerDomainp = sharedTriggerDomainp;
                        SubgraphSharedHelperApplyFailReason applyFail
                            = SubgraphSharedHelperApplyFailReason::NONE;
                        if (!sharedFuncp) {
                            applyFail = SubgraphSharedHelperApplyFailReason::CALL_FUNCTION;
                        } else if (orderCacheRemapVariantp) {
                            if (!state.populateSharedHelperArgs(
                                    plan.m_instance, artifact,
                                    orderCacheRemapVariantp->m_helperArgs, group.m_scopep,
                                    orderCacheTemplateVarMap, state.m_stats)) {
                                applyFail = SubgraphSharedHelperApplyFailReason::ARGUMENTS;
                            }
                        } else {
                            applyFail = state.populateSharedHelperInstance(
                                plan.m_instance, artifact, group.m_scopep,
                                orderCacheTemplateVarMap, subgraphLogic, true, state.m_stats);
                        }
                        if (applyFail == SubgraphSharedHelperApplyFailReason::NONE) {
                            populateSubgraphScheduleInstanceContract(plan.m_instance, state,
                                                                     subgraphLogic);
                            state.discardLogic(subgraphLogic);
                            plan.m_phase = phase;
                            plan.m_wrapper = wrapper;
                            ++state.m_stats.m_logicSigBuildsAvoided;
                            ++state.m_stats.m_orderCacheHits;
                            ++state.m_stats.m_orderCacheRecipeHits;
                            ++state.m_stats.m_orderCacheRecipeSharedHits;
                            ++state.m_stats.m_orderCacheSharedHits;
                            ++state.m_stats.m_schedulePlans;
                            if (tailFuncps) ++state.m_stats.m_artifactTailReuses;
                            return plan;
                        }
                        state.noteOrderCacheSharedSkip(applyFail);
                        plan = SubgraphSchedulePlan{};
                    } else {
                        ++state.m_stats.m_orderCacheSharedSkipVarMap;
                    }
                }
                if (tag != "stl" && replayCachedOrder(cacheEntry)) return plan;
                if (cacheEntry.m_cloneable && cacheEntry.m_artifactp) {
                    AstCFunc* const clonedFuncp = SubgraphLoweringState::cloneOrderedFuncGraph(
                        cacheEntry.m_funcp, group.m_scopep, orderCacheTemplateVarMap,
                        orderCacheTemplateConstMap, state.m_stats, group.m_senTreep);
                    if (clonedFuncp) {
                        if (tailFuncps) {
                            for (AstCFunc* const tailFuncp : *tailFuncps) {
                                plan.m_instance.m_tailFuncps.push_back(tailFuncp);
                            }
                        }
                        plan.m_artifactp = cacheEntry.m_artifactp;
                        plan.m_instance.m_callFuncp = clonedFuncp;
                        plan.m_instance.m_scopep = group.m_scopep;
                        const bool populatedArgs = state.populateSharedHelperArgs(
                            plan.m_instance, *cacheEntry.m_artifactp, group.m_scopep,
                            orderCacheTemplateVarMap, state.m_stats);
                        const bool populatedConstants
                            = populatedArgs
                              && SubgraphLoweringState::sharedHelperConstantsMatch(
                                  *cacheEntry.m_artifactp, subgraphLogic, false);
                        if (populatedConstants) {
                            populateSubgraphScheduleInstanceContract(plan.m_instance, state);
                            state.discardLogic(subgraphLogic);
                            plan.m_phase = phase;
                            plan.m_wrapper = wrapper;
                            ++state.m_stats.m_logicSigBuildsAvoided;
                            ++state.m_stats.m_orderCacheHits;
                            ++state.m_stats.m_orderCacheRecipeClones;
                            ++state.m_stats.m_orderCacheRecipeHits;
                            ++state.m_stats.m_orderedFuncClones;
                            ++state.m_stats.m_schedulePlans;
                            if (tailFuncps) ++state.m_stats.m_artifactTailReuses;
                            return plan;
                        }
                        if (populatedArgs) {
                            ++state.m_stats.m_orderCacheCloneApplyFailConstants;
                        } else {
                            ++state.m_stats.m_orderCacheCloneApplyFailArguments;
                        }
                        plan = SubgraphSchedulePlan{};
                    } else {
                        ++state.m_stats.m_orderCacheCloneNull;
                    }
                }
                matchedOrderCacheEntryp = nullptr;
            } else if (!cacheEntry.m_cloneable && !cacheEntry.m_triggeredShareable) {
                ++state.m_stats.m_orderCacheSkipTriggered;
                ++state.m_stats.m_orderCacheSkipTriggeredNotShareable;
                if (cacheEntry.m_triggeredWritesInstanceLocal) {
                    ++state.m_stats.m_orderCacheSkipTriggeredInstanceLocal;
                }
            } else {
                if (!cacheEntry.m_cloneable && cacheEntry.m_triggeredShareable) {
                    if (tag == "stl") {
                        ++state.m_stats.m_orderCacheSkipTriggered;
                        ++state.m_stats.m_orderCacheSkipTriggeredStl;
                    } else if (!cacheEntry.m_artifactp) {
                        ++state.m_stats.m_orderCacheSkipTriggered;
                        ++state.m_stats.m_orderCacheSkipTriggeredNoArtifact;
                    } else {
                        SubgraphSharedHelperSkipReason sharedSkipReason
                            = SubgraphLoweringState::classifySharedHelperUse(
                                cacheEntry.m_artifactp, sharedContext);
                        if (sharedSkipReason == SubgraphSharedHelperSkipReason::NONE
                            && !orderCacheSharedHelperCovers && !orderCacheRemapVariantp) {
                            ++state.m_stats.m_orderCacheSharedSkipVarMap;
                            ++state.m_stats.m_orderCacheSkipTriggered;
                            matchedOrderCacheEntryp = nullptr;
                        }
                        if (matchedOrderCacheEntryp
                            && sharedSkipReason != SubgraphSharedHelperSkipReason::NONE) {
                            ++state.m_stats.m_orderCacheSkipTriggered;
                            state.noteOrderCacheSharedSkip(sharedSkipReason);
                        } else if (matchedOrderCacheEntryp) {
                            if (tailFuncps) {
                                for (AstCFunc* const tailFuncp : *tailFuncps) {
                                    plan.m_instance.m_tailFuncps.push_back(tailFuncp);
                                }
                            }
                            const SubgraphSharedHelperApplyFailReason applyFail
                                = populateOrderCacheSharedHelper(false);
                            if (applyFail == SubgraphSharedHelperApplyFailReason::NONE) {
                                populateSubgraphScheduleInstanceContract(plan.m_instance, state,
                                                                         subgraphLogic);
                                state.discardLogic(subgraphLogic);
                                plan.m_phase = phase;
                                plan.m_wrapper = wrapper;
                                ++state.m_stats.m_orderCacheHits;
                                ++state.m_stats.m_orderCacheSharedHits;
                                ++state.m_stats.m_logicSigBuildsAvoided;
                                ++state.m_stats.m_schedulePlans;
                                if (tailFuncps) ++state.m_stats.m_artifactTailReuses;
                                return plan;
                            } else {
                                state.noteOrderCacheSharedSkip(applyFail);
                                plan = SubgraphSchedulePlan{};
                            }
                        }
                    }
                } else if (cacheEntry.m_artifactp
                           && SubgraphLoweringState::canUseSharedHelper(cacheEntry.m_artifactp,
                                                                        sharedContext)
                           && (orderCacheSharedHelperCovers || orderCacheRemapVariantp)) {
                    if (tailFuncps) {
                        for (AstCFunc* const tailFuncp : *tailFuncps) {
                            plan.m_instance.m_tailFuncps.push_back(tailFuncp);
                        }
                    }
                    const SubgraphSharedHelperApplyFailReason applyFail
                        = populateOrderCacheSharedHelper(false);
                    if (applyFail == SubgraphSharedHelperApplyFailReason::NONE) {
                        populateSubgraphScheduleInstanceContract(plan.m_instance, state,
                                                                 subgraphLogic);
                        state.discardLogic(subgraphLogic);
                        plan.m_phase = phase;
                        plan.m_wrapper = wrapper;
                        ++state.m_stats.m_orderCacheHits;
                        ++state.m_stats.m_orderCacheSharedHits;
                        ++state.m_stats.m_logicSigBuildsAvoided;
                        ++state.m_stats.m_schedulePlans;
                        if (tailFuncps) ++state.m_stats.m_artifactTailReuses;
                        return plan;
                    } else {
                        state.noteOrderCacheSharedSkip(applyFail);
                        plan = SubgraphSchedulePlan{};
                        if (tag != "stl" && replayCachedOrder(cacheEntry)) return plan;
                    }
                } else if (cacheEntry.m_cloneable && cacheEntry.m_artifactp) {
                    if (SubgraphLoweringState::canUseSharedHelper(cacheEntry.m_artifactp,
                                                                  sharedContext)
                        && !orderCacheSharedHelperCovers && !orderCacheRemapVariantp) {
                        ++state.m_stats.m_orderCacheSharedSkipVarMap;
                    }
                    if (tag != "stl" && replayCachedOrder(cacheEntry)) return plan;
                    AstCFunc* const clonedFuncp = SubgraphLoweringState::cloneOrderedFuncGraph(
                        cacheEntry.m_funcp, group.m_scopep, orderCacheTemplateVarMap, {},
                        state.m_stats);
                    if (clonedFuncp) {
                        if (tailFuncps) {
                            for (AstCFunc* const tailFuncp : *tailFuncps) {
                                plan.m_instance.m_tailFuncps.push_back(tailFuncp);
                            }
                        }
                        plan.m_artifactp = cacheEntry.m_artifactp;
                        plan.m_instance.m_callFuncp = clonedFuncp;
                        plan.m_instance.m_scopep = group.m_scopep;
                        const bool populatedArgs = state.populateSharedHelperArgs(
                            plan.m_instance, *cacheEntry.m_artifactp, group.m_scopep,
                            orderCacheTemplateVarMap, state.m_stats);
                        const bool populatedConstants
                            = populatedArgs
                              && SubgraphLoweringState::sharedHelperConstantsMatch(
                                  *cacheEntry.m_artifactp, subgraphLogic, false);
                        if (populatedConstants) {
                            populateSubgraphScheduleInstanceContract(plan.m_instance, state);
                            state.discardLogic(subgraphLogic);
                            plan.m_phase = phase;
                            plan.m_wrapper = wrapper;
                            ++state.m_stats.m_orderCacheHits;
                            ++state.m_stats.m_orderedFuncClones;
                            ++state.m_stats.m_logicSigBuildsAvoided;
                            ++state.m_stats.m_schedulePlans;
                            if (tailFuncps) ++state.m_stats.m_artifactTailReuses;
                            return plan;
                        }
                        if (populatedArgs) {
                            ++state.m_stats.m_orderCacheCloneApplyFailConstants;
                        } else {
                            ++state.m_stats.m_orderCacheCloneApplyFailArguments;
                        }
                        plan = SubgraphSchedulePlan{};
                    } else {
                        ++state.m_stats.m_orderCacheCloneNull;
                    }
                }
            }
        }
    }
    if (!funcp) {
        if (canShare) ++state.m_stats.m_orderCacheMisses;
        if (canShare) {
            const uint64_t logicSigStartUsecs = statStartUsecs();
            logicSig = SubgraphLoweringState::buildLogicSig(subgraphLogic);
            addElapsedUsecs(state.m_stats.m_timeBuildLogicSigUsecs, logicSigStartUsecs);
            ++state.m_stats.m_logicSigBuilds;
        }
        const std::string orderTag = tag + "_subgraph_" + cvtToStr(subgraphIndex++);
        const uint64_t orderStartUsecs = statStartUsecs();
        std::shared_ptr<const V3Order::OrderRecipe> orderRecipep;
        funcp = V3Order::order(netlistp, {&subgraphLogic}, trigToSen, orderTag, false, slow,
                               externalDomains, group.m_scopep, &orderRecipep);
        const uint64_t orderUsecs = orderStartUsecs ? V3Os::timeUsecs() - orderStartUsecs : 0;
        state.m_stats.m_timeInternalOrderUsecs += orderUsecs;
        state.m_stats.noteInternalOrder(orderTag, orderUsecs, cacheKey.m_modp, phase, wrapper,
                                        cacheKey, logicSig);
        if (funcp) {
            const uint64_t splitStartUsecs = statStartUsecs();
            util::splitCheck(funcp);
            addElapsedUsecs(state.m_stats.m_timeSplitOrderedFuncsUsecs, splitStartUsecs);
            AstCFunc* scheduledFuncp = funcp;
            if (tag == "stl") {
                originalTriggeredInfo = SubgraphLoweringState::analyzeOrderedFuncTriggeredRefs(
                    funcp, group.m_scopep, state.m_stats);
                stlTailFuncp = cloneUnguardedFuncBody(funcp, group.m_scopep, "__tail", slow);
                scheduledFuncp = stlTailFuncp;
            }
            scheduledTriggeredInfo = SubgraphLoweringState::analyzeOrderedFuncTriggeredRefs(
                scheduledFuncp, group.m_scopep, state.m_stats);
            if (tag != "stl") originalTriggeredInfo = scheduledTriggeredInfo;
            const bool parameterizeInstanceLocal = scheduledTriggeredInfo.m_hasTriggered
                                                   && scheduledTriggeredInfo.m_shareable
                                                   && scheduledTriggeredInfo.m_writesInstanceLocal;
            if (cacheableArtifact && tag == "stl"
                && SubgraphLoweringState::sharedHelperNeedsArguments(
                    scheduledFuncp, group.m_scopep, logicSig, parameterizeInstanceLocal)) {
                cacheableArtifact = false;
                ++state.m_stats.m_sharedHelperStlArgumentSkips;
            }
            if (cacheableArtifact) {
                const uint64_t parameterizeStartUsecs = statStartUsecs();
                const bool parameterized = SubgraphLoweringState::parameterizeSharedHelper(
                    scheduledFuncp, group.m_scopep, logicSig, parameterizeInstanceLocal,
                    helperArgs, state.m_stats);
                addElapsedUsecs(state.m_stats.m_timeParameterizeHelpersUsecs,
                                parameterizeStartUsecs);
                if (!parameterized) cacheableArtifact = false;
            }
            if (cacheableArtifact && !matchedOrderCacheEntryp) {
                const SubgraphTriggeredRefInfo triggeredInfo
                    = tag == "stl" ? scheduledTriggeredInfo
                                   : SubgraphLoweringState::analyzeOrderedFuncTriggeredRefs(
                                         scheduledFuncp, group.m_scopep, state.m_stats, false);
                const bool triggeredShareable
                    = SubgraphLoweringState::canShareTriggeredArtifact(triggeredInfo);
                const bool triggeredCloneable
                    = SubgraphLoweringState::canCloneTriggeredOrderCacheEntry(triggeredInfo);
                SubgraphOrderCacheBucket& bucket = state.m_subgraphOrderCache[cacheKey];
                if (bucket.m_entries.empty()) ++state.m_stats.m_orderCacheVariantBuckets;
                bucket.m_entries.push_back(SubgraphOrderCacheEntry{
                    nullptr, domainShapes.m_exact, scheduledFuncp, logicSig, orderRecipep,
                    triggeredCloneable, triggeredShareable, triggeredInfo.m_writesDelayedShadow,
                    triggeredInfo.m_writesInstanceLocal, triggeredInfo.m_writesLocalTemp,
                    triggeredInfo.m_writesNonLocal, triggeredInfo.m_writesTriggerTemp,
                    triggeredInfo.m_writesVlemTemp});
                if (orderRecipep && bucket.m_recipeIndex == std::numeric_limits<size_t>::max()) {
                    bucket.m_recipeIndex = bucket.m_entries.size() - 1;
                }
                insertedOrderCacheEntryp = &bucket.m_entries.back();
                ++state.m_stats.m_orderCacheEntries;
                state.m_stats.m_orderCacheVariantMax = std::max<uint64_t>(
                    state.m_stats.m_orderCacheVariantMax, bucket.m_entries.size());
            }
        }
    }
    if (!funcp) return plan;

    AstCFunc* const callFuncp = stlTailFuncp ? stlTailFuncp : funcp;
    const SubgraphTriggeredRefInfo triggeredInfo
        = tag == "stl" ? scheduledTriggeredInfo
                       : SubgraphLoweringState::analyzeOrderedFuncTriggeredRefs(
                             callFuncp, group.m_scopep, state.m_stats, false);
    const bool cloneableArtifact
        = SubgraphLoweringState::canCloneTriggeredOrderCacheEntry(triggeredInfo);
    const bool triggeredShareableArtifact
        = SubgraphLoweringState::canShareTriggeredArtifact(triggeredInfo);
    if (triggeredInfo.m_hasTriggered) {
        ++state.m_stats.m_triggeredArtifactCandidates;
        if (!triggeredInfo.m_shareable) ++state.m_stats.m_triggeredArtifactUnshareable;
        if (originalTriggeredInfo.m_writesInstanceLocal) {
            ++state.m_stats.m_triggeredArtifactWritesInstanceLocal;
        }
        if (originalTriggeredInfo.m_writesDelayedShadow) {
            ++state.m_stats.m_triggeredArtifactWritesDelayedShadow;
        }
        if (originalTriggeredInfo.m_writesLocalTemp) {
            ++state.m_stats.m_triggeredArtifactWritesLocalTemp;
        }
        if (triggeredInfo.m_writesNonLocal) ++state.m_stats.m_triggeredArtifactWritesNonLocal;
        if (originalTriggeredInfo.m_writesTriggerTemp) {
            ++state.m_stats.m_triggeredArtifactWritesTriggerTemp;
        }
        if (originalTriggeredInfo.m_writesVlemTemp) {
            ++state.m_stats.m_triggeredArtifactWritesVlemTemp;
        }
        if (!triggeredInfo.m_writesNonLocal) {
            ++state.m_stats.m_triggeredArtifactNoNonLocalWrites;
            if (triggeredInfo.m_writesInstanceLocal) {
                ++state.m_stats.m_triggeredArtifactNoNonLocalInstanceLocalWrites;
            }
        }
        if (bundleContext.m_currentScopeTailContract.m_writesBoundaryInput) {
            ++state.m_stats.m_triggeredArtifactInputTailWrites;
            if (triggeredShareableArtifact) {
                ++state.m_stats.m_triggeredArtifactInputTailShareable;
            }
        }
        if (triggeredShareableArtifact) { ++state.m_stats.m_triggeredArtifactShareable; }
    }
    if (tailFuncps) {
        for (AstCFunc* const tailFuncp : *tailFuncps) {
            plan.m_instance.m_tailFuncps.push_back(tailFuncp);
        }
    }
    plan.m_artifactp = state.makeSubgraphScheduleArtifact(
        artifactKey, group.m_scopep, callFuncp, std::move(helperArgs), std::move(logicSig),
        bundleContext.m_currentScopeTailContract, cloneableArtifact, triggeredInfo.m_hasTriggered,
        triggeredShareableArtifact, cacheableArtifact);
    if (insertedOrderCacheEntryp) insertedOrderCacheEntryp->m_artifactp = plan.m_artifactp;
    plan.m_instance.m_callFuncp = callFuncp;
    plan.m_instance.m_scopep = group.m_scopep;
    state.populateSharedHelperArgs(plan.m_instance, *plan.m_artifactp, group.m_scopep, {},
                                   state.m_stats);
    populateSubgraphScheduleInstanceContract(plan.m_instance, state);
    plan.m_phase = phase;
    plan.m_wrapper = wrapper;
    ++state.m_stats.m_schedulePlans;
    return plan;
}

void finalizeStlSchedulePlan(SubgraphSchedulePlan& plan, SubgraphLoweringState& state, bool slow) {
    if (!plan.m_artifactp) return;
    const SubgraphScheduleInstance& instance = plan.m_instance;
    AstCFunc* const wrapperp = state.makeStlTailWrapper(instance, slow);
    state.m_stlSubgraphFuncs[instance.m_scopep].push_back(wrapperp);
}

void appendSubgraphScheduleBundlePlan(
    SubgraphScheduleBundle& bundle, AstNetlist* netlistp, LogicByScope& subgraphLogic,
    const SubgraphWrapper& wrapper, bool isEarly, const std::vector<AstCFunc*>* tailFuncps,
    const SubgraphGroup& group, const SubgraphScheduleBundleContext& bundleContext,
    SubgraphLoweringState& state, const V3Order::TrigToSenMap& trigToSen, const std::string& tag,
    bool slow, const V3Order::ExternalDomainsProvider& externalDomains, unsigned& subgraphIndex) {
    if (subgraphLogic.empty()) return;
    const uint64_t buildPlanStartUsecs = statStartUsecs();
    SubgraphSchedulePlan plan = buildSubgraphSchedulePlan(
        netlistp, subgraphLogic, wrapper, isEarly, tailFuncps, group, bundleContext, state,
        trigToSen, tag, slow, externalDomains, subgraphIndex);
    if (tag == "stl") finalizeStlSchedulePlan(plan, state, slow);
    addElapsedUsecs(state.m_stats.m_timeBuildPlansUsecs, buildPlanStartUsecs);
    if (plan.m_artifactp) {
        bundle.m_plans.push_back(std::move(plan));
        ++state.m_stats.m_bundlePlans;
    }
}

AstSubgraphInstance* materializeSubgraphSchedulePlan(
    const SubgraphGroup& group, const SubgraphSchedulePlan& plan, SubgraphLoweringState& state,
    AstActive* subgraphActivep,
    std::unordered_map<SubgraphBatchKey, AstSubgraphInstance*, SubgraphBatchKeyHash>& batches) {
    if (!plan.m_artifactp) return nullptr;
    const SubgraphScheduleInstance& instance = plan.m_instance;
    AstCCall* const callp = new AstCCall{instance.m_callFuncp->fileline(), instance.m_callFuncp};
    if (instance.m_sharedCall) {
        callp->selfPointer(
            VSelfPointerText{VSelfPointerText::VlSyms{}, instance.m_scopep->nameDotless()});
    }
    for (const SubgraphSharedHelperArg& arg : instance.m_helperArgs) {
        callp->addArgsp(new AstVarRef{callp->fileline(), arg.m_vscp,
                                      SubgraphLoweringState::sharedHelperArgAccess(arg)});
    }
    state.m_stats.m_sharedHelperCallArgs += instance.m_helperArgs.size();
    state.m_stats.m_sharedHelperCallArgsMax
        = std::max(state.m_stats.m_sharedHelperCallArgsMax,
                   static_cast<uint64_t>(instance.m_helperArgs.size()));
    callp->dtypeSetVoid();
    AstNodeStmt* stmtsp = callp->makeStmt();
    if (instance.m_triggerDomainp) {
        AstIf* const guardp = util::createIfFromSenTree(instance.m_triggerDomainp);
        guardp->addThensp(stmtsp);
        stmtsp = guardp;
    }
    for (AstCFunc* const tailFuncp : instance.m_tailFuncps) {
        stmtsp->addNext(util::callVoidFunc(tailFuncp));
    }
    AstSubgraphInstance* const subgraphp = getOrCreateSubgraphBatch(
        group, plan.m_wrapper, plan.m_phase == AstSubgraphInstance::Phase::PRE, subgraphActivep,
        batches, state);
    subgraphp->addStmtsp(stmtsp);
    return subgraphp;
}

void materializeSubgraphScheduleBundle(
    const SubgraphGroup& group, const SubgraphScheduleBundle& bundle, SubgraphLoweringState& state,
    AstActive* subgraphActivep,
    std::unordered_map<SubgraphBatchKey, AstSubgraphInstance*, SubgraphBatchKeyHash>& batches) {
    ++state.m_stats.m_bundleMaterialized;
    state.m_stats.m_bundleMaterializedPlans += bundle.m_plans.size();
    const uint64_t materializeStartUsecs = statStartUsecs();
    std::unordered_map<AstSubgraphInstance*, SubgraphInstanceContract> contractBySubgraph;
    for (const SubgraphSchedulePlan& plan : bundle.m_plans) {
        AstSubgraphInstance* const subgraphp
            = materializeSubgraphSchedulePlan(group, plan, state, subgraphActivep, batches);
        if (!subgraphp) continue;
        contractBySubgraph[subgraphp].mergeFrom(plan.m_instance.m_contract);
    }
    for (const auto& pair : contractBySubgraph) {
        populateSubgraphInstanceContract(pair.first, pair.second);
    }
    addElapsedUsecs(state.m_stats.m_timeMaterializeUsecs, materializeStartUsecs);
}

AstVarScope* newSnapshotHelperArg(AstCFunc* funcp, AstNodeDType* dtypep, const std::string& name,
                                  VDirection direction) {
    FileLine* const flp = funcp->fileline();
    AstScope* const scopep = funcp->scopep();
    AstVar* const varp = new AstVar{flp, VVarType::BLOCKTEMP, name, dtypep};
    varp->funcLocal(true);
    varp->direction(direction);
    funcp->addArgsp(varp);
    AstVarScope* const vscp = new AstVarScope{flp, scopep, varp};
    scopep->addVarsp(vscp);
    return vscp;
}

const SnapshotHelperEntry& getOrCreateSnapshotHelper(AstScope* boundaryScopep,
                                                     const std::vector<AstVarScope*>& sourceVscps,
                                                     SubgraphLoweringState& state, bool slow) {
    static unsigned s_snapshotHelperIndex = 0;
    SnapshotHelperKey key;
    key.m_scopep = boundaryScopep;
    key.m_slow = slow;
    key.m_sourceVars = sourceVscps;
    const auto it = state.m_snapshotHelpers.find(key);
    if (it != state.m_snapshotHelpers.end()) {
        ++state.m_stats.m_snapshotHelperReuses;
        return it->second;
    }

    FileLine* const flp = sourceVscps.front()->fileline();
    std::string helperCName = "__VsubgraphSnapshotHelper";
    for (AstVarScope* const sourceVscp : sourceVscps) {
        helperCName += "__" + sourceVscp->varp()->shortName();
    }
    AstCFunc* const funcp = new AstCFunc{
        flp, "__VsubgraphSnapshotHelper__sgclone_" + cvtToStr(s_snapshotHelperIndex++),
        boundaryScopep, ""};
    ++state.m_stats.m_snapshotHelpers;
    funcp->dontCombine(true);
    funcp->isStatic(false);
    funcp->isLoose(true);
    funcp->slow(slow);
    funcp->isConst(false);
    funcp->declPrivate(true);
    funcp->cname(helperCName);
    boundaryScopep->addBlocksp(funcp);

    SnapshotHelperEntry entry;
    entry.m_funcp = funcp;
    for (size_t i = 0; i < sourceVscps.size(); ++i) {
        AstVarScope* const sourceVscp = sourceVscps[i];
        AstVarScope* const outArgVscp = newSnapshotHelperArg(
            funcp, sourceVscp->dtypep(), "out" + cvtToStr(i), VDirection::OUTPUT);
        AstVarScope* const inArgVscp = newSnapshotHelperArg(
            funcp, sourceVscp->dtypep(), "in" + cvtToStr(i), VDirection::CONSTREF);
        funcp->addStmtsp(new AstAssign{flp, new AstVarRef{flp, outArgVscp, VAccess::WRITE},
                                       new AstVarRef{flp, inArgVscp, VAccess::READ}});
    }
    return state.m_snapshotHelpers.emplace(std::move(key), std::move(entry)).first->second;
}

bool lessSnapshotSource(AstVarScope* lhsp, AstVarScope* rhsp) {
    const string lhsScope = lhsp->scopep()->name();
    const string rhsScope = rhsp->scopep()->name();
    if (lhsScope != rhsScope) return lhsScope < rhsScope;
    const string lhsName = lhsp->varp()->name();
    const string rhsName = rhsp->varp()->name();
    if (lhsName != rhsName) return lhsName < rhsName;
    return lhsp < rhsp;
}

void canonicalizeSnapshotBucketSources(SnapshotBucket& bucket) {
    std::sort(bucket.m_sourceVars.begin(), bucket.m_sourceVars.end(), lessSnapshotSource);
}

void registerSnapshotSourceSet(SnapshotBucket& bucket, SubgraphLoweringState& state) {
    SnapshotSourceSetKey key;
    key.m_sourceVars = bucket.m_sourceVars;
    if (state.m_snapshotSourceSets.insert(std::move(key)).second) {
        ++state.m_stats.m_snapshotSourceSets;
    } else {
        ++state.m_stats.m_snapshotSourceSetDuplicates;
    }
}

void emitSnapshotProcedureForBucket(const SnapshotBucket& bucket, SubgraphLoweringState& state,
                                    bool slow) {
    AstScope* const topScopep = v3Global.rootp()->topScopep()->scopep();
    if (bucket.m_sourceVars.empty()) return;
    ++state.m_stats.m_snapshotProcedures;
    FileLine* const flp = bucket.m_sourceVars.front()->fileline();
    AstSubgraphInstance* const snapshotp = new AstSubgraphInstance{flp, topScopep, nullptr};
    snapshotp->keyword(VAlwaysKwd::ALWAYS);
    snapshotp->phase(AstSubgraphInstance::Phase::SNAPSHOT);
    snapshotp->wrapperKind(AstSubgraphInstance::WrapperKind::ALWAYS);
    std::unordered_map<AstScope*, std::vector<AstVarScope*>> localBoundarySources;
    for (AstVarScope* const sourceVscp : bucket.m_sourceVars) {
        if (sourceVscp->scopep()->modp()->subgraphBoundary() && sourceVscp->varp()->isIO()
            && sourceVscp->varp()->direction().isNonOutput()) {
            localBoundarySources[sourceVscp->scopep()].push_back(sourceVscp);
        }
    }
    std::vector<AstScope*> localBoundaryScopes;
    localBoundaryScopes.reserve(localBoundarySources.size());
    for (const auto& pair : localBoundarySources) localBoundaryScopes.push_back(pair.first);
    std::sort(localBoundaryScopes.begin(), localBoundaryScopes.end(),
              [](AstScope* lhsp, AstScope* rhsp) { return lhsp->name() < rhsp->name(); });
    for (AstScope* const boundaryScopep : localBoundaryScopes) {
        const std::vector<AstVarScope*>& sourceVscps = localBoundarySources[boundaryScopep];
        FileLine* const flp = sourceVscps.front()->fileline();
        const SnapshotHelperEntry& helper
            = getOrCreateSnapshotHelper(boundaryScopep, sourceVscps, state, slow);
        AstCCall* const callp = new AstCCall{flp, helper.m_funcp};
        callp->dtypeSetVoid();
        for (size_t i = 0; i < sourceVscps.size(); ++i) {
            AstVarScope* const sourceVscp = sourceVscps[i];
            const SnapshotRef& snapshotRef
                = state.getSnapshotRef(bucket.m_ownerp, bucket.m_senTreep, sourceVscp);
            callp->addArgsp(
                SubgraphLoweringState::makeSnapshotExpr(snapshotRef, flp, VAccess::WRITE));
            callp->addArgsp(new AstVarRef{flp, sourceVscp, VAccess::READ});
        }
        snapshotp->addStmtsp(callp->makeStmt());
    }
    for (AstVarScope* const sourceVscp : bucket.m_sourceVars) {
        if (localBoundarySources.count(sourceVscp->scopep())
            && sourceVscp->scopep()->modp()->subgraphBoundary() && sourceVscp->varp()->isIO()
            && sourceVscp->varp()->direction().isNonOutput()) {
            continue;
        }
        const SnapshotRef& snapshotRef
            = state.getSnapshotRef(bucket.m_ownerp, bucket.m_senTreep, sourceVscp);
        snapshotp->addStmtsp(
            new AstAssign{sourceVscp->fileline(),
                          SubgraphLoweringState::makeSnapshotExpr(
                              snapshotRef, sourceVscp->fileline(), VAccess::WRITE),
                          new AstVarRef{sourceVscp->fileline(), sourceVscp, VAccess::READ}});
    }
    bucket.m_ownerp->add(topScopep, bucket.m_senTreep, snapshotp);
}

void collectRegionWrittenVars(const std::vector<LogicByScope*>& logic,
                              SubgraphLoweringState& state) {
    for (LogicByScope* const lbsp : logic) {
        lbsp->foreachLogic([&](AstNode* logicp) {
            logicp->foreach([&](AstVarRef* refp) {
                if (!refp->access().isWriteOrRW()) return;
                state.m_regionWrittenVars.insert(refp->varScopep());
            });
        });
    }
}

void prepareSubgraphSnapshots(std::vector<SubgraphGroup>& groups, SubgraphLoweringState& state,
                              const std::string& tag) {
    for (SubgraphGroup& group : groups) {
        state.collectCrossBoundaryReadsInLogic(group.m_earlyLogic, group.m_scopep, group.m_ownerp,
                                               group.m_senTreep);
        state.collectCrossBoundaryReadsInLogic(group.m_lateLogic, group.m_scopep, group.m_ownerp,
                                               group.m_senTreep);
        if (tag != "stl" && tag != "ico") {
            auto& stlFuncs = state.m_stlSubgraphFuncs;
            const auto it = stlFuncs.find(group.m_scopep);
            if (it != stlFuncs.end()) {
                for (AstCFunc* const tailFuncp : it->second) {
                    if (!state.tailNeedsNbaClone(tailFuncp, group.m_scopep)
                        || !tailFuncp->stmtsp()) {
                        continue;
                    }
                    state.collectCrossBoundaryReads(tailFuncp->stmtsp(), group.m_scopep,
                                                    group.m_ownerp, group.m_senTreep);
                }
            }
        }
    }
    SnapshotNameAllocator snapshotNames;
    for (SnapshotBucket& bucket : state.m_snapshotBuckets) {
        canonicalizeSnapshotBucketSources(bucket);
        registerSnapshotSourceSet(bucket, state);
        struct SnapshotDTypeGroup final {
            AstScope* m_scopep = nullptr;
            AstNodeDType* m_dtypep = nullptr;
            std::vector<AstVarScope*> m_vars;
        };
        std::vector<SnapshotDTypeGroup> dtypeGroups;
        for (AstVarScope* const sourceVscp : bucket.m_sourceVars) {
            AstNodeDType* const dtypep = sourceVscp->dtypep();
            AstScope* const scopep = sourceVscp->scopep();
            auto it = std::find_if(
                dtypeGroups.begin(), dtypeGroups.end(), [&](const SnapshotDTypeGroup& group) {
                    return group.m_scopep == scopep && group.m_dtypep->similarDType(dtypep);
                });
            if (it == dtypeGroups.end()) {
                dtypeGroups.push_back(SnapshotDTypeGroup{scopep, dtypep, {sourceVscp}});
            } else {
                it->m_vars.push_back(sourceVscp);
            }
        }
        unsigned bundleIndex = 0;
        for (const SnapshotDTypeGroup& group : dtypeGroups) {
            const std::vector<AstVarScope*>& groupedVars = group.m_vars;
            if (groupedVars.size() == 1) {
                AstVarScope* const sourceVscp = groupedVars.front();
                const string baseName = "__VsubgraphSnapshot__"
                                        + sourceVscp->scopep()->nameDotless() + "__"
                                        + sourceVscp->varp()->shortName();
                const string name = snapshotNames.get(sourceVscp->scopep(), baseName);
                AstVarScope* const snapshotp
                    = sourceVscp->scopep()->createTempLike(name, sourceVscp);
                bucket.m_snapshotRefs.emplace(sourceVscp, SnapshotRef{snapshotp, 0, false});
                ++state.m_stats.m_snapshotScalars;
                continue;
            }

            FileLine* const flp = groupedVars.front()->fileline();
            AstRange* const rangep
                = new AstRange{flp, static_cast<int>(groupedVars.size() - 1), 0};
            AstNodeDType* const bundleDTypep
                = new AstUnpackArrayDType{flp, group.m_dtypep, rangep};
            v3Global.rootp()->typeTablep()->addTypesp(bundleDTypep);
            const string bundleBaseName = "__VsubgraphSnapshot__" + group.m_scopep->nameDotless()
                                          + "__bundle" + cvtToStr(bundleIndex++);
            const string bundleName = snapshotNames.get(group.m_scopep, bundleBaseName);
            AstVarScope* const bundleVscp = group.m_scopep->createTemp(bundleName, bundleDTypep);
            ++state.m_stats.m_snapshotBundles;
            state.m_stats.m_snapshotBundleElems += groupedVars.size();
            for (uint32_t i = 0; i < groupedVars.size(); ++i) {
                bucket.m_snapshotRefs.emplace(groupedVars[i], SnapshotRef{bundleVscp, i, true});
            }
        }
    }
    for (SubgraphGroup& group : groups) {
        state.rewriteCrossBoundaryReadsInLogic(group.m_earlyLogic, group.m_scopep, group.m_ownerp,
                                               group.m_senTreep);
        state.rewriteCrossBoundaryReadsInLogic(group.m_lateLogic, group.m_scopep, group.m_ownerp,
                                               group.m_senTreep);
    }
}

void lowerSubgraphGroups(AstNetlist* netlistp, std::vector<SubgraphGroup>& groups,
                         SubgraphLoweringState& state, const V3Order::TrigToSenMap& trigToSen,
                         const std::string& tag, bool slow,
                         const V3Order::ExternalDomainsProvider& externalDomains) {
    unsigned subgraphIndex = 0;
    std::unordered_map<SubgraphActiveKey, SubgraphActiveEntry, SubgraphActiveKeyHash>
        subgraphActives;
    std::unordered_map<SubgraphBatchKey, AstSubgraphInstance*, SubgraphBatchKeyHash> batches;
    std::vector<SubgraphScheduledGroup> scheduledGroups;
    for (SubgraphGroup& group : groups) {
        AstActive* const subgraphActivep = getOrCreateSubgraphActive(group, subgraphActives);
        SubgraphScheduleBundle bundle;
        SubgraphScheduleBundleContext bundleContext;
        bundleContext.m_currentScopep = group.m_scopep;
        bundleContext.m_currentScopeHasDerivedBoundaryReads
            = SubgraphLoweringState::scopeHasDerivedBoundaryReads(group.m_scopep);
        ++state.m_stats.m_bundleBuilds;
        if (!group.m_earlyLogic.empty()) {
            const SubgraphWrapper wrapper
                = wrapperFromLogic(group.m_earlyLogic.front().second->stmtsp());
            refreshSubgraphScheduleBundleContext(bundleContext, state);
            appendSubgraphScheduleBundlePlan(bundle, netlistp, group.m_earlyLogic, wrapper, true,
                                             nullptr, group, bundleContext, state, trigToSen, tag,
                                             slow, externalDomains, subgraphIndex);
        }
        if (!group.m_lateLogic.empty()) {
            SubgraphWrapper wrapper = lateWrapperForGroup(group);
            disableLifePostForExternalReads(group.m_lateLogic, group.m_scopep);
            const std::vector<AstCFunc*>* tailFuncps = nullptr;
            std::vector<AstCFunc*> activeTailFuncps;
            if (tag != "stl" && tag != "ico") {
                auto& stlFuncs = state.m_stlSubgraphFuncs;
                const auto it = stlFuncs.find(group.m_scopep);
                if (it != stlFuncs.end()) {
                    if (state.m_snapshotCrossBoundaryReads) {
                        activeTailFuncps.reserve(it->second.size());
                        for (AstCFunc* const tailFuncp : it->second) {
                            AstCFunc* const activeTailFuncp
                                = state.tailNeedsNbaClone(tailFuncp, group.m_scopep)
                                      ? state.cloneTailFuncForNba(tailFuncp, group.m_scopep,
                                                                  group.m_ownerp, group.m_senTreep,
                                                                  slow)
                                      : tailFuncp;
                            activeTailFuncps.push_back(activeTailFuncp);
                        }
                        tailFuncps = &activeTailFuncps;
                    } else {
                        tailFuncps = &it->second;
                    }
                }
            }
            refreshSubgraphScheduleBundleContext(bundleContext, state);
            appendSubgraphScheduleBundlePlan(bundle, netlistp, group.m_lateLogic, wrapper, false,
                                             tailFuncps, group, bundleContext, state, trigToSen,
                                             tag, slow, externalDomains, subgraphIndex);
        }
        if (bundle.empty()) {
            ++state.m_stats.m_bundleEmpty;
        } else {
            scheduledGroups.push_back(
                SubgraphScheduledGroup{std::move(bundle), &group, subgraphActivep});
        }
    }
    for (const SubgraphScheduledGroup& scheduled : scheduledGroups) {
        materializeSubgraphScheduleBundle(*scheduled.m_groupp, scheduled.m_bundle, state,
                                          scheduled.m_subgraphActivep, batches);
    }
    for (const auto& pair : subgraphActives) {
        AstActive* const subgraphActivep = pair.second.m_activep;
        if (subgraphActivep->stmtsp()) {
            pair.first.m_ownerp->emplace_back(pair.second.m_scopep, subgraphActivep);
        } else {
            subgraphActivep->deleteTree();
        }
    }
}

class SubgraphLowerer final {
    AstNetlist* const m_netlistp;
    const std::vector<LogicByScope*>& m_logic;
    const V3Order::TrigToSenMap& m_trigToSen;
    const string& m_tag;
    const bool m_slow;
    const V3Order::ExternalDomainsProvider& m_externalDomains;
    const uint64_t m_totalStartUsecs = statStartUsecs();
    SubgraphLoweringState m_state;
    std::vector<SubgraphGroup> m_groups;

public:
    SubgraphLowerer(AstNetlist* netlistp, const std::vector<LogicByScope*>& logic,
                    const V3Order::TrigToSenMap& trigToSen, const string& tag, bool slow,
                    const V3Order::ExternalDomainsProvider& externalDomains)
        : m_netlistp{netlistp}
        , m_logic{logic}
        , m_trigToSen{trigToSen}
        , m_tag{tag}
        , m_slow{slow}
        , m_externalDomains{externalDomains}
        , m_state{tag} {
        uint64_t startUsecs = statStartUsecs();
        setSubgraphInputStatsBefore(m_state.m_stats, collectSubgraphLogicInputStats(m_logic));
        addElapsedUsecs(m_state.m_stats.m_timeCollectInputStatsUsecs, startUsecs);
        startUsecs = statStartUsecs();
        if (m_tag == "stl") m_state.collectParentConsumedSubgraphVars(m_logic);
        collectSubgraphGroups(m_logic, m_groups);
        addElapsedUsecs(m_state.m_stats.m_timeCollectGroupsUsecs, startUsecs);
        m_state.m_stats.m_groups = m_groups.size();
    }

    void run() {
        if (m_state.m_snapshotCrossBoundaryReads) {
            const uint64_t startUsecs = statStartUsecs();
            collectRegionWrittenVars(m_logic, m_state);
            addElapsedUsecs(m_state.m_stats.m_timeCollectRegionWrittenVarsUsecs, startUsecs);
        }
        if (m_state.m_snapshotCrossBoundaryReads) {
            const uint64_t startUsecs = statStartUsecs();
            prepareSubgraphSnapshots(m_groups, m_state, m_tag);
            addElapsedUsecs(m_state.m_stats.m_timePrepareSnapshotsUsecs, startUsecs);
            m_state.m_stats.m_snapshotBuckets = m_state.m_snapshotBuckets.size();
            for (const SnapshotBucket& bucket : m_state.m_snapshotBuckets) {
                m_state.m_stats.m_snapshotSources += bucket.m_sourceVars.size();
            }
        }

        uint64_t startUsecs = statStartUsecs();
        lowerSubgraphGroups(m_netlistp, m_groups, m_state, m_trigToSen, m_tag, m_slow,
                            m_externalDomains);
        addElapsedUsecs(m_state.m_stats.m_timeLowerGroupsUsecs, startUsecs);

        startUsecs = statStartUsecs();
        for (const SnapshotBucket& bucket : m_state.m_snapshotBuckets) {
            emitSnapshotProcedureForBucket(bucket, m_state, m_slow);
        }
        addElapsedUsecs(m_state.m_stats.m_timeEmitSnapshotsUsecs, startUsecs);
        startUsecs = statStartUsecs();
        setSubgraphInputStatsAfter(m_state.m_stats, collectSubgraphLogicInputStats(m_logic));
        addElapsedUsecs(m_state.m_stats.m_timeCollectInputStatsUsecs, startUsecs);
        m_state.updateArtifactKeyStats();
        addElapsedUsecs(m_state.m_stats.m_timeTotalUsecs, m_totalStartUsecs);
        m_state.m_stats.report(m_tag);
    }
};

}  // namespace

void lowerSubgraphLogic(AstNetlist* netlistp, const std::vector<LogicByScope*>& logic,
                        const V3Order::TrigToSenMap& trigToSen, const string& tag, bool slow,
                        const V3Order::ExternalDomainsProvider& externalDomains) {
    if (!v3Global.opt.subgraphSchedule()) return;
    SubgraphLowerer{netlistp, logic, trigToSen, tag, slow, externalDomains}.run();
}

}  // namespace V3Sched
