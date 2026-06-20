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

#include "V3Stats.h"
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

namespace {

enum class SubgraphWrapperKind : uint8_t {
    Always,
    AlwaysObserved,
    AlwaysPost,
    AlwaysPre,
    AlwaysReactive,
    InitialAutomatic,
    Stmt
};

struct SubgraphWrapper final {
    SubgraphWrapperKind m_kind = SubgraphWrapperKind::Stmt;
    VAlwaysKwd m_keyword = VAlwaysKwd::ALWAYS;
};

bool operator==(const SubgraphWrapper& lhs, const SubgraphWrapper& rhs) {
    return lhs.m_kind == rhs.m_kind && lhs.m_keyword == rhs.m_keyword;
}

SubgraphWrapper wrapperFromLogic(AstNode* nodep) {
    SubgraphWrapper result;
    AstNodeProcedure* const origp = VN_CAST(nodep, NodeProcedure);
    if (const AstAlways* const alwaysp = VN_CAST(origp, Always)) {
        result.m_kind = SubgraphWrapperKind::Always;
        result.m_keyword = alwaysp->keyword();
    } else if (VN_IS(origp, AlwaysObserved)) {
        result.m_kind = SubgraphWrapperKind::AlwaysObserved;
    } else if (VN_IS(origp, AlwaysPost)) {
        result.m_kind = SubgraphWrapperKind::AlwaysPost;
    } else if (VN_IS(origp, AlwaysPre)) {
        result.m_kind = SubgraphWrapperKind::AlwaysPre;
    } else if (VN_IS(origp, AlwaysReactive)) {
        result.m_kind = SubgraphWrapperKind::AlwaysReactive;
    } else if (VN_IS(origp, InitialAutomatic)) {
        result.m_kind = SubgraphWrapperKind::InitialAutomatic;
    }
    return result;
}

AstNode* makeSubgraphWrapperLogic(FileLine* flp, const SubgraphWrapper& wrapper,
                                  AstNodeStmt* callp) {
    if (wrapper.m_kind == SubgraphWrapperKind::Always) {
        return new AstAlways{flp, wrapper.m_keyword, nullptr, callp};
    }
    if (wrapper.m_kind == SubgraphWrapperKind::AlwaysPre) {
        AstAlwaysPre* const procp = new AstAlwaysPre{flp};
        procp->addStmtsp(callp);
        return procp;
    }
    if (wrapper.m_kind == SubgraphWrapperKind::AlwaysPost) {
        AstAlwaysPost* const procp = new AstAlwaysPost{flp};
        procp->addStmtsp(callp);
        return procp;
    }
    if (wrapper.m_kind == SubgraphWrapperKind::AlwaysObserved) {
        return new AstAlwaysObserved{flp, nullptr, callp};
    }
    if (wrapper.m_kind == SubgraphWrapperKind::AlwaysReactive) {
        return new AstAlwaysReactive{flp, nullptr, callp};
    }
    if (wrapper.m_kind == SubgraphWrapperKind::InitialAutomatic) {
        return new AstInitialAutomatic{flp, callp};
    }
    return callp;
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

struct WrapperActiveKey final {
    LogicByScope* m_ownerp = nullptr;
    AstSenTree* m_senTreep = nullptr;

    bool operator==(const WrapperActiveKey& other) const {
        return m_ownerp == other.m_ownerp && m_senTreep == other.m_senTreep;
    }
};

struct WrapperActiveKeyHash final {
    size_t operator()(const WrapperActiveKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_ownerp);
        hash ^= std::hash<const void*>{}(key.m_senTreep) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                + (hash >> 2);
        return hash;
    }
};

struct WrapperActiveEntry final {
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
    uintptr_t m_type = 0;
    std::vector<SubgraphLogicRefSig> m_refs;
};

using SubgraphLogicSig = std::vector<SubgraphLogicNodeSig>;

struct SubgraphOrderCacheEntry final {
    AstCFunc* m_funcp = nullptr;
    SubgraphLogicSig m_logicSig;
    bool m_cloneable = true;
};

struct SubgraphOrderCacheKey final {
    std::vector<uintptr_t> m_domainShape;
    AstNodeModule* m_modp = nullptr;
    AstSenTree* m_senTreep = nullptr;
    bool m_isEarly = false;

