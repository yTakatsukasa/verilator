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

#include "V3Hash.h"
#include "V3Stats.h"
#include "V3SubgraphContract.h"

#include <unordered_map>
#include <unordered_set>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3Sched {

namespace {

bool isGlobalTriggerState(const AstVarScope* vscp);

struct SubgraphSnapshot final {
    AstVarScope* m_sourceVscp = nullptr;
    AstVarScope* m_storageVscp = nullptr;
};

struct SubgraphLocalTriggerId final {
    uint32_t m_value = 0;

    bool operator==(const SubgraphLocalTriggerId& rhs) const { return m_value == rhs.m_value; }
};

struct SubgraphInstanceTriggerBinding final {
    SubgraphLocalTriggerId m_id;
    AstSenTree* m_senTreep = nullptr;
    const AstSenTree* m_parentDomainp = nullptr;

    AstSenTree* bind(SubgraphLocalTriggerId id) const {
        UASSERT(id == m_id, "Unknown subgraph-local trigger ID");
        UASSERT(m_senTreep && m_parentDomainp, "Incomplete subgraph trigger binding");
        return m_senTreep;
    }

    const AstSenTree* parentDomain(SubgraphLocalTriggerId id) const {
        bind(id);
        return m_parentDomainp;
    }

    AstVarScope* triggerStateVscp(SubgraphLocalTriggerId id) const {
        AstSenTree* const senTreep = bind(id);
        AstVarScope* resultp = nullptr;
        senTreep->foreach([&](AstNodeVarRef* refp) {
            AstVarScope* const vscp = refp->varScopep();
            if (!isGlobalTriggerState(vscp)) return;
            UASSERT_OBJ(!resultp || resultp == vscp, refp,
                        "Subgraph instance trigger uses multiple trigger vectors");
            resultp = vscp;
        });
        UASSERT_OBJ(resultp, senTreep, "Subgraph instance trigger has no trigger vector");
        return resultp;
    }
};

struct SubgraphGroup final {
    AstScope* m_boundaryScopep = nullptr;
    AstSenTree* m_senTreep = nullptr;
    const AstSenTree* m_domainKeyp = nullptr;  // Original, unremapped event domain
    FileLine* m_filelinep = nullptr;
    LogicByScope* m_ownerp = nullptr;
    LogicByScope m_preLogic;
    LogicByScope m_postLogic;
    LogicByScope m_refreshLogic;
    std::vector<SubgraphSnapshot> m_snapshots;
};

struct PendingSubgraphMaterialization final {
    AstSubgraphInstance* m_instancep = nullptr;
    const SubgraphGroup* m_groupp = nullptr;
    std::unique_ptr<V3SubgraphContract> m_contractp;
    size_t m_artifactIndex = std::numeric_limits<size_t>::max();
    size_t m_externalBindingsBegin = 0;
    size_t m_externalBindingsCount = 0;
};

bool isUnderScope(const AstScope* scopep, const AstScope* basep);

bool isDpiCallTarget(const AstCFunc* funcp) {
    return funcp
           && (funcp->dpiContext() || funcp->dpiExportDispatcher() || funcp->dpiExportImpl()
               || funcp->dpiImportPrototype() || funcp->dpiImportWrapper());
}

bool isSafeSharedCallTarget(const AstCFunc* funcp, const AstScope* boundaryScopep) {
    return funcp && funcp->scopep() == boundaryScopep && !funcp->isStatic() && !funcp->funcPublic()
           && !isDpiCallTarget(funcp) && !funcp->isConstructor() && !funcp->isDestructor()
           && !funcp->isVirtual() && !funcp->needProcess() && !funcp->recursive()
           && !funcp->isCoroutine() && !funcp->isCovergroupSample();
}

bool isTaskCallTemp(const AstVarScope* vscp) {
    // V3Task creates per-call result/argument temporaries with instance-specific declarations.
    // They are local to the call sequence, so compare them by traversal slot like function locals.
    const string& name = vscp->varp()->name();
    return name.rfind("__Vfunc_", 0) == 0 || name.rfind("__Vtask_", 0) == 0;
}

bool isGlobalTriggerState(const AstVarScope* vscp) {
    const string& name = vscp->varp()->name();
    return name.find("__VactTriggered") != string::npos
           || name.find("__VicoTriggered") != string::npos
           || name.find("__VnbaTriggered") != string::npos;
}

struct SharedHelperAbiAnalysis final {
    uint64_t m_calls = 0;
    uint64_t m_constants = 0;
    uint64_t m_dpiCalls = 0;
    uint64_t m_externalVars = 0;
    uint64_t m_generatedTemps = 0;
    uint64_t m_globalTriggerRefs = 0;
    uint64_t m_hiddenUses = 0;
    uint64_t m_inputVars = 0;
    uint64_t m_outputVars = 0;
    uint64_t m_stateVars = 0;
    bool m_hasTriggeredState = false;
    bool m_eligible = true;
};

class SharedHelperAbiAnalyzer final : public VNVisitor {
    AstCFunc* const m_rootFuncp;
    AstScope* const m_boundaryScopep;
    const std::unordered_set<AstVarScope*> m_contractVscps;
    std::unordered_set<const AstCFunc*> m_visitedFuncps;
    std::unordered_map<AstVarScope*, std::pair<bool, bool>> m_accesses;
    std::unordered_set<AstVarScope*> m_hiddenVscps;
    std::unordered_set<AstVarScope*> m_rootLocalVscps;
    SharedHelperAbiAnalysis m_result;
    AstCFunc* m_funcp = nullptr;

    void visit(AstCFunc* nodep) override {
        if (!m_visitedFuncps.emplace(nodep).second) return;
        if ((nodep == m_rootFuncp && !nodep->isLoose())
            || (nodep != m_rootFuncp && !isSafeSharedCallTarget(nodep, m_boundaryScopep))
            || (nodep == m_rootFuncp && (nodep->entryPoint() || nodep->needProcess()))
            || nodep->recursive() || nodep->isCoroutine()) {
            m_result.m_eligible = false;
        }
        VL_RESTORER(m_funcp);
        m_funcp = nodep;
        iterateChildren(nodep);
    }
    void visit(AstCCall* nodep) override {
        ++m_result.m_calls;
        iterateChildren(nodep);
        AstCFunc* const funcp = nodep->funcp();
        if (isDpiCallTarget(funcp)) {
            ++m_result.m_dpiCalls;
            m_result.m_eligible = false;
            return;
        }
        if (!isSafeSharedCallTarget(funcp, m_boundaryScopep)) {
            m_result.m_eligible = false;
            return;
        }
        iterate(funcp);
    }
    void visit(AstConst* nodep) override {
        ++m_result.m_constants;
        iterateChildren(nodep);
    }
    void visit(AstNodeVarRef* nodep) override {
        AstVarScope* const vscp = nodep->varScopep();
        if (!vscp) {
            m_result.m_eligible = false;
            return;
        }
        if (m_funcp != m_rootFuncp && !vscp->varp()->isFuncLocal()
            && !isUnderScope(vscp->scopep(), m_boundaryScopep)) {
            m_result.m_eligible = false;
        }
        auto& access = m_accesses[vscp];
        access.first |= nodep->access().isReadOrRW();
        access.second |= nodep->access().isWriteOrRW();
        if (m_funcp == m_rootFuncp && vscp->varp()->isFuncLocal()) {
            m_rootLocalVscps.insert(vscp);
        }
        if (!vscp->varp()->isFuncLocal() && !isTaskCallTemp(vscp)
            && !m_contractVscps.count(vscp)) {
            m_hiddenVscps.insert(vscp);
        }
        if (isGlobalTriggerState(vscp)) ++m_result.m_globalTriggerRefs;
        const string& name = vscp->varp()->name();
        const string::size_type dot = name.rfind("__DOT__");
        const string leafName = dot == string::npos ? name : name.substr(dot + 7);
        const bool triggered
            = leafName.size() >= 9 && leafName.compare(leafName.size() - 9, 9, "Triggered") == 0;
        const bool triggeredAcc
            = leafName.size() >= 12
              && leafName.compare(leafName.size() - 12, 12, "TriggeredAcc") == 0;
        if (leafName.rfind("__V", 0) == 0
            && (triggeredAcc || (triggered && nodep->access().isWriteOrRW()))) {
            m_result.m_hasTriggeredState = true;
        }
        iterateChildren(nodep);
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    SharedHelperAbiAnalyzer(AstCFunc* funcp, AstScope* boundaryScopep,
                            const V3SubgraphContract& contract)
        : m_rootFuncp{funcp}
        , m_boundaryScopep{boundaryScopep}
        , m_contractVscps{[&]() {
            std::unordered_set<AstVarScope*> result;
            const auto addUses = [&](const std::vector<V3SubgraphContract::Use>& uses) {
                for (const V3SubgraphContract::Use& use : uses) result.insert(use.m_varScopep);
            };
            addUses(contract.boundaryUses());
            addUses(contract.externalUses());
            addUses(contract.internalUses());
            return result;
        }()} {
        iterate(funcp);
        m_result.m_hiddenUses = m_hiddenVscps.size();
        if (m_result.m_hiddenUses) m_result.m_eligible = false;
        for (const auto& pair : m_accesses) {
            AstVarScope* const vscp = pair.first;
            if (vscp->varp()->isFuncLocal() || isTaskCallTemp(vscp)) {
                if (m_rootLocalVscps.count(vscp)) ++m_result.m_generatedTemps;
                continue;
            }
            if (isUnderScope(vscp->scopep(), m_boundaryScopep)) {
                ++m_result.m_stateVars;
            } else {
                ++m_result.m_externalVars;
            }
            if (pair.second.first) ++m_result.m_inputVars;
            if (pair.second.second) ++m_result.m_outputVars;
        }
    }
    ~SharedHelperAbiAnalyzer() override = default;

    const SharedHelperAbiAnalysis& result() const { return m_result; }
};

struct SharedHelperArg final {
    AstVarScope* m_vscp = nullptr;
    bool m_read = false;
    bool m_write = false;
    bool m_state = false;
};

struct SharedScheduleVarId final {
    AstVar* m_varp = nullptr;
    std::vector<const AstCell*> m_cellPath;
    size_t m_externalSlot = 0;
    size_t m_localSlot = 0;
    bool m_external = false;
    bool m_local = false;

    bool operator==(const SharedScheduleVarId& rhs) const {
        return m_varp == rhs.m_varp && m_cellPath == rhs.m_cellPath
               && m_externalSlot == rhs.m_externalSlot && m_localSlot == rhs.m_localSlot
               && m_external == rhs.m_external && m_local == rhs.m_local;
    }
};

V3Hash hashSharedScheduleVarId(const SharedScheduleVarId& id) {
    V3Hash hash{id.m_varp};
    hash += static_cast<uint64_t>(id.m_cellPath.size());
    for (const AstCell* const cellp : id.m_cellPath) hash += cellp;
    hash += static_cast<uint64_t>(id.m_externalSlot);
    hash += static_cast<uint64_t>(id.m_localSlot);
    hash += static_cast<uint32_t>(id.m_external);
    hash += static_cast<uint32_t>(id.m_local);
    return hash;
}

class SharedScheduleScopeResolver final {
    std::unordered_map<const AstScope*, std::vector<AstScope*>> m_children;

public:
    explicit SharedScheduleScopeResolver(AstNetlist* netlistp) {
        netlistp->foreach([&](AstScope* scopep) {
            if (scopep->aboveScopep()) m_children[scopep->aboveScopep()].push_back(scopep);
        });
    }

