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

namespace V3Sched {

namespace {

struct SubgraphInstanceContract final {
    std::vector<AstSubgraphInstance::BoundaryReadContract> m_boundaryReads;
    std::vector<AstVarScope*> m_boundaryWrites;
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
    void addBoundaryWrite(AstVarScope* vscp) {
        for (AstVarScope* const scanp : m_boundaryWrites) {
            if (scanp == vscp) return;
        }
        m_boundaryWrites.push_back(vscp);
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
        for (const AstSubgraphInstance::ExternalUseContract& use : other.m_externalUses) {
            addExternalUse(use.m_varscp, use.m_read, use.m_write);
        }
    }
};

using SubgraphInstanceContractMap = std::unordered_map<const AstScope*, SubgraphInstanceContract>;

struct SubgraphRegistry final {
    SubgraphInstanceContractMap m_scopeContracts;
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
    AstSenTree* m_senTreep = nullptr;
    SubgraphWrapper m_wrapper;
    AstSubgraphInstance::Phase m_phase = AstSubgraphInstance::Phase::NONE;

    bool operator==(const SubgraphBatchKey& other) const {
        return m_ownerp == other.m_ownerp && m_senTreep == other.m_senTreep
               && m_wrapper == other.m_wrapper && m_phase == other.m_phase;
    }
};

struct SubgraphBatchKeyHash final {
    size_t operator()(const SubgraphBatchKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_ownerp);
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

struct SubgraphLogicNodeSig final {
    std::vector<std::string> m_constValues;
    std::vector<uintptr_t> m_nodeTypes;
    uintptr_t m_type = 0;
    std::vector<SubgraphLogicRefSig> m_refs;
};

using SubgraphLogicSig = std::vector<SubgraphLogicNodeSig>;

struct SubgraphScheduleArtifact;

enum class SubgraphTemplateMapFailReason : uint8_t {
    NONE,
    NODE_COUNT,
    NODE_SHAPE,
    NODE_TYPE,
    REF_ACCESS,
    REF_CONFLICT,
    REF_COUNT,
};

struct SubgraphOrderCacheEntry final {
    SubgraphScheduleArtifact* m_artifactp = nullptr;
    AstCFunc* m_funcp = nullptr;
    SubgraphLogicSig m_logicSig;
    bool m_cloneable = true;
    bool m_triggeredShareable = false;
    bool m_triggeredWritesDelayedShadow = false;
    bool m_triggeredWritesInstanceLocal = false;
    bool m_triggeredWritesLocalTemp = false;
    bool m_triggeredWritesNonLocal = false;
    bool m_triggeredWritesTriggerTemp = false;
    bool m_triggeredWritesVlemTemp = false;
};

struct SubgraphOrderCacheKey final {
    std::vector<uintptr_t> m_domainShape;
    AstNodeModule* m_modp = nullptr;
    AstSubgraphInstance::Phase m_phase = AstSubgraphInstance::Phase::NONE;
    SubgraphWrapper m_wrapper;

    bool operator==(const SubgraphOrderCacheKey& other) const {
        return m_modp == other.m_modp && m_phase == other.m_phase && m_wrapper == other.m_wrapper
               && m_domainShape == other.m_domainShape;
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
        return hash;
    }
};

struct SubgraphScheduleArtifactKey final {
    std::vector<uintptr_t> m_domainShape;
    AstNodeModule* m_modp = nullptr;
    AstSubgraphInstance::Phase m_phase = AstSubgraphInstance::Phase::NONE;

