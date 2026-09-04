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
#include "V3SubgraphContract.h"

#include <unordered_map>
#include <unordered_set>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3Sched {

namespace {

struct SubgraphSnapshot final {
    AstVarScope* m_sourceVscp = nullptr;
    AstVarScope* m_storageVscp = nullptr;
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
};

class SubgraphUsePatternInterner final {
    std::unordered_map<string, std::shared_ptr<const V3SubgraphUsePattern>> m_patterns;

    static string key(const V3SubgraphUsePattern& pattern) {
        string result;
        result.reserve(pattern.size());
        for (const V3SubgraphUsePatternEntry& entry : pattern) {
            const unsigned value = static_cast<unsigned>(entry.kind()) | (entry.read() << 3)
                                   | (entry.write() << 4) | (entry.cuttable() << 5);
            result.push_back(static_cast<char>(value));
        }
        return result;
    }

public:
    std::pair<std::shared_ptr<const V3SubgraphUsePattern>, bool>
    intern(V3SubgraphUsePattern&& pattern) {
        string patternKey = key(pattern);
        const auto it = m_patterns.find(patternKey);
        if (it != m_patterns.end()) return {it->second, false};
        const std::shared_ptr<const V3SubgraphUsePattern> patternp
            = std::make_shared<const V3SubgraphUsePattern>(std::move(pattern));
        m_patterns.emplace(std::move(patternKey), patternp);
        return {patternp, true};
    }
};

bool isUnderScope(const AstScope* scopep, const AstScope* basep);

struct SharedHelperAbiAnalysis final {
    uint64_t m_calls = 0;
    uint64_t m_constants = 0;
    uint64_t m_dpiCalls = 0;
    uint64_t m_externalVars = 0;
    uint64_t m_generatedTemps = 0;
    uint64_t m_hiddenUses = 0;
    uint64_t m_inputVars = 0;
    uint64_t m_outputVars = 0;
    uint64_t m_stateVars = 0;
    bool m_contextSafe = true;
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
    SharedHelperAbiAnalysis m_result;
    AstCFunc* m_cfuncp = nullptr;