    AstVarScope* bind(AstScope* boundaryScopep, const SharedScheduleVarId& id) const {
        if (id.m_external || id.m_local) return nullptr;
        AstScope* scopep = boundaryScopep;
        for (auto it = id.m_cellPath.rbegin(); it != id.m_cellPath.rend(); ++it) {
            const auto childrenIt = m_children.find(scopep);
            if (childrenIt == m_children.end()) return nullptr;
            AstScope* childp = nullptr;
            for (AstScope* const candidatep : childrenIt->second) {
                if (candidatep->aboveCellp() != *it) continue;
                if (childp) return nullptr;
                childp = candidatep;
            }
            if (!childp) return nullptr;
            scopep = childp;
        }
        AstVarScope* resultp = nullptr;
        for (AstVarScope* vscp = scopep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
            if (vscp->varp() != id.m_varp) continue;
            if (resultp) return nullptr;
            resultp = vscp;
        }
        return resultp;
    }
};

struct SharedScheduleLogicRef final {
    uintptr_t m_access = 0;
    SharedScheduleVarId m_id;
    AstVarScope* m_vscp = nullptr;
};

struct SharedScheduleLogicNode final {
    uintptr_t m_type = 0;
    std::vector<uintptr_t> m_nodeTypes;
    std::vector<string> m_constValues;
    std::vector<SharedScheduleLogicRef> m_refs;
};

using SharedScheduleLogicSig = std::vector<SharedScheduleLogicNode>;

struct SharedScheduleBoundaryAbiSlot final {
    string m_name;
    AstNodeDType* m_dtypep = nullptr;
    AstVarScope* m_storageVscp = nullptr;
    bool m_read = false;
    bool m_write = false;
};

using SharedScheduleBoundaryAbi = std::vector<SharedScheduleBoundaryAbiSlot>;

SharedScheduleBoundaryAbi makeSharedScheduleBoundaryAbi(AstScope* boundaryScopep) {
    SharedScheduleBoundaryAbi result;
    for (AstNode* memberp = boundaryScopep->modp()->stmtsp(); memberp;
         memberp = memberp->nextp()) {
        AstVar* const varp = VN_CAST(memberp, Var);
        if (!varp) continue;
        if (!varp->isIO() || !varp->direction().isNonOutput()) continue;
        AstVarScope* storageVscp = nullptr;
        for (AstVarScope* vscp = boundaryScopep->varsp(); vscp;
             vscp = VN_AS(vscp->nextp(), VarScope)) {
            if (vscp->varp()->origName() != varp->origName()) continue;
            UASSERT_OBJ(!storageVscp, vscp, "Duplicate subgraph boundary input storage");
            storageVscp = vscp;
        }
        result.push_back(SharedScheduleBoundaryAbiSlot{
            varp->origName(), varp->dtypep(), storageVscp, varp->direction().isNonOutput(),
            varp->direction().isWritable()});
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.m_name != rhs.m_name) return lhs.m_name < rhs.m_name;
        return lhs.m_dtypep < rhs.m_dtypep;
    });
    return result;
}

bool matchSharedScheduleBoundaryAbi(const SharedScheduleBoundaryAbi& source,
                                    const SharedScheduleBoundaryAbi& candidate) {
    if (source.size() != candidate.size()) return false;
    for (size_t index = 0; index < source.size(); ++index) {
        const SharedScheduleBoundaryAbiSlot& sourceSlot = source[index];
        const SharedScheduleBoundaryAbiSlot& candidateSlot = candidate[index];
        if (sourceSlot.m_name != candidateSlot.m_name || sourceSlot.m_read != candidateSlot.m_read
            || sourceSlot.m_write != candidateSlot.m_write
            || !sourceSlot.m_dtypep->similarDType(candidateSlot.m_dtypep)) {
            return false;
        }
    }
    return true;
}

SharedScheduleVarId
makeSharedScheduleVarId(AstVarScope* vscp, AstScope* boundaryScopep,
                        std::unordered_map<AstVarScope*, size_t>& externalSlots,
                        std::unordered_map<AstVarScope*, size_t>& localSlots) {
    SharedScheduleVarId result;
    if (vscp->varp()->isFuncLocal() || isTaskCallTemp(vscp)) {
        result.m_local = true;
        const auto inserted = localSlots.emplace(vscp, localSlots.size());
        result.m_localSlot = inserted.first->second;
        return result;
    } else if (!isUnderScope(vscp->scopep(), boundaryScopep)) {
        result.m_external = true;
        const auto inserted = externalSlots.emplace(vscp, externalSlots.size());
        result.m_externalSlot = inserted.first->second;
        return result;
    }

    result.m_varp = vscp->varp();
    for (const AstScope* scopep = vscp->scopep(); scopep != boundaryScopep;
         scopep = scopep->aboveScopep()) {
        UASSERT_OBJ(scopep, vscp, "Subgraph variable has no path to boundary scope");
        result.m_cellPath.push_back(scopep->aboveCellp());
    }
    return result;
}

SharedScheduleLogicSig makeSharedScheduleLogicSig(const LogicByScope& logic,
                                                  AstScope* boundaryScopep, V3Hash& logicHash) {
    SharedScheduleLogicSig result;
    std::unordered_map<AstVarScope*, size_t> externalSlots;
    std::unordered_map<AstVarScope*, size_t> localSlots;
    logicHash += static_cast<uint64_t>(logic.size());
    logic.foreachLogic([&](AstNode* logicp) {
        result.emplace_back();
        SharedScheduleLogicNode& node = result.back();
        node.m_type = static_cast<uintptr_t>(logicp->type());
        logicHash += static_cast<uint64_t>(node.m_type);
        logicp->foreach([&](AstNode* scanp) {
            node.m_nodeTypes.push_back(static_cast<uintptr_t>(scanp->type()));
            logicHash += static_cast<uint64_t>(node.m_nodeTypes.back());
            if (const AstConst* const constp = VN_CAST(scanp, Const)) {
                node.m_constValues.push_back(constp->num().toString());
                logicHash += node.m_constValues.back();
            }
        });
        logicHash += static_cast<uint64_t>(node.m_nodeTypes.size());
        logicHash += static_cast<uint64_t>(node.m_constValues.size());
        logicp->foreach([&](AstVarRef* refp) {
            AstVarScope* const vscp = refp->varScopep();
            node.m_refs.push_back(SharedScheduleLogicRef{
                static_cast<uintptr_t>(refp->access()),
                makeSharedScheduleVarId(vscp, boundaryScopep, externalSlots, localSlots), vscp});
            logicHash += static_cast<uint64_t>(node.m_refs.back().m_access);
            logicHash += hashSharedScheduleVarId(node.m_refs.back().m_id);
        });
        logicHash += static_cast<uint64_t>(node.m_refs.size());
    });
    return result;
}

enum class SharedScheduleMatchResult : uint8_t {
    MATCH,
    ABI,
    DTYPE_ACCESS,
    RELATIVE_PATH,
    TOPOLOGY
};

SharedScheduleMatchResult
matchSharedScheduleLogic(const SharedScheduleLogicSig& source,
                         const SharedScheduleLogicSig& candidate,
                         std::unordered_map<AstVarScope*, AstVarScope*>& sourceToCandidate) {
    if (source.size() != candidate.size()) return SharedScheduleMatchResult::TOPOLOGY;

    std::unordered_map<AstVarScope*, AstVarScope*> candidateToSource;
    for (size_t nodeIndex = 0; nodeIndex < source.size(); ++nodeIndex) {
        const SharedScheduleLogicNode& sourceNode = source[nodeIndex];
        const SharedScheduleLogicNode& candidateNode = candidate[nodeIndex];
        if (sourceNode.m_type != candidateNode.m_type
            || sourceNode.m_nodeTypes != candidateNode.m_nodeTypes
            || sourceNode.m_constValues != candidateNode.m_constValues) {
            return SharedScheduleMatchResult::TOPOLOGY;
        }

        if (sourceNode.m_refs.size() != candidateNode.m_refs.size()) {
            return SharedScheduleMatchResult::TOPOLOGY;
        }
        for (size_t refIndex = 0; refIndex < candidateNode.m_refs.size(); ++refIndex) {
            const SharedScheduleLogicRef& sourceRef = sourceNode.m_refs[refIndex];
            const SharedScheduleLogicRef& candidateRef = candidateNode.m_refs[refIndex];
            AstVarScope* const candidateVscp = candidateRef.m_vscp;
            if (sourceRef.m_access != candidateRef.m_access
                || !sourceRef.m_vscp->dtypep()->similarDType(candidateVscp->dtypep())) {
                return SharedScheduleMatchResult::DTYPE_ACCESS;
            }
            if (!(sourceRef.m_id == candidateRef.m_id)) {
                return sourceRef.m_id.m_external || candidateRef.m_id.m_external
                               || sourceRef.m_id.m_local || candidateRef.m_id.m_local
                           ? SharedScheduleMatchResult::ABI
                           : SharedScheduleMatchResult::RELATIVE_PATH;
            }
            const auto sourceIt = sourceToCandidate.find(sourceRef.m_vscp);
            if (sourceIt != sourceToCandidate.end()) {
                if (sourceIt->second != candidateVscp) return SharedScheduleMatchResult::ABI;
            } else {
                const auto candidateIt = candidateToSource.find(candidateVscp);
                if (candidateIt != candidateToSource.end()
                    && candidateIt->second != sourceRef.m_vscp) {
                    return SharedScheduleMatchResult::ABI;
                }
                sourceToCandidate.emplace(sourceRef.m_vscp, candidateVscp);
                candidateToSource.emplace(candidateVscp, sourceRef.m_vscp);
            }
        }
    }
    return SharedScheduleMatchResult::MATCH;
}

struct SharedScheduleKey final {
    AstNodeModule* m_modp = nullptr;
    VSubgraphPhase m_phase;
    const AstSenTree* m_domainKeyp = nullptr;
    V3Hash m_logicHash;
    SharedScheduleBoundaryAbi m_boundaryAbi;
    SharedScheduleLogicSig m_logicSig;

    V3Hash bucketHash() const {
        V3Hash hash{m_modp};
        hash += static_cast<uint32_t>(m_phase);
        hash += m_logicHash;
        return hash;
    }

    SharedScheduleMatchResult
    matchLogic(const SharedScheduleKey& candidate,
               std::unordered_map<AstVarScope*, AstVarScope*>& sourceToCandidate) const {
        if (!matchSharedScheduleBoundaryAbi(m_boundaryAbi, candidate.m_boundaryAbi)) {
            return SharedScheduleMatchResult::ABI;
        }
        return matchSharedScheduleLogic(m_logicSig, candidate.m_logicSig, sourceToCandidate);
    }
};

struct PreclassifiedSubgraphPhase final {
    SubgraphGroup* m_groupp = nullptr;
    LogicByScope* m_logicp = nullptr;
    const char* m_phaseName = nullptr;
    VSubgraphPhase m_phase;
    SharedScheduleKey m_key;
    size_t m_classIndex = std::numeric_limits<size_t>::max();
};

struct SubgraphScheduleEquivalenceClass final {
    size_t m_representativeIndex = std::numeric_limits<size_t>::max();
    size_t m_artifactIndex = std::numeric_limits<size_t>::max();
    uint64_t m_members = 0;
};

const SharedScheduleLogicRef* findSharedScheduleLogicRef(const SharedScheduleLogicSig& signature,
                                                         AstVarScope* vscp) {
    for (const SharedScheduleLogicNode& node : signature) {
        const auto it
            = std::find_if(node.m_refs.begin(), node.m_refs.end(),
                           [&](const SharedScheduleLogicRef& ref) { return ref.m_vscp == vscp; });
        if (it != node.m_refs.end()) return &*it;
    }
    return nullptr;
}

struct SharedScheduleUseRecipe final {
    SharedScheduleVarId m_id;
    SubgraphLocalTriggerId m_triggerId;
    // Order-introduced trigger dependencies are absent from the input logic signature. Represent
    // them by a subgraph-local trigger ID and bind them to each instance's concrete trigger
    // vector.
    AstVarScope* m_sourceVscp = nullptr;
    bool m_localTrigger = false;
    bool m_relative = false;
    bool m_read = false;
    bool m_write = false;
    bool m_cuttable = false;
};

struct SharedScheduleContractRecipe final {
    std::vector<SharedScheduleUseRecipe> m_boundaryUses;
    std::vector<SharedScheduleUseRecipe> m_externalUses;
    std::vector<SharedScheduleUseRecipe> m_internalUses;
    bool m_post = false;

    static SharedScheduleContractRecipe make(const V3SubgraphContract& contract,
                                             const SharedScheduleLogicSig& signature,
                                             SubgraphLocalTriggerId triggerId) {
        SharedScheduleContractRecipe result;
        result.m_post = contract.post();
        const auto makeUses
            = [&](const std::vector<V3SubgraphContract::Use>& uses,
                  std::vector<SharedScheduleUseRecipe>& recipes, bool requireRelative) {
                  recipes.reserve(uses.size());
                  for (const V3SubgraphContract::Use& use : uses) {
                      const SharedScheduleLogicRef* const refp
                          = findSharedScheduleLogicRef(signature, use.m_varScopep);
                      SharedScheduleVarId id;
                      if (refp) {
                          id = refp->m_id;
                      } else if (requireRelative) {
                          std::unordered_map<AstVarScope*, size_t> externalSlots;
                          std::unordered_map<AstVarScope*, size_t> localSlots;
                          id = makeSharedScheduleVarId(use.m_varScopep, contract.boundaryScopep(),
                                                       externalSlots, localSlots);
                          UASSERT_OBJ(!id.m_external && !id.m_local, use.m_varScopep,
                                      "Canonical subgraph contract use has no relative identity");
                      }
                      const bool localTrigger
                          = !refp && !requireRelative && isGlobalTriggerState(use.m_varScopep);
                      recipes.push_back(SharedScheduleUseRecipe{
                          std::move(id), triggerId, use.m_varScopep, localTrigger,
                          refp || requireRelative, use.m_read, use.m_write, use.m_cuttable});
                  }
              };
        makeUses(contract.boundaryUses(), result.m_boundaryUses, true);
        makeUses(contract.externalUses(), result.m_externalUses, false);
        makeUses(contract.internalUses(), result.m_internalUses, true);
        return result;
    }