    bool operator==(const SubgraphScheduleArtifactKey& other) const {
        return m_modp == other.m_modp && m_phase == other.m_phase
               && m_domainShape == other.m_domainShape;
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

struct SubgraphScheduleArtifact final {
    AstCFunc* m_callFuncp = nullptr;
    std::vector<SubgraphSharedHelperArg> m_helperArgs;
    SubgraphScheduleArtifactKey m_key;
    SubgraphLogicSig m_logicSig;
    AstScope* m_scopep = nullptr;
    SubgraphTailContract m_tailContract;
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

struct SubgraphLoweringStats final {
    uint64_t m_artifactKeyMaxEntriesPerFullKey = 0;
    uint64_t m_artifactKeyUniqueDomainShapes = 0;
    uint64_t m_artifactKeyUniqueFull = 0;
    uint64_t m_artifactKeyUniqueModules = 0;
    uint64_t m_artifactMisses = 0;
    uint64_t m_artifactReuseLookups = 0;
    uint64_t m_artifactReuseMissLogicMismatch = 0;
    uint64_t m_artifactReuseMissNoEntry = 0;
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
    uint64_t m_artifactReuseTemplateMapFailNodeCount = 0;
    uint64_t m_artifactReuseTemplateMapFailNodeShape = 0;
    uint64_t m_artifactReuseTemplateMapFailNodeType = 0;
    uint64_t m_artifactReuseTemplateMapFailRefAccess = 0;
    uint64_t m_artifactReuseTemplateMapFailRefConflict = 0;
    uint64_t m_artifactReuseTemplateMapFailRefCount = 0;
    uint64_t m_artifactReuseTemplateMapFails = 0;
    uint64_t m_artifactReuses = 0;
    uint64_t m_artifactReuseCloneFails = 0;
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
    uint64_t m_orderCacheEntries = 0;
    uint64_t m_orderCacheEntryHits = 0;
    uint64_t m_orderCacheHits = 0;
    uint64_t m_orderCacheLookups = 0;
    uint64_t m_orderCacheMisses = 0;
    uint64_t m_orderCacheSharedHits = 0;
    uint64_t m_orderCacheSharedSkipCloneFail = 0;
    uint64_t m_orderCacheSharedSkipModuleMismatch = 0;
    uint64_t m_orderCacheSharedSkipNonLoose = 0;
    uint64_t m_orderCacheSharedSkipOther = 0;
    uint64_t m_orderCacheSharedSkipPhase = 0;
    uint64_t m_orderCacheSharedSkipTriggered = 0;
    uint64_t m_orderCacheSharedSkipTriggeredInputTail = 0;
    uint64_t m_orderCacheSharedSkipTriggeredNotShareable = 0;
    uint64_t m_orderCacheSharedSkipTriggeredOther = 0;
    uint64_t m_orderCacheSkipTriggered = 0;
    uint64_t m_orderCacheSkipTriggeredInstanceLocal = 0;
    uint64_t m_orderCacheSkipTriggeredNoArtifact = 0;
    uint64_t m_orderCacheSkipTriggeredNotShareable = 0;
    uint64_t m_orderCacheSkipTriggeredStl = 0;
    uint64_t m_orderCacheTemplateMapFailNodeCount = 0;
    uint64_t m_orderCacheTemplateMapFailNodeShape = 0;
    uint64_t m_orderCacheTemplateMapFailNodeType = 0;
    uint64_t m_orderCacheTemplateMapFailRefAccess = 0;
    uint64_t m_orderCacheTemplateMapFailRefConflict = 0;
    uint64_t m_orderCacheTemplateMapFailRefCount = 0;
    uint64_t m_orderCacheTemplateMapFails = 0;
    uint64_t m_orderCacheVariantBuckets = 0;
    uint64_t m_orderCacheVariantCandidates = 0;
    uint64_t m_orderCacheVariantMax = 0;
    uint64_t m_orderedFuncClones = 0;
    uint64_t m_schedulePlans = 0;
    uint64_t m_sharedHelperExternalArgs = 0;
    uint64_t m_sharedHelperParameterizationFails = 0;
    uint64_t m_sharedHelperParameterizations = 0;
    uint64_t m_sharedHelperParameterizedFuncs = 0;
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
    uint64_t m_timeBuildPlansUsecs = 0;
    uint64_t m_timeCollectGroupsUsecs = 0;
    uint64_t m_timeCollectInputStatsUsecs = 0;
    uint64_t m_timeCollectRegionWrittenVarsUsecs = 0;
    uint64_t m_timeEmitSnapshotsUsecs = 0;
    uint64_t m_timeInternalOrderUsecs = 0;
    uint64_t m_timeLookupArtifactsUsecs = 0;
    uint64_t m_timeLowerGroupsUsecs = 0;
    uint64_t m_timeMaterializeUsecs = 0;
    uint64_t m_timePrepareSnapshotsUsecs = 0;
    uint64_t m_timeTotalUsecs = 0;
    uint64_t m_triggeredArtifactCandidates = 0;
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
    std::vector<std::pair<std::string, uint64_t>> m_internalOrderTimes;

    static uint64_t ratioPermille(uint64_t numerator, uint64_t denominator) {
        if (!denominator) return 0;
        return numerator * 1000 / denominator;
    }

    static double seconds(uint64_t usecs) { return usecs / 1.0e6; }

    static void addTemplateMapFailStats(const string& prefix, const string& path,
                                        uint64_t nodeCount, uint64_t nodeShape, uint64_t nodeType,
                                        uint64_t refAccess, uint64_t refConflict,
                                        uint64_t refCount, uint64_t total) {
        V3Stats::addStat(prefix + path + " template map fail node count", nodeCount);
        V3Stats::addStat(prefix + path + " template map fail node shape", nodeShape);
        V3Stats::addStat(prefix + path + " template map fail node type", nodeType);
        V3Stats::addStat(prefix + path + " template map fail ref access", refAccess);
        V3Stats::addStat(prefix + path + " template map fail ref conflict", refConflict);
        V3Stats::addStat(prefix + path + " template map fail ref count", refCount);
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
        V3Stats::addStat(prefix + "artifact reuse lookups", m_artifactReuseLookups);
        V3Stats::addStat(prefix + "artifact reuse miss logic mismatch",
                         m_artifactReuseMissLogicMismatch);
        V3Stats::addStat(prefix + "artifact reuse miss no entry", m_artifactReuseMissNoEntry);
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
            prefix, "artifact reuse", m_artifactReuseTemplateMapFailNodeCount,
            m_artifactReuseTemplateMapFailNodeShape, m_artifactReuseTemplateMapFailNodeType,
            m_artifactReuseTemplateMapFailRefAccess, m_artifactReuseTemplateMapFailRefConflict,
            m_artifactReuseTemplateMapFailRefCount, m_artifactReuseTemplateMapFails);
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
        V3Stats::addStat(prefix + "order cache entries", m_orderCacheEntries);
        V3Stats::addStat(prefix + "order cache entry hits", m_orderCacheEntryHits);
        V3Stats::addStat(prefix + "order cache hits", m_orderCacheHits);
        V3Stats::addStat(prefix + "order cache lookups", m_orderCacheLookups);
        V3Stats::addStat(prefix + "order cache hit permille",
                         ratioPermille(m_orderCacheHits, orderCacheLookups));
        V3Stats::addStat(prefix + "order cache misses", m_orderCacheMisses);
        V3Stats::addStat(prefix + "order cache shared hits", m_orderCacheSharedHits);
        V3Stats::addStat(prefix + "order cache shared skip clone fail",
                         m_orderCacheSharedSkipCloneFail);
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
        V3Stats::addStat(prefix + "order cache skip triggered", m_orderCacheSkipTriggered);
        V3Stats::addStat(prefix + "order cache skip triggered instance local",
                         m_orderCacheSkipTriggeredInstanceLocal);
        V3Stats::addStat(prefix + "order cache skip triggered no artifact",
                         m_orderCacheSkipTriggeredNoArtifact);
        V3Stats::addStat(prefix + "order cache skip triggered not shareable",
                         m_orderCacheSkipTriggeredNotShareable);
        V3Stats::addStat(prefix + "order cache skip triggered stl", m_orderCacheSkipTriggeredStl);
        addTemplateMapFailStats(
            prefix, "order cache", m_orderCacheTemplateMapFailNodeCount,
            m_orderCacheTemplateMapFailNodeShape, m_orderCacheTemplateMapFailNodeType,
            m_orderCacheTemplateMapFailRefAccess, m_orderCacheTemplateMapFailRefConflict,
            m_orderCacheTemplateMapFailRefCount, m_orderCacheTemplateMapFails);
        V3Stats::addStat(prefix + "order cache variant buckets", m_orderCacheVariantBuckets);
        V3Stats::addStat(prefix + "order cache variant candidates", m_orderCacheVariantCandidates);
        V3Stats::addStat(prefix + "order cache variant max", m_orderCacheVariantMax);
        V3Stats::addStat(prefix + "ordered function clones", m_orderedFuncClones);
        V3Stats::addStat(prefix + "schedule plans", m_schedulePlans);
        V3Stats::addStat(prefix + "shared helper external args", m_sharedHelperExternalArgs);
        V3Stats::addStat(prefix + "shared helper parameterization fails",
                         m_sharedHelperParameterizationFails);
        V3Stats::addStat(prefix + "shared helper parameterizations",
                         m_sharedHelperParameterizations);
        V3Stats::addStat(prefix + "shared helper parameterized funcs",
                         m_sharedHelperParameterizedFuncs);
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
        V3Stats::addStat(prefix + "time build plans sec", seconds(m_timeBuildPlansUsecs), 6);
        V3Stats::addStat(prefix + "time collect groups sec", seconds(m_timeCollectGroupsUsecs), 6);
        V3Stats::addStat(prefix + "time collect input stats sec",
                         seconds(m_timeCollectInputStatsUsecs), 6);
        V3Stats::addStat(prefix + "time collect region written vars sec",
                         seconds(m_timeCollectRegionWrittenVarsUsecs), 6);
        V3Stats::addStat(prefix + "time emit snapshots sec", seconds(m_timeEmitSnapshotsUsecs), 6);
        V3Stats::addStat(prefix + "time internal order sec", seconds(m_timeInternalOrderUsecs), 6);
        V3Stats::addStat(prefix + "time lookup artifacts sec", seconds(m_timeLookupArtifactsUsecs),
                         6);
        V3Stats::addStat(prefix + "time lower groups sec", seconds(m_timeLowerGroupsUsecs), 6);
        V3Stats::addStat(prefix + "time materialize sec", seconds(m_timeMaterializeUsecs), 6);
        V3Stats::addStat(prefix + "time prepare snapshots sec",
                         seconds(m_timePrepareSnapshotsUsecs), 6);
        V3Stats::addStat(prefix + "time total sec", seconds(m_timeTotalUsecs), 6);
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
    }

    void noteOrderCacheCloneFailName(const string& name) {
        if (m_orderCacheCloneFailNames.size() >= 16 && !m_orderCacheCloneFailNames.count(name))
            return;
        ++m_orderCacheCloneFailNames[name];
    }

    void noteArtifactTemplateMapFail(SubgraphTemplateMapFailReason reason) {
        ++m_artifactReuseTemplateMapFails;
        switch (reason) {
        case SubgraphTemplateMapFailReason::NODE_COUNT:
            ++m_artifactReuseTemplateMapFailNodeCount;
            return;
        case SubgraphTemplateMapFailReason::NODE_SHAPE:
            ++m_artifactReuseTemplateMapFailNodeShape;
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
        case SubgraphTemplateMapFailReason::NONE: return;
        }
    }

    void noteInternalOrderTime(const std::string& name, uint64_t usecs) {
        if (!usecs) return;
        m_internalOrderTimes.emplace_back(name, usecs);
    }

    void noteOrderCacheTemplateMapFail(SubgraphTemplateMapFailReason reason) {
        ++m_orderCacheTemplateMapFails;
        switch (reason) {
        case SubgraphTemplateMapFailReason::NODE_COUNT:
            ++m_orderCacheTemplateMapFailNodeCount;
            return;
        case SubgraphTemplateMapFailReason::NODE_SHAPE:
            ++m_orderCacheTemplateMapFailNodeShape;
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
        case SubgraphTemplateMapFailReason::NONE: return;
        }
    }
};

class SubgraphLoweringState final {
    static std::unordered_map<AstScope*, std::vector<AstCFunc*>>& stlSubgraphFuncsStorage() {
        static std::unordered_map<AstScope*, std::vector<AstCFunc*>> s_stlSubgraphFuncs;
        return s_stlSubgraphFuncs;
    }

public:
    explicit SubgraphLoweringState(const string& tag)
        : m_snapshotCrossBoundaryReads{tag == "nba"}
        , m_stlSubgraphFuncs{stlSubgraphFuncsStorage()} {
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

    static void discardLogic(LogicByScope& logic) {
        for (const auto& pair : logic) {
            AstActive* const activep = pair.second;
            if (activep->backp()) activep->unlinkFrBack();
            activep->deleteTree();
        }
        logic.clear();
    }

    static std::vector<uintptr_t>
    computeDomainShape(const LogicByScope& logic, AstScope* boundaryScopep,
                       const V3Order::ExternalDomainsProvider& externalDomains) {
        std::vector<uintptr_t> result;
        logic.foreachLogic([&](AstNode* logicp) {
            result.push_back(static_cast<uintptr_t>(logicp->type()));
            logicp->foreach([&](AstVarRef* refp) {
                result.push_back(static_cast<uintptr_t>(refp->access()));
                const AstVarScope* const vscp = refp->varScopep();
                result.push_back(isUnderBoundaryScope(vscp->scopep(), boundaryScopep));
                std::vector<AstSenTree*> domains;
                externalDomains(vscp, domains);
                result.push_back(domains.size());
                for (AstSenTree* const domainp : domains) {
                    result.push_back(reinterpret_cast<uintptr_t>(domainp));
                }
            });
        });
        return result;
    }

    static SubgraphLogicSig buildLogicSig(const LogicByScope& logic) {
        SubgraphLogicSig result;
        logic.foreachLogic([&](AstNode* logicp) {
            result.push_back(SubgraphLogicNodeSig{});
            SubgraphLogicNodeSig& nodeSig = result.back();
            nodeSig.m_type = static_cast<uintptr_t>(logicp->type());
            logicp->foreach([&](AstNode* nodep) {
                nodeSig.m_nodeTypes.push_back(static_cast<uintptr_t>(nodep->type()));
                if (AstConst* const constp = VN_CAST(nodep, Const)) {
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

    static SubgraphTemplateMapFailReason
    buildTemplateVarScopeMap(const SubgraphLogicSig& templateSig, const LogicByScope& currentLogic,
                             std::unordered_map<const AstVarScope*, AstVarScope*>& result) {
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
                    currentNodeSig.m_constValues.push_back(constp->num().toString());
                }
            });
            if (templateNode.m_nodeTypes != currentNodeSig.m_nodeTypes
                || templateNode.m_constValues != currentNodeSig.m_constValues) {
                return SubgraphTemplateMapFailReason::NODE_SHAPE;
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
                const auto it = result.find(templateRef.m_vscp);
                if (it != result.end()) {
                    if (it->second != currentVscp) {
                        return SubgraphTemplateMapFailReason::REF_CONFLICT;
                    }
                } else {
                    result.emplace(templateRef.m_vscp, currentVscp);
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

    static SubgraphInstanceLocalVarKind instanceLocalVarKind(const string& name) {
        if (name.rfind("__Vdly", 0) == 0) { return SubgraphInstanceLocalVarKind::DELAYED_SHADOW; }
        if (name.rfind("__Vtrigcurr", 0) == 0) {
            return SubgraphInstanceLocalVarKind::TRIGGER_CURR;
        }
        if (name.rfind("__Vtrigprev", 0) == 0) {
            return SubgraphInstanceLocalVarKind::TRIGGER_PREV;
        }
        if (name.rfind("__VtrigSched", 0) == 0) {
            return SubgraphInstanceLocalVarKind::TRIGGER_SCHED;
        }
        if (name.rfind("__V", 0) == 0 && nameEndsWith(name, "Triggered")) {
            return SubgraphInstanceLocalVarKind::TRIGGER_STATE;
        }
        if (name.rfind("__V", 0) == 0 && nameEndsWith(name, "TriggeredAcc")) {
            return SubgraphInstanceLocalVarKind::TRIGGER_STATE_ACC;
        }
        if (name.rfind("__Vcell", 0) == 0 || name.rfind("__Vfunc", 0) == 0
            || name.rfind("__Vtemp", 0) == 0) {
            return SubgraphInstanceLocalVarKind::LOCAL_TEMP;
        }
        if (name.rfind("__Vlem", 0) == 0) { return SubgraphInstanceLocalVarKind::VLEM_TEMP; }
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
                                                                    SubgraphLoweringStats& stats) {
        std::unordered_set<AstCFunc*> seenFuncs;
        SubgraphTriggeredRefInfo result;
        std::function<void(AstCFunc*)> gather = [&](AstCFunc* scanFuncp) {
            if (!seenFuncs.insert(scanFuncp).second) return;
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const vscp = refp->varScopep();
                noteTriggeredRefKind(vscp, stats);
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
        return result;
    }

    static bool canShareTriggeredArtifact(const SubgraphTriggeredRefInfo& info) {
        if (!info.m_hasTriggered || !info.m_shareable) return false;
        if (info.m_writesNonLocal) return true;
        return !info.m_writesInstanceLocal;
    }

    static bool canCloneTriggeredOrderCacheEntry(const SubgraphTriggeredRefInfo& info) {
        return !info.m_hasTriggered
               || (info.m_shareable
                   && (!info.m_writesInstanceLocal
                       || info.writesOnlySharedHelperSafeInstanceLocal()));
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

    static bool parameterizeSharedHelper(AstCFunc* funcp, AstScope* boundaryScopep,
                                         const SubgraphLogicSig& logicSig,
                                         std::vector<SubgraphSharedHelperArg>& helperArgs,
                                         SubgraphLoweringStats& stats) {
        std::unordered_set<const AstVarScope*> eligibleVscps;
        for (const SubgraphLogicNodeSig& node : logicSig) {
            for (const SubgraphLogicRefSig& ref : node.m_refs) {
                const AstVarScope* const vscp = ref.m_vscp;
                if (!isUnderBoundaryScope(vscp->scopep(), boundaryScopep)) {
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

        std::unordered_map<AstVarScope*, size_t> argIndex;
        bool safe = true;
        for (AstCFunc* const scanFuncp : funcs) {
            scanFuncp->foreach([&](AstVarRef* refp) {
                AstVarScope* const vscp = refp->varScopep();
                if (isUnderBoundaryScope(vscp->scopep(), boundaryScopep)) return;
                if (!eligibleVscps.count(vscp)) {
                    if (!isRemappableInstanceLocalVar(vscp)) return;
                    safe = false;
                    return;
                }
                const auto inserted = argIndex.emplace(vscp, helperArgs.size());
                if (inserted.second) helperArgs.push_back(SubgraphSharedHelperArg{vscp});
                SubgraphSharedHelperArg& arg = helperArgs[inserted.first->second];
                arg.m_reads |= refp->access().isReadOrRW();
                arg.m_writes |= refp->access().isWriteOrRW();
            });
        }
        if (!safe) {
            helperArgs.clear();
            ++stats.m_sharedHelperParameterizationFails;
            return false;
        }
        if (helperArgs.empty()) return true;

        std::unordered_map<AstCFunc*, std::vector<AstVarScope*>> argsByFunc;
        for (AstCFunc* const scanFuncp : funcs) {
            std::vector<AstVarScope*>& args = argsByFunc[scanFuncp];
            args.reserve(helperArgs.size());
            for (size_t i = 0; i < helperArgs.size(); ++i) {
                args.push_back(newSharedHelperArg(scanFuncp, helperArgs[i], i));
            }
        }
        for (AstCFunc* const scanFuncp : funcs) {
            const std::vector<AstVarScope*>& args = argsByFunc.at(scanFuncp);
            scanFuncp->foreach([&](AstVarRef* refp) {
                const auto it = argIndex.find(refp->varScopep());
                if (it == argIndex.end()) return;
                AstVarScope* const argVscp = args[it->second];
                refp->varp(argVscp->varp());
                refp->varScopep(argVscp);
                refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                const auto it = argsByFunc.find(callp->funcp());
                if (it == argsByFunc.end()) return;
                for (size_t i = 0; i < helperArgs.size(); ++i) {
                    callp->addArgsp(new AstVarRef{callp->fileline(), args[i],
                                                  sharedHelperArgAccess(helperArgs[i])});
                }
            });
        }
        stats.m_sharedHelperExternalArgs += helperArgs.size();
        stats.m_sharedHelperParameterizedFuncs += funcs.size();
        ++stats.m_sharedHelperParameterizations;
        return true;
    }

    static bool populateSharedHelperArgs(
        SubgraphScheduleInstance& instance, const SubgraphScheduleArtifact& artifact,
        AstScope* currentScopep,
        const std::unordered_map<const AstVarScope*, AstVarScope*>& templateVarMap) {
        instance.m_helperArgs.clear();
        instance.m_helperArgs.reserve(artifact.m_helperArgs.size());
        if (artifact.m_scopep != currentScopep) {
            for (AstVarScope* vscp = currentScopep->varsp(); vscp;
                 vscp = VN_AS(vscp->nextp(), VarScope)) {
                if (instanceLocalVarKind(vscp) == SubgraphInstanceLocalVarKind::DELAYED_SHADOW) {
                    vscp->optimizeLifePost(false);
                }
            }
        }
        for (const SubgraphSharedHelperArg& arg : artifact.m_helperArgs) {
            AstVarScope* currentVscp = arg.m_vscp;
            if (artifact.m_scopep != currentScopep) {
                const auto it = templateVarMap.find(arg.m_vscp);
                if (it == templateVarMap.end()) {
                    instance.m_helperArgs.clear();
                    return false;
                }
                currentVscp = it->second;
            }
            if (arg.m_writes) currentVscp->optimizeLifePost(false);
            instance.m_helperArgs.push_back(
                SubgraphSharedHelperArg{currentVscp, arg.m_reads, arg.m_writes});
        }
        return true;
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

    static AstCFunc* cloneOrderedFuncGraph(
        AstCFunc* funcp, AstScope* destBoundaryScopep,
        const std::unordered_map<const AstVarScope*, AstVarScope*>& templateVarMap,
        SubgraphLoweringStats& stats) {
        static unsigned s_cloneIndex = 0;

        std::vector<AstCFunc*> orderedFuncs;
        std::unordered_set<AstCFunc*> seenFuncs;
        std::function<void(AstCFunc*)> gatherFuncs = [&](AstCFunc* scanFuncp) {
            if (!seenFuncs.insert(scanFuncp).second) return;
            orderedFuncs.push_back(scanFuncp);
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint()) return;
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
            if (canRemapGeneratedCloneVarByName(pair.first)) continue;
            if (mapsToOtherSubgraphBoundary(pair.second, destBoundaryScopep)) continue;
            resolvedVarMap.emplace(pair);
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
                const auto varIt = resolvedVarMap.find(refp->varScopep());
                if (varIt == resolvedVarMap.end()) {
                    noteUnmappedVar(refp->varScopep());
                    failed = true;
                    return;
                }
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
            });
            if (failed) {
                VL_DO_DANGLING(bodyp->deleteTree(), bodyp);
                return nullptr;
            }
            clonedFuncp->addStmtsp(bodyp);
        }

        return clonedFuncs.at(funcp);
    }

    void appendContractExternalUses(SubgraphInstanceContract& contract, AstCFunc* funcp,
                                    AstScope* boundaryScopep) {
        std::unordered_set<AstCFunc*> seen;
        std::function<void(AstCFunc*)> gather = [&](AstCFunc* scanFuncp) {
            if (!seen.insert(scanFuncp).second) return;
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const vscp = refp->varScopep();
                if (0 == vscp->varp()->name().rfind("__VsubgraphSnapshot__", 0)) {
                    ++m_stats.m_contractExternalUseSnapshotSkips;
                    return;
                }
                const bool externalToSubgraph
                    = !isUnderBoundaryScope(vscp->scopep(), boundaryScopep);
                if (!externalToSubgraph) return;
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
                const bool externalToSubgraph
                    = !isUnderBoundaryScope(vscp->scopep(), boundaryScopep);
                if (!externalToSubgraph) return;
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
            if (artifactp->m_tailContract.m_writesBoundaryInput
                || bundleContext.m_currentScopeTailContract.m_writesBoundaryInput) {
                return SubgraphSharedHelperSkipReason::TRIGGERED_INPUT_TAIL;
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
            const SubgraphTemplateMapFailReason templateMapFail = buildTemplateVarScopeMap(
                artifactp->m_logicSig, currentLogic, reuse.m_remap.m_templateVarMap);
            if (templateMapFail != SubgraphTemplateMapFailReason::NONE) {
                m_stats.noteArtifactTemplateMapFail(templateMapFail);
                sawLogicMismatch = true;
                continue;
            }
            if (artifactp->m_scopep != currentScopep
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

    void noteArtifactNoEntry(const SubgraphScheduleArtifactKey& key) {
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
        }
        if (sameDomainShape) ++m_stats.m_artifactReuseMissNoEntrySameDomainShape;
        if (sameModule) ++m_stats.m_artifactReuseMissNoEntrySameModule;
        if (sameModuleDomainShape) ++m_stats.m_artifactReuseMissNoEntrySameModuleDomainShape;
        if (sameModulePhase) ++m_stats.m_artifactReuseMissNoEntrySameModulePhase;
    }

    SubgraphScheduleArtifact* makeSubgraphScheduleArtifact(
        const SubgraphScheduleArtifactKey& key, AstScope* scopep, AstCFunc* callFuncp,
        std::vector<SubgraphSharedHelperArg>&& helperArgs, SubgraphLogicSig&& logicSig,
        const SubgraphTailContract& tailContract, bool cloneable, bool hasTriggered,
        bool triggeredShareable, bool cacheable) {
        std::unique_ptr<SubgraphScheduleArtifact> artifactp{new SubgraphScheduleArtifact};
        artifactp->m_callFuncp = callFuncp;
        artifactp->m_helperArgs = std::move(helperArgs);
        artifactp->m_key = key;
        artifactp->m_logicSig = std::move(logicSig);
        artifactp->m_scopep = scopep;
        artifactp->m_tailContract = tailContract;
        artifactp->m_cloneable = cloneable;
        artifactp->m_hasTriggered = hasTriggered;
        artifactp->m_triggeredShareable = triggeredShareable;
        if (!cloneable)
            artifactp->m_uncloneableReason = SubgraphArtifactUncloneableReason::TRIGGERED;
        SubgraphScheduleArtifact* const resultp = artifactp.get();
        m_subgraphArtifacts.push_back(std::move(artifactp));
        if (cacheable) m_subgraphArtifactCache[key].push_back(resultp);
        ++m_stats.m_artifactMisses;
        ++m_stats.m_artifacts;
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
    std::unordered_map<SubgraphScheduleArtifactKey, std::vector<SubgraphScheduleArtifact*>,
                       SubgraphScheduleArtifactKeyHash>
        m_subgraphArtifactCache;
    std::vector<std::unique_ptr<SubgraphScheduleArtifact>> m_subgraphArtifacts;
    std::unordered_map<SubgraphOrderCacheKey, std::vector<SubgraphOrderCacheEntry>,
                       SubgraphOrderCacheKeyHash>
        m_subgraphOrderCache;
    std::unordered_map<SnapshotBucketKey, size_t, SnapshotBucketKeyHash> m_snapshotBucketIndex;
    std::vector<SnapshotBucket> m_snapshotBuckets;
    std::unordered_map<SnapshotHelperKey, SnapshotHelperEntry, SnapshotHelperKeyHash>
        m_snapshotHelpers;
    std::unordered_set<SnapshotSourceSetKey, SnapshotSourceSetKeyHash> m_snapshotSourceSets;
    std::unordered_map<TailCloneKey, AstCFunc*, TailCloneKeyHash> m_tailCloneCache;
    std::unordered_set<AstVarScope*> m_regionWrittenVars;
    std::unordered_map<AstScope*, std::vector<AstCFunc*>>& m_stlSubgraphFuncs;
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
    if (summaryp) contract = *summaryp;
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
    instance.m_contract = buildSubgraphSchedulePlanContract(instance.m_scopep);
    state.appendContractExternalUses(instance.m_contract, instance.m_callFuncp, instance.m_scopep);
    for (AstCFunc* const tailFuncp : instance.m_tailFuncps) {
        state.appendContractExternalUses(instance.m_contract, tailFuncp, instance.m_scopep);
    }
}

void populateSubgraphScheduleInstanceContract(SubgraphScheduleInstance& instance,
                                              SubgraphLoweringState& state,
                                              const LogicByScope& logic) {
    instance.m_contract = buildSubgraphSchedulePlanContract(instance.m_scopep);
    state.appendContractExternalUses(instance.m_contract, logic, instance.m_scopep);
    for (AstCFunc* const tailFuncp : instance.m_tailFuncps) {
        state.appendContractExternalUses(instance.m_contract, tailFuncp, instance.m_scopep);
    }
}

SubgraphTailContract buildSubgraphTailContract(const std::vector<AstCFunc*>& tailFuncps,
                                               AstScope* scopep) {
    SubgraphTailContract contract;
    std::unordered_set<AstCFunc*> seenFuncps;
    std::function<void(AstCFunc*)> gather = [&](AstCFunc* funcp) {
        if (!seenFuncps.insert(funcp).second) return;
        funcp->foreach([&](AstNodeVarRef* refp) {
            AstVarScope* const vscp = refp->varScopep();
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
    const SubgraphBatchKey key{group.m_ownerp, group.m_senTreep, wrapper, phase};
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
    cacheKey.m_domainShape = SubgraphLoweringState::computeDomainShape(
        subgraphLogic, group.m_scopep, externalDomains);
    cacheKey.m_modp = group.m_scopep->modp();
    SubgraphOrderCacheEntry* insertedOrderCacheEntryp = nullptr;
    SubgraphLogicSig logicSig;
    if (canShare) logicSig = SubgraphLoweringState::buildLogicSig(subgraphLogic);
    const AstSubgraphInstance::Phase phase = subgraphPhaseFor(wrapper, isEarly);
    cacheKey.m_phase = phase;
    cacheKey.m_wrapper = wrapper;
    SubgraphScheduleArtifactKey artifactKey;
    artifactKey.m_domainShape = cacheKey.m_domainShape;
    artifactKey.m_modp = cacheKey.m_modp;
    artifactKey.m_phase = phase;
    bool cacheableArtifact = canShare && tag != "stl";
    const SubgraphSharedHelperContext sharedContext{&bundleContext, phase};
    if (cacheableArtifact) {
        if (tailFuncps) ++state.m_stats.m_artifactTailReuseCandidates;
        const uint64_t lookupStartUsecs = statStartUsecs();
        SubgraphScheduleArtifactReuse reuse = state.findReusableSubgraphScheduleArtifact(
            artifactKey, subgraphLogic, sharedContext);
        addElapsedUsecs(state.m_stats.m_timeLookupArtifactsUsecs, lookupStartUsecs);
        SubgraphScheduleArtifact* const artifactp = reuse.m_artifactp;
        if (artifactp) {
            AstCFunc* callFuncp = nullptr;
            bool sharedCall = false;
            if (artifactp->m_scopep == group.m_scopep) {
                callFuncp = artifactp->m_callFuncp;
            } else if (SubgraphLoweringState::canUseSharedHelper(artifactp, sharedContext)) {
                callFuncp = artifactp->m_callFuncp;
                sharedCall = true;
                ++state.m_stats.m_artifactReuseSharedCalls;
            }
            if (callFuncp
                && SubgraphLoweringState::populateSharedHelperArgs(
                    plan.m_instance, *artifactp, group.m_scopep, reuse.m_remap.m_templateVarMap)) {
                if (tag == "stl") state.m_stlSubgraphFuncs[group.m_scopep].push_back(callFuncp);
                plan.m_artifactp = artifactp;
                plan.m_instance.m_callFuncp = callFuncp;
                plan.m_instance.m_scopep = group.m_scopep;
                plan.m_instance.m_sharedCall = sharedCall;
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
                SubgraphLoweringState::discardLogic(subgraphLogic);
                plan.m_phase = phase;
                plan.m_wrapper = wrapper;
                ++state.m_stats.m_artifactReuses;
                if (tailFuncps) ++state.m_stats.m_artifactTailReuses;
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
    std::vector<SubgraphSharedHelperArg> helperArgs;
    SubgraphOrderCacheEntry* matchedOrderCacheEntryp = nullptr;
    std::unordered_map<const AstVarScope*, AstVarScope*> orderCacheTemplateVarMap;
    if (canShare) {
        ++state.m_stats.m_orderCacheLookups;
        const auto cacheIt = state.m_subgraphOrderCache.find(cacheKey);
        if (cacheIt != state.m_subgraphOrderCache.end()) {
            ++state.m_stats.m_orderCacheEntryHits;
            state.m_stats.m_orderCacheVariantCandidates += cacheIt->second.size();
            for (SubgraphOrderCacheEntry& cacheEntry : cacheIt->second) {
                orderCacheTemplateVarMap.clear();
                const SubgraphTemplateMapFailReason templateMapFail
                    = SubgraphLoweringState::buildTemplateVarScopeMap(
                        cacheEntry.m_logicSig, subgraphLogic, orderCacheTemplateVarMap);
                if (templateMapFail == SubgraphTemplateMapFailReason::NONE) {
                    matchedOrderCacheEntryp = &cacheEntry;
                    break;
                }
                state.m_stats.noteOrderCacheTemplateMapFail(templateMapFail);
            }
        }
        if (matchedOrderCacheEntryp) {
            SubgraphOrderCacheEntry& cacheEntry = *matchedOrderCacheEntryp;
            if (!cacheEntry.m_cloneable && !cacheEntry.m_triggeredShareable) {
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
                        const SubgraphSharedHelperSkipReason sharedSkipReason
                            = SubgraphLoweringState::classifySharedHelperUse(
                                cacheEntry.m_artifactp, sharedContext);
                        if (sharedSkipReason != SubgraphSharedHelperSkipReason::NONE) {
                            ++state.m_stats.m_orderCacheSkipTriggered;
                            state.noteOrderCacheSharedSkip(sharedSkipReason);
                        } else {
                            if (tailFuncps) {
                                for (AstCFunc* const tailFuncp : *tailFuncps) {
                                    plan.m_instance.m_tailFuncps.push_back(tailFuncp);
                                }
                            }
                            plan.m_artifactp = cacheEntry.m_artifactp;
                            plan.m_instance.m_callFuncp = cacheEntry.m_funcp;
                            plan.m_instance.m_scopep = group.m_scopep;
                            plan.m_instance.m_sharedCall
                                = cacheEntry.m_funcp->scopep() != group.m_scopep;
                            if (SubgraphLoweringState::populateSharedHelperArgs(
                                    plan.m_instance, *cacheEntry.m_artifactp, group.m_scopep,
                                    orderCacheTemplateVarMap)) {
                                populateSubgraphScheduleInstanceContract(plan.m_instance, state,
                                                                         subgraphLogic);
                                SubgraphLoweringState::discardLogic(subgraphLogic);
                                plan.m_phase = phase;
                                plan.m_wrapper = wrapper;
                                ++state.m_stats.m_orderCacheHits;
                                ++state.m_stats.m_orderCacheSharedHits;
                                ++state.m_stats.m_schedulePlans;
                                if (tailFuncps) ++state.m_stats.m_artifactTailReuses;
                                return plan;
                            } else {
                                ++state.m_stats.m_orderCacheSharedSkipOther;
                                plan = SubgraphSchedulePlan{};
                            }
                        }
                    }
                } else if (cacheEntry.m_artifactp
                           && SubgraphLoweringState::canUseSharedHelper(cacheEntry.m_artifactp,
                                                                        sharedContext)) {
                    if (tailFuncps) {
                        for (AstCFunc* const tailFuncp : *tailFuncps) {
                            plan.m_instance.m_tailFuncps.push_back(tailFuncp);
                        }
                    }
                    plan.m_artifactp = cacheEntry.m_artifactp;
                    plan.m_instance.m_callFuncp = cacheEntry.m_funcp;
                    plan.m_instance.m_scopep = group.m_scopep;
                    plan.m_instance.m_sharedCall = cacheEntry.m_funcp->scopep() != group.m_scopep;
                    if (SubgraphLoweringState::populateSharedHelperArgs(
                            plan.m_instance, *cacheEntry.m_artifactp, group.m_scopep,
                            orderCacheTemplateVarMap)) {
                        populateSubgraphScheduleInstanceContract(plan.m_instance, state,
                                                                 subgraphLogic);
                        SubgraphLoweringState::discardLogic(subgraphLogic);
                        plan.m_phase = phase;
                        plan.m_wrapper = wrapper;
                        ++state.m_stats.m_orderCacheHits;
                        ++state.m_stats.m_orderCacheSharedHits;
                        ++state.m_stats.m_schedulePlans;
                        if (tailFuncps) ++state.m_stats.m_artifactTailReuses;
                        return plan;
                    } else {
                        ++state.m_stats.m_orderCacheSharedSkipOther;
                        plan = SubgraphSchedulePlan{};
                    }
                }
            }
        }
    }
    if (!funcp) {
        if (canShare) ++state.m_stats.m_orderCacheMisses;
        const std::string orderTag = tag + "_subgraph_" + cvtToStr(subgraphIndex++);
        const uint64_t orderStartUsecs = statStartUsecs();
        funcp = V3Order::order(netlistp, {&subgraphLogic}, trigToSen, orderTag, false, slow,
                               externalDomains, group.m_scopep);
        const uint64_t orderUsecs = orderStartUsecs ? V3Os::timeUsecs() - orderStartUsecs : 0;
        state.m_stats.m_timeInternalOrderUsecs += orderUsecs;
        state.m_stats.noteInternalOrderTime(orderTag, orderUsecs);
        if (funcp) {
            util::splitCheck(funcp);
            if (cacheableArtifact
                && !SubgraphLoweringState::parameterizeSharedHelper(
                    funcp, group.m_scopep, logicSig, helperArgs, state.m_stats)) {
                cacheableArtifact = false;
            }
            if (cacheableArtifact && !matchedOrderCacheEntryp) {
                const SubgraphTriggeredRefInfo triggeredInfo
                    = SubgraphLoweringState::analyzeOrderedFuncTriggeredRefs(funcp, group.m_scopep,
                                                                             state.m_stats);
                const bool triggeredShareable
                    = SubgraphLoweringState::canShareTriggeredArtifact(triggeredInfo);
                const bool triggeredCloneable
                    = SubgraphLoweringState::canCloneTriggeredOrderCacheEntry(triggeredInfo);
                std::vector<SubgraphOrderCacheEntry>& entries
                    = state.m_subgraphOrderCache[cacheKey];
                if (entries.empty()) ++state.m_stats.m_orderCacheVariantBuckets;
                entries.push_back(SubgraphOrderCacheEntry{
                    nullptr, funcp, logicSig, triggeredCloneable, triggeredShareable,
                    triggeredInfo.m_writesDelayedShadow, triggeredInfo.m_writesInstanceLocal,
                    triggeredInfo.m_writesLocalTemp, triggeredInfo.m_writesNonLocal,
                    triggeredInfo.m_writesTriggerTemp, triggeredInfo.m_writesVlemTemp});
                insertedOrderCacheEntryp = &entries.back();
                ++state.m_stats.m_orderCacheEntries;
                state.m_stats.m_orderCacheVariantMax
                    = std::max<uint64_t>(state.m_stats.m_orderCacheVariantMax, entries.size());
            }
        }
    }
    if (!funcp) return plan;

    AstCFunc* callFuncp = funcp;
    if (tag == "stl") {
        AstCFunc* const tailFuncp = cloneUnguardedFuncBody(funcp, group.m_scopep, "__tail", slow);
        state.m_stlSubgraphFuncs[group.m_scopep].push_back(tailFuncp);
        callFuncp = tailFuncp;
    }
    const SubgraphTriggeredRefInfo triggeredInfo
        = SubgraphLoweringState::analyzeOrderedFuncTriggeredRefs(callFuncp, group.m_scopep,
                                                                 state.m_stats);
    const bool cloneableArtifact
        = SubgraphLoweringState::canCloneTriggeredOrderCacheEntry(triggeredInfo);
    const bool triggeredShareableArtifact
        = SubgraphLoweringState::canShareTriggeredArtifact(triggeredInfo);
    if (triggeredInfo.m_hasTriggered) {
        ++state.m_stats.m_triggeredArtifactCandidates;
        if (!triggeredInfo.m_shareable) ++state.m_stats.m_triggeredArtifactUnshareable;
        if (triggeredInfo.m_writesInstanceLocal) {
            ++state.m_stats.m_triggeredArtifactWritesInstanceLocal;
        }
        if (triggeredInfo.m_writesDelayedShadow) {
            ++state.m_stats.m_triggeredArtifactWritesDelayedShadow;
        }
        if (triggeredInfo.m_writesLocalTemp) ++state.m_stats.m_triggeredArtifactWritesLocalTemp;
        if (triggeredInfo.m_writesNonLocal) ++state.m_stats.m_triggeredArtifactWritesNonLocal;
        if (triggeredInfo.m_writesTriggerTemp) {
            ++state.m_stats.m_triggeredArtifactWritesTriggerTemp;
        }
        if (triggeredInfo.m_writesVlemTemp) ++state.m_stats.m_triggeredArtifactWritesVlemTemp;
        if (!triggeredInfo.m_writesNonLocal) {
            ++state.m_stats.m_triggeredArtifactNoNonLocalWrites;
            if (triggeredInfo.m_writesInstanceLocal) {
                ++state.m_stats.m_triggeredArtifactNoNonLocalInstanceLocalWrites;
            }
        }
        if (bundleContext.m_currentScopeTailContract.m_writesBoundaryInput) {
            ++state.m_stats.m_triggeredArtifactInputTailWrites;
        }
        if (triggeredShareableArtifact
            && !bundleContext.m_currentScopeTailContract.m_writesBoundaryInput) {
            ++state.m_stats.m_triggeredArtifactShareable;
        }
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
    SubgraphLoweringState::populateSharedHelperArgs(plan.m_instance, *plan.m_artifactp,
                                                    group.m_scopep, {});
    populateSubgraphScheduleInstanceContract(plan.m_instance, state);
    plan.m_phase = phase;
    plan.m_wrapper = wrapper;
    ++state.m_stats.m_schedulePlans;
    return plan;
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
    callp->dtypeSetVoid();
    AstNodeStmt* stmtsp = callp->makeStmt();
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
