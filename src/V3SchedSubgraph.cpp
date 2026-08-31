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
    bool m_hasTriggeredState = false;
    bool m_eligible = true;
};

class SharedHelperAbiAnalyzer final : public VNVisitor {
    AstScope* const m_boundaryScopep;
    const std::unordered_set<AstVarScope*> m_contractVscps;
    std::unordered_set<const AstCFunc*> m_visitedFuncps;
    std::unordered_map<AstVarScope*, std::pair<bool, bool>> m_accesses;
    std::unordered_set<AstVarScope*> m_hiddenVscps;
    SharedHelperAbiAnalysis m_result;

    void visit(AstCFunc* nodep) override {
        if (!m_visitedFuncps.emplace(nodep).second) return;
        if (!nodep->isLoose() || nodep->entryPoint() || nodep->needProcess() || nodep->recursive()
            || nodep->isCoroutine()) {
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
        if (funcp->entryPoint()) {
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
        : m_boundaryScopep{boundaryScopep}
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

SharedScheduleLogicSig makeSharedScheduleLogicSig(const LogicByScope& logic) {
    SharedScheduleLogicSig result;
    logic.foreachLogic([&](AstNode* logicp) {
        result.emplace_back();
        SharedScheduleLogicNode& node = result.back();
        node.m_type = static_cast<uintptr_t>(logicp->type());
        logicp->foreach([&](AstNode* scanp) {
            node.m_nodeTypes.push_back(static_cast<uintptr_t>(scanp->type()));
            if (const AstConst* const constp = VN_CAST(scanp, Const)) {
                node.m_constValues.push_back(constp->num().toString());
            }
        });
        logicp->foreach([&](AstVarRef* refp) {
            node.m_refs.push_back(
                SharedScheduleLogicRef{static_cast<uintptr_t>(refp->access()), refp->varScopep()});
        });
    });
    return result;
}

bool matchSharedScheduleLogic(const SharedScheduleLogicSig& source, const LogicByScope& candidate,
                              std::unordered_map<AstVarScope*, AstVarScope*>& sourceToCandidate) {
    std::vector<AstNode*> candidateNodes;
    candidate.foreachLogic([&](AstNode* logicp) { candidateNodes.push_back(logicp); });
    if (source.size() != candidateNodes.size()) return false;

    std::unordered_map<AstVarScope*, AstVarScope*> candidateToSource;
    for (size_t nodeIndex = 0; nodeIndex < source.size(); ++nodeIndex) {
        const SharedScheduleLogicNode& sourceNode = source[nodeIndex];
        AstNode* const candidateNodep = candidateNodes[nodeIndex];
        if (sourceNode.m_type != static_cast<uintptr_t>(candidateNodep->type())) return false;

        std::vector<uintptr_t> nodeTypes;
        std::vector<string> constValues;
        candidateNodep->foreach([&](AstNode* scanp) {
            nodeTypes.push_back(static_cast<uintptr_t>(scanp->type()));
            if (const AstConst* const constp = VN_CAST(scanp, Const)) {
                constValues.push_back(constp->num().toString());
            }
        });
        if (sourceNode.m_nodeTypes != nodeTypes || sourceNode.m_constValues != constValues) {
            return false;
        }

        std::vector<AstVarRef*> refs;
        candidateNodep->foreach([&](AstVarRef* refp) { refs.push_back(refp); });
        if (sourceNode.m_refs.size() != refs.size()) return false;
        for (size_t refIndex = 0; refIndex < refs.size(); ++refIndex) {
            const SharedScheduleLogicRef& sourceRef = sourceNode.m_refs[refIndex];
            AstVarScope* const candidateVscp = refs[refIndex]->varScopep();
            if (sourceRef.m_access != static_cast<uintptr_t>(refs[refIndex]->access())
                || !sourceRef.m_vscp->dtypep()->similarDType(candidateVscp->dtypep())) {
                return false;
            }
            const auto sourceIt = sourceToCandidate.find(sourceRef.m_vscp);
            if (sourceIt != sourceToCandidate.end()) {
                if (sourceIt->second != candidateVscp) return false;
            } else {
                const auto candidateIt = candidateToSource.find(candidateVscp);
                if (candidateIt != candidateToSource.end()
                    && candidateIt->second != sourceRef.m_vscp) {
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
    const AstSenTree* m_domainKeyp = nullptr;
    AstCFunc* m_funcp = nullptr;
    AstCCall* m_firstCallp = nullptr;
    AstNode* m_templateStmtsp = nullptr;
    SharedScheduleLogicSig m_logicSig;
    std::vector<SharedHelperArg> m_args;
    std::unique_ptr<V3SubgraphContract> m_contractp;
    SharedHelperAbiAnalysis m_abi;
    bool m_parameterized = false;
};

VAccess sharedHelperArgAccess(const SharedHelperArg& arg) {
    if (arg.m_read && arg.m_write) return VAccess::READWRITE;
    return arg.m_write ? VAccess::WRITE : VAccess::READ;
}

std::vector<SharedHelperArg> collectSharedHelperArgs(AstCFunc* funcp, AstScope* boundaryScopep) {
    std::vector<SharedHelperArg> args;
    std::unordered_map<AstVarScope*, size_t> argIndex;
    funcp->foreach([&](AstNodeVarRef* refp) {
        AstVarScope* const vscp = refp->varScopep();
        if (vscp->varp()->isFuncLocal()) return;
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
                                    bool slow, bool refresh) {
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
        UASSERT_OBJ(refp->varp()->isFuncLocal(), refp,
                    "Shared subgraph helper retained an implicit instance reference");
    });
    addSharedHelperCallArgs(artifact.m_firstCallp, artifact.m_args);
    artifact.m_funcp->isStatic(true);
    artifact.m_funcp->noLife(true);
    artifact.m_parameterized = true;
}

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

    // Compare alpha-equivalent bodies by temporarily mapping candidate variables to the
    // canonical argument order. Constants and every other AST property remain exact.
    std::unordered_map<AstVarScope*, AstVarScope*> remap;
    for (size_t index = 0; index < artifact.m_args.size(); ++index) {
        remap.emplace(candidateArgs[index].m_vscp, artifact.m_args[index].m_vscp);
    }
    struct RestoreRef final {
        AstNodeVarRef* m_refp;
        AstVarScope* m_vscp;
        AstVar* m_varp;
        VSelfPointerText m_selfPointer;
    };
    std::vector<RestoreRef> restores;
    candidateFuncp->foreach([&](AstNodeVarRef* refp) {
        const auto it = remap.find(refp->varScopep());
        if (it == remap.end()) return;
        restores.push_back(RestoreRef{refp, refp->varScopep(), refp->varp(), refp->selfPointer()});
        refp->varp(it->second->varp());
        refp->varScopep(it->second);
        refp->selfPointer(VSelfPointerText{VSelfPointerText::Empty()});
    });
    const bool same = candidateFuncp->stmtsp()->sameTree(artifact.m_templateStmtsp);
    for (const RestoreRef& restore : restores) {
        restore.m_refp->varp(restore.m_varp);
        restore.m_refp->varScopep(restore.m_vscp);
        restore.m_refp->selfPointer(restore.m_selfPointer);
    }
    return same;
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

    uint64_t snapshotInstances = 0;
    uint64_t snapshotSources = 0;
    prepareSubgraphSnapshots(groups, regionWrittenVscps, snapshotInstances, snapshotSources);

    uint64_t orderedLogic = 0;
    uint64_t contractBoundaryUses = 0;
    uint64_t contractExternalUses = 0;
    uint64_t contractInternalUses = 0;
    uint64_t contracts = 0;
    uint64_t coarseNodes = 0;
    uint64_t logicalUses = 0;
    uint64_t refreshHelpers = 0;
    uint64_t sharedAbiAnalyses = 0;
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
    uint64_t sharedHelperParameterizations = 0;
    uint64_t sharedHelperReuses = 0;
    uint64_t sharedHelperSkippedCalls = 0;
    uint64_t sharedHelperSkippedComposite = 0;
    uint64_t sharedHelperSkippedGeneratedTemps = 0;
    uint64_t sharedHelperSkippedOversized = 0;
    uint64_t sharedHelperSkippedTriggered = 0;
    uint64_t sharedOrderCacheLogicMatches = 0;
    uint64_t sharedOrderCacheLogicMismatches = 0;
    uint64_t sharedOrderCacheLookups = 0;
    uint64_t sharedOrderCacheOrderCallsAvoided = 0;
    struct EligibleModulePhase final {
        AstNodeModule* m_modp;
        VSubgraphPhase m_phase;
    };
    std::vector<EligibleModulePhase> eligibleModulePhases;
    std::vector<SharedHelperArtifact> sharedHelperArtifacts;
    const auto noteSharedAbi = [&](const SharedHelperAbiAnalysis& abi,
                                   VSubgraphPhase subgraphPhase, AstNodeModule* modp) {
        ++sharedAbiAnalyses;
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

        const auto orderPhase = [&](LogicByScope& logic, const string& phase,
                                    VSubgraphPhase subgraphPhase) {
            if (logic.empty()) return;
            const SharedScheduleLogicSig logicSig = makeSharedScheduleLogicSig(logic);
            SharedHelperArtifact* cachedArtifactp = nullptr;
            std::unordered_map<AstVarScope*, AstVarScope*> sourceToCandidate;
            std::vector<SharedHelperArg> cachedArgs;
            for (SharedHelperArtifact& artifact : sharedHelperArtifacts) {
                if (artifact.m_modp != group.m_boundaryScopep->modp()
                    || artifact.m_phase != subgraphPhase
                    || !artifact.m_domainKeyp->sameTree(group.m_domainKeyp)) {
                    continue;
                }
                ++sharedOrderCacheLookups;
                sourceToCandidate.clear();
                if (!matchSharedScheduleLogic(artifact.m_logicSig, logic, sourceToCandidate)) {
                    ++sharedOrderCacheLogicMismatches;
                    continue;
                }
                if (!canRemapSharedScheduleContract(artifact, group.m_boundaryScopep,
                                                    sourceToCandidate)) {
                    continue;
                }
                cachedArgs.clear();
                for (const SharedHelperArg& arg : artifact.m_args) {
                    const auto it = sourceToCandidate.find(arg.m_vscp);
                    if (it == sourceToCandidate.end()) break;
                    cachedArgs.push_back(
                        SharedHelperArg{it->second, arg.m_read, arg.m_write, arg.m_state});
                }
                if (cachedArgs.size() != artifact.m_args.size()) continue;
                ++sharedOrderCacheLogicMatches;
                cachedArtifactp = &artifact;
                break;
            }
            const string tag = "nba_subgraph_" + phase + "_" + cvtToStr(groupIndex);
            const bool refresh = subgraphPhase == VSubgraphPhase{VSubgraphPhase::REFRESH};
            AstCFunc* funcp = nullptr;
            std::unique_ptr<V3SubgraphContract> contractp;
            SharedHelperAbiAnalysis abi;
            if (cachedArtifactp) {
                if (!cachedArtifactp->m_parameterized) {
                    parameterizeSharedHelper(*cachedArtifactp);
                    sharedHelperArguments += cachedArtifactp->m_args.size();
                    ++sharedHelperParameterizations;
                }
                funcp = makeSharedScheduleWrapper(netlistp, group.m_boundaryScopep,
                                                  group.m_senTreep, cachedArtifactp->m_funcp,
                                                  cachedArgs, tag, slow, refresh);
                discardSharedScheduleLogic(logic);
                contractp = std::make_unique<V3SubgraphContract>(V3SubgraphContract::remap(
                    *cachedArtifactp->m_contractp, group.m_boundaryScopep, group.m_senTreep,
                    sourceToCandidate));
                abi = cachedArtifactp->m_abi;
                ++sharedHelperBodyChecks;
                ++sharedHelperReuses;
                ++sharedOrderCacheOrderCallsAvoided;
            } else {
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
                funcp = V3Order::order(netlistp, {&logic}, trigToSen, tag, false, slow,
                                       phaseExternalDomains, group.m_boundaryScopep);
                if (funcp) {
                    if (refresh) removeSingleDomainGuard(funcp);
                    util::splitCheck(funcp);
                    contractp = std::make_unique<V3SubgraphContract>(V3SubgraphContract::make(
                        funcp, group.m_boundaryScopep, group.m_senTreep,
                        subgraphPhase == VSubgraphPhase{VSubgraphPhase::POST}, refresh));
                    abi = SharedHelperAbiAnalyzer{funcp, group.m_boundaryScopep, *contractp}
                              .result();
                }
            }
            if (!funcp) return;
            const V3SubgraphContract& contract = *contractp;
            contractBoundaryUses += contract.boundaryUses().size();
            contractExternalUses += contract.externalUses().size();
            contractInternalUses += contract.internalUses().size();
            ++contracts;
            noteSharedAbi(abi, subgraphPhase, group.m_boundaryScopep->modp());

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
            const bool shareCandidate = sharedAbi.m_eligible && !sharedAbi.m_hasTriggeredState
                                        && !sharedAbi.m_calls && !generatedTemps;
            const std::vector<SharedHelperArg> args
                = shareCandidate ? collectSharedHelperArgs(sharedFuncp, group.m_boundaryScopep)
                                 : std::vector<SharedHelperArg>{};
            const bool compositeArgs
                = std::any_of(args.begin(), args.end(), [](const SharedHelperArg& arg) {
                      AstNodeDType* const dtypep = arg.m_vscp->dtypep()->skipRefp();
                      return !VN_IS(dtypep, BasicDType) || dtypep->isWide();
                  });
            // Composite and very wide ABIs require additional alias/lifetime validation. Start
            // with the small scalar case and leave the wrapper unchanged for all other helpers.
            static constexpr size_t kMaxSharedHelperArgs = 8;
            if (cachedArtifactp) {
                // The cached artifact was already validated and accounted for above.
            } else if (shareCandidate && !compositeArgs && args.size() <= kMaxSharedHelperArgs) {
                SharedHelperArtifact* matchingArtifactp = nullptr;
                for (SharedHelperArtifact& artifact : sharedHelperArtifacts) {
                    if (artifact.m_modp != group.m_boundaryScopep->modp()
                        || artifact.m_phase != subgraphPhase) {
                        continue;
                    }
                    ++sharedHelperBodyChecks;
                    if (sameSharedHelperBody(artifact, sharedFuncp, args)) {
                        matchingArtifactp = &artifact;
                        break;
                    }
                    ++sharedHelperBodyMismatches;
                }
                if (matchingArtifactp) {
                    if (!matchingArtifactp->m_parameterized) {
                        parameterizeSharedHelper(*matchingArtifactp);
                        sharedHelperArguments += matchingArtifactp->m_args.size();
                        ++sharedHelperParameterizations;
                    }
                    sharedCallp->funcp(matchingArtifactp->m_funcp);
                    addSharedHelperCallArgs(sharedCallp, args);
                    ++sharedHelperReuses;
                } else {
                    sharedHelperArtifacts.push_back(SharedHelperArtifact{
                        group.m_boundaryScopep->modp(), subgraphPhase, group.m_domainKeyp,
                        sharedFuncp, sharedCallp, cloneSharedHelperTemplate(sharedFuncp), logicSig,
                        args, std::make_unique<V3SubgraphContract>(contract), abi, false});
                    ++sharedHelperArtifactCount;
                }
            } else if (compositeArgs) {
                ++sharedHelperSkippedComposite;
            } else if (shareCandidate) {
                ++sharedHelperSkippedOversized;
            } else if (sharedAbi.m_hasTriggeredState) {
                ++sharedHelperSkippedTriggered;
            } else if (sharedAbi.m_calls) {
                ++sharedHelperSkippedCalls;
            } else if (generatedTemps) {
                ++sharedHelperSkippedGeneratedTemps;
            }
            AstSubgraphInstance* const instancep = new AstSubgraphInstance{
                group.m_filelinep, group.m_boundaryScopep, subgraphPhase, callp};
            for (const V3SubgraphContract::LogicalUse& use :
                 V3SubgraphContract::makeLogicalBoundaryUses(group.m_boundaryScopep)) {
                instancep->addLogicalUse(use.m_name, use.m_read, use.m_write);
            }
            const auto addMaterializedUses = [&](const std::vector<V3SubgraphContract::Use>& uses,
                                                 VSubgraphUseKind kind) {
                for (const V3SubgraphContract::Use& use : uses) {
                    const bool snapshotStorage
                        = kind == VSubgraphUseKind{VSubgraphUseKind::EXTERNAL}
                          && std::any_of(group.m_snapshots.begin(), group.m_snapshots.end(),
                                         [&](const auto& snapshot) {
                                             return snapshot.m_storageVscp == use.m_varScopep;
                                         });
                    instancep->addMaterializedUse(
                        use.m_varScopep,
                        snapshotStorage ? VSubgraphUseKind{VSubgraphUseKind::SNAPSHOT_STORAGE}
                                        : kind,
                        use.m_read, use.m_write, use.m_cuttable);
                }
            };
            addMaterializedUses(contract.boundaryUses(), VSubgraphUseKind::BOUNDARY);
            addMaterializedUses(contract.externalUses(), VSubgraphUseKind::EXTERNAL);
            addMaterializedUses(contract.internalUses(), VSubgraphUseKind::INTERNAL);
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
        };

        orderPhase(group.m_preLogic, "pre", VSubgraphPhase::PRE);
        orderPhase(group.m_postLogic, "post", VSubgraphPhase::POST);
        if (!group.m_refreshLogic.empty()) ++refreshHelpers;
        orderPhase(group.m_refreshLogic, "refresh", VSubgraphPhase::REFRESH);
        ++groupIndex;
    }

    for (SharedHelperArtifact& artifact : sharedHelperArtifacts) {
        artifact.m_templateStmtsp->deleteTree();
        artifact.m_templateStmtsp = nullptr;
    }

    V3Stats::addStat("Scheduling, Subgraph NBA groups", groups.size());
    V3Stats::addStat("Scheduling, Subgraph NBA internal actives", orderedLogic);
    V3Stats::addStat("Scheduling, Subgraph NBA contract boundary uses", contractBoundaryUses);
    V3Stats::addStat("Scheduling, Subgraph NBA contract external uses", contractExternalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA contract internal uses", contractInternalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA contracts", contracts);
    V3Stats::addStat("Scheduling, Subgraph NBA coarse nodes", coarseNodes);
    V3Stats::addStat("Scheduling, Subgraph NBA logical uses", logicalUses);
    V3Stats::addStat("Scheduling, Subgraph NBA refresh helpers", refreshHelpers);
    V3Stats::addStat("Scheduling, Subgraph NBA snapshot instances", snapshotInstances);
    V3Stats::addStat("Scheduling, Subgraph NBA snapshot sources", snapshotSources);
    V3Stats::addStat("Scheduling, Subgraph shared ABI analyses", sharedAbiAnalyses);
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
    V3Stats::addStat("Scheduling, Subgraph shared helper artifacts", sharedHelperArtifactCount);
    V3Stats::addStat("Scheduling, Subgraph shared helper body checks", sharedHelperBodyChecks);
    V3Stats::addStat("Scheduling, Subgraph shared helper body mismatches",
                     sharedHelperBodyMismatches);
    V3Stats::addStat("Scheduling, Subgraph shared helper parameterizations",
                     sharedHelperParameterizations);
    V3Stats::addStat("Scheduling, Subgraph shared helper reuses", sharedHelperReuses);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped calls", sharedHelperSkippedCalls);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped composite",
                     sharedHelperSkippedComposite);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped generated temps",
                     sharedHelperSkippedGeneratedTemps);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped oversized",
                     sharedHelperSkippedOversized);
    V3Stats::addStat("Scheduling, Subgraph shared helper skipped triggered",
                     sharedHelperSkippedTriggered);
    V3Stats::addStat("Scheduling, Subgraph shared order cache logic matches",
                     sharedOrderCacheLogicMatches);
    V3Stats::addStat("Scheduling, Subgraph shared order cache logic mismatches",
                     sharedOrderCacheLogicMismatches);
    V3Stats::addStat("Scheduling, Subgraph shared order cache lookups", sharedOrderCacheLookups);
    V3Stats::addStat("Scheduling, Subgraph shared order cache order calls avoided",
                     sharedOrderCacheOrderCallsAvoided);
}

}  // namespace V3Sched