    bool operator==(const SubgraphOrderCacheKey& other) const {
        return m_modp == other.m_modp && m_senTreep == other.m_senTreep
               && m_isEarly == other.m_isEarly && m_domainShape == other.m_domainShape;
    }
};

struct SubgraphOrderCacheKeyHash final {
    size_t operator()(const SubgraphOrderCacheKey& key) const {
        size_t hash = std::hash<const void*>{}(key.m_modp);
        hash ^= std::hash<const void*>{}(key.m_senTreep) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                + (hash >> 2);
        hash ^= std::hash<bool>{}(key.m_isEarly) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                + (hash >> 2);
        for (const uintptr_t value : key.m_domainShape) {
            hash ^= std::hash<uintptr_t>{}(value) + 0x9e3779b97f4a7c15ULL + (hash << 6)
                    + (hash >> 2);
        }
        return hash;
    }
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

struct SubgraphLoweringStats final {
    uint64_t m_orderCacheCloneFailOther = 0;
    uint64_t m_orderCacheCloneFailState = 0;
    uint64_t m_orderCacheCloneFailShadow = 0;
    uint64_t m_orderCacheCloneFailTemp = 0;
    uint64_t m_orderCacheCloneFailVlem = 0;
    std::map<string, uint64_t> m_orderCacheCloneFailNames;
    uint64_t m_callUsageSummaries = 0;
    uint64_t m_groups = 0;
    uint64_t m_orderCacheEntries = 0;
    uint64_t m_orderCacheHits = 0;
    uint64_t m_orderCacheMisses = 0;
    uint64_t m_orderCacheSkipTriggered = 0;
    uint64_t m_orderedFuncClones = 0;
    uint64_t m_snapshotBuckets = 0;
    uint64_t m_snapshotBundleElems = 0;
    uint64_t m_snapshotBundles = 0;
    uint64_t m_snapshotHelpers = 0;
    uint64_t m_snapshotProcedures = 0;
    uint64_t m_snapshotScalars = 0;
    uint64_t m_snapshotSources = 0;
    uint64_t m_tailClones = 0;
    uint64_t m_wrapperInstances = 0;

    void report(const string& tag) const {
        if (!v3Global.opt.stats()) return;
        const string prefix = "Scheduling, Subgraph " + tag + ", ";
        V3Stats::addStat(prefix + "call usage summaries", m_callUsageSummaries);
        V3Stats::addStat(prefix + "groups", m_groups);
        V3Stats::addStat(prefix + "order cache clone fail other", m_orderCacheCloneFailOther);
        V3Stats::addStat(prefix + "order cache clone fail state", m_orderCacheCloneFailState);
        V3Stats::addStat(prefix + "order cache clone fail shadow", m_orderCacheCloneFailShadow);
        V3Stats::addStat(prefix + "order cache clone fail temp", m_orderCacheCloneFailTemp);
        V3Stats::addStat(prefix + "order cache clone fail vlem", m_orderCacheCloneFailVlem);
        for (const auto& itr : m_orderCacheCloneFailNames) {
            V3Stats::addStat(prefix + "order cache clone fail name " + itr.first, itr.second);
        }
        V3Stats::addStat(prefix + "order cache entries", m_orderCacheEntries);
        V3Stats::addStat(prefix + "order cache hits", m_orderCacheHits);
        V3Stats::addStat(prefix + "order cache misses", m_orderCacheMisses);
        V3Stats::addStat(prefix + "order cache skip triggered", m_orderCacheSkipTriggered);
        V3Stats::addStat(prefix + "ordered function clones", m_orderedFuncClones);
        V3Stats::addStat(prefix + "snapshot buckets", m_snapshotBuckets);
        V3Stats::addStat(prefix + "snapshot bundle elements", m_snapshotBundleElems);
        V3Stats::addStat(prefix + "snapshot bundles", m_snapshotBundles);
        V3Stats::addStat(prefix + "snapshot helpers", m_snapshotHelpers);
        V3Stats::addStat(prefix + "snapshot procedures", m_snapshotProcedures);
        V3Stats::addStat(prefix + "snapshot scalars", m_snapshotScalars);
        V3Stats::addStat(prefix + "snapshot sources", m_snapshotSources);
        V3Stats::addStat(prefix + "tail clones", m_tailClones);
        V3Stats::addStat(prefix + "wrapper instances", m_wrapperInstances);
    }