    bool shareable() const {
        const auto relativeUses = [](const std::vector<SharedScheduleUseRecipe>& uses,
                                     bool external) {
            return std::all_of(uses.begin(), uses.end(), [&](const auto& use) {
                return use.m_relative && use.m_id.m_external == external && !use.m_id.m_local;
            });
        };
        const bool externalUsesShareable
            = std::all_of(m_externalUses.begin(), m_externalUses.end(), [](const auto& use) {
                  return use.m_localTrigger || !use.m_relative
                         || (use.m_id.m_external && !use.m_id.m_local);
              });
        return relativeUses(m_boundaryUses, false) && externalUsesShareable
               && relativeUses(m_internalUses, false);
    }

    uint64_t directExternalUses() const {
        return std::count_if(m_externalUses.begin(), m_externalUses.end(),
                             [](const auto& use) { return !use.m_relative; });
    }
    uint64_t directGlobalTriggerUses() const {
        return std::count_if(m_externalUses.begin(), m_externalUses.end(), [](const auto& use) {
            return !use.m_relative && isGlobalTriggerState(use.m_sourceVscp);
        });
    }
    uint64_t localTriggerUses() const {
        return std::count_if(m_externalUses.begin(), m_externalUses.end(),
                             [](const auto& use) { return use.m_localTrigger; });
    }

    bool validateBinding(AstScope* boundaryScopep,
                         const std::unordered_map<AstVarScope*, AstVarScope*>& sourceToCandidate,
                         const SharedScheduleScopeResolver& resolver, VSubgraphPhase phase,
                         const SubgraphInstanceTriggerBinding& triggerBinding) const {
        const bool post = phase == VSubgraphPhase{VSubgraphPhase::POST};
        const bool boundaryCuttable = post || phase == VSubgraphPhase{VSubgraphPhase::REFRESH};
        const auto validSemantics
            = [](const std::vector<SharedScheduleUseRecipe>& uses, bool cuttableReads) {
                  return std::all_of(uses.begin(), uses.end(), [&](const auto& use) {
                      return (use.m_read || use.m_write)
                             && use.m_cuttable == (use.m_read && cuttableReads);
                  });
              };
        if (!shareable() || m_post != post || !validSemantics(m_boundaryUses, boundaryCuttable)
            || !validSemantics(m_externalUses, boundaryCuttable)
            || !validSemantics(m_internalUses, post)) {
            return false;
        }
        const auto validateRelative = [&](const std::vector<SharedScheduleUseRecipe>& uses) {
            return std::all_of(uses.begin(), uses.end(), [&](const auto& use) {
                AstVarScope* const resolvedVscp = resolver.bind(boundaryScopep, use.m_id);
                const auto it = sourceToCandidate.find(use.m_sourceVscp);
                return resolvedVscp
                       && (it == sourceToCandidate.end() || it->second == resolvedVscp);
            });
        };
        const bool externalValid
            = std::all_of(m_externalUses.begin(), m_externalUses.end(), [&](const auto& use) {
                  if (use.m_localTrigger) {
                      AstVarScope* const instanceVscp
                          = triggerBinding.triggerStateVscp(use.m_triggerId);
                      return use.m_sourceVscp->dtypep()->similarDType(instanceVscp->dtypep());
                  }
                  return use.m_relative ? sourceToCandidate.count(use.m_sourceVscp) != 0
                                        : use.m_sourceVscp != nullptr;
              });
        return externalValid && validateRelative(m_boundaryUses)
               && validateRelative(m_internalUses);
    }

    void
    appendExternalBindings(const std::unordered_map<AstVarScope*, AstVarScope*>& sourceToCandidate,
                           const SubgraphInstanceTriggerBinding& triggerBinding,
                           std::vector<AstVarScope*>& result) const {
        result.reserve(result.size() + m_externalUses.size());
        for (const SharedScheduleUseRecipe& use : m_externalUses) {
            const auto it = sourceToCandidate.find(use.m_sourceVscp);
            AstVarScope* const vscp
                = use.m_localTrigger
                      ? triggerBinding.triggerStateVscp(use.m_triggerId)
                      : (use.m_relative ? (it == sourceToCandidate.end() ? nullptr : it->second)
                                        : use.m_sourceVscp);
            UASSERT_OBJ(vscp, use.m_sourceVscp, "Canonical external contract failed to bind");
            result.push_back(vscp);
        }
    }
};

struct SharedHelperArtifact final {
    SharedScheduleKey m_key;
    SubgraphLocalTriggerId m_triggerId;
    AstCFunc* m_funcp = nullptr;
    AstCCall* m_firstCallp = nullptr;
    std::vector<SharedHelperArg> m_args;
    SharedScheduleContractRecipe m_contractRecipe;
    SharedHelperAbiAnalysis m_abi;
    bool m_instanceContext = false;
    bool m_parameterized = false;
};

VAccess sharedHelperArgAccess(const SharedHelperArg& arg) {
    if (arg.m_read && arg.m_write) return VAccess::READWRITE;
    return arg.m_write ? VAccess::WRITE : VAccess::READ;
}

std::vector<SharedHelperArg> collectSharedHelperArgs(AstCFunc* funcp, AstScope* boundaryScopep,
                                                     bool externalOnly = false) {
    std::vector<SharedHelperArg> args;
    std::unordered_map<AstVarScope*, size_t> argIndex;
    funcp->foreach([&](AstNodeVarRef* refp) {
        AstVarScope* const vscp = refp->varScopep();
        if (vscp->varp()->isFuncLocal()) return;
        if (externalOnly && isUnderScope(vscp->scopep(), boundaryScopep)) return;
        const auto inserted = argIndex.emplace(vscp, args.size());
        if (inserted.second) {
            args.push_back(
                SharedHelperArg{vscp, false, false, isUnderScope(vscp->scopep(), boundaryScopep)});
        }
        SharedHelperArg& arg = args[inserted.first->second];
        arg.m_read |= refp->access().isReadOrRW();
        arg.m_write |= refp->access().isWriteOrRW();
    });
    return args;
}

AstVarScope* newSharedHelperArg(AstCFunc* funcp, const SharedHelperArg& arg, size_t index) {
    FileLine* const flp = funcp->fileline();
    AstScope* const scopep = funcp->scopep();
    AstVar* const varp = new AstVar{flp, VVarType::BLOCKTEMP, "__VsubgraphArg" + cvtToStr(index),
                                    arg.m_vscp->dtypep()};
    varp->direction(arg.m_write ? (arg.m_read ? VDirection::INOUT : VDirection::OUTPUT)
                                : VDirection::CONSTREF);
    varp->funcLocal(true);
    funcp->addArgsp(varp);
    AstVarScope* const vscp = new AstVarScope{flp, scopep, varp};
    scopep->addVarsp(vscp);
    return vscp;
}

void addSharedHelperCallArgs(AstCCall* callp, const std::vector<SharedHelperArg>& args) {
    for (const SharedHelperArg& arg : args) {
        callp->addArgsp(new AstVarRef{callp->fileline(), arg.m_vscp, sharedHelperArgAccess(arg)});
    }
}

AstCFunc* makeSharedScheduleWrapper(AstNetlist* netlistp, AstScope* scopep,
                                    const SubgraphInstanceTriggerBinding& triggerBinding,
                                    SubgraphLocalTriggerId triggerId, AstCFunc* sharedFuncp,
                                    const std::vector<SharedHelperArg>& args, const string& tag,
                                    bool slow, bool refresh, bool instanceContext) {
    AstSenTree* const senTreep = triggerBinding.bind(triggerId);
    UASSERT_OBJ(triggerBinding.parentDomain(triggerId), scopep,
                "Shared subgraph wrapper has no exact parent domain");
    AstCFunc* const funcp = new AstCFunc{netlistp->fileline(), "_eval_body__" + tag, scopep, ""};
    funcp->dontCombine(true);
    funcp->isStatic(false);
    funcp->isLoose(true);
    funcp->slow(slow);
    funcp->isConst(false);
    funcp->declPrivate(true);
    scopep->addBlocksp(funcp);

    AstCCall* const callp = new AstCCall{funcp->fileline(), sharedFuncp};
    callp->dtypeSetVoid();
    callp->useCallerSelf(instanceContext);
    addSharedHelperCallArgs(callp, args);
    AstNodeStmt* const stmtp = callp->makeStmt();
    if (refresh) {
        funcp->addStmtsp(stmtp);
    } else {
        AstIf* const guardp = util::createIfFromSenTree(senTreep);
        guardp->addThensp(stmtp);
        funcp->addStmtsp(guardp);
    }
    return funcp;
}

void discardSharedScheduleLogic(LogicByScope& logic) {
    for (const auto& pair : logic) {
        AstActive* const activep = pair.second;
        if (activep->backp()) activep->unlinkFrBack();
        activep->deleteTree();
    }
    logic.clear();
}

void preserveSharedInstanceUses(const V3SubgraphContract& contract) {
    const auto preserveUses = [&](const std::vector<V3SubgraphContract::Use>& uses) {
        for (const V3SubgraphContract::Use& use : uses) {
            use.m_varScopep->optimizeLifePost(false);
            use.m_varScopep->subgraphSharedUse(true);
        }
    };
    preserveUses(contract.boundaryUses());
    preserveUses(contract.internalUses());
}

void preserveSharedInstanceUses(const SharedScheduleContractRecipe& recipe) {
    const auto preserveRecipes = [&](const std::vector<SharedScheduleUseRecipe>& uses) {
        for (const SharedScheduleUseRecipe& use : uses) {
            use.m_sourceVscp->optimizeLifePost(false);
            use.m_sourceVscp->subgraphSharedUse(true);
        }
    };
    preserveRecipes(recipe.m_boundaryUses);
    preserveRecipes(recipe.m_internalUses);
}

void preserveSharedInstanceUses(const SharedScheduleContractRecipe& recipe,
                                AstScope* boundaryScopep,
                                const SharedScheduleScopeResolver& resolver) {
    const auto preserveRecipes = [&](const std::vector<SharedScheduleUseRecipe>& uses) {
        for (const SharedScheduleUseRecipe& use : uses) {
            AstVarScope* const vscp = resolver.bind(boundaryScopep, use.m_id);
            UASSERT_OBJ(vscp, boundaryScopep, "Canonical subgraph use failed to resolve");
            vscp->optimizeLifePost(false);
            vscp->subgraphSharedUse(true);
        }
    };
    preserveRecipes(recipe.m_boundaryUses);
    preserveRecipes(recipe.m_internalUses);
}

void prepareSharedCallClosure(AstCFunc* rootFuncp, AstScope* boundaryScopep) {
    std::unordered_set<AstCFunc*> visitedFuncps;
    std::function<void(AstCFunc*)> prepare = [&](AstCFunc* funcp) {
        if (!visitedFuncps.emplace(funcp).second) return;
        funcp->subgraphCallerSelf(true);
        funcp->noLife(true);
        funcp->foreach([&](AstCCall* callp) {
            AstCFunc* const calledFuncp = callp->funcp();
            UASSERT_OBJ(isSafeSharedCallTarget(calledFuncp, boundaryScopep), callp,
                        "Canonical subgraph has an unsafe call target");
            calledFuncp->entryPoint(false);
            if (calledFuncp->isLoose()) callp->useCallerSelf(true);
            prepare(calledFuncp);
        });
    };
    prepare(rootFuncp);
}

void releaseDiscardedCallClosureEntries(const LogicByScope& logic, AstScope* boundaryScopep) {
    std::unordered_set<AstCFunc*> visitedFuncps;
    std::function<void(AstCFunc*)> release = [&](AstCFunc* funcp) {
        if (!visitedFuncps.emplace(funcp).second) return;
        if (!isSafeSharedCallTarget(funcp, boundaryScopep)) return;
        funcp->entryPoint(false);
        funcp->foreach([&](AstCCall* callp) { release(callp->funcp()); });
    };
    logic.foreachLogic([&](AstNode* logicp) {
        logicp->foreach([&](AstCCall* callp) { release(callp->funcp()); });
    });
}

struct SharedHelperIsolationAnalysis final {
    uint64_t m_globalTriggerRefs = 0;
    uint64_t m_refs = 0;
    uint64_t m_unboundRefs = 0;
};