    void visit(AstCFunc* nodep) override {
        if (!m_visitedFuncps.emplace(nodep).second) return;
        VL_RESTORER(m_cfuncp);
        m_cfuncp = nodep;
        if (!nodep->isStatic() && !isUnderScope(nodep->scopep(), m_boundaryScopep)) {
            m_result.m_contextSafe = false;
        }
        if ((nodep == m_rootFuncp && (!nodep->isLoose() || nodep->entryPoint()))
            || nodep->needProcess() || nodep->recursive() || nodep->isCoroutine()) {
            m_result.m_eligible = false;
        }
        iterateChildren(nodep);
    }
    void visit(AstCCall* nodep) override {
        ++m_result.m_calls;
        iterateChildren(nodep);
        AstCFunc* const funcp = nodep->funcp();
        if (funcp->dpiImportPrototype() || funcp->dpiImportWrapper() || funcp->dpiContext()) {
            ++m_result.m_dpiCalls;
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
        auto& access = m_accesses[vscp];
        access.first |= nodep->access().isReadOrRW();
        access.second |= nodep->access().isWriteOrRW();
        if (m_cfuncp != m_rootFuncp && !vscp->varp()->isFuncLocal()
            && !isUnderScope(vscp->scopep(), m_boundaryScopep)) {
            m_result.m_contextSafe = false;
        }
        if (!vscp->varp()->isFuncLocal() && !m_contractVscps.count(vscp)) {
            m_hiddenVscps.insert(vscp);
        }
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
            if (vscp->varp()->isFuncLocal()) {
                ++m_result.m_generatedTemps;
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

struct SharedScheduleLogicRef final {
    uintptr_t m_access = 0;
    AstVarScope* m_vscp = nullptr;
};

struct SharedScheduleLogicNode final {
    uintptr_t m_type = 0;
    std::vector<uintptr_t> m_nodeTypes;
    std::vector<string> m_constValues;
    std::vector<SharedScheduleLogicRef> m_refs;
};

using SharedScheduleLogicSig = std::vector<SharedScheduleLogicNode>;

class SharedScheduleLogicNodeBuilder final : public VNVisitor {
    SharedScheduleLogicNode& m_result;
    std::unordered_set<const AstCFunc*> m_visitedFuncps;

    void record(AstNode* nodep) {
        m_result.m_nodeTypes.push_back(static_cast<uintptr_t>(nodep->type()));
    }
    void visit(AstCFunc* nodep) override {
        if (!m_visitedFuncps.emplace(nodep).second) return;
        iterateChildren(nodep);
    }
    void visit(AstCCall* nodep) override {
        record(nodep);
        iterateChildren(nodep);
        iterate(nodep->funcp());
    }
    void visit(AstConst* nodep) override {
        record(nodep);
        m_result.m_constValues.push_back(nodep->num().toString());
        iterateChildren(nodep);
    }
    void visit(AstVarRef* nodep) override {
        record(nodep);
        m_result.m_refs.push_back(
            SharedScheduleLogicRef{static_cast<uintptr_t>(nodep->access()), nodep->varScopep()});
        iterateChildren(nodep);
    }
    void visit(AstNode* nodep) override {
        record(nodep);
        iterateChildren(nodep);
    }

public:
    SharedScheduleLogicNodeBuilder(AstNode* nodep, SharedScheduleLogicNode& result)
        : m_result{result} {
        iterate(nodep);
    }
    ~SharedScheduleLogicNodeBuilder() override = default;
};

SharedScheduleLogicNode makeSharedScheduleLogicNode(AstNode* logicp) {
    SharedScheduleLogicNode result;
    result.m_type = static_cast<uintptr_t>(logicp->type());
    SharedScheduleLogicNodeBuilder{logicp, result};
    return result;
}

SharedScheduleLogicSig makeSharedScheduleLogicSig(const LogicByScope& logic) {
    SharedScheduleLogicSig result;
    logic.foreachLogic(
        [&](AstNode* logicp) { result.push_back(makeSharedScheduleLogicNode(logicp)); });
    return result;
}

bool matchSharedScheduleLogic(const SharedScheduleLogicSig& source,
                              const SharedScheduleLogicSig& candidate,
                              std::unordered_map<AstVarScope*, AstVarScope*>& sourceToCandidate) {
    if (source.size() != candidate.size()) {
        UINFO(9, "Subgraph logic mismatch: node count " << source.size() << " vs "
                                                        << candidate.size());
        return false;
    }

    std::unordered_map<AstVarScope*, AstVarScope*> candidateToSource;
    for (const auto& pair : sourceToCandidate) {
        if (!candidateToSource.emplace(pair.second, pair.first).second) return false;
    }
    for (size_t nodeIndex = 0; nodeIndex < source.size(); ++nodeIndex) {
        const SharedScheduleLogicNode& sourceNode = source[nodeIndex];
        const SharedScheduleLogicNode& candidateNode = candidate[nodeIndex];
        if (sourceNode.m_type != candidateNode.m_type
            || sourceNode.m_nodeTypes != candidateNode.m_nodeTypes
            || sourceNode.m_constValues != candidateNode.m_constValues
            || sourceNode.m_refs.size() != candidateNode.m_refs.size()) {
            UINFO(9, "Subgraph logic mismatch: topology at node " << nodeIndex);
            return false;
        }
        for (size_t refIndex = 0; refIndex < candidateNode.m_refs.size(); ++refIndex) {
            const SharedScheduleLogicRef& sourceRef = sourceNode.m_refs[refIndex];
            const SharedScheduleLogicRef& candidateRef = candidateNode.m_refs[refIndex];
            AstVarScope* const candidateVscp = candidateRef.m_vscp;
            AstVar* const sourceVarp = sourceRef.m_vscp->varp();
            AstVar* const candidateVarp = candidateVscp->varp();
            if (sourceRef.m_access != candidateRef.m_access
                || !sourceRef.m_vscp->dtypep()->similarDType(candidateVscp->dtypep())
                || sourceVarp->varType() != candidateVarp->varType()
                || sourceVarp->direction() != candidateVarp->direction()
                || sourceVarp->lifetime() != candidateVarp->lifetime()) {
                UINFO(9, "Subgraph logic mismatch: reference at node " << nodeIndex << " index "
                                                                       << refIndex);
                return false;
            }
            const auto sourceIt = sourceToCandidate.find(sourceRef.m_vscp);
            if (sourceIt != sourceToCandidate.end()) {
                if (sourceIt->second != candidateVscp) {
                    UINFO(9, "Subgraph logic mismatch: source mapping at node "
                                 << nodeIndex << " index " << refIndex);
                    return false;
                }
            } else {
                const auto candidateIt = candidateToSource.find(candidateVscp);
                if (candidateIt != candidateToSource.end()
                    && candidateIt->second != sourceRef.m_vscp) {
                    UINFO(9, "Subgraph logic mismatch: candidate mapping at node "
                                 << nodeIndex << " index " << refIndex);
                    return false;
                }
                sourceToCandidate.emplace(sourceRef.m_vscp, candidateVscp);
                candidateToSource.emplace(candidateVscp, sourceRef.m_vscp);
            }
        }
    }
    return true;
}

struct SharedHelperArtifact final {
    AstNodeModule* m_modp = nullptr;
    VSubgraphPhase m_phase;
    AstCFunc* m_funcp = nullptr;
    AstCCall* m_firstCallp = nullptr;
    AstNode* m_templateStmtsp = nullptr;
    SharedScheduleLogicSig m_logicSig;
    SharedScheduleLogicSig m_domainSig;
    std::vector<SharedHelperArg> m_args;
    std::unique_ptr<V3SubgraphContract> m_contractp;
    SharedHelperAbiAnalysis m_abi;
    bool m_instanceContext = false;
    bool m_orderReusable = false;
    bool m_parameterized = false;
};

VAccess sharedHelperArgAccess(const SharedHelperArg& arg) {
    if (arg.m_read && arg.m_write) return VAccess::READWRITE;
    return arg.m_write ? VAccess::WRITE : VAccess::READ;
}

bool hasCompositeSharedHelperArgs(const std::vector<SharedHelperArg>& args) {
    return std::any_of(args.begin(), args.end(), [](const SharedHelperArg& arg) {
        AstNodeDType* const dtypep = arg.m_vscp->dtypep()->skipRefp();
        return !VN_IS(dtypep, BasicDType) || dtypep->isWide();
    });
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

AstCFunc* makeSharedScheduleWrapper(AstNetlist* netlistp, AstScope* scopep, AstSenTree* senTreep,
                                    AstCFunc* sharedFuncp,
                                    const std::vector<SharedHelperArg>& args, const string& tag,
                                    bool slow, bool refresh, bool instanceContext) {
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

bool canRemapSharedScheduleContract(
    const SharedHelperArtifact& artifact, AstScope* targetBoundaryScopep,
    const std::unordered_map<AstVarScope*, AstVarScope*>& sourceToTarget) {
    AstScope* const sourceBoundaryScopep = artifact.m_funcp->scopep();
    for (const auto& pair : sourceToTarget) {
        AstVarScope* const sourceVscp = pair.first;
        AstVarScope* const targetVscp = pair.second;
        const bool sourceUnderBoundary = isUnderScope(sourceVscp->scopep(), sourceBoundaryScopep);
        const bool targetUnderBoundary = isUnderScope(targetVscp->scopep(), targetBoundaryScopep);
        if (sourceUnderBoundary != targetUnderBoundary) return false;
        if (!sourceUnderBoundary) continue;
        const bool sourceBoundaryIo
            = sourceVscp->scopep() == sourceBoundaryScopep && sourceVscp->varp()->isIO();
        const bool targetBoundaryIo
            = targetVscp->scopep() == targetBoundaryScopep && targetVscp->varp()->isIO();
        if (sourceBoundaryIo != targetBoundaryIo) return false;
    }
    const auto allMapped = [&](const std::vector<V3SubgraphContract::Use>& uses) {
        return std::all_of(uses.begin(), uses.end(), [&](const V3SubgraphContract::Use& use) {
            return sourceToTarget.find(use.m_varScopep) != sourceToTarget.end();
        });
    };
    return allMapped(artifact.m_contractp->boundaryUses())
           && allMapped(artifact.m_contractp->internalUses());
}

void preserveSharedInstanceUses(
    const SharedHelperArtifact& artifact,
    const std::unordered_map<AstVarScope*, AstVarScope*>& sourceToTarget) {
    const auto preserveUses = [&](const std::vector<V3SubgraphContract::Use>& uses) {
        for (const V3SubgraphContract::Use& use : uses) {
            const auto it = sourceToTarget.find(use.m_varScopep);
            UASSERT_OBJ(it != sourceToTarget.end(), artifact.m_funcp,
                        "Shared subgraph contract variable is not remapped");
            it->second->optimizeLifePost(false);
            it->second->subgraphSharedUse(true);
        }
    };
    preserveUses(artifact.m_contractp->boundaryUses());
    preserveUses(artifact.m_contractp->internalUses());
}

void parameterizeSharedHelper(SharedHelperArtifact& artifact) {
    UASSERT_OBJ(!artifact.m_parameterized, artifact.m_funcp,
                "Shared subgraph helper parameterized twice");
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
    artifact.m_funcp->foreach([&](AstNodeVarRef* refp) {
        if (artifact.m_instanceContext) {
            UASSERT_OBJ(
                refp->varp()->isFuncLocal()
                    || isUnderScope(refp->varScopep()->scopep(), artifact.m_funcp->scopep()),
                refp, "Shared subgraph helper retained an external instance reference");
        } else {
            UASSERT_OBJ(refp->varp()->isFuncLocal(), refp,
                        "Shared subgraph helper retained an implicit instance reference");
        }
    });
    addSharedHelperCallArgs(artifact.m_firstCallp, artifact.m_args);
    artifact.m_firstCallp->useCallerSelf(artifact.m_instanceContext);
    if (artifact.m_instanceContext) {
        artifact.m_funcp->subgraphCallerSelf(true);
    } else {
        artifact.m_funcp->isStatic(true);
    }
    artifact.m_funcp->noLife(true);
    artifact.m_parameterized = true;
}

bool shareableFuncVar(const AstVar* varp) {
    const VDirection direction = varp->direction();
    const AstBasicDType* const basicp = VN_CAST(varp->dtypep()->skipRefp(), BasicDType);
    return basicp && basicp->isIntegralOrPacked() && !varp->lifetime().isStatic()
           && !direction.isInout() && !direction.isRef();
}

bool shareableFuncLocals(AstCFunc* funcp) {
    bool safe = true;
    for (AstVar* varp = funcp->varsp(); varp; varp = VN_AS(varp->nextp(), Var)) {
        if (!shareableFuncVar(varp)) safe = false;
    }
    funcp->foreach([&](AstNodeVarRef* refp) {
        AstVar* const varp = refp->varp();
        if (varp->isFuncLocal() && !shareableFuncVar(varp)) safe = false;
    });
    return safe;
}

bool selfContainedCalledFunc(AstCFunc* funcp) {
    if (funcp->dpiImportPrototype() || funcp->dpiImportWrapper() || funcp->dpiContext()
        || funcp->needProcess() || funcp->recursive() || funcp->isCoroutine()) {
        return false;
    }
    bool safe = true;
    for (AstVar* varp = funcp->argsp(); varp; varp = VN_AS(varp->nextp(), Var)) {
        if (!shareableFuncVar(varp)) safe = false;
    }
    safe &= shareableFuncLocals(funcp);
    funcp->foreach([&](AstNode* nodep) {
        if (AstNodeVarRef* const refp = VN_CAST(nodep, NodeVarRef)) {
            AstVarScope* const vscp = refp->varScopep();
            if (!vscp || !vscp->varp()->isFuncLocal() || !shareableFuncVar(vscp->varp())) {
                safe = false;
            }
        } else if (VN_IS(nodep, ThisRef) || VN_IS(nodep, CStmt)) {
            safe = false;
        } else if (!VN_IS(nodep, CCall) && nodep->isOutputter()) {
            safe = false;
        }
    });
    return safe;
}

class SharedHelperCallChecker final {
    std::unordered_set<AstCFunc*> m_visited;

    bool checkFunc(AstCFunc* funcp) {
        if (!m_visited.emplace(funcp).second) return true;
        bool safe = selfContainedCalledFunc(funcp);
        funcp->foreach([&](AstCCall* callp) { safe &= checkFunc(callp->funcp()); });
        return safe;
    }

public:
    bool checkCalls(AstCFunc* funcp) {
        bool safe = true;
        funcp->foreach([&](AstCCall* callp) { safe &= checkFunc(callp->funcp()); });
        return safe;
    }
};

class SharedHelperBodyComparator final {
    std::unordered_map<AstVarScope*, AstVarScope*> m_candidateToSourceVscps;
    std::unordered_map<AstVarScope*, AstVarScope*> m_sourceToCandidateVscps;
    std::unordered_map<const AstCFunc*, const AstCFunc*> m_candidateToSourceFuncps;
    std::unordered_map<const AstCFunc*, const AstCFunc*> m_sourceToCandidateFuncps;

    struct RestoreRef final {
        AstNodeVarRef* m_refp;
        AstVarScope* m_vscp;
        AstVar* m_varp;
        VSelfPointerText m_selfPointer;
    };
    struct RestoreCall final {
        AstCCall* m_callp;
        AstCFunc* m_funcp;
    };

    bool mapVarScope(AstVarScope* sourceVscp, AstVarScope* candidateVscp) {
        const auto candidateIt = m_candidateToSourceVscps.find(candidateVscp);
        if (candidateIt != m_candidateToSourceVscps.end()) {
            return candidateIt->second == sourceVscp;
        }
        const auto sourceIt = m_sourceToCandidateVscps.find(sourceVscp);
        if (sourceIt != m_sourceToCandidateVscps.end()) {
            return sourceIt->second == candidateVscp;
        }
        m_candidateToSourceVscps.emplace(candidateVscp, sourceVscp);
        m_sourceToCandidateVscps.emplace(sourceVscp, candidateVscp);
        return true;
    }

    static AstVarScope* findVarScope(AstCFunc* funcp, AstVar* varp) {
        for (AstVarScope* vscp = funcp->scopep()->varsp(); vscp;
             vscp = VN_AS(vscp->nextp(), VarScope)) {
            if (vscp->varp() == varp) return vscp;
        }
        return nullptr;
    }

    bool sameFuncVars(AstCFunc* sourceFuncp, AstVar* sourceVarp, AstCFunc* candidateFuncp,
                      AstVar* candidateVarp) {
        while (sourceVarp && candidateVarp) {
            if (!shareableFuncVar(sourceVarp) || !shareableFuncVar(candidateVarp)
                || sourceVarp->direction() != candidateVarp->direction()
                || sourceVarp->varType() != candidateVarp->varType()
                || !sourceVarp->dtypep()->similarDType(candidateVarp->dtypep())) {
                return false;
            }
            AstVarScope* const sourceVscp = findVarScope(sourceFuncp, sourceVarp);
            AstVarScope* const candidateVscp = findVarScope(candidateFuncp, candidateVarp);
            if (!sourceVscp || !candidateVscp || !mapVarScope(sourceVscp, candidateVscp)) {
                return false;
            }
            sourceVarp = VN_CAST(sourceVarp->nextp(), Var);
            candidateVarp = VN_CAST(candidateVarp->nextp(), Var);
        }
        return !sourceVarp && !candidateVarp;
    }

    bool sameCalledFunc(AstCFunc* sourceFuncp, AstCFunc* candidateFuncp) {
        const auto candidateIt = m_candidateToSourceFuncps.find(candidateFuncp);
        if (candidateIt != m_candidateToSourceFuncps.end()) {
            return candidateIt->second == sourceFuncp;
        }
        const auto sourceIt = m_sourceToCandidateFuncps.find(sourceFuncp);
        if (sourceIt != m_sourceToCandidateFuncps.end()) {
            return sourceIt->second == candidateFuncp;
        }
        if (!selfContainedCalledFunc(sourceFuncp) || !selfContainedCalledFunc(candidateFuncp)
            || sourceFuncp->scopep()->modp() != candidateFuncp->scopep()->modp()
            || sourceFuncp->rtnTypeVoid() != candidateFuncp->rtnTypeVoid()
            || sourceFuncp->argTypes() != candidateFuncp->argTypes()
            || sourceFuncp->isStatic() != candidateFuncp->isStatic()
            || sourceFuncp->isLoose() != candidateFuncp->isLoose()) {
            return false;
        }
        if (sourceFuncp == candidateFuncp) return true;
        m_candidateToSourceFuncps.emplace(candidateFuncp, sourceFuncp);
        m_sourceToCandidateFuncps.emplace(sourceFuncp, candidateFuncp);
        if (!sameFuncVars(sourceFuncp, sourceFuncp->argsp(), candidateFuncp,
                          candidateFuncp->argsp())
            || !sameFuncVars(sourceFuncp, sourceFuncp->varsp(), candidateFuncp,
                             candidateFuncp->varsp())) {
            return false;
        }
        return sameTree(sourceFuncp->stmtsp(), candidateFuncp->stmtsp(), true);
    }

    bool sameTree(AstNode* sourcep, AstNode* candidatep, bool calledFunc) {
        if (!sourcep || !candidatep) return sourcep == candidatep;
        std::vector<AstNodeVarRef*> sourceRefs;
        std::vector<AstNodeVarRef*> candidateRefs;
        sourcep->foreach([&](AstNodeVarRef* refp) { sourceRefs.push_back(refp); });
        candidatep->foreach([&](AstNodeVarRef* refp) { candidateRefs.push_back(refp); });
        if (sourceRefs.size() != candidateRefs.size()) return false;
        for (size_t index = 0; index < sourceRefs.size(); ++index) {
            AstNodeVarRef* const sourceRefp = sourceRefs[index];
            AstNodeVarRef* const candidateRefp = candidateRefs[index];
            AstVarScope* const sourceVscp = sourceRefp->varScopep();
            AstVarScope* const candidateVscp = candidateRefp->varScopep();
            if (!sourceVscp || !candidateVscp || sourceRefp->access() != candidateRefp->access()
                || !sourceVscp->dtypep()->similarDType(candidateVscp->dtypep())) {
                return false;
            }
            const bool sourceLocal = sourceVscp->varp()->isFuncLocal();
            const bool candidateLocal = candidateVscp->varp()->isFuncLocal();
            if (sourceLocal != candidateLocal || (calledFunc && !sourceLocal)
                || (sourceLocal
                    && (sourceVscp->varp()->lifetime().isStatic()
                        || candidateVscp->varp()->lifetime().isStatic()))
                || !mapVarScope(sourceVscp, candidateVscp)) {
                return false;
            }
        }

        std::vector<AstCCall*> sourceCalls;
        std::vector<AstCCall*> candidateCalls;
        sourcep->foreach([&](AstCCall* callp) { sourceCalls.push_back(callp); });
        candidatep->foreach([&](AstCCall* callp) { candidateCalls.push_back(callp); });
        if (sourceCalls.size() != candidateCalls.size()) return false;
        for (size_t index = 0; index < sourceCalls.size(); ++index) {
            if (!sameCalledFunc(sourceCalls[index]->funcp(), candidateCalls[index]->funcp())) {
                return false;
            }
        }

        std::vector<RestoreRef> restoreRefs;
        for (AstNodeVarRef* const refp : candidateRefs) {
            AstVarScope* const sourceVscp = m_candidateToSourceVscps.at(refp->varScopep());
            restoreRefs.push_back(
                RestoreRef{refp, refp->varScopep(), refp->varp(), refp->selfPointer()});
            refp->varp(sourceVscp->varp());
            refp->varScopep(sourceVscp);
            refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
        }
        std::vector<RestoreCall> restoreCalls;
        for (size_t index = 0; index < candidateCalls.size(); ++index) {
            AstCCall* const callp = candidateCalls[index];
            restoreCalls.push_back(RestoreCall{callp, callp->funcp()});
            callp->funcp(sourceCalls[index]->funcp());
        }
        const bool same = candidatep->sameTree(sourcep);
        for (const RestoreCall& restore : restoreCalls) restore.m_callp->funcp(restore.m_funcp);
        for (const RestoreRef& restore : restoreRefs) {
            restore.m_refp->varp(restore.m_varp);
            restore.m_refp->varScopep(restore.m_vscp);
            restore.m_refp->selfPointer(restore.m_selfPointer);
        }
        return same;
    }

public:
    SharedHelperBodyComparator(const SharedHelperArtifact& artifact,
                               const std::vector<SharedHelperArg>& candidateArgs) {
        UASSERT_OBJ(artifact.m_args.size() == candidateArgs.size(), artifact.m_funcp,
                    "Mismatched shared helper argument counts");
        for (size_t index = 0; index < artifact.m_args.size(); ++index) {
            mapVarScope(artifact.m_args[index].m_vscp, candidateArgs[index].m_vscp);
        }
    }

    bool same(const SharedHelperArtifact& artifact, AstCFunc* candidateFuncp) {
        if (!sameFuncVars(artifact.m_funcp, artifact.m_funcp->varsp(), candidateFuncp,
                          candidateFuncp->varsp())) {
            return false;
        }
        return sameTree(artifact.m_templateStmtsp, candidateFuncp->stmtsp(), false);
    }
};

bool sameSharedHelperBody(const SharedHelperArtifact& artifact, AstCFunc* candidateFuncp,
                          const std::vector<SharedHelperArg>& candidateArgs) {
    if (artifact.m_args.size() != candidateArgs.size()) return false;
    for (size_t index = 0; index < artifact.m_args.size(); ++index) {
        const SharedHelperArg& source = artifact.m_args[index];
        const SharedHelperArg& candidate = candidateArgs[index];
        if (source.m_read != candidate.m_read || source.m_write != candidate.m_write
            || source.m_state != candidate.m_state
            || !source.m_vscp->dtypep()->similarDType(candidate.m_vscp->dtypep())) {
            return false;
        }
    }
    return SharedHelperBodyComparator{artifact, candidateArgs}.same(artifact, candidateFuncp);
}

bool sameSharedScheduleInput(const SharedHelperArtifact& artifact,
                             const SharedScheduleLogicSig& candidateLogic) {
    std::unordered_map<AstVarScope*, AstVarScope*> sourceToCandidate;
    return matchSharedScheduleLogic(artifact.m_logicSig, candidateLogic, sourceToCandidate);
}

void preserveSubgraphContractUses(const V3SubgraphContract& contract) {
    const auto preserveUses = [](const std::vector<V3SubgraphContract::Use>& uses) {
        for (const V3SubgraphContract::Use& use : uses) {
            use.m_varScopep->optimizeLifePost(false);
            use.m_varScopep->subgraphSharedUse(true);
        }
    };
    preserveUses(contract.boundaryUses());
    preserveUses(contract.internalUses());
}

AstNode* cloneSharedHelperTemplate(AstCFunc* funcp) {
    AstNode* const templatep = funcp->stmtsp()->cloneTree(true);
    templatep->foreach([&](AstNodeVarRef* refp) {
        refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
    });
    return templatep;
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
    uint64_t contracts = 0;
    uint64_t coarseNodes = 0;
    uint64_t logicalUses = 0;
    uint64_t materializedBoundaryUses = 0;
    uint64_t materializedCanonicalPatternEntries = 0;
    uint64_t materializedCanonicalPatternReuses = 0;
    uint64_t materializedCanonicalPatterns = 0;
    uint64_t materializedExternalUses = 0;
    uint64_t materializedInternalUses = 0;
    uint64_t prunedInternalUses = 0;
    uint64_t refreshHelpers = 0;
    uint64_t sharedAbiAnalyses = 0;
    uint64_t sharedAbiCalls = 0;
    uint64_t sharedAbiConstants = 0;
    uint64_t sharedAbiDpiCalls = 0;
    uint64_t sharedAbiEligibleHelpers = 0;
    uint64_t sharedAbiExternalVars = 0;
    uint64_t sharedAbiGeneratedTemps = 0;
    uint64_t sharedAbiHiddenUses = 0;
    uint64_t sharedAbiInputVars = 0;
    uint64_t sharedAbiModulePhaseCandidates = 0;
    uint64_t sharedAbiOutputVars = 0;
    uint64_t sharedAbiStateVars = 0;
    uint64_t sharedHelperArguments = 0;
    uint64_t sharedHelperArtifactCount = 0;
    uint64_t sharedHelperBodyChecks = 0;
    uint64_t sharedHelperBodyMismatches = 0;
    uint64_t sharedHelperCallArtifacts = 0;
    uint64_t sharedHelperCallReuses = 0;
    uint64_t sharedHelperCompositeArtifacts = 0;
    uint64_t sharedHelperCompositeReuses = 0;
    uint64_t sharedHelperContextArtifacts = 0;
    uint64_t sharedHelperContextReuses = 0;
    uint64_t sharedHelperParameterizations = 0;
    uint64_t sharedHelperReuses = 0;
    uint64_t sharedHelperSkippedCalls = 0;
    uint64_t sharedHelperSkippedCallAbiIneligible = 0;
    uint64_t sharedHelperSkippedCallContextUnsafe = 0;
    uint64_t sharedHelperSkippedCallTargetsUnsafe = 0;
    uint64_t sharedHelperSkippedComposite = 0;
    uint64_t sharedHelperSkippedGeneratedTemps = 0;
    uint64_t sharedHelperSkippedDpiCalls = 0;
    uint64_t sharedHelperSkippedOversized = 0;
    uint64_t sharedHelperSkippedRefreshContext = 0;
    uint64_t sharedHelperSkippedTriggered = 0;
    uint64_t sharedHelperArgumentCandidates = 0;
    uint64_t sharedHelperExternalArgumentCandidates = 0;
    uint64_t sharedHelperInternalArgumentCandidates = 0;
    uint64_t sharedHelperMaximumArguments = 0;
    uint64_t sharedHelperOversizedArguments = 0;
    uint64_t sharedOrderCacheArgumentMismatches = 0;
    uint64_t sharedOrderCacheContractMismatches = 0;
    uint64_t sharedOrderCacheDomainMatches = 0;
    uint64_t sharedOrderCacheDomainMismatches = 0;
    uint64_t sharedOrderCacheLogicMatches = 0;
    uint64_t sharedOrderCacheLogicMismatches = 0;
    uint64_t sharedOrderCacheLookups = 0;
    uint64_t sharedOrderCacheNotReusable = 0;
    uint64_t sharedOrderCacheOrderCallsExecuted = 0;
    uint64_t sharedOrderCacheOrderCallsAvoided = 0;
    struct EligibleModulePhase final {
        AstNodeModule* m_modp;
        VSubgraphPhase m_phase;
    };
    std::vector<EligibleModulePhase> eligibleModulePhases;
    std::vector<PendingSubgraphMaterialization> pendingMaterializations;
    SubgraphUsePatternInterner usePatternInterner;
    std::unordered_set<AstVarScope*> postWrittenInternalVscps;
    std::unordered_set<AstVarScope*> refreshReadInternalVscps;
    std::vector<SharedHelperArtifact> sharedHelperArtifacts;
    const auto noteSharedAbi = [&](const SharedHelperAbiAnalysis& abi,
                                   VSubgraphPhase subgraphPhase, AstNodeModule* modp) {
        ++sharedAbiAnalyses;
        sharedAbiCalls += abi.m_calls;
        sharedAbiConstants += abi.m_constants;
        sharedAbiDpiCalls += abi.m_dpiCalls;
        sharedAbiExternalVars += abi.m_externalVars;
        sharedAbiGeneratedTemps += abi.m_generatedTemps;
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
    unsigned groupIndex = 0;
    for (SubgraphGroup& group : groups) {
        orderedLogic
            += group.m_preLogic.size() + group.m_postLogic.size() + group.m_refreshLogic.size();
        UASSERT_OBJ(group.m_senTreep, group.m_boundaryScopep, "Subgraph NBA logic has no domain");
        const VlOs::DeltaWallTime domainSignatureTimer{measure};
        const SharedScheduleLogicSig domainSig{
            makeSharedScheduleLogicNode(const_cast<AstSenTree*>(group.m_domainKeyp))};
        if (measure) logicSignatureWallTime += domainSignatureTimer.deltaTime();

        const auto orderPhase = [&](LogicByScope& logic, const string& phase,
                                    VSubgraphPhase subgraphPhase) {
            if (logic.empty()) return;
            const VlOs::DeltaWallTime logicSignatureTimer{measure};
            const SharedScheduleLogicSig logicSig = makeSharedScheduleLogicSig(logic);
            if (measure) logicSignatureWallTime += logicSignatureTimer.deltaTime();
            SharedHelperArtifact* cachedArtifactp = nullptr;
            std::unordered_map<AstVarScope*, AstVarScope*> sourceToCandidate;
            std::vector<SharedHelperArg> cachedArgs;
            const VlOs::DeltaWallTime cacheLookupTimer{measure};
            for (SharedHelperArtifact& artifact : sharedHelperArtifacts) {
                if (artifact.m_modp != group.m_boundaryScopep->modp()
                    || artifact.m_phase != subgraphPhase) {
                    continue;
                }
                // Helpers with a fully explicit ABI are reusable immediately. A helper that
                // retains caller context first requires an independently ordered match.
                if (!artifact.m_orderReusable) {
                    ++sharedOrderCacheNotReusable;
                    continue;
                }
                ++sharedOrderCacheLookups;
                sourceToCandidate.clear();
                if (!matchSharedScheduleLogic(artifact.m_logicSig, logicSig, sourceToCandidate)) {
                    ++sharedOrderCacheLogicMismatches;
                    continue;
                }
                ++sharedOrderCacheLogicMatches;
                if (!matchSharedScheduleLogic(artifact.m_domainSig, domainSig,
                                              sourceToCandidate)) {
                    ++sharedOrderCacheDomainMismatches;
                    continue;
                }
                ++sharedOrderCacheDomainMatches;
                if (!canRemapSharedScheduleContract(artifact, group.m_boundaryScopep,
                                                    sourceToCandidate)) {
                    ++sharedOrderCacheContractMismatches;
                    continue;
                }
                cachedArgs.clear();
                for (const SharedHelperArg& arg : artifact.m_args) {
                    const auto it = sourceToCandidate.find(arg.m_vscp);
                    // Every helper argument must be mapped to the candidate instance. Falling
                    // back to the canonical variable can make one instance read another
                    // instance's state.
                    if (it == sourceToCandidate.end()) break;
                    cachedArgs.push_back(
                        SharedHelperArg{it->second, arg.m_read, arg.m_write, arg.m_state});
                }
                if (cachedArgs.size() != artifact.m_args.size()) {
                    ++sharedOrderCacheArgumentMismatches;
                    continue;
                }
                cachedArtifactp = &artifact;
                break;
            }
            if (measure) cacheLookupWallTime += cacheLookupTimer.deltaTime();
            const string tag = "nba_subgraph_" + phase + "_" + cvtToStr(groupIndex);
            const bool refresh = subgraphPhase == VSubgraphPhase{VSubgraphPhase::REFRESH};
            AstCFunc* funcp = nullptr;
            std::unique_ptr<V3SubgraphContract> contractp;
            SharedHelperAbiAnalysis abi;
            if (cachedArtifactp) {
                const VlOs::DeltaWallTime cacheReuseTimer{measure};
                if (cachedArtifactp->m_instanceContext) {
                    preserveSharedInstanceUses(*cachedArtifactp, sourceToCandidate);
                }
                if (!cachedArtifactp->m_parameterized) {
                    parameterizeSharedHelper(*cachedArtifactp);
                    sharedHelperArguments += cachedArtifactp->m_args.size();
                    ++sharedHelperParameterizations;
                }
                funcp = makeSharedScheduleWrapper(
                    netlistp, group.m_boundaryScopep, group.m_senTreep, cachedArtifactp->m_funcp,
                    cachedArgs, tag, slow, refresh, cachedArtifactp->m_instanceContext);
                discardSharedScheduleLogic(logic);
                contractp = std::make_unique<V3SubgraphContract>(V3SubgraphContract::remap(
                    *cachedArtifactp->m_contractp, group.m_boundaryScopep, group.m_senTreep,
                    sourceToCandidate));
                abi = SharedHelperAbiAnalyzer{funcp, group.m_boundaryScopep, *contractp}.result();
                ++sharedHelperBodyChecks;
                ++sharedHelperReuses;
                if (cachedArtifactp->m_abi.m_calls) ++sharedHelperCallReuses;
                if (hasCompositeSharedHelperArgs(cachedArtifactp->m_args)) {
                    ++sharedHelperCompositeReuses;
                }
                if (cachedArtifactp->m_instanceContext) ++sharedHelperContextReuses;
                ++sharedOrderCacheOrderCallsAvoided;
                if (measure) cacheReuseWallTime += cacheReuseTimer.deltaTime();
            } else {
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
                        funcp, group.m_boundaryScopep, group.m_senTreep,
                        subgraphPhase == VSubgraphPhase{VSubgraphPhase::POST}, refresh));
                    abi = SharedHelperAbiAnalyzer{funcp, group.m_boundaryScopep, *contractp}
                              .result();
                    if (measure) contractAbiWallTime += contractAbiTimer.deltaTime();
                }
            }
            if (!funcp) return;
            const VlOs::DeltaWallTime contractAccountingTimer{measure};
            const V3SubgraphContract& contract = *contractp;
            contractBoundaryUses += contract.boundaryUses().size();
            contractExternalUses += contract.externalUses().size();
            contractInternalUses += contract.internalUses().size();
            for (const V3SubgraphContract::Use& use : contract.internalUses()) {
                if (V3SubgraphContract::isDelayedState(use.m_varScopep)) {
                    parentAccessedVscps.insert(use.m_varScopep);
                }
                if (subgraphPhase == VSubgraphPhase{VSubgraphPhase::POST} && use.m_write) {
                    postWrittenInternalVscps.insert(use.m_varScopep);
                }
                if (subgraphPhase == VSubgraphPhase{VSubgraphPhase::REFRESH} && use.m_read) {
                    refreshReadInternalVscps.insert(use.m_varScopep);
                }
            }
            ++contracts;
            noteSharedAbi(abi, subgraphPhase, group.m_boundaryScopep->modp());
            if (measure) contractAbiWallTime += contractAccountingTimer.deltaTime();

            const VlOs::DeltaWallTime helperSharingTimer{measure};
            AstActive* const wrapperp
                = new AstActive{group.m_filelinep, "subgraph", group.m_senTreep};
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
                        = SharedHelperAbiAnalyzer{sharedFuncp, group.m_boundaryScopep, contract}
                              .result();
                }
            }
            const bool generatedTemps
                = sharedAbi.m_generatedTemps || sharedFuncp->varsp() || sharedFuncp->argsp();
            const bool shareableCalls
                = !sharedAbi.m_calls || SharedHelperCallChecker{}.checkCalls(sharedFuncp);
            static constexpr size_t kMaxSharedHelperArgs = 8;
            const bool explicitBaseCandidate
                = sharedAbi.m_eligible && sharedAbi.m_contextSafe && !sharedAbi.m_hasTriggeredState
                  && !sharedFuncp->argsp() && shareableCalls && shareableFuncLocals(sharedFuncp);
            const std::vector<SharedHelperArg> explicitArgs
                = explicitBaseCandidate
                      ? collectSharedHelperArgs(sharedFuncp, group.m_boundaryScopep)
                      : std::vector<SharedHelperArg>{};
            const bool compositeArgs = hasCompositeSharedHelperArgs(explicitArgs);
            if (explicitBaseCandidate) {
                sharedHelperArgumentCandidates += explicitArgs.size();
                sharedHelperMaximumArguments
                    = std::max<uint64_t>(sharedHelperMaximumArguments, explicitArgs.size());
                for (const SharedHelperArg& arg : explicitArgs) {
                    if (arg.m_state) {
                        ++sharedHelperInternalArgumentCandidates;
                    } else {
                        ++sharedHelperExternalArgumentCandidates;
                    }
                }
            }
            const bool oversized = explicitArgs.size() > kMaxSharedHelperArgs;
            const bool instanceContext = explicitBaseCandidate && oversized && !refresh;
            const bool helperInstanceContext = instanceContext || sharedAbi.m_calls != 0;
            const std::vector<SharedHelperArg> contextArgs
                = instanceContext
                      ? collectSharedHelperArgs(sharedFuncp, group.m_boundaryScopep, true)
                      : std::vector<SharedHelperArg>{};
            const std::vector<SharedHelperArg>& helperArgs
                = instanceContext ? contextArgs : explicitArgs;
            const bool shareCandidate
                = explicitBaseCandidate && helperArgs.size() <= kMaxSharedHelperArgs;
            if (cachedArtifactp) {
                // The cached artifact was already validated and accounted for above.
            } else if (shareCandidate) {
                // Only share helpers after independently running V3Order and comparing their
                // ordered bodies. A matching pre-order signature is insufficient to prove that
                // caller-instance context and the resulting dependency contract are equivalent.
                SharedHelperArtifact* matchingArtifactp = nullptr;
                for (SharedHelperArtifact& artifact : sharedHelperArtifacts) {
                    if (artifact.m_modp != group.m_boundaryScopep->modp()
                        || artifact.m_phase != subgraphPhase
                        || artifact.m_instanceContext != helperInstanceContext) {
                        continue;
                    }
                    ++sharedHelperBodyChecks;
                    if (sameSharedScheduleInput(artifact, logicSig)
                        && sameSharedHelperBody(artifact, sharedFuncp, helperArgs)) {
                        matchingArtifactp = &artifact;
                        break;
                    }
                    ++sharedHelperBodyMismatches;
                }
                if (matchingArtifactp) {
                    matchingArtifactp->m_orderReusable = true;
                    if (instanceContext) {
                        preserveSubgraphContractUses(*matchingArtifactp->m_contractp);
                        preserveSubgraphContractUses(contract);
                    }
                    if (!matchingArtifactp->m_parameterized) {
                        parameterizeSharedHelper(*matchingArtifactp);
                        sharedHelperArguments += matchingArtifactp->m_args.size();
                        ++sharedHelperParameterizations;
                    }
                    sharedCallp->funcp(matchingArtifactp->m_funcp);
                    sharedCallp->useCallerSelf(matchingArtifactp->m_instanceContext);
                    addSharedHelperCallArgs(sharedCallp, helperArgs);
                    ++sharedHelperReuses;
                    if (sharedAbi.m_calls) ++sharedHelperCallReuses;
                    if (compositeArgs) ++sharedHelperCompositeReuses;
                    if (instanceContext) ++sharedHelperContextReuses;
                } else {
                    sharedHelperArtifacts.push_back(SharedHelperArtifact{
                        group.m_boundaryScopep->modp(), subgraphPhase, sharedFuncp, sharedCallp,
                        cloneSharedHelperTemplate(sharedFuncp), logicSig, domainSig, helperArgs,
                        std::make_unique<V3SubgraphContract>(contract), sharedAbi,
                        helperInstanceContext, !instanceContext && sharedAbi.m_calls == 0, false});
                    ++sharedHelperArtifactCount;
                    if (sharedAbi.m_calls) ++sharedHelperCallArtifacts;
                    if (compositeArgs) ++sharedHelperCompositeArtifacts;
                    if (instanceContext) ++sharedHelperContextArtifacts;
                }
            } else if (explicitBaseCandidate) {
                sharedHelperOversizedArguments += explicitArgs.size();
                ++sharedHelperSkippedOversized;
                if (refresh && oversized) ++sharedHelperSkippedRefreshContext;
            } else if (sharedAbi.m_hasTriggeredState) {
                ++sharedHelperSkippedTriggered;
            } else if (sharedAbi.m_calls) {
                ++sharedHelperSkippedCalls;
                if (abi.m_dpiCalls || sharedAbi.m_dpiCalls) ++sharedHelperSkippedDpiCalls;
                if (!sharedAbi.m_eligible) ++sharedHelperSkippedCallAbiIneligible;
                if (!sharedAbi.m_contextSafe) ++sharedHelperSkippedCallContextUnsafe;
                if (!shareableCalls) ++sharedHelperSkippedCallTargetsUnsafe;
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
            pendingMaterializations.push_back(
                PendingSubgraphMaterialization{instancep, &group, std::move(contractp)});
            if (measure) materializeWallTime += materializeTimer.deltaTime();
        };

        orderPhase(group.m_preLogic, "pre", VSubgraphPhase::PRE);
        orderPhase(group.m_postLogic, "post", VSubgraphPhase::POST);
        if (!group.m_refreshLogic.empty()) ++refreshHelpers;
        orderPhase(group.m_refreshLogic, "refresh", VSubgraphPhase::REFRESH);
        ++groupIndex;
    }

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
        const V3SubgraphContract& contract = *pending.m_contractp;
        const auto keepInternalUse = [&](const V3SubgraphContract::Use& use) {
            const bool delayedState = V3SubgraphContract::isDelayedState(use.m_varScopep);
            const bool parentAccessed = parentAccessedVscps.count(use.m_varScopep);
            const bool publishRead = instancep->phase() == VSubgraphPhase{VSubgraphPhase::REFRESH};
            return delayedState || (parentAccessed && (publishRead || use.m_write));
        };
        const std::vector<V3SubgraphContract::Use>& internalUses = contract.internalUses();
        const size_t keptInternalUses
            = std::count_if(internalUses.begin(), internalUses.end(), keepInternalUse);
        const size_t materializedUseCount
            = contract.boundaryUses().size() + contract.externalUses().size() + keptInternalUses;
        std::vector<AstVarScope*> materializedVscps;
        materializedVscps.reserve(materializedUseCount);
        V3SubgraphUsePattern materializedPattern;
        materializedPattern.reserve(materializedUseCount);
        const auto addMaterializedUse
            = [&](AstVarScope* vscp, VSubgraphUseKind kind, bool read, bool write, bool cuttable) {
                  materializedVscps.push_back(vscp);
                  materializedPattern.emplace_back(kind, read, write, cuttable);
              };
        for (const V3SubgraphContract::Use& use : contract.boundaryUses()) {
            addMaterializedUse(use.m_varScopep, VSubgraphUseKind::BOUNDARY, use.m_read,
                               use.m_write, use.m_cuttable);
            ++materializedBoundaryUses;
        }
        for (const V3SubgraphContract::Use& use : contract.externalUses()) {
            const bool snapshotStorage = std::any_of(
                group.m_snapshots.begin(), group.m_snapshots.end(),
                [&](const auto& snapshot) { return snapshot.m_storageVscp == use.m_varScopep; });
            addMaterializedUse(use.m_varScopep,
                               snapshotStorage
                                   ? VSubgraphUseKind{VSubgraphUseKind::SNAPSHOT_STORAGE}
                                   : VSubgraphUseKind{VSubgraphUseKind::EXTERNAL},
                               use.m_read, use.m_write, use.m_cuttable);
            ++materializedExternalUses;
        }
        for (const V3SubgraphContract::Use& use : internalUses) {
            if (!keepInternalUse(use)) {
                ++prunedInternalUses;
                continue;
            }
            addMaterializedUse(use.m_varScopep, VSubgraphUseKind::INTERNAL, use.m_read,
                               use.m_write, use.m_cuttable);
            ++materializedInternalUses;
        }
        const auto interned = usePatternInterner.intern(std::move(materializedPattern));
        if (interned.second) {
            ++materializedCanonicalPatterns;
            materializedCanonicalPatternEntries += interned.first->size();
        } else {
            ++materializedCanonicalPatternReuses;
        }
        instancep->setMaterializedUses(std::move(materializedVscps), interned.first);
    }
    if (measure) materializeWallTime += deferredMaterializeTimer.deltaTime();

    for (SharedHelperArtifact& artifact : sharedHelperArtifacts) {
        if (artifact.m_templateStmtsp) artifact.m_templateStmtsp->deleteTree();
        artifact.m_templateStmtsp = nullptr;
    }

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
    V3Stats::addStat("Scheduling, Subgraph NBA contract internal uses", contractInternalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA contracts", contracts);
    V3Stats::addStat("Scheduling, Subgraph NBA coarse nodes", coarseNodes);
    V3Stats::addStat("Scheduling, Subgraph NBA logical uses", logicalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized boundary uses",
                     materializedBoundaryUses);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized external uses",
                     materializedExternalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized internal uses",
                     materializedInternalUses);
    const uint64_t materializedMetadataBytes
        = (materializedBoundaryUses + materializedExternalUses + materializedInternalUses
           + 2 * snapshotSources)
              * sizeof(AstVarScope*)
          + (materializedCanonicalPatternEntries + 2 * snapshotSources)
                * sizeof(V3SubgraphUsePatternEntry);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized metadata bytes",
                     materializedMetadataBytes);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized canonical pattern entries",
                     materializedCanonicalPatternEntries);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized canonical pattern reuses",
                     materializedCanonicalPatternReuses);
    V3Stats::addStat("Scheduling, Subgraph NBA materialized canonical patterns",
                     materializedCanonicalPatterns);
    V3Stats::addStat("Scheduling, Subgraph NBA pruned internal uses", prunedInternalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA refresh helpers", refreshHelpers);
    V3Stats::addStat("Scheduling, Subgraph NBA snapshot instances", snapshotInstances);
    V3Stats::addStat("Scheduling, Subgraph NBA snapshot sources", snapshotSources);
    V3Stats::addStat("Scheduling, Subgraph shared ABI analyses", sharedAbiAnalyses);
    V3Stats::addStat("Scheduling, Subgraph shared ABI calls", sharedAbiCalls);
    V3Stats::addStat("Scheduling, Subgraph shared ABI constants", sharedAbiConstants);
    V3Stats::addStat("Scheduling, Subgraph shared ABI DPI calls", sharedAbiDpiCalls);
    V3Stats::addStat("Scheduling, Subgraph shared ABI eligible helpers", sharedAbiEligibleHelpers);
    V3Stats::addStat("Scheduling, Subgraph shared ABI external vars", sharedAbiExternalVars);
    V3Stats::addStat("Scheduling, Subgraph shared ABI generated temps", sharedAbiGeneratedTemps);
    V3Stats::addStat("Scheduling, Subgraph shared ABI hidden uses", sharedAbiHiddenUses);
    V3Stats::addStat("Scheduling, Subgraph shared ABI input vars", sharedAbiInputVars);
    V3Stats::addStat("Scheduling, Subgraph shared ABI module-phase candidates",
                     sharedAbiModulePhaseCandidates);
    V3Stats::addStat("Scheduling, Subgraph shared ABI output vars", sharedAbiOutputVars);
    V3Stats::addStat("Scheduling, Subgraph shared ABI state vars", sharedAbiStateVars);
    V3Stats::addStat("Scheduling, Subgraph shared helper arguments", sharedHelperArguments);
    V3Stats::addStat("Scheduling, Subgraph shared helper argument candidates",
                     sharedHelperArgumentCandidates);
    V3Stats::addStat("Scheduling, Subgraph shared helper external argument candidates",
                     sharedHelperExternalArgumentCandidates);
    V3Stats::addStat("Scheduling, Subgraph shared helper internal argument candidates",
                     sharedHelperInternalArgumentCandidates);
    V3Stats::addStat("Scheduling, Subgraph shared helper maximum arguments",
                     sharedHelperMaximumArguments);
    V3Stats::addStat("Scheduling, Subgraph shared helper oversized arguments",
                     sharedHelperOversizedArguments);
    V3Stats::addStat("Scheduling, Subgraph shared helper artifacts", sharedHelperArtifactCount);
    V3Stats::addStat("Scheduling, Subgraph shared helper body checks", sharedHelperBodyChecks);
    V3Stats::addStat("Scheduling, Subgraph shared helper body mismatches",
                     sharedHelperBodyMismatches);
    V3Stats::addStat("Scheduling, Subgraph shared helper call artifacts",
                     sharedHelperCallArtifacts);
    V3Stats::addStat("Scheduling, Subgraph shared helper call reuses", sharedHelperCallReuses);
    V3Stats::addStat("Scheduling, Subgraph shared helper composite artifacts",
                     sharedHelperCompositeArtifacts);
    V3Stats::addStat("Scheduling, Subgraph shared helper composite reuses",
                     sharedHelperCompositeReuses);
    V3Stats::addStat("Scheduling, Subgraph shared helper context artifacts",
                     sharedHelperContextArtifacts);
    V3Stats::addStat("Scheduling, Subgraph shared helper context reuses",
                     sharedHelperContextReuses);
    V3Stats::addStat("Scheduling, Subgraph shared helper parameterizations",
                     sharedHelperParameterizations);
    V3Stats::addStat("Scheduling, Subgraph shared helper reuses", sharedHelperReuses);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped calls", sharedHelperSkippedCalls);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped call ABI ineligible",
                     sharedHelperSkippedCallAbiIneligible);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped call context unsafe",
                     sharedHelperSkippedCallContextUnsafe);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped call targets unsafe",
                     sharedHelperSkippedCallTargetsUnsafe);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped composite",
                     sharedHelperSkippedComposite);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped generated temps",
                     sharedHelperSkippedGeneratedTemps);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped DPI calls",
                     sharedHelperSkippedDpiCalls);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped oversized",
                     sharedHelperSkippedOversized);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped refresh context",
                     sharedHelperSkippedRefreshContext);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped triggered",
                     sharedHelperSkippedTriggered);
    V3Stats::addStat("Scheduling, Subgraph shared order cache logic matches",
                     sharedOrderCacheLogicMatches);
    V3Stats::addStat("Scheduling, Subgraph shared order cache logic mismatches",
                     sharedOrderCacheLogicMismatches);
    V3Stats::addStat("Scheduling, Subgraph shared order cache lookups", sharedOrderCacheLookups);
    V3Stats::addStat("Scheduling, Subgraph shared order cache argument mismatches",
                     sharedOrderCacheArgumentMismatches);
    V3Stats::addStat("Scheduling, Subgraph shared order cache contract mismatches",
                     sharedOrderCacheContractMismatches);
    V3Stats::addStat("Scheduling, Subgraph shared order cache domain matches",
                     sharedOrderCacheDomainMatches);
    V3Stats::addStat("Scheduling, Subgraph shared order cache domain mismatches",
                     sharedOrderCacheDomainMismatches);
    V3Stats::addStat("Scheduling, Subgraph shared order cache not reusable",
                     sharedOrderCacheNotReusable);
    V3Stats::addStat("Scheduling, Subgraph shared order cache order calls executed",
                     sharedOrderCacheOrderCallsExecuted);
    V3Stats::addStat("Scheduling, Subgraph shared order cache order calls avoided",
                     sharedOrderCacheOrderCallsAvoided);
}

}  // namespace V3Sched