    void noteOrderCacheCloneFailName(const string& name) {
        if (m_orderCacheCloneFailNames.size() >= 16 && !m_orderCacheCloneFailNames.count(name))
            return;
        ++m_orderCacheCloneFailNames[name];
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
            logicp->foreach([&](AstVarRef* refp) {
                nodeSig.m_refs.push_back(
                    {static_cast<uintptr_t>(refp->access()), refp->varScopep()});
            });
        });
        return result;
    }

    static bool
    buildTemplateVarScopeMap(const SubgraphLogicSig& templateSig, const LogicByScope& currentLogic,
                             std::unordered_map<const AstVarScope*, AstVarScope*>& result) {
        std::vector<AstNode*> currentNodes;
        currentLogic.foreachLogic([&](AstNode* logicp) { currentNodes.push_back(logicp); });
        if (templateSig.size() != currentNodes.size()) return false;

        for (size_t i = 0; i < templateSig.size(); ++i) {
            const SubgraphLogicNodeSig& templateNode = templateSig[i];
            AstNode* const currentNodep = currentNodes[i];
            if (templateNode.m_type != static_cast<uintptr_t>(currentNodep->type())) {
                return false;
            }

            std::vector<AstVarRef*> currentRefs;
            currentNodep->foreach([&](AstVarRef* refp) { currentRefs.push_back(refp); });
            if (templateNode.m_refs.size() != currentRefs.size()) return false;

            for (size_t j = 0; j < templateNode.m_refs.size(); ++j) {
                const SubgraphLogicRefSig& templateRef = templateNode.m_refs[j];
                AstVarRef* const currentRefp = currentRefs[j];
                if (templateRef.m_access != static_cast<uintptr_t>(currentRefp->access())) {
                    return false;
                }
                AstVarScope* const currentVscp = currentRefp->varScopep();
                const auto it = result.find(templateRef.m_vscp);
                if (it != result.end()) {
                    if (it->second != currentVscp) return false;
                } else {
                    result.emplace(templateRef.m_vscp, currentVscp);
                }
            }
        }
        return true;
    }

    static bool canShareSubgraphLogic(const LogicByScope& logic, AstScope* boundaryScopep) {
        bool shareable = true;
        logic.foreachLogic([&](AstNode* logicp) {
            if (!shareable) return;
            logicp->foreach([&](AstVarRef* refp) {
                if (!shareable) return;
                if (!isUnderBoundaryScope(refp->varScopep()->scopep(), boundaryScopep)) {
                    shareable = false;
                }
            });
        });
        return shareable;
    }