SharedHelperIsolationAnalysis parameterizeSharedHelper(SharedHelperArtifact& artifact) {
    UASSERT_OBJ(!artifact.m_parameterized, artifact.m_funcp,
                "Shared subgraph helper parameterized twice");
    if (artifact.m_instanceContext) {
        preserveSharedInstanceUses(artifact.m_contractRecipe);
        prepareSharedCallClosure(artifact.m_funcp, artifact.m_funcp->scopep());
    }
    std::unordered_map<AstVarScope*, AstVarScope*> argVscps;
    for (size_t index = 0; index < artifact.m_args.size(); ++index) {
        const SharedHelperArg& arg = artifact.m_args[index];
        argVscps.emplace(arg.m_vscp, newSharedHelperArg(artifact.m_funcp, arg, index));
    }
    artifact.m_funcp->foreach([&](AstNodeVarRef* refp) {
        const auto it = argVscps.find(refp->varScopep());
        if (it == argVscps.end()) return;
        AstVarScope* const argVscp = it->second;
        refp->varp(argVscp->varp());
        refp->varScopep(argVscp);
        refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
    });
    SharedHelperIsolationAnalysis isolation;
    artifact.m_funcp->foreach([&](AstNodeVarRef* refp) {
        ++isolation.m_refs;
        if (isGlobalTriggerState(refp->varScopep())) ++isolation.m_globalTriggerRefs;
        const bool bound
            = refp->varp()->isFuncLocal()
              || (artifact.m_instanceContext
                  && isUnderScope(refp->varScopep()->scopep(), artifact.m_funcp->scopep()));
        if (!bound) ++isolation.m_unboundRefs;
    });
    UASSERT_OBJ(!isolation.m_unboundRefs, artifact.m_funcp,
                "Shared subgraph helper retained an implicit nonlocal reference");
    UASSERT_OBJ(!isolation.m_globalTriggerRefs, artifact.m_funcp,
                "Shared subgraph helper retained a concrete global trigger reference");
    addSharedHelperCallArgs(artifact.m_firstCallp, artifact.m_args);
    if (!artifact.m_instanceContext) { artifact.m_funcp->isStatic(true); }
    artifact.m_funcp->noLife(true);
    artifact.m_parameterized = true;
    return isolation;
}

AstCCall* soleLocalHelperCall(AstCFunc* funcp) {
    // V3Order normally emits a small per-instance trigger wrapper around one process function.
    // Keep that wrapper private to the instance and share only the process function.
    AstCCall* resultp = nullptr;
    bool multiple = false;
    funcp->foreach([&](AstCCall* callp) {
        if (resultp) {
            multiple = true;
        } else {
            resultp = callp;
        }
    });
    if (multiple || !resultp || resultp->argsp()) return nullptr;
    AstCFunc* const calledFuncp = resultp->funcp();
    if (calledFuncp->entryPoint() || calledFuncp->dpiImportPrototype()
        || calledFuncp->dpiImportWrapper() || calledFuncp->scopep() != funcp->scopep()) {
        return nullptr;
    }
    return resultp;
}

class SnapshotNameAllocator final {
    std::unordered_map<AstScope*, std::unordered_set<string>> m_usedNames;

    std::unordered_set<string>& usedNamesFor(AstScope* scopep) {
        std::unordered_set<string>& usedNames = m_usedNames[scopep];
        if (!usedNames.empty()) return usedNames;
        for (AstVarScope* vscp = scopep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
            usedNames.insert(vscp->varp()->name());
        }
        return usedNames;
    }

public:
    string get(AstScope* scopep, const string& base) {
        std::unordered_set<string>& usedNames = usedNamesFor(scopep);
        if (usedNames.insert(base).second) return base;
        for (unsigned index = 1;; ++index) {
            const string name = base + "__" + cvtToStr(index);
            if (usedNames.insert(name).second) return name;
        }
    }
};

AstScope* findBoundaryScope(AstScope* scopep) {
    for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
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

SubgraphGroup& findOrCreateGroup(std::vector<SubgraphGroup>& groups, LogicByScope* ownerp,
                                 AstScope* boundaryScopep, AstSenTree* senTreep,
                                 const AstSenTree* domainKeyp, FileLine* filelinep) {
    for (SubgraphGroup& group : groups) {
        if (group.m_boundaryScopep == boundaryScopep && group.m_domainKeyp == domainKeyp) {
            return group;
        }
    }
    groups.emplace_back();
    SubgraphGroup& group = groups.back();
    group.m_boundaryScopep = boundaryScopep;
    group.m_senTreep = senTreep;
    group.m_domainKeyp = domainKeyp;
    group.m_filelinep = filelinep;
    group.m_ownerp = ownerp;
    return group;
}

void addSubgraphLogic(SubgraphGroup& group, AstScope* scopep, AstActive* activep) {
    AstSenTree* const senTreep = activep->sentreep();

    for (AstNode *nodep = activep->stmtsp(), *nextp; nodep; nodep = nextp) {
        nextp = nodep->nextp();
        nodep->unlinkFrBack();
        LogicByScope& phaseLogic = senTreep->hasCombo()       ? group.m_refreshLogic
                                   : VN_IS(nodep, AlwaysPost) ? group.m_postLogic
                                                              : group.m_preLogic;
        phaseLogic.add(scopep, senTreep, nodep);
    }
    if (activep->backp()) activep->unlinkFrBack();
    activep->deleteTree();
}

void removeSingleDomainGuard(AstCFunc* funcp) {
    AstIf* const guardp = VN_CAST(funcp->stmtsp(), If);
    UASSERT_OBJ(guardp && !guardp->nextp() && !guardp->elsesp() && guardp->thensp(), funcp,
                "Subgraph refresh helper should have one artificial domain guard");
    AstNode* const bodyp = guardp->thensp()->unlinkFrBackWithNext();
    guardp->unlinkFrBack()->deleteTree();
    funcp->addStmtsp(bodyp);
}

void prepareSubgraphSnapshots(std::vector<SubgraphGroup>& groups,
                              const std::unordered_set<AstVarScope*>& regionWrittenVscps,
                              uint64_t& snapshotInstances, uint64_t& snapshotSources) {
    SnapshotNameAllocator nameAllocator;
    unsigned domainIndex = 0;
    for (SubgraphGroup& group : groups) {
        std::vector<AstVarScope*> sourceVscps;
        std::unordered_set<AstVarScope*> seenSourceVscps;
        group.m_preLogic.foreachLogic([&](AstNode* logicp) {
            logicp->foreach([&](AstVarRef* refp) {
                if (refp->access() != VAccess::READ) return;
                AstVarScope* const sourceVscp = refp->varScopep();
                if (isUnderScope(sourceVscp->scopep(), group.m_boundaryScopep)) return;
                if (!regionWrittenVscps.count(sourceVscp)) return;
                if (seenSourceVscps.insert(sourceVscp).second) sourceVscps.push_back(sourceVscp);
            });
        });
        if (sourceVscps.empty()) {
            ++domainIndex;
            continue;
        }

        std::sort(sourceVscps.begin(), sourceVscps.end(),
                  [](AstVarScope* lhsp, AstVarScope* rhsp) {
                      if (lhsp->scopep()->name() != rhsp->scopep()->name()) {
                          return lhsp->scopep()->name() < rhsp->scopep()->name();
                      }
                      if (lhsp->varp()->name() != rhsp->varp()->name()) {
                          return lhsp->varp()->name() < rhsp->varp()->name();
                      }
                      return lhsp < rhsp;
                  });

        AstNode* assignmentsp = nullptr;
        for (AstVarScope* const sourceVscp : sourceVscps) {
            AstScope* const storageScopep = sourceVscp->scopep();
            const string baseName = "__VsubgraphSnapshot__" + group.m_boundaryScopep->nameDotless()
                                    + "__d" + cvtToStr(domainIndex) + "__"
                                    + sourceVscp->varp()->shortName();
            const string name = nameAllocator.get(storageScopep, baseName);
            AstVarScope* const storageVscp = storageScopep->createTempLike(name, sourceVscp);
            group.m_snapshots.push_back(SubgraphSnapshot{sourceVscp, storageVscp});
            AstAssign* const assignp
                = new AstAssign{sourceVscp->fileline(),
                                new AstVarRef{sourceVscp->fileline(), storageVscp, VAccess::WRITE},
                                new AstVarRef{sourceVscp->fileline(), sourceVscp, VAccess::READ}};
            if (assignmentsp) {
                assignmentsp->addNext(assignp);
            } else {
                assignmentsp = assignp;
            }
        }

        group.m_preLogic.foreachLogic([&](AstNode* logicp) {
            logicp->foreach([&](AstVarRef* refp) {
                if (refp->access() != VAccess::READ) return;
                const auto it = std::find_if(group.m_snapshots.begin(), group.m_snapshots.end(),
                                             [&](const auto& snapshot) {
                                                 return snapshot.m_sourceVscp == refp->varScopep();
                                             });
                if (it == group.m_snapshots.end()) return;
                AstVarRef* const replacementp
                    = new AstVarRef{refp->fileline(), it->m_storageVscp, VAccess::READ};
                refp->replaceWith(replacementp);
                VL_DO_DANGLING(refp->deleteTree(), refp);
            });
        });

        FileLine* const flp = sourceVscps.front()->fileline();
        AstSubgraphInstance* const instancep = new AstSubgraphInstance{
            flp, group.m_boundaryScopep, VSubgraphPhase::SNAPSHOT, assignmentsp};
        instancep->reserveMaterializedUses(2 * group.m_snapshots.size());
        for (const SubgraphSnapshot& snapshot : group.m_snapshots) {
            instancep->addMaterializedUse(snapshot.m_sourceVscp, VSubgraphUseKind::SNAPSHOT_SOURCE,
                                          true, false, false);
            instancep->addMaterializedUse(snapshot.m_storageVscp,
                                          VSubgraphUseKind::SNAPSHOT_STORAGE, false, true, false);
        }
        AstAlwaysPre* const prep = new AstAlwaysPre{flp};
        prep->addStmtsp(instancep);
        AstActive* const wrapperp = new AstActive{flp, "subgraph-snapshot", group.m_senTreep};
        wrapperp->addStmtsp(prep);
        group.m_ownerp->emplace_back(group.m_boundaryScopep, wrapperp);
        ++snapshotInstances;
        snapshotSources += group.m_snapshots.size();
        ++domainIndex;
    }
}

class SealSubgraphMetadataVisitor final : public VNVisitor {
    std::unordered_set<const AstCFunc*> m_visitedFuncps;

    void visit(AstCFunc* nodep) override {
        if (!m_visitedFuncps.emplace(nodep).second) return;
        iterateChildren(nodep);
    }
    void visit(AstCCall* nodep) override {
        iterateChildren(nodep);
        if (!nodep->funcp()->entryPoint()) iterate(nodep->funcp());
    }
    void visit(AstSubgraphInstance* nodep) override {
        nodep->sealSchedulingMetadata();
        iterateChildren(nodep);
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    explicit SealSubgraphMetadataVisitor(AstCFunc* funcp) { iterate(funcp); }
    ~SealSubgraphMetadataVisitor() override = default;
};

}  // namespace

void sealSubgraphSchedulingMetadata(AstCFunc* funcp) {
    if (!funcp) return;
    SealSubgraphMetadataVisitor{funcp};
}

void lowerSubgraphNbaLogic(AstNetlist* netlistp, const std::vector<LogicByScope*>& logic,
                           const V3Order::TrigToSenMap& trigToSen, bool slow,
                           const V3Order::ExternalDomainsProvider& externalDomains) {
    if (!v3Global.opt.subgraphSchedule()) return;

    const bool measure = v3Global.opt.stats();
    const bool freshOrder = v3Global.opt.debugSubgraphFreshOrder();
    double cacheLookupWallTime = 0.0;
    double cacheReuseWallTime = 0.0;
    double collectWallTime = 0.0;
    double contractAbiWallTime = 0.0;
    double helperSharingWallTime = 0.0;
    double logicSignatureWallTime = 0.0;
    double materializeWallTime = 0.0;
    double orderCallsWallTime = 0.0;
    double snapshotsWallTime = 0.0;
    const VlOs::DeltaWallTime collectTimer{measure};
    std::unordered_set<AstVarScope*> regionWrittenVscps;
    for (LogicByScope* const lbsp : logic) {
        for (const auto& pair : *lbsp) {
            AstActive* const activep = pair.second;
            activep->foreach([&](AstNodeVarRef* refp) {
                if (!refp->access().isWriteOrRW()) return;
                regionWrittenVscps.insert(refp->varScopep());
            });
        }
    }

    std::vector<SubgraphGroup> groups;
    for (LogicByScope* const lbsp : logic) {
        LogicByScope parentLogic;
        parentLogic.reserve(lbsp->size());
        for (const auto& pair : *lbsp) {
            AstScope* const scopep = pair.first;
            AstActive* const activep = pair.second;
            AstScope* const boundaryScopep = findBoundaryScope(scopep);
            if (!boundaryScopep) {
                parentLogic.emplace_back(pair);
                continue;
            }
            AstSenTree* const senTreep = activep->sentreep();
            const AstSenTree* const domainKeyp
                = senTreep->hasCombo() ? senTreep : trigToSen.at(senTreep);
            SubgraphGroup& group = findOrCreateGroup(groups, lbsp, boundaryScopep, senTreep,
                                                     domainKeyp, activep->fileline());
            addSubgraphLogic(group, scopep, activep);
        }
        *lbsp = std::move(parentLogic);
    }
    // Capture parent accesses before adding coarse wrappers. Internal helper state not observed by
    // this logic can remain private and need not become parent order-graph metadata.
    std::unordered_set<AstVarScope*> parentAccessedVscps;
    for (LogicByScope* const lbsp : logic) {
        for (const auto& pair : *lbsp) {
            pair.second->foreach(
                [&](AstNodeVarRef* refp) { parentAccessedVscps.insert(refp->varScopep()); });
        }
    }
    if (measure) collectWallTime = collectTimer.deltaTime();

    uint64_t snapshotInstances = 0;
    uint64_t snapshotSources = 0;
    const VlOs::DeltaWallTime snapshotsTimer{measure};
    prepareSubgraphSnapshots(groups, regionWrittenVscps, snapshotInstances, snapshotSources);
    if (measure) snapshotsWallTime = snapshotsTimer.deltaTime();

    uint64_t orderedLogic = 0;
    uint64_t contractBoundaryUses = 0;
    uint64_t contractExternalUses = 0;
    uint64_t contractInternalUses = 0;
    uint64_t contractInstanceBindings = 0;
    uint64_t contractRecipeFallbacks = 0;
    uint64_t contractUsesNotExpanded = 0;
    uint64_t contracts = 0;
    uint64_t coarseNodes = 0;
    uint64_t logicalUses = 0;
    uint64_t materializedBoundaryUses = 0;
    uint64_t materializedExternalUses = 0;
    uint64_t materializedInternalUses = 0;
    uint64_t prunedInternalUses = 0;
    uint64_t refreshHelpers = 0;
    uint64_t canonicalContextArtifacts = 0;
    uint64_t canonicalContextReuses = 0;
    uint64_t canonicalOrderCallsAvoided = 0;
    uint64_t sharedBoundaryAbiAnalyses = 0;
    uint64_t sharedBoundaryAbiExternalSlots = 0;
    uint64_t sharedBoundaryAbiInstanceStorageBindings = 0;
    uint64_t sharedBoundaryAbiSlots = 0;
    uint64_t sharedScheduleEquivalenceBindingRejects = 0;
    uint64_t sharedScheduleEquivalenceFallbackOrderCalls = 0;
    uint64_t sharedScheduleEquivalenceFreshOrderCalls = 0;
    uint64_t sharedScheduleEquivalenceMaxClassSize = 0;
    uint64_t sharedScheduleEquivalenceRepresentativeOrderCalls = 0;
    uint64_t sharedScheduleEquivalenceReuses = 0;
    uint64_t canonicalContractDirectExternalUses = 0;
    uint64_t canonicalContractDirectGlobalTriggerUses = 0;
    uint64_t canonicalContractDirectOtherUses = 0;
    uint64_t canonicalContractLocalTriggerUses = 0;
    uint64_t sharedAbiAnalyses = 0;
    uint64_t sharedAbiConstants = 0;
    uint64_t sharedAbiDpiCalls = 0;
    uint64_t sharedAbiEligibleHelpers = 0;
    uint64_t sharedAbiExternalVars = 0;
    uint64_t sharedAbiGeneratedTemps = 0;
    uint64_t sharedAbiGlobalTriggerRefs = 0;
    uint64_t sharedAbiHiddenUses = 0;
    uint64_t sharedAbiInputVars = 0;
    uint64_t sharedAbiModulePhaseCandidates = 0;
    uint64_t sharedAbiOutputVars = 0;
    uint64_t sharedAbiStateVars = 0;
    uint64_t sharedHelperArguments = 0;
    uint64_t sharedHelperArtifactCount = 0;
    uint64_t sharedHelperBodyChecks = 0;
    uint64_t sharedHelperBodyMismatches = 0;
    uint64_t sharedHelperParameterizations = 0;
    uint64_t sharedHelperReuses = 0;
    uint64_t sharedHelperIsolationChecks = 0;
    uint64_t sharedHelperIsolationGlobalTriggerRefs = 0;
    uint64_t sharedHelperIsolationRefs = 0;
    uint64_t sharedHelperIsolationUnboundRefs = 0;
    uint64_t sharedHelperSkippedCalls = 0;
    uint64_t sharedHelperSkippedComposite = 0;
    uint64_t sharedHelperSkippedDpiCalls = 0;
    uint64_t sharedHelperSkippedGeneratedTemps = 0;
    uint64_t sharedHelperSkippedOversized = 0;
    uint64_t sharedHelperSkippedTriggered = 0;
    uint64_t sharedExactParentDomainReusedBodyWrappers = 0;
    uint64_t sharedExactParentDomainWrapperBindings = 0;
    uint64_t sharedLocalTriggerIds = 0;
    uint64_t sharedLocalTriggerInstanceBindings = 0;
    uint64_t sharedOrderCacheLogicMatches = 0;
    uint64_t sharedOrderCacheLogicMismatches = 0;
    uint64_t sharedOrderCacheLookups = 0;
    uint64_t sharedOrderCacheHashCandidates = 0;
    uint64_t sharedOrderCacheHashCollisions = 0;
    uint64_t sharedOrderCacheHashLookups = 0;
    uint64_t sharedOrderCacheHashMisses = 0;
    uint64_t sharedOrderCacheHashMaxCandidates = 0;
    uint64_t sharedOrderCacheMissAbi = 0;
    uint64_t sharedOrderCacheMissDomain = 0;
    uint64_t sharedOrderCacheMissDTypeAccess = 0;
    uint64_t sharedOrderCacheMissPhase = 0;
    uint64_t sharedOrderCacheMissRelativePath = 0;
    uint64_t sharedOrderCacheMissTopology = 0;
    uint64_t sharedOrderCacheOrderCallsExecuted = 0;
    uint64_t sharedOrderCacheOrderCallsAvoided = 0;
    struct EligibleModulePhase final {
        AstNodeModule* m_modp;
        VSubgraphPhase m_phase;
    };
    std::vector<EligibleModulePhase> eligibleModulePhases;
    std::vector<AstVarScope*> contractExternalBindings;
    std::vector<PendingSubgraphMaterialization> pendingMaterializations;
    std::unordered_set<AstVarScope*> postWrittenInternalVscps;
    std::unordered_set<AstVarScope*> refreshReadInternalVscps;
    std::vector<SharedHelperArtifact> sharedHelperArtifacts;
    const SharedScheduleScopeResolver scopeResolver{netlistp};
    const auto noteSharedAbi = [&](const SharedHelperAbiAnalysis& abi,
                                   VSubgraphPhase subgraphPhase, AstNodeModule* modp) {
        ++sharedAbiAnalyses;
        sharedAbiConstants += abi.m_constants;
        sharedAbiDpiCalls += abi.m_dpiCalls;
        sharedAbiExternalVars += abi.m_externalVars;
        sharedAbiGeneratedTemps += abi.m_generatedTemps;
        sharedAbiGlobalTriggerRefs += abi.m_globalTriggerRefs;
        sharedAbiHiddenUses += abi.m_hiddenUses;
        sharedAbiInputVars += abi.m_inputVars;
        sharedAbiOutputVars += abi.m_outputVars;
        sharedAbiStateVars += abi.m_stateVars;
        if (!abi.m_eligible) return;
        ++sharedAbiEligibleHelpers;
        const auto it = std::find_if(eligibleModulePhases.begin(), eligibleModulePhases.end(),
                                     [&](const EligibleModulePhase& key) {
                                         return key.m_modp == modp && key.m_phase == subgraphPhase;
                                     });
        if (it == eligibleModulePhases.end()) {
            eligibleModulePhases.push_back(EligibleModulePhase{modp, subgraphPhase});
        } else {
            ++sharedAbiModulePhaseCandidates;
        }
    };
    std::vector<PreclassifiedSubgraphPhase> phaseWorks;
    phaseWorks.reserve(3 * groups.size());
    const VlOs::DeltaWallTime logicSignatureTimer{measure};
    const auto addPhaseWork = [&](SubgraphGroup& group, LogicByScope& logic, const char* phaseName,
                                  VSubgraphPhase phase) {
        if (logic.empty()) return;
        V3Hash logicHash;
        SharedScheduleLogicSig logicSig
            = makeSharedScheduleLogicSig(logic, group.m_boundaryScopep, logicHash);
        SharedScheduleBoundaryAbi boundaryAbi
            = makeSharedScheduleBoundaryAbi(group.m_boundaryScopep);
        ++sharedBoundaryAbiAnalyses;
        sharedBoundaryAbiSlots += boundaryAbi.size();
        const uint64_t instanceStorageBindings
            = std::count_if(boundaryAbi.begin(), boundaryAbi.end(),
                            [](const auto& slot) { return slot.m_storageVscp != nullptr; });
        sharedBoundaryAbiInstanceStorageBindings += instanceStorageBindings;
        sharedBoundaryAbiExternalSlots += boundaryAbi.size() - instanceStorageBindings;
        phaseWorks.push_back(PreclassifiedSubgraphPhase{
            &group, &logic, phaseName, phase,
            SharedScheduleKey{group.m_boundaryScopep->modp(), phase, group.m_domainKeyp, logicHash,
                              std::move(boundaryAbi), std::move(logicSig)}});
    };
    for (SubgraphGroup& group : groups) {
        addPhaseWork(group, group.m_preLogic, "pre", VSubgraphPhase::PRE);
        addPhaseWork(group, group.m_postLogic, "post", VSubgraphPhase::POST);
        addPhaseWork(group, group.m_refreshLogic, "refresh", VSubgraphPhase::REFRESH);
    }
    std::vector<SubgraphScheduleEquivalenceClass> equivalenceClasses;
    std::unordered_multimap<V3Hash, size_t> equivalenceClassBuckets;
    for (size_t workIndex = 0; workIndex < phaseWorks.size(); ++workIndex) {
        PreclassifiedSubgraphPhase& work = phaseWorks[workIndex];
        ++sharedOrderCacheHashLookups;
        const auto bucketRange = equivalenceClassBuckets.equal_range(work.m_key.bucketHash());
        const uint64_t bucketCandidates
            = static_cast<uint64_t>(std::distance(bucketRange.first, bucketRange.second));
        sharedOrderCacheHashCandidates += bucketCandidates;
        sharedOrderCacheHashMaxCandidates
            = std::max(sharedOrderCacheHashMaxCandidates, bucketCandidates);
        if (!bucketCandidates) ++sharedOrderCacheHashMisses;
        for (auto bucketIt = bucketRange.first; bucketIt != bucketRange.second; ++bucketIt) {
            const size_t classIndex = bucketIt->second;
            SubgraphScheduleEquivalenceClass& equivalenceClass = equivalenceClasses[classIndex];
            const SharedScheduleKey& representativeKey
                = phaseWorks[equivalenceClass.m_representativeIndex].m_key;
            if (representativeKey.m_modp != work.m_key.m_modp
                || representativeKey.m_phase != work.m_key.m_phase
                || representativeKey.m_logicHash != work.m_key.m_logicHash) {
                ++sharedOrderCacheHashCollisions;
                continue;
            }
            if (!representativeKey.m_domainKeyp->sameTree(work.m_key.m_domainKeyp)) {
                ++sharedOrderCacheMissDomain;
                continue;
            }
            std::unordered_map<AstVarScope*, AstVarScope*> sourceToCandidate;
            const SharedScheduleMatchResult match
                = representativeKey.matchLogic(work.m_key, sourceToCandidate);
            if (match != SharedScheduleMatchResult::MATCH) {
                ++sharedOrderCacheHashCollisions;
                ++sharedOrderCacheLogicMismatches;
                switch (match) {
                case SharedScheduleMatchResult::ABI: ++sharedOrderCacheMissAbi; break;
                case SharedScheduleMatchResult::DTYPE_ACCESS:
                    ++sharedOrderCacheMissDTypeAccess;
                    break;
                case SharedScheduleMatchResult::RELATIVE_PATH:
                    ++sharedOrderCacheMissRelativePath;
                    break;
                case SharedScheduleMatchResult::TOPOLOGY: ++sharedOrderCacheMissTopology; break;
                case SharedScheduleMatchResult::MATCH: break;
                }
                continue;
            }
            work.m_classIndex = classIndex;
            ++equivalenceClass.m_members;
            break;
        }
        if (work.m_classIndex == std::numeric_limits<size_t>::max()) {
            work.m_classIndex = equivalenceClasses.size();
            equivalenceClasses.push_back(SubgraphScheduleEquivalenceClass{
                workIndex, std::numeric_limits<size_t>::max(), 1});
            equivalenceClassBuckets.emplace(work.m_key.bucketHash(), work.m_classIndex);
        }
    }
    for (const SubgraphScheduleEquivalenceClass& equivalenceClass : equivalenceClasses) {
        sharedScheduleEquivalenceMaxClassSize
            = std::max(sharedScheduleEquivalenceMaxClassSize, equivalenceClass.m_members);
    }
    if (measure) logicSignatureWallTime += logicSignatureTimer.deltaTime();
    unsigned groupIndex = 0;
    size_t phaseWorkIndex = 0;
    for (SubgraphGroup& group : groups) {
        orderedLogic
            += group.m_preLogic.size() + group.m_postLogic.size() + group.m_refreshLogic.size();
        UASSERT_OBJ(group.m_senTreep, group.m_boundaryScopep, "Subgraph NBA logic has no domain");

        const auto orderPhase = [&](LogicByScope& logic, const string& phase,
                                    VSubgraphPhase subgraphPhase) {
            if (logic.empty()) return;
            UASSERT(phaseWorkIndex < phaseWorks.size(), "Missing preclassified subgraph phase");
            const size_t currentWorkIndex = phaseWorkIndex++;
            PreclassifiedSubgraphPhase& phaseWork = phaseWorks[currentWorkIndex];
            UASSERT_OBJ(phaseWork.m_groupp == &group && phaseWork.m_logicp == &logic
                            && phaseWork.m_phaseName == phase
                            && phaseWork.m_phase == subgraphPhase,
                        group.m_boundaryScopep, "Preclassified subgraph phase order changed");
            SubgraphScheduleEquivalenceClass& equivalenceClass
                = equivalenceClasses[phaseWork.m_classIndex];
            const bool classRepresentative
                = equivalenceClass.m_representativeIndex == currentWorkIndex;
            const SubgraphInstanceTriggerBinding triggerBinding{
                SubgraphLocalTriggerId{0}, group.m_senTreep, group.m_domainKeyp};
            AstSenTree* const instanceSenTreep = triggerBinding.bind(triggerBinding.m_id);
            UASSERT_OBJ(triggerBinding.parentDomain(triggerBinding.m_id) == group.m_domainKeyp,
                        group.m_boundaryScopep, "Subgraph wrapper lost its exact parent domain");
            SharedScheduleKey scheduleKey = std::move(phaseWork.m_key);
            SharedHelperArtifact* cachedArtifactp = nullptr;
            size_t cachedArtifactIndex = std::numeric_limits<size_t>::max();
            std::unordered_map<AstVarScope*, AstVarScope*> sourceToCandidate;
            std::vector<SharedHelperArg> cachedArgs;
            const VlOs::DeltaWallTime cacheLookupTimer{measure};
            if (!freshOrder && !classRepresentative
                && equivalenceClass.m_artifactIndex != std::numeric_limits<size_t>::max()) {
                const size_t artifactIndex = equivalenceClass.m_artifactIndex;
                SharedHelperArtifact& artifact = sharedHelperArtifacts[artifactIndex];
                UASSERT_OBJ(artifact.m_key.m_modp == scheduleKey.m_modp
                                && artifact.m_key.m_phase == scheduleKey.m_phase
                                && artifact.m_key.m_logicHash == scheduleKey.m_logicHash
                                && artifact.m_key.m_domainKeyp->sameTree(scheduleKey.m_domainKeyp),
                            group.m_boundaryScopep,
                            "Preclassified subgraph representative key changed");
                ++sharedOrderCacheLookups;
                const SharedScheduleMatchResult match
                    = artifact.m_key.matchLogic(scheduleKey, sourceToCandidate);
                if (match != SharedScheduleMatchResult::MATCH) {
                    ++sharedOrderCacheLogicMismatches;
                    ++sharedScheduleEquivalenceBindingRejects;
                    switch (match) {
                    case SharedScheduleMatchResult::ABI: ++sharedOrderCacheMissAbi; break;
                    case SharedScheduleMatchResult::DTYPE_ACCESS:
                        ++sharedOrderCacheMissDTypeAccess;
                        break;
                    case SharedScheduleMatchResult::RELATIVE_PATH:
                        ++sharedOrderCacheMissRelativePath;
                        break;
                    case SharedScheduleMatchResult::TOPOLOGY:
                        ++sharedOrderCacheMissTopology;
                        break;
                    case SharedScheduleMatchResult::MATCH: break;
                    }
                } else if (!artifact.m_contractRecipe.validateBinding(
                               group.m_boundaryScopep, sourceToCandidate, scopeResolver,
                               subgraphPhase, triggerBinding)) {
                    ++sharedOrderCacheMissAbi;
                    ++contractRecipeFallbacks;
                    ++sharedScheduleEquivalenceBindingRejects;
                } else {
                    for (const SharedHelperArg& arg : artifact.m_args) {
                        const auto it = sourceToCandidate.find(arg.m_vscp);
                        if (it == sourceToCandidate.end()) break;
                        cachedArgs.push_back(
                            SharedHelperArg{it->second, arg.m_read, arg.m_write, arg.m_state});
                    }
                    if (cachedArgs.size() != artifact.m_args.size()) {
                        ++sharedOrderCacheMissAbi;
                        ++sharedScheduleEquivalenceBindingRejects;
                    } else {
                        ++sharedOrderCacheLogicMatches;
                        UASSERT_OBJ(
                            triggerBinding.bind(artifact.m_triggerId) == group.m_senTreep,
                            group.m_boundaryScopep,
                            "Canonical subgraph trigger binding changed its instance domain");
                        cachedArtifactp = &artifact;
                        cachedArtifactIndex = artifactIndex;
                        ++sharedScheduleEquivalenceReuses;
                    }
                }
            }
            if (measure) cacheLookupWallTime += cacheLookupTimer.deltaTime();
            const string tag = "nba_subgraph_" + phase + "_" + cvtToStr(groupIndex);
            const bool refresh = subgraphPhase == VSubgraphPhase{VSubgraphPhase::REFRESH};
            AstCFunc* funcp = nullptr;
            std::unique_ptr<V3SubgraphContract> contractp;
            const size_t externalBindingsBegin = contractExternalBindings.size();
            size_t externalBindingsCount = 0;
            SharedHelperAbiAnalysis abi;
            if (cachedArtifactp) {
                const VlOs::DeltaWallTime cacheReuseTimer{measure};
                const SharedScheduleContractRecipe& recipe = cachedArtifactp->m_contractRecipe;
                recipe.appendExternalBindings(sourceToCandidate, triggerBinding,
                                              contractExternalBindings);
                externalBindingsCount = contractExternalBindings.size() - externalBindingsBegin;
                contractInstanceBindings += externalBindingsCount;
                contractUsesNotExpanded += recipe.m_boundaryUses.size()
                                           + recipe.m_externalUses.size()
                                           + recipe.m_internalUses.size();
                if (cachedArtifactp->m_instanceContext) {
                    preserveSharedInstanceUses(recipe, group.m_boundaryScopep, scopeResolver);
                }
                if (!cachedArtifactp->m_parameterized) {
                    const SharedHelperIsolationAnalysis isolation
                        = parameterizeSharedHelper(*cachedArtifactp);
                    ++sharedHelperIsolationChecks;
                    sharedHelperIsolationGlobalTriggerRefs += isolation.m_globalTriggerRefs;
                    sharedHelperIsolationRefs += isolation.m_refs;
                    sharedHelperIsolationUnboundRefs += isolation.m_unboundRefs;
                    sharedHelperArguments += cachedArtifactp->m_args.size();
                    ++sharedHelperParameterizations;
                }
                funcp = makeSharedScheduleWrapper(netlistp, group.m_boundaryScopep, triggerBinding,
                                                  cachedArtifactp->m_triggerId,
                                                  cachedArtifactp->m_funcp, cachedArgs, tag, slow,
                                                  refresh, cachedArtifactp->m_instanceContext);
                ++sharedExactParentDomainReusedBodyWrappers;
                if (cachedArtifactp->m_instanceContext) {
                    releaseDiscardedCallClosureEntries(logic, group.m_boundaryScopep);
                }
                discardSharedScheduleLogic(logic);
                abi = cachedArtifactp->m_abi;
                ++sharedHelperReuses;
                ++sharedOrderCacheOrderCallsAvoided;
                if (cachedArtifactp->m_instanceContext) {
                    ++canonicalContextReuses;
                    ++canonicalOrderCallsAvoided;
                }
                if (measure) cacheReuseWallTime += cacheReuseTimer.deltaTime();
            } else {
                if (freshOrder) {
                    ++sharedScheduleEquivalenceFreshOrderCalls;
                } else if (classRepresentative) {
                    ++sharedScheduleEquivalenceRepresentativeOrderCalls;
                } else {
                    ++sharedScheduleEquivalenceFallbackOrderCalls;
                }
                const VlOs::DeltaWallTime orderCallsTimer{measure};
                V3Order::ExternalDomainsProvider phaseExternalDomains = externalDomains;
                if (refresh) {
                    // Isolated combinational logic has no visible external drivers, so V3Order
                    // would prune it as unreachable. Give every input one artificial NBA domain
                    // while ordering; the parent coarse node derives the real domain from its
                    // contract.
                    AstSenTree* orderDomainp = nullptr;
                    for (const SubgraphGroup& candidate : groups) {
                        if (candidate.m_boundaryScopep == group.m_boundaryScopep
                            && !candidate.m_senTreep->hasCombo()) {
                            orderDomainp = candidate.m_senTreep;
                            break;
                        }
                    }
                    if (!orderDomainp) {
                        for (const auto& pair : trigToSen) {
                            if (pair.first->hasCombo()) continue;
                            orderDomainp = const_cast<AstSenTree*>(pair.first);
                            break;
                        }
                    }
                    UASSERT_OBJ(orderDomainp, group.m_boundaryScopep,
                                "Subgraph refresh helper has no NBA domain");
                    phaseExternalDomains
                        = [orderDomainp](const AstVarScope*, std::vector<AstSenTree*>& out) {
                              out.push_back(orderDomainp);
                          };
                }
                ++sharedOrderCacheOrderCallsExecuted;
                funcp = V3Order::order(netlistp, {&logic}, trigToSen, tag, false, slow,
                                       phaseExternalDomains, group.m_boundaryScopep);
                if (measure) orderCallsWallTime += orderCallsTimer.deltaTime();
                if (funcp) {
                    const VlOs::DeltaWallTime contractAbiTimer{measure};
                    if (refresh) removeSingleDomainGuard(funcp);
                    util::splitCheck(funcp);
                    contractp = std::make_unique<V3SubgraphContract>(V3SubgraphContract::make(
                        funcp, group.m_boundaryScopep, instanceSenTreep,
                        subgraphPhase == VSubgraphPhase{VSubgraphPhase::POST}, refresh));
                    abi = SharedHelperAbiAnalyzer{funcp, group.m_boundaryScopep, *contractp}
                              .result();
                    if (measure) contractAbiWallTime += contractAbiTimer.deltaTime();
                }
            }
            if (!funcp) return;
            ++sharedLocalTriggerInstanceBindings;
            const VlOs::DeltaWallTime contractAccountingTimer{measure};
            const auto accountInternalUse = [&](AstVarScope* vscp, bool read, bool write) {
                if (V3SubgraphContract::isDelayedState(vscp)) { parentAccessedVscps.insert(vscp); }
                if (subgraphPhase == VSubgraphPhase{VSubgraphPhase::POST} && write) {
                    postWrittenInternalVscps.insert(vscp);
                }
                if (subgraphPhase == VSubgraphPhase{VSubgraphPhase::REFRESH} && read) {
                    refreshReadInternalVscps.insert(vscp);
                }
            };
            if (cachedArtifactp) {
                const SharedScheduleContractRecipe& recipe = cachedArtifactp->m_contractRecipe;
                contractBoundaryUses += recipe.m_boundaryUses.size();
                contractExternalUses += recipe.m_externalUses.size();
                contractInternalUses += recipe.m_internalUses.size();
                for (const SharedScheduleUseRecipe& use : recipe.m_internalUses) {
                    AstVarScope* const vscp = scopeResolver.bind(group.m_boundaryScopep, use.m_id);
                    UASSERT_OBJ(vscp, group.m_boundaryScopep,
                                "Canonical internal contract failed to resolve");
                    accountInternalUse(vscp, use.m_read, use.m_write);
                }
            } else {
                const V3SubgraphContract& contract = *contractp;
                contractBoundaryUses += contract.boundaryUses().size();
                contractExternalUses += contract.externalUses().size();
                contractInternalUses += contract.internalUses().size();
                for (const V3SubgraphContract::Use& use : contract.internalUses()) {
                    accountInternalUse(use.m_varScopep, use.m_read, use.m_write);
                }
            }
            ++contracts;
            noteSharedAbi(abi, subgraphPhase, group.m_boundaryScopep->modp());
            if (measure) contractAbiWallTime += contractAccountingTimer.deltaTime();

            const VlOs::DeltaWallTime helperSharingTimer{measure};
            AstActive* const wrapperp
                = new AstActive{group.m_filelinep, "subgraph", instanceSenTreep};
            ++sharedExactParentDomainWrapperBindings;
            AstCCall* const callExprp = new AstCCall{funcp->fileline(), funcp};
            callExprp->dtypeSetVoid();
            AstNode* const callp = callExprp->makeStmt();
            AstCFunc* sharedFuncp = funcp;
            AstCCall* sharedCallp = callExprp;
            SharedHelperAbiAnalysis sharedAbi = abi;
            if (!cachedArtifactp && abi.m_calls) {
                if (AstCCall* const localCallp = soleLocalHelperCall(funcp)) {
                    sharedFuncp = localCallp->funcp();
                    sharedCallp = localCallp;
                    sharedAbi
                        = SharedHelperAbiAnalyzer{sharedFuncp, group.m_boundaryScopep, *contractp}
                              .result();
                }
            }
            const bool generatedTemps
                = sharedAbi.m_generatedTemps || sharedFuncp->varsp() || sharedFuncp->argsp();
            const bool shareCandidate = sharedAbi.m_eligible && !sharedAbi.m_globalTriggerRefs
                                        && !sharedAbi.m_hasTriggeredState && !generatedTemps;
            const std::vector<SharedHelperArg> args
                = shareCandidate ? collectSharedHelperArgs(sharedFuncp, group.m_boundaryScopep)
                                 : std::vector<SharedHelperArg>{};
            // A canonical context helper keeps boundary-local state behind the caller's instance
            // pointer. It is keyed by module specialization, phase, event domain, and exact
            // relative variable identity, so sibling paths are never alpha-renamed together.
            static constexpr size_t kMaxSharedHelperArgs = 8;
            const bool canonicalContextCandidate
                = shareCandidate && sharedFuncp->isLoose() && !sharedFuncp->isStatic()
                  && sharedFuncp->scopep() == group.m_boundaryScopep;
            const std::vector<SharedHelperArg> contextArgs
                = canonicalContextCandidate
                      ? collectSharedHelperArgs(sharedFuncp, group.m_boundaryScopep, true)
                      : std::vector<SharedHelperArg>{};
            const bool compositeArgs
                = std::any_of(args.begin(), args.end(), [](const SharedHelperArg& arg) {
                      AstNodeDType* const dtypep = arg.m_vscp->dtypep()->skipRefp();
                      return !VN_IS(dtypep, BasicDType) || dtypep->isWide();
                  });
            // Canonical external slots have already been checked for one-to-one aliasing, dtype,
            // and access equivalence. Read-only composite arguments use constref; writable ones
            // use output or inout. The exact-key fallback remains scalar-only.
            if (cachedArtifactp) {
                // The cached artifact was already validated and accounted for above.
            } else if (canonicalContextCandidate && contextArgs.size() <= kMaxSharedHelperArgs) {
                SharedScheduleContractRecipe contractRecipe = SharedScheduleContractRecipe::make(
                    *contractp, scheduleKey.m_logicSig, triggerBinding.m_id);
                const uint64_t directExternalUses = contractRecipe.directExternalUses();
                const uint64_t directGlobalTriggerUses = contractRecipe.directGlobalTriggerUses();
                UASSERT_OBJ(directGlobalTriggerUses == contractRecipe.localTriggerUses(), funcp,
                            "Canonical global trigger use has no local trigger ID");
                canonicalContractDirectExternalUses += directExternalUses;
                canonicalContractDirectGlobalTriggerUses += directGlobalTriggerUses;
                canonicalContractDirectOtherUses += directExternalUses - directGlobalTriggerUses;
                canonicalContractLocalTriggerUses += contractRecipe.localTriggerUses();
                sharedHelperArtifacts.push_back(SharedHelperArtifact{
                    std::move(scheduleKey), SubgraphLocalTriggerId{0}, sharedFuncp, sharedCallp,
                    contextArgs, std::move(contractRecipe), abi, true, false});
                if (equivalenceClass.m_artifactIndex == std::numeric_limits<size_t>::max()) {
                    equivalenceClass.m_artifactIndex = sharedHelperArtifacts.size() - 1;
                }
                ++sharedHelperArtifactCount;
                ++canonicalContextArtifacts;
                ++sharedLocalTriggerIds;
            } else if (!canonicalContextCandidate && shareCandidate && !compositeArgs
                       && args.size() <= kMaxSharedHelperArgs) {
                // Keep unsupported helper shapes reusable only through their exact schedule key.
                // Canonical helpers above never clone or compare an already ordered C++ body.
                SharedScheduleContractRecipe contractRecipe = SharedScheduleContractRecipe::make(
                    *contractp, scheduleKey.m_logicSig, triggerBinding.m_id);
                const uint64_t directExternalUses = contractRecipe.directExternalUses();
                const uint64_t directGlobalTriggerUses = contractRecipe.directGlobalTriggerUses();
                UASSERT_OBJ(directGlobalTriggerUses == contractRecipe.localTriggerUses(), funcp,
                            "Canonical global trigger use has no local trigger ID");
                canonicalContractDirectExternalUses += directExternalUses;
                canonicalContractDirectGlobalTriggerUses += directGlobalTriggerUses;
                canonicalContractDirectOtherUses += directExternalUses - directGlobalTriggerUses;
                canonicalContractLocalTriggerUses += contractRecipe.localTriggerUses();
                sharedHelperArtifacts.push_back(SharedHelperArtifact{
                    std::move(scheduleKey), SubgraphLocalTriggerId{0}, sharedFuncp, sharedCallp,
                    args, std::move(contractRecipe), abi, false, false});
                if (equivalenceClass.m_artifactIndex == std::numeric_limits<size_t>::max()) {
                    equivalenceClass.m_artifactIndex = sharedHelperArtifacts.size() - 1;
                }
                ++sharedHelperArtifactCount;
                ++sharedLocalTriggerIds;
            } else if (!canonicalContextCandidate && compositeArgs) {
                ++sharedHelperSkippedComposite;
            } else if (shareCandidate) {
                ++sharedHelperSkippedOversized;
            } else if (sharedAbi.m_dpiCalls) {
                ++sharedHelperSkippedDpiCalls;
            } else if (sharedAbi.m_hasTriggeredState) {
                ++sharedHelperSkippedTriggered;
            } else if (sharedAbi.m_calls) {
                ++sharedHelperSkippedCalls;
            } else if (generatedTemps) {
                ++sharedHelperSkippedGeneratedTemps;
            }
            if (measure) helperSharingWallTime += helperSharingTimer.deltaTime();
            const VlOs::DeltaWallTime materializeTimer{measure};
            AstSubgraphInstance* const instancep = new AstSubgraphInstance{
                group.m_filelinep, group.m_boundaryScopep, subgraphPhase, callp};
            for (const V3SubgraphContract::LogicalUse& use :
                 V3SubgraphContract::makeLogicalBoundaryUses(group.m_boundaryScopep)) {
                instancep->addLogicalUse(use.m_name, use.m_read, use.m_write);
            }
            logicalUses += instancep->logicalUseCount();
            ++coarseNodes;
            if (subgraphPhase == VSubgraphPhase{VSubgraphPhase::POST}) {
                AstAlwaysPost* const postp = new AstAlwaysPost{group.m_filelinep};
                postp->addStmtsp(instancep);
                wrapperp->addStmtsp(postp);
            } else {
                wrapperp->addStmtsp(instancep);
            }
            group.m_ownerp->emplace_back(group.m_boundaryScopep, wrapperp);
            pendingMaterializations.push_back(PendingSubgraphMaterialization{
                instancep, &group, std::move(contractp), cachedArtifactIndex,
                externalBindingsBegin, externalBindingsCount});
            if (measure) materializeWallTime += materializeTimer.deltaTime();
        };

        orderPhase(group.m_preLogic, "pre", VSubgraphPhase::PRE);
        orderPhase(group.m_postLogic, "post", VSubgraphPhase::POST);
        if (!group.m_refreshLogic.empty()) ++refreshHelpers;
        orderPhase(group.m_refreshLogic, "refresh", VSubgraphPhase::REFRESH);
        ++groupIndex;
    }
    UASSERT(phaseWorkIndex == phaseWorks.size(), "Unused preclassified subgraph phase");

    for (AstVarScope* const vscp : refreshReadInternalVscps) {
        if (postWrittenInternalVscps.count(vscp)) parentAccessedVscps.insert(vscp);
    }
    // Materialize only internal uses that the parent graph consumes. Delaying this until all
    // contracts are known preserves POST-to-REFRESH dependencies without allocating metadata for
    // helper-private state.
    const VlOs::DeltaWallTime deferredMaterializeTimer{measure};
    for (PendingSubgraphMaterialization& pending : pendingMaterializations) {
        AstSubgraphInstance* const instancep = pending.m_instancep;
        const SubgraphGroup& group = *pending.m_groupp;
        const auto keepInternalUse = [&](AstVarScope* vscp, bool write) {
            const bool delayedState = V3SubgraphContract::isDelayedState(vscp);
            const bool parentAccessed = parentAccessedVscps.count(vscp);
            const bool publishRead = instancep->phase() == VSubgraphPhase{VSubgraphPhase::REFRESH};
            return delayedState || (parentAccessed && (publishRead || write));
        };
        const auto addExternalUse = [&](AstVarScope* vscp, bool read, bool write, bool cuttable) {
            const bool snapshotStorage = std::any_of(
                group.m_snapshots.begin(), group.m_snapshots.end(),
                [&](const auto& snapshot) { return snapshot.m_storageVscp == vscp; });
            instancep->addMaterializedUse(
                vscp,
                snapshotStorage ? VSubgraphUseKind{VSubgraphUseKind::SNAPSHOT_STORAGE}
                                : VSubgraphUseKind{VSubgraphUseKind::EXTERNAL},
                read, write, cuttable);
            ++materializedExternalUses;
        };
        if (pending.m_contractp) {
            const V3SubgraphContract& contract = *pending.m_contractp;
            const std::vector<V3SubgraphContract::Use>& internalUses = contract.internalUses();
            const size_t keptInternalUses
                = std::count_if(internalUses.begin(), internalUses.end(), [&](const auto& use) {
                      return keepInternalUse(use.m_varScopep, use.m_write);
                  });
            instancep->reserveMaterializedUses(contract.boundaryUses().size()
                                               + contract.externalUses().size()
                                               + keptInternalUses);
            for (const V3SubgraphContract::Use& use : contract.boundaryUses()) {
                instancep->addMaterializedUse(use.m_varScopep, VSubgraphUseKind::BOUNDARY,
                                              use.m_read, use.m_write, use.m_cuttable);
                ++materializedBoundaryUses;
            }
            for (const V3SubgraphContract::Use& use : contract.externalUses()) {
                addExternalUse(use.m_varScopep, use.m_read, use.m_write, use.m_cuttable);
            }
            for (const V3SubgraphContract::Use& use : internalUses) {
                if (!keepInternalUse(use.m_varScopep, use.m_write)) {
                    ++prunedInternalUses;
                    continue;
                }
                instancep->addMaterializedUse(use.m_varScopep, VSubgraphUseKind::INTERNAL,
                                              use.m_read, use.m_write, use.m_cuttable);
                ++materializedInternalUses;
            }
            continue;
        }

        UASSERT_OBJ(pending.m_artifactIndex < sharedHelperArtifacts.size(), instancep,
                    "Canonical subgraph contract has no artifact");
        const SharedScheduleContractRecipe& recipe
            = sharedHelperArtifacts[pending.m_artifactIndex].m_contractRecipe;
        size_t keptInternalUses = 0;
        for (const SharedScheduleUseRecipe& use : recipe.m_internalUses) {
            AstVarScope* const vscp = scopeResolver.bind(group.m_boundaryScopep, use.m_id);
            UASSERT_OBJ(vscp, instancep, "Canonical internal contract failed to resolve");
            keptInternalUses += keepInternalUse(vscp, use.m_write);
        }
        UASSERT_OBJ(pending.m_externalBindingsCount == recipe.m_externalUses.size(), instancep,
                    "Canonical external contract binding count mismatch");
        UASSERT_OBJ(pending.m_externalBindingsBegin + pending.m_externalBindingsCount
                        <= contractExternalBindings.size(),
                    instancep, "Canonical external contract binding range is invalid");
        instancep->reserveMaterializedUses(recipe.m_boundaryUses.size()
                                           + recipe.m_externalUses.size() + keptInternalUses);
        for (const SharedScheduleUseRecipe& use : recipe.m_boundaryUses) {
            AstVarScope* const vscp = scopeResolver.bind(group.m_boundaryScopep, use.m_id);
            UASSERT_OBJ(vscp, instancep, "Canonical boundary contract failed to resolve");
            instancep->addMaterializedUse(vscp, VSubgraphUseKind::BOUNDARY, use.m_read,
                                          use.m_write, use.m_cuttable);
            ++materializedBoundaryUses;
        }
        for (size_t index = 0; index < recipe.m_externalUses.size(); ++index) {
            const SharedScheduleUseRecipe& use = recipe.m_externalUses[index];
            addExternalUse(contractExternalBindings[pending.m_externalBindingsBegin + index],
                           use.m_read, use.m_write, use.m_cuttable);
        }
        for (const SharedScheduleUseRecipe& use : recipe.m_internalUses) {
            AstVarScope* const vscp = scopeResolver.bind(group.m_boundaryScopep, use.m_id);
            UASSERT_OBJ(vscp, instancep, "Canonical internal contract failed to resolve");
            if (!keepInternalUse(vscp, use.m_write)) {
                ++prunedInternalUses;
                continue;
            }
            instancep->addMaterializedUse(vscp, VSubgraphUseKind::INTERNAL, use.m_read,
                                          use.m_write, use.m_cuttable);
            ++materializedInternalUses;
        }
    }
    if (measure) materializeWallTime += deferredMaterializeTimer.deltaTime();

    V3Stats::addStatPerf("Scheduling, Subgraph NBA elapsed time (sec), cache lookup",
                         cacheLookupWallTime);
    V3Stats::addStatPerf("Scheduling, Subgraph NBA elapsed time (sec), cache reuse",
                         cacheReuseWallTime);
    V3Stats::addStatPerf("Scheduling, Subgraph NBA elapsed time (sec), collect", collectWallTime);
    V3Stats::addStatPerf("Scheduling, Subgraph NBA elapsed time (sec), contract and ABI",
                         contractAbiWallTime);
    V3Stats::addStatPerf("Scheduling, Subgraph NBA elapsed time (sec), helper sharing",
                         helperSharingWallTime);
    V3Stats::addStatPerf("Scheduling, Subgraph NBA elapsed time (sec), logic signatures",
                         logicSignatureWallTime);
    V3Stats::addStatPerf("Scheduling, Subgraph NBA elapsed time (sec), materialize coarse nodes",
                         materializeWallTime);
    V3Stats::addStatPerf("Scheduling, Subgraph NBA elapsed time (sec), order calls",
                         orderCallsWallTime);
    V3Stats::addStatPerf("Scheduling, Subgraph NBA elapsed time (sec), snapshots",
                         snapshotsWallTime);
    V3Stats::addStat("Scheduling, Subgraph NBA groups", groups.size());
    V3Stats::addStat("Scheduling, Subgraph NBA internal actives", orderedLogic);
    V3Stats::addStat("Scheduling, Subgraph NBA contract boundary uses", contractBoundaryUses);
    V3Stats::addStat("Scheduling, Subgraph NBA contract external uses", contractExternalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA contract instance bindings",
                     contractInstanceBindings);
    V3Stats::addStat("Scheduling, Subgraph NBA contract internal uses", contractInternalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA contract recipe fallbacks",
                     contractRecipeFallbacks);
    V3Stats::addStat("Scheduling, Subgraph NBA contract uses not expanded",
                     contractUsesNotExpanded);
    // External uses retain one pointer-sized instance binding; boundary and internal uses retain
    // no per-instance Use record. Exclude container/object overhead for a conservative figure.
    V3Stats::addStat("Scheduling, Subgraph NBA contract metadata bytes avoided",
                     contractUsesNotExpanded * sizeof(V3SubgraphContract::Use)
                         - contractInstanceBindings * sizeof(AstVarScope*));
    V3Stats::addStat("Scheduling, Subgraph NBA contracts", contracts);
    V3Stats::addStat("Scheduling, Subgraph NBA coarse nodes", coarseNodes);
    V3Stats::addStat("Scheduling, Subgraph NBA logical uses", logicalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized boundary uses",
                     materializedBoundaryUses);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized external uses",
                     materializedExternalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized internal uses",
                     materializedInternalUses);
    const uint64_t materializedMetadataBytes = (materializedBoundaryUses + materializedExternalUses
                                                + materializedInternalUses + 2 * snapshotSources)
                                               * sizeof(V3SubgraphMaterializedUse);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized metadata bytes",
                     materializedMetadataBytes);
    V3Stats::addStat("Scheduling, Subgraph NBA pruned internal uses", prunedInternalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA refresh helpers", refreshHelpers);
    V3Stats::addStat("Scheduling, Subgraph NBA snapshot instances", snapshotInstances);
    V3Stats::addStat("Scheduling, Subgraph NBA snapshot sources", snapshotSources);
    V3Stats::addStat("Scheduling, Subgraph schedule equivalence binding rejects",
                     sharedScheduleEquivalenceBindingRejects);
    V3Stats::addStat("Scheduling, Subgraph schedule equivalence classes",
                     equivalenceClasses.size());
    V3Stats::addStat("Scheduling, Subgraph schedule equivalence fallback order calls",
                     sharedScheduleEquivalenceFallbackOrderCalls);
    V3Stats::addStat("Scheduling, Subgraph schedule equivalence fresh order calls",
                     sharedScheduleEquivalenceFreshOrderCalls);
    V3Stats::addStat("Scheduling, Subgraph schedule equivalence instances", phaseWorks.size());
    V3Stats::addStat("Scheduling, Subgraph schedule equivalence max class size",
                     sharedScheduleEquivalenceMaxClassSize);
    V3Stats::addStat("Scheduling, Subgraph schedule equivalence potential reuses",
                     phaseWorks.size() - equivalenceClasses.size());
    V3Stats::addStat("Scheduling, Subgraph schedule equivalence representative order calls",
                     sharedScheduleEquivalenceRepresentativeOrderCalls);
    V3Stats::addStat("Scheduling, Subgraph schedule equivalence reuses",
                     sharedScheduleEquivalenceReuses);
    V3Stats::addStat("Scheduling, Subgraph canonical context artifacts",
                     canonicalContextArtifacts);
    V3Stats::addStat("Scheduling, Subgraph canonical contract direct external uses",
                     canonicalContractDirectExternalUses);
    V3Stats::addStat("Scheduling, Subgraph canonical contract direct global trigger uses",
                     canonicalContractDirectGlobalTriggerUses);
    V3Stats::addStat("Scheduling, Subgraph canonical contract direct other uses",
                     canonicalContractDirectOtherUses);
    V3Stats::addStat("Scheduling, Subgraph canonical contract local trigger uses",
                     canonicalContractLocalTriggerUses);
    V3Stats::addStat("Scheduling, Subgraph canonical C++ bodies", canonicalContextArtifacts);
    V3Stats::addStat("Scheduling, Subgraph canonical context reuses", canonicalContextReuses);
    V3Stats::addStat("Scheduling, Subgraph canonical order calls avoided",
                     canonicalOrderCallsAvoided);
    V3Stats::addStat("Scheduling, Subgraph shared boundary ABI analyses",
                     sharedBoundaryAbiAnalyses);
    V3Stats::addStat("Scheduling, Subgraph shared boundary ABI external slots",
                     sharedBoundaryAbiExternalSlots);
    V3Stats::addStat("Scheduling, Subgraph shared boundary ABI instance storage bindings",
                     sharedBoundaryAbiInstanceStorageBindings);
    V3Stats::addStat("Scheduling, Subgraph shared boundary ABI slots", sharedBoundaryAbiSlots);
    V3Stats::addStat("Scheduling, Subgraph shared ABI analyses", sharedAbiAnalyses);
    V3Stats::addStat("Scheduling, Subgraph shared ABI constants", sharedAbiConstants);
    V3Stats::addStat("Scheduling, Subgraph shared ABI DPI calls", sharedAbiDpiCalls);
    V3Stats::addStat("Scheduling, Subgraph shared ABI eligible helpers", sharedAbiEligibleHelpers);
    V3Stats::addStat("Scheduling, Subgraph shared ABI external vars", sharedAbiExternalVars);
    V3Stats::addStat("Scheduling, Subgraph shared ABI generated temps", sharedAbiGeneratedTemps);
    V3Stats::addStat("Scheduling, Subgraph shared ABI global trigger refs",
                     sharedAbiGlobalTriggerRefs);
    V3Stats::addStat("Scheduling, Subgraph shared ABI hidden uses", sharedAbiHiddenUses);
    V3Stats::addStat("Scheduling, Subgraph shared ABI input vars", sharedAbiInputVars);
    V3Stats::addStat("Scheduling, Subgraph shared ABI module-phase candidates",
                     sharedAbiModulePhaseCandidates);
    V3Stats::addStat("Scheduling, Subgraph shared ABI output vars", sharedAbiOutputVars);
    V3Stats::addStat("Scheduling, Subgraph shared ABI state vars", sharedAbiStateVars);
    V3Stats::addStat("Scheduling, Subgraph shared exact parent domain reused body wrappers",
                     sharedExactParentDomainReusedBodyWrappers);
    V3Stats::addStat("Scheduling, Subgraph shared exact parent domain wrapper bindings",
                     sharedExactParentDomainWrapperBindings);
    V3Stats::addStat("Scheduling, Subgraph shared helper arguments", sharedHelperArguments);
    V3Stats::addStat("Scheduling, Subgraph shared helper artifacts", sharedHelperArtifactCount);
    V3Stats::addStat("Scheduling, Subgraph shared helper body checks", sharedHelperBodyChecks);
    V3Stats::addStat("Scheduling, Subgraph shared helper body mismatches",
                     sharedHelperBodyMismatches);
    V3Stats::addStat("Scheduling, Subgraph shared helper isolation checks",
                     sharedHelperIsolationChecks);
    V3Stats::addStat("Scheduling, Subgraph shared helper isolation global trigger refs",
                     sharedHelperIsolationGlobalTriggerRefs);
    V3Stats::addStat("Scheduling, Subgraph shared helper isolation refs",
                     sharedHelperIsolationRefs);
    V3Stats::addStat("Scheduling, Subgraph shared helper isolation unbound refs",
                     sharedHelperIsolationUnboundRefs);
    V3Stats::addStat("Scheduling, Subgraph shared helper parameterizations",
                     sharedHelperParameterizations);
    V3Stats::addStat("Scheduling, Subgraph shared helper reuses", sharedHelperReuses);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped calls", sharedHelperSkippedCalls);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped composite",
                     sharedHelperSkippedComposite);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped DPI calls",
                     sharedHelperSkippedDpiCalls);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped generated temps",
                     sharedHelperSkippedGeneratedTemps);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped oversized",
                     sharedHelperSkippedOversized);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped triggered",
                     sharedHelperSkippedTriggered);
    V3Stats::addStat("Scheduling, Subgraph shared local trigger IDs", sharedLocalTriggerIds);
    V3Stats::addStat("Scheduling, Subgraph shared local trigger instance bindings",
                     sharedLocalTriggerInstanceBindings);
    V3Stats::addStat("Scheduling, Subgraph shared order cache logic matches",
                     sharedOrderCacheLogicMatches);
    V3Stats::addStat("Scheduling, Subgraph shared order cache logic mismatches",
                     sharedOrderCacheLogicMismatches);
    V3Stats::addStat("Scheduling, Subgraph shared order cache lookups", sharedOrderCacheLookups);
    V3Stats::addStat("Scheduling, Subgraph shared order cache hash candidates",
                     sharedOrderCacheHashCandidates);
    V3Stats::addStat("Scheduling, Subgraph shared order cache hash collisions",
                     sharedOrderCacheHashCollisions);
    V3Stats::addStat("Scheduling, Subgraph shared order cache hash lookups",
                     sharedOrderCacheHashLookups);
    V3Stats::addStat("Scheduling, Subgraph shared order cache hash max candidates",
                     sharedOrderCacheHashMaxCandidates);
    V3Stats::addStat("Scheduling, Subgraph shared order cache hash misses",
                     sharedOrderCacheHashMisses);
    V3Stats::addStat("Scheduling, Subgraph shared order cache miss ABI", sharedOrderCacheMissAbi);
    V3Stats::addStat("Scheduling, Subgraph shared order cache miss domain",
                     sharedOrderCacheMissDomain);
    V3Stats::addStat("Scheduling, Subgraph shared order cache miss dtype/access",
                     sharedOrderCacheMissDTypeAccess);
    V3Stats::addStat("Scheduling, Subgraph shared order cache miss phase",
                     sharedOrderCacheMissPhase);
    V3Stats::addStat("Scheduling, Subgraph shared order cache miss relative path",
                     sharedOrderCacheMissRelativePath);
    V3Stats::addStat("Scheduling, Subgraph shared order cache miss topology",
                     sharedOrderCacheMissTopology);
    V3Stats::addStat("Scheduling, Subgraph shared order cache order calls executed",
                     sharedOrderCacheOrderCallsExecuted);
    V3Stats::addStat("Scheduling, Subgraph shared order cache order calls avoided",
                     sharedOrderCacheOrderCallsAvoided);
}

}  // namespace V3Sched