    static bool orderedFuncHasTriggeredRefs(AstCFunc* funcp) {
        std::unordered_set<AstCFunc*> seenFuncs;
        bool found = false;
        std::function<void(AstCFunc*)> gather = [&](AstCFunc* scanFuncp) {
            if (found || !seenFuncs.insert(scanFuncp).second) return;
            scanFuncp->foreach([&](AstVarRef* refp) {
                if (found) return;
                const string& name = refp->varp()->name();
                if (name.rfind("__V", 0) == 0 && name.size() >= 9
                    && name.substr(name.size() - 9) == "Triggered") {
                    found = true;
                }
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                if (found) return;
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint()) return;
                gather(calledFuncp);
            });
        };
        gather(funcp);
        return found;
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
            if (name.rfind("__PVT__", 0) == 0) {
                ++stats.m_orderCacheCloneFailState;
            } else if (name.rfind("__Vdly", 0) == 0 || name.rfind("__Vtrig", 0) == 0) {
                ++stats.m_orderCacheCloneFailShadow;
            } else if (name.rfind("__Vfunc", 0) == 0 || name.rfind("__Vtemp", 0) == 0
                       || name.rfind("__Vcell", 0) == 0) {
                ++stats.m_orderCacheCloneFailTemp;
            } else if (name.rfind("__VlemCall", 0) == 0) {
                ++stats.m_orderCacheCloneFailVlem;
            } else {
                ++stats.m_orderCacheCloneFailOther;
            }
        };

        std::unordered_map<const AstVarScope*, AstVarScope*> resolvedVarMap = templateVarMap;
        std::unordered_map<const AstCFunc*, std::unordered_set<const AstVar*>> argVarsByFunc;
        for (AstCFunc* const origFuncp : orderedFuncs) {
            std::unordered_set<const AstVar*>& argVars = argVarsByFunc[origFuncp];
            for (AstVar* argp = origFuncp->argsp(); argp; argp = VN_AS(argp->nextp(), Var)) {
                argVars.insert(argp);
            }
            bool failed = false;
            origFuncp->foreach([&](AstVarRef* refp) {
                if (failed) return;
                if (argVars.count(refp->varp())) return;
                const AstVarScope* const sourceVscp = refp->varScopep();
                if (resolvedVarMap.find(sourceVscp) == resolvedVarMap.end()) {
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
            clonep->cname(origFuncp->cname());
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
            bodyp->foreach([&](AstCCall* callp) {
                if (failed) return;
                const auto it = clonedFuncs.find(callp->funcp());
                if (it == clonedFuncs.end()) return;
                callp->funcp(it->second);
            });
            bodyp->foreach([&](AstVarRef* refp) {
                if (failed) return;
                const auto argIt = clonedArgVscps.find(refp->varp());
                if (argIt != clonedArgVscps.end()) {
                    refp->varp(argIt->second->varp());
                    refp->varScopep(argIt->second);
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
            });
            if (failed) {
                VL_DO_DANGLING(bodyp->deleteTree(), bodyp);
                return nullptr;
            }
            clonedFuncp->addStmtsp(bodyp);
        }

        return clonedFuncs.at(funcp);
    }

    static std::vector<SubgraphCallUsageSummary>
    buildSubgraphCallUsageSummary(AstCFunc* funcp, AstScope* boundaryScopep) {
        std::vector<SubgraphCallUsageSummary> uses;
        std::unordered_map<AstVarScope*, size_t> useIndices;
        std::unordered_set<AstCFunc*> seen;
        std::function<void(AstCFunc*)> gather = [&](AstCFunc* scanFuncp) {
            if (!seen.insert(scanFuncp).second) return;
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const vscp = refp->varScopep();
                const bool externalToSubgraph
                    = !isUnderBoundaryScope(vscp->scopep(), boundaryScopep);
                if (!externalToSubgraph) return;
                const auto pair = useIndices.emplace(vscp, uses.size());
                if (pair.second) uses.push_back(SubgraphCallUsageSummary{vscp, false, false});
                SubgraphCallUsageSummary& use = uses[pair.first->second];
                use.m_read |= refp->access().isReadOrRW();
                use.m_write |= refp->access().isWriteOrRW();
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint()) return;
                gather(calledFuncp);
            });
        };
        gather(funcp);
        return uses;
    }

    void registerSubgraphCallUsageSummary(AstCFunc* funcp, AstScope* boundaryScopep) {
        V3Sched::registerSubgraphCallUsageSummary(
            funcp, buildSubgraphCallUsageSummary(funcp, boundaryScopep));
        ++m_stats.m_callUsageSummaries;
    }

    SnapshotBucket& getSnapshotBucket(LogicByScope* ownerp, AstSenTree* senTreep) {
        for (SnapshotBucket& bucket : m_snapshotBuckets) {
            if (bucket.m_ownerp == ownerp && bucket.m_senTreep == senTreep) return bucket;
        }
        m_snapshotBuckets.emplace_back();
        SnapshotBucket& bucket = m_snapshotBuckets.back();
        bucket.m_ownerp = ownerp;
        bucket.m_senTreep = senTreep;
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

        if (sourceScopep != boundaryScopep && boundaryScopeFor(sourceScopep)) return true;

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
    std::unordered_map<SubgraphOrderCacheKey, SubgraphOrderCacheEntry, SubgraphOrderCacheKeyHash>
        m_subgraphOrderCache;
    std::vector<SnapshotBucket> m_snapshotBuckets;
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

SubgraphWrapper lateWrapperForGroup(const SubgraphGroup& group) {
    if (group.m_hasNonPostLate) return group.m_lateWrapper;
    if (group.m_hasPost) return SubgraphWrapper{SubgraphWrapperKind::AlwaysPost};
    return wrapperFromLogic(group.m_lateLogic.front().second->stmtsp());
}

void populateSubgraphInstanceContract(AstSubgraphInstance* subgraphp, AstScope* scopep) {
    if (const auto* const summaryp = V3SubgraphSummary::getScopeSummary(scopep)) {
        const V3SubgraphSummary::ParentStubContract& contract = summaryp->m_parentStub;
        subgraphp->hasClockedState(contract.m_hasClockedState);
        subgraphp->hasPostPhase(contract.m_hasPostPhase);
        subgraphp->readsExternalVars(contract.m_readsExternalVars);
        for (AstVarScope* const vscp : contract.m_boundaryReads) {
            subgraphp->addBoundaryRead(vscp, V3SubgraphSummary::isDerivedBoundaryInput(vscp));
        }
        for (AstVarScope* const vscp : contract.m_boundaryWrites) {
            subgraphp->addBoundaryWrite(vscp);
        }
    }
}

void appendSubgraphExternalUses(AstSubgraphInstance* subgraphp, AstScope* boundaryScopep,
                                AstCFunc* funcp,
                                std::unordered_map<AstVarScope*, size_t>& useIndices) {
    const auto* const summarysp = V3Sched::getSubgraphCallUsageSummary(funcp);
    if (!summarysp) return;
    for (const SubgraphCallUsageSummary& summary : *summarysp) {
        AstVarScope* const vscp = summary.m_varscp;
        if (vscp && !SubgraphLoweringState::isUnderBoundaryScope(vscp->scopep(), boundaryScopep)) {
            const auto pair = useIndices.emplace(vscp, useIndices.size());
            if (pair.second) subgraphp->addExternalUse(vscp, false, false);
            AstSubgraphInstance::ExternalUseContract& use
                = subgraphp->externalUses()[pair.first->second];
            use.m_read |= summary.m_read;
            use.m_write |= summary.m_write;
        }
    }
}

AstSubgraphInstance::Phase subgraphPhaseFor(const SubgraphWrapper& wrapper, bool isEarly) {
    if (wrapper.m_kind == SubgraphWrapperKind::AlwaysPre || isEarly) {
        return AstSubgraphInstance::Phase::PRE;
    }
    return AstSubgraphInstance::Phase::POST;
}

AstActive* getOrCreateWrapperActive(const SubgraphGroup& group,
                                    std::unordered_map<WrapperActiveKey, WrapperActiveEntry,
                                                       WrapperActiveKeyHash>& wrapperActives) {
    const WrapperActiveKey key{group.m_ownerp, group.m_senTreep};
    const auto it = wrapperActives.find(key);
    if (it != wrapperActives.end()) return it->second.m_activep;
    AstActive* const activep = new AstActive{group.m_flp, "subgraph", group.m_senTreep};
    wrapperActives.emplace(key, WrapperActiveEntry{group.m_scopep, activep});
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
    activep->addStmtsp(makeSubgraphWrapperLogic(group.m_flp, wrapper, subgraphp));
    batches.emplace(key, subgraphp);
    ++state.m_stats.m_wrapperInstances;
    return subgraphp;
}

void lowerSubgraphActiveGroup(
    AstNetlist* netlistp, LogicByScope& subgraphLogic, const SubgraphWrapper& wrapper,
    bool isEarly, const std::vector<AstCFunc*>* tailFuncps, const SubgraphGroup& group,
    SubgraphLoweringState& state, const V3Order::TrigToSenMap& trigToSen, const std::string& tag,
    bool slow, const V3Order::ExternalDomainsProvider& externalDomains, unsigned& subgraphIndex,
    AstActive* wrapperActivep,
    std::unordered_map<SubgraphBatchKey, AstSubgraphInstance*, SubgraphBatchKeyHash>& batches) {
    if (subgraphLogic.empty()) return;
    const bool canShare
        = SubgraphLoweringState::canShareSubgraphLogic(subgraphLogic, group.m_scopep);
    SubgraphOrderCacheKey cacheKey;
    cacheKey.m_domainShape = SubgraphLoweringState::computeDomainShape(
        subgraphLogic, group.m_scopep, externalDomains);
    cacheKey.m_modp = group.m_scopep->modp();
    cacheKey.m_senTreep = group.m_senTreep;
    cacheKey.m_isEarly = isEarly;
    AstCFunc* funcp = nullptr;
    if (canShare) {
        const auto cacheIt = state.m_subgraphOrderCache.find(cacheKey);
        if (cacheIt != state.m_subgraphOrderCache.end()) {
            if (!cacheIt->second.m_cloneable) {
                ++state.m_stats.m_orderCacheSkipTriggered;
            } else {
                std::unordered_map<const AstVarScope*, AstVarScope*> templateVarMap;
                if (SubgraphLoweringState::buildTemplateVarScopeMap(
                        cacheIt->second.m_logicSig, subgraphLogic, templateVarMap)) {
                    funcp = SubgraphLoweringState::cloneOrderedFuncGraph(
                        cacheIt->second.m_funcp, group.m_scopep, templateVarMap, state.m_stats);
                    if (funcp) {
                        ++state.m_stats.m_orderCacheHits;
                        ++state.m_stats.m_orderedFuncClones;
                        SubgraphLoweringState::discardLogic(subgraphLogic);
                    }
                }
            }
        }
    }
    if (!funcp) {
        if (canShare) ++state.m_stats.m_orderCacheMisses;
        SubgraphLogicSig logicSig;
        if (canShare) logicSig = SubgraphLoweringState::buildLogicSig(subgraphLogic);
        funcp = V3Order::order(netlistp, {&subgraphLogic}, trigToSen,
                               tag + "_subgraph_" + cvtToStr(subgraphIndex++), false, slow,
                               externalDomains, group.m_scopep);
        if (funcp) {
            util::splitCheck(funcp);
            state.registerSubgraphCallUsageSummary(funcp, group.m_scopep);
            if (canShare
                && state.m_subgraphOrderCache.find(cacheKey) == state.m_subgraphOrderCache.end()) {
                state.m_subgraphOrderCache.emplace(
                    cacheKey, SubgraphOrderCacheEntry{
                                  funcp, std::move(logicSig),
                                  !SubgraphLoweringState::orderedFuncHasTriggeredRefs(funcp)});
                ++state.m_stats.m_orderCacheEntries;
            }
        }
    }
    if (!funcp) return;

    AstCFunc* callFuncp = funcp;
    if (tag == "stl") {
        AstCFunc* const tailFuncp = cloneUnguardedFuncBody(funcp, group.m_scopep, "__tail", slow);
        state.registerSubgraphCallUsageSummary(tailFuncp, group.m_scopep);
        state.m_stlSubgraphFuncs[group.m_scopep].push_back(tailFuncp);
        callFuncp = tailFuncp;
    }
    AstNodeStmt* stmtsp = util::callVoidFunc(callFuncp);
    if (tailFuncps) {
        for (AstCFunc* const tailFuncp : *tailFuncps) {
            stmtsp->addNext(util::callVoidFunc(tailFuncp));
        }
    }
    AstSubgraphInstance* const subgraphp
        = getOrCreateSubgraphBatch(group, wrapper, isEarly, wrapperActivep, batches, state);
    subgraphp->addStmtsp(stmtsp);
    populateSubgraphInstanceContract(subgraphp, group.m_scopep);
    {
        std::unordered_map<AstVarScope*, size_t> useIndices;
        appendSubgraphExternalUses(subgraphp, group.m_scopep, callFuncp, useIndices);
        if (tailFuncps) {
            for (AstCFunc* const tailFuncp : *tailFuncps) {
                appendSubgraphExternalUses(subgraphp, group.m_scopep, tailFuncp, useIndices);
            }
        }
    }
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

void emitSnapshotProcedureForBucket(const SnapshotBucket& bucket, SubgraphLoweringState& state,
                                    bool slow) {
    static unsigned s_snapshotHelperIndex = 0;
    AstScope* const topScopep = v3Global.rootp()->topScopep()->scopep();
    if (bucket.m_sourceVars.empty()) return;
    ++state.m_stats.m_snapshotProcedures;
    AstAlways* const procp = new AstAlways{bucket.m_sourceVars.front()->fileline(),
                                           VAlwaysKwd::ALWAYS, nullptr, nullptr};
    std::unordered_map<AstScope*, std::vector<AstVarScope*>> localBoundarySources;
    for (AstVarScope* const sourceVscp : bucket.m_sourceVars) {
        if (sourceVscp->scopep()->modp()->subgraphBoundary() && sourceVscp->varp()->isIO()
            && sourceVscp->varp()->direction().isNonOutput()) {
            localBoundarySources[sourceVscp->scopep()].push_back(sourceVscp);
        }
    }
    for (const auto& pair : localBoundarySources) {
        AstScope* const boundaryScopep = pair.first;
        const std::vector<AstVarScope*>& sourceVscps = pair.second;
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

        AstCCall* const callp = new AstCCall{flp, funcp};
        callp->dtypeSetVoid();
        for (size_t i = 0; i < sourceVscps.size(); ++i) {
            AstVarScope* const sourceVscp = sourceVscps[i];
            const SnapshotRef& snapshotRef
                = state.getSnapshotRef(bucket.m_ownerp, bucket.m_senTreep, sourceVscp);
            AstVarScope* const outArgVscp = newSnapshotHelperArg(
                funcp, sourceVscp->dtypep(), "out" + cvtToStr(i), VDirection::OUTPUT);
            AstVarScope* const inArgVscp = newSnapshotHelperArg(
                funcp, sourceVscp->dtypep(), "in" + cvtToStr(i), VDirection::CONSTREF);
            funcp->addStmtsp(new AstAssign{flp, new AstVarRef{flp, outArgVscp, VAccess::WRITE},
                                           new AstVarRef{flp, inArgVscp, VAccess::READ}});
            callp->addArgsp(
                SubgraphLoweringState::makeSnapshotExpr(snapshotRef, flp, VAccess::WRITE));
            callp->addArgsp(new AstVarRef{flp, sourceVscp, VAccess::READ});
        }
        procp->addStmtsp(callp->makeStmt());
    }
    for (AstVarScope* const sourceVscp : bucket.m_sourceVars) {
        if (localBoundarySources.count(sourceVscp->scopep())
            && sourceVscp->scopep()->modp()->subgraphBoundary() && sourceVscp->varp()->isIO()
            && sourceVscp->varp()->direction().isNonOutput()) {
            continue;
        }
        const SnapshotRef& snapshotRef
            = state.getSnapshotRef(bucket.m_ownerp, bucket.m_senTreep, sourceVscp);
        procp->addStmtsp(
            new AstAssign{sourceVscp->fileline(),
                          SubgraphLoweringState::makeSnapshotExpr(
                              snapshotRef, sourceVscp->fileline(), VAccess::WRITE),
                          new AstVarRef{sourceVscp->fileline(), sourceVscp, VAccess::READ}});
    }
    V3Sched::rememberSubgraphSnapshotProcedure(procp);
    bucket.m_ownerp->add(topScopep, bucket.m_senTreep, procp);
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
    for (SnapshotBucket& bucket : state.m_snapshotBuckets) {
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
                const string name = "__VsubgraphSnapshot__" + sourceVscp->scopep()->nameDotless()
                                    + "__" + sourceVscp->varp()->shortName();
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
            const string bundleName = "__VsubgraphSnapshot__" + group.m_scopep->nameDotless()
                                      + "__bundle" + cvtToStr(bundleIndex++);
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
    std::unordered_map<WrapperActiveKey, WrapperActiveEntry, WrapperActiveKeyHash> wrapperActives;
    std::unordered_map<SubgraphBatchKey, AstSubgraphInstance*, SubgraphBatchKeyHash> batches;
    for (SubgraphGroup& group : groups) {
        AstActive* const wrapperActivep = getOrCreateWrapperActive(group, wrapperActives);
        if (!group.m_earlyLogic.empty()) {
            lowerSubgraphActiveGroup(netlistp, group.m_earlyLogic,
                                     wrapperFromLogic(group.m_earlyLogic.front().second->stmtsp()),
                                     true, nullptr, group, state, trigToSen, tag, slow,
                                     externalDomains, subgraphIndex, wrapperActivep, batches);
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
                            if (activeTailFuncp != tailFuncp) {
                                state.registerSubgraphCallUsageSummary(activeTailFuncp,
                                                                       group.m_scopep);
                            }
                        }
                        tailFuncps = &activeTailFuncps;
                    } else {
                        tailFuncps = &it->second;
                    }
                }
            }
            lowerSubgraphActiveGroup(netlistp, group.m_lateLogic, wrapper, false, tailFuncps,
                                     group, state, trigToSen, tag, slow, externalDomains,
                                     subgraphIndex, wrapperActivep, batches);
        }
    }
    for (const auto& pair : wrapperActives) {
        AstActive* const wrapperActivep = pair.second.m_activep;
        if (wrapperActivep->stmtsp()) {
            pair.first.m_ownerp->emplace_back(pair.second.m_scopep, wrapperActivep);
        } else {
            wrapperActivep->deleteTree();
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
        collectSubgraphGroups(m_logic, m_groups);
        m_state.m_stats.m_groups = m_groups.size();
    }

    void run() {
        if (m_state.m_snapshotCrossBoundaryReads) collectRegionWrittenVars(m_logic, m_state);
        if (m_state.m_snapshotCrossBoundaryReads) {
            prepareSubgraphSnapshots(m_groups, m_state, m_tag);
            m_state.m_stats.m_snapshotBuckets = m_state.m_snapshotBuckets.size();
            for (const SnapshotBucket& bucket : m_state.m_snapshotBuckets) {
                m_state.m_stats.m_snapshotSources += bucket.m_sourceVars.size();
            }
        }

        lowerSubgraphGroups(m_netlistp, m_groups, m_state, m_trigToSen, m_tag, m_slow,
                            m_externalDomains);

        for (const SnapshotBucket& bucket : m_state.m_snapshotBuckets) {
            emitSnapshotProcedureForBucket(bucket, m_state, m_slow);
        }
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
