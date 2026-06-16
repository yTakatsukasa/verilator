// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Code scheduling
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
//
// V3Sched::schedule is the top level entry-point to the scheduling algorithm
// at a high level, the process is:
//
//  - Gather and classify all logic in the design based on what triggers its execution
//  - Schedule static, initial and final logic classes in source order
//  - Break combinational cycles by introducing hybrid logic
//  - Create 'settle' region that restores the combinational invariant
//  - Partition the clocked and combinational (including hybrid) logic into pre/act/nba.
//    All clocks (signals referenced in an AstSenTree) generated via a blocking assignment
//    (including combinationally generated signals) are computed within the act region.
//  - Replicate combinational logic
//  - Create input combinational logic loop
//  - Create the pre/act/nba triggers
//  - Create the 'act' region evaluation function
//  - Create the 'nba' region evaluation function
//  - Bolt it all together to create the '_eval' function
//
// Details of the algorithm are described in the internals documentation docs/internals.rst
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3Sched.h"

#include "V3Const.h"
#include "V3EmitCBase.h"
#include "V3EmitV.h"
#include "V3Order.h"
#include "V3SenExprBuilder.h"
#include "V3Stats.h"

VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3Sched {

namespace {

using SubgraphCallUsageSummaryMap
    = std::unordered_map<const AstCFunc*, std::vector<SubgraphCallUsageSummary>>;
SubgraphCallUsageSummaryMap s_subgraphCallUsageSummaries;
std::unordered_set<const AstNodeProcedure*> s_subgraphSnapshotProcedures;

//============================================================================
// Utility functions

std::vector<const AstSenTree*> getSenTreesUsedBy(const std::vector<const LogicByScope*>& lbsps) {
    const VNUser1InUse user1InUse;
    std::vector<const AstSenTree*> result;
    for (const LogicByScope* const lbsp : lbsps) {
        for (const auto& pair : *lbsp) {
            AstActive* const activep = pair.second;
            AstSenTree* const senTreep = activep->sentreep();
            if (senTreep->user1SetOnce()) continue;
            if (senTreep->hasClocked() || senTreep->hasHybrid()) result.push_back(senTreep);
        }
    }
    return result;
}

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

void remapSensitivities(const LogicByScope& lbs,
                        const std::unordered_map<const AstSenTree*, AstSenTree*>& senTreeMap) {
    for (const auto& pair : lbs) {
        AstActive* const activep = pair.second;
        AstSenTree* const senTreep = activep->sentreep();
        if (senTreep->hasCombo()) continue;
        activep->sentreep(senTreeMap.at(senTreep));
    }
}

void invertAndMergeSenTreeMap(
    V3Order::TrigToSenMap& result,
    const std::unordered_map<const AstSenTree*, AstSenTree*>& senTreeMap) {
    for (const auto& pair : senTreeMap) result.emplace(pair.second, pair.first);
}

// Find VIF triggers that a given VarScope should be sensitive to.
// Case 2 (non-virtual interface read): sensitive to that specific VarScope's trigger only
// Case 3 (virtual interface read): sensitive to all member triggers of the same interface type
std::vector<AstSenTree*> findTriggeredIface(const AstVarScope* vscp,
                                            const VirtIfaceTriggers::VscpSensMap& vscpToSens,
                                            const VirtIfaceTriggers& virtIfaceTriggers) {
    std::vector<AstSenTree*> result;
    if (vscp->varp()->isVirtIface()) {
        // Virtual interface variable -- sensitive to all member triggers of this interface type
        const AstIface* const ifacep = VN_AS(vscp->varp()->dtypep(), IfaceRefDType)->ifacep();
        for (const auto& entry : virtIfaceTriggers.m_triggers) {
            if (entry.m_ifacep == ifacep) {
                const auto it = vscpToSens.find(entry.m_vscp);
                if (it != vscpToSens.end()) result.push_back(it->second);
            }
        }
    } else {
        // Non-virtual interface member -- sensitive to this VarScope's trigger only
        const auto it = vscpToSens.find(vscp);
        if (it != vscpToSens.end()) result.push_back(it->second);
    }
    // May be empty if sensIfacep() is set but no VIF write targets this member
    return result;
}

//============================================================================
// Eval loop builder

struct EvalLoop final {
    // Flag set to true on entry to the first iteration of the loop
    AstVarScope* firstIterp;
    // The loop itself and statements around it
    AstNodeStmt* stmtsp;
};

// Create an eval loop with all the trimmings.
EvalLoop createEvalLoop(
    AstNetlist* netlistp,  //
    const std::string& tag,  // Tag for current phase
    const string& name,  // Name of current phase
    bool slow,  // Should create slow functions
    const TriggerKit& trigKit,  // The trigger kit
    AstVarScope* trigp,  // The trigger vector - may be nullptr if no triggers or using 'condp'
    AstNodeExpr* condp,  // Explicit condition that must be true to run 'phaseWorkp'
    AstNodeStmt* innerp,  // The inner loop, if any
    AstNodeStmt* phasePrepp,  // Prep statements run before checking triggers
    AstNodeStmt* phaseWorkp,  // The work to do if anything triggered
    // Extra statements to run after the work, even if no triggers fired. This function is
    // passed a variable, which must be set to true if we must continue and loop again,
    // and must be unmodified otherwise.
    std::function<AstNodeStmt*(AstVarScope*)> phaseExtra = [](AstVarScope*) { return nullptr; }  //
) {
    UASSERT(!trigp || !condp, "Cannot use both 'trigp' and 'condp' in 'createEvalLoop'");

    // All work is under a trigger or condition, so if there are none,
    // there is nothing to do besides executing the inner loop.
    if (!trigp && !condp) return {nullptr, innerp};

    const std::string varPrefix = "__V" + tag;
    AstScope* const scopeTopp = netlistp->topScopep()->scopep();
    FileLine* const flp = netlistp->fileline();

    // We wrap the prep/cond/work in a function for readability
    AstCFunc* const phaseFuncp = util::makeTopFunction(netlistp, "_eval_phase__" + tag, slow);
    {
        // Add the preparatory statements
        phaseFuncp->addStmtsp(phasePrepp);

        // The execute flag
        AstVarScope* const executeFlagp = scopeTopp->createTemp(varPrefix + "Execute", 1);
        executeFlagp->varp()->noReset(true);

        // If there is work in this phase, execute it if any triggers fired
        if (phaseWorkp) {
            AstNodeExpr* const lhsp = new AstVarRef{flp, executeFlagp, VAccess::WRITE};
            // If using explicit condition, that directly determines whether to execute,
            // otherwise check if any triggers are fired
            AstNodeExpr* const rhsp = condp ? condp : trigKit.newAnySetCall(trigp);
            phaseFuncp->addStmtsp(new AstAssign{flp, lhsp, rhsp});

            // Add the work
            AstIf* const ifp = new AstIf{flp, new AstVarRef{flp, executeFlagp, VAccess::READ}};
            ifp->addThensp(phaseWorkp);
            phaseFuncp->addStmtsp(ifp);
        }

        // Construct the extra statements
        AstNodeStmt* const extraWorkp = phaseExtra(executeFlagp);
        if (extraWorkp) phaseFuncp->addStmtsp(extraWorkp);

        // The function returns ture iff it did run work
        phaseFuncp->rtnType("bool");
        AstNodeExpr* const retp
            = phaseWorkp || extraWorkp
                  ? static_cast<AstNodeExpr*>(new AstVarRef{flp, executeFlagp, VAccess::READ})
                  : static_cast<AstNodeExpr*>(new AstConst{flp, AstConst::BitFalse{}});
        phaseFuncp->addStmtsp(new AstCReturn{flp, retp});
    }

    // The result statements
    AstNodeStmt* stmtps = nullptr;

    // Prof-exec section push
    if (v3Global.opt.profExec()) {  //
        stmtps = AstCStmt::profExecSectionPush(flp, "loop " + tag);
    }

    const auto addVar = [&](const std::string& name, int width, uint32_t initVal, bool init) {
        const string tempName{"__V" + tag + name};
        AstVarScope* const vscp = tempName == "__VstlFirstIteration"
                                      ? netlistp->stlFirstIterationp()
                                      : scopeTopp->createTemp(tempName, width);
        vscp->varp()->noReset(true);
        vscp->varp()->isInternal(true);
        if (init) stmtps = AstNode::addNext(stmtps, util::setVar(vscp, initVal));
        return vscp;
    };

    // The iteration counter
    AstVarScope* const counterp = addVar("IterCount", 32, 0, true);
    // The first iteration flag - cleared in 'phasePrepp' if used
    AstVarScope* const firstIterFlagp = addVar("FirstIteration", 1, 1, true);
    // Phase function result
    AstVarScope* const phaseResultp = addVar("PhaseResult", 1, 0, false);

    // The loop
    {
        AstLoop* const loopp = new AstLoop{flp};
        stmtps->addNext(loopp);

        // Check the iteration limit (aborts if exceeded). Dump triggers if using triggers.
        AstNodeStmt* dumpCallp = trigp ? trigKit.newDumpCall(trigp, tag, false) : nullptr;
        loopp->addStmtsp(util::checkIterationLimit(netlistp, name, counterp, dumpCallp));
        // Increment the iteration counter
        loopp->addStmtsp(util::incrementVar(counterp));

        // Execute the inner loop
        loopp->addStmtsp(innerp);

        // Call the phase function to execute the current work. If we did
        // work, then need to loop again, so set the continuation flag.
        // If used, the first iteration flag is cleared when consumed, no
        // need to reset it
        AstCCall* const callp = new AstCCall{flp, phaseFuncp};
        callp->dtypeSetBit();
        AstAssign* const resultAssignp
            = new AstAssign{flp, new AstVarRef{flp, phaseResultp, VAccess::WRITE}, callp};
        loopp->addStmtsp(resultAssignp);
        // Clear FirstIteration flag
        AstAssign* const firstClearp
            = new AstAssign{flp, new AstVarRef{flp, firstIterFlagp, VAccess::WRITE},
                            new AstConst{flp, AstConst::BitFalse()}};
        loopp->addStmtsp(firstClearp);
        // Continues until the continuation flag is clear
        loopp->addStmtsp(
            new AstLoopTest{flp, loopp, new AstVarRef{flp, phaseResultp, VAccess::READ}});
    }

    // Prof-exec section pop
    if (v3Global.opt.profExec()) {
        stmtps->addNext(AstCStmt::profExecSectionPop(flp, "loop " + tag));
    }

    return {firstIterFlagp, stmtps};
}

//============================================================================
// Collect and classify all logic in the design

LogicClasses gatherLogicClasses(AstNetlist* netlistp) {
    LogicClasses result;

    netlistp->foreach([&](AstScope* scopep) {
        scopep->foreach([&](AstActive* activep) {
            AstSenTree* const senTreep = activep->sentreep();
            if (senTreep->hasStatic()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "static initializer with additional sensitivities");
                result.m_static.emplace_back(scopep, activep);
            } else if (senTreep->hasInitial()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "'initial' logic with additional sensitivities");
                result.m_initial.emplace_back(scopep, activep);
            } else if (senTreep->hasFinal()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "'final' logic with additional sensitivities");
                result.m_final.emplace_back(scopep, activep);
            } else if (senTreep->hasCombo()) {
                UASSERT_OBJ(!senTreep->sensesp()->nextp(), activep,
                            "combinational logic with additional sensitivities");
                if (VN_IS(activep->stmtsp(), AlwaysPostponed)) {
                    result.m_postponed.emplace_back(scopep, activep);
                } else {
                    result.m_comb.emplace_back(scopep, activep);
                }
            } else {
                UASSERT_OBJ(senTreep->hasClocked(), activep, "What else could it be?");
                if (VN_IS(activep->stmtsp(), AlwaysObserved)) {
                    result.m_observed.emplace_back(scopep, activep);
                } else if (VN_IS(activep->stmtsp(), AlwaysReactive)) {
                    result.m_reactive.emplace_back(scopep, activep);
                } else {
                    result.m_clocked.emplace_back(scopep, activep);
                }
            }
        });
    });

    return result;
}

//============================================================================
// Simple ordering in source order

void orderSequentially(AstCFunc* funcp, const LogicByScope& lbs) {
    // Create new subfunc for scope
    const auto createNewSubFuncp = [&](AstScope* const scopep) {
        const string subName{funcp->name() + "__" + scopep->nameDotless()};
        AstCFunc* const subFuncp = new AstCFunc{scopep->fileline(), subName, scopep};
        subFuncp->isLoose(true);
        subFuncp->isConst(false);
        subFuncp->declPrivate(true);
        subFuncp->slow(funcp->slow());
        scopep->addBlocksp(subFuncp);
        // Call it from the top function
        funcp->addStmtsp(util::callVoidFunc(subFuncp));
        return subFuncp;
    };
    const VNUser1InUse user1InUse;  // AstScope -> AstCFunc: the sub-function for the scope
    const VNUser2InUse user2InUse;  // AstScope -> int: sub-function counter used for names
    for (const auto& pair : lbs) {
        AstScope* const scopep = pair.first;
        AstActive* const activep = pair.second;
        // Create a sub-function per scope so we can V3Combine them later
        if (!scopep->user1p()) scopep->user1p(createNewSubFuncp(scopep));
        // Add statements to sub-function
        for (AstNode *logicp = activep->stmtsp(), *nextp; logicp; logicp = nextp) {
            auto* subFuncp = VN_AS(scopep->user1p(), CFunc);
            nextp = logicp->nextp();
            if (AstNodeProcedure* const procp = VN_CAST(logicp, NodeProcedure)) {
                if (AstNode* bodyp = procp->stmtsp()) {
                    bodyp->unlinkFrBackWithNext();
                    // If the process is suspendable, we need a separate function (a coroutine)
                    if (procp->isSuspendable()) {
                        funcp->slow(false);
                        subFuncp = createNewSubFuncp(scopep);
                        subFuncp->name(subFuncp->name() + "__Vtiming__"
                                       + cvtToStr(scopep->user2Inc()));
                        subFuncp->rtnType("VlCoroutine");
                        if (VN_IS(procp, Always)) {
                            subFuncp->slow(false);
                            FileLine* const flp = procp->fileline();
                            AstNodeExpr* const condp = new AstCExpr{
                                flp, "VL_LIKELY(!vlSymsp->_vm_contextp__->gotFinish())", 1};
                            AstLoop* const loopp = new AstLoop{flp};
                            loopp->addStmtsp(new AstLoopTest{flp, loopp, condp});
                            loopp->addStmtsp(bodyp);
                            bodyp = loopp;
                        }
                    }
                    subFuncp->addStmtsp(bodyp);
                    if (procp->needProcess()) subFuncp->setNeedProcess();
                    util::splitCheck(subFuncp);
                }
            } else {
                logicp->unlinkFrBack();
                subFuncp->addStmtsp(logicp);
            }
        }
        if (activep->backp()) activep->unlinkFrBack();
        VL_DO_DANGLING(activep->deleteTree(), activep);
    }
}

//============================================================================
// Create simply ordered functions

AstCFunc* createStatic(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    AstCFunc* const funcp = util::makeTopFunction(netlistp, "_eval_static", /* slow: */ true);

    const LogicByScope& orig = logicClasses.m_static;
    if (orig.size() <= 1) {
        orderSequentially(funcp, orig);
        return funcp;
    }

    // Level-based module sorting can reorder packages so that an importing
    // package runs before the imported one.  Re-sort package entries by source
    // file position to restore compilation order (IEEE 1800-2023 26.3).
    std::vector<size_t> indices(orig.size());
    for (size_t i = 0; i < orig.size(); ++i) indices[i] = i;
    std::stable_sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        const AstNodeModule* const modA = orig[a].first->modp();
        const AstNodeModule* const modB = orig[b].first->modp();
        const bool isPkgA = VN_IS(modA, Package);
        const bool isPkgB = VN_IS(modB, Package);
        if (isPkgA != isPkgB) return isPkgA;  // Packages before non-packages
        if (isPkgA && isPkgB) {
            // Sort packages by source file position (compilation order)
            return modA->fileline()->operatorCompare(*modB->fileline()) < 0;
        }
        return false;  // Both non-package: preserve original order
    });
    LogicByScope sorted;
    sorted.reserve(orig.size());
    for (const size_t i : indices) sorted.emplace_back(orig[i].first, orig[i].second);

    orderSequentially(funcp, sorted);
    return funcp;  // Not splitting yet as it is not final
}

void createInitial(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    AstCFunc* const funcp = util::makeTopFunction(netlistp, "_eval_initial", /* slow: */ true);
    orderSequentially(funcp, logicClasses.m_initial);
    util::splitCheck(funcp);
}

AstCFunc* createPostponed(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    if (logicClasses.m_postponed.empty()) return nullptr;
    AstCFunc* const funcp = util::makeTopFunction(netlistp, "_eval_postponed", /* slow: */ true);
    orderSequentially(funcp, logicClasses.m_postponed);
    util::splitCheck(funcp);
    return funcp;
}

void createFinal(AstNetlist* netlistp, const LogicClasses& logicClasses) {
    AstCFunc* const funcp = util::makeTopFunction(netlistp, "_eval_final", /* slow: */ true);
    orderSequentially(funcp, logicClasses.m_final);
    util::splitCheck(funcp);
}

//============================================================================
// Helper that creates virtual interface value-change triggers

void addVirtIfaceTriggerAssignments(AstNetlist* netlistp, AstCFunc* initFuncp,
                                    const VirtIfaceTriggers& virtIfaceTriggers,
                                    uint32_t firstIndex, const TriggerKit& trigKit) {
    uint32_t index = firstIndex;
    for (const auto& entry : virtIfaceTriggers.m_triggers) {
        trigKit.addValueChangeTriggerAssignment(netlistp, initFuncp, entry.m_vscp, index);
        ++index;
    }
}

void lowerSubgraphLogic(AstNetlist* netlistp, const std::vector<LogicByScope*>& logic,
                        const V3Order::TrigToSenMap& trigToSen, const string& tag, bool slow,
                        const V3Order::ExternalDomainsProvider& externalDomains) {
    if (!v3Global.opt.subgraphSchedule()) return;
    static std::unordered_map<AstScope*, std::vector<AstCFunc*>> s_stlSubgraphFuncs;
    if (tag == "stl") s_stlSubgraphFuncs.clear();

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

    const auto wrapperFromLogic = [](AstNode* nodep) {
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
    };

    const auto makeWrapperLogic
        = [](FileLine* flp, const SubgraphWrapper& wrapper, AstNodeStmt* callp) -> AstNode* {
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
    };

    const auto disableLifePostForExternalReads
        = [](const LogicByScope& subgraphLogic, AstScope* subgraphScopep) {
              subgraphLogic.foreachLogic([&](AstNode* logicp) {
                  logicp->foreach([&](AstVarRef* refp) {
                      if (refp->access().isWriteOnly()) return;
                      AstVarScope* const vscp = refp->varScopep();
                      if (vscp->scopep() == subgraphScopep) return;
                      vscp->optimizeLifePost(false);
                  });
              });
          };

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

    const auto boundaryScopeFor = [](AstScope* scopep) -> AstScope* {
        for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
            if (scanp->modp()->subgraphBoundary()) return scanp;
        }
        return nullptr;
    };
    const auto discardLogic = [](LogicByScope& logic) {
        for (const auto& pair : logic) {
            AstActive* const activep = pair.second;
            if (activep->backp()) activep->unlinkFrBack();
            activep->deleteTree();
        }
        logic.clear();
    };

    const bool snapshotCrossBoundaryReads = tag == "nba";
    std::unordered_map<SubgraphOrderCacheKey, SubgraphOrderCacheEntry, SubgraphOrderCacheKeyHash>
        subgraphOrderCache;
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
    std::vector<SnapshotBucket> snapshotBuckets;
    std::unordered_set<AstVarScope*> regionWrittenVars;
    const auto getSnapshotBucket
        = [&](LogicByScope* ownerp, AstSenTree* senTreep) -> SnapshotBucket& {
        for (SnapshotBucket& bucket : snapshotBuckets) {
            if (bucket.m_ownerp == ownerp && bucket.m_senTreep == senTreep) return bucket;
        }
        snapshotBuckets.emplace_back();
        SnapshotBucket& bucket = snapshotBuckets.back();
        bucket.m_ownerp = ownerp;
        bucket.m_senTreep = senTreep;
        return bucket;
    };
    const auto addSnapshotRequirement
        = [&](LogicByScope* ownerp, AstSenTree* senTreep, AstVarScope* sourceVscp) {
              SnapshotBucket& bucket = getSnapshotBucket(ownerp, senTreep);
              if (!bucket.m_seen.insert(sourceVscp).second) return;
              bucket.m_sourceVars.push_back(sourceVscp);
          };
    const auto getSnapshotRef = [&](LogicByScope* ownerp, AstSenTree* senTreep,
                                    AstVarScope* sourceVscp) -> const SnapshotRef& {
        SnapshotBucket& bucket = getSnapshotBucket(ownerp, senTreep);
        const auto it = bucket.m_snapshotRefs.find(sourceVscp);
        UASSERT_OBJ(it != bucket.m_snapshotRefs.end(), sourceVscp,
                    "Missing subgraph snapshot reference");
        return it->second;
    };
    const auto makeSnapshotExpr
        = [&](const SnapshotRef& snapshotRef, FileLine* flp, VAccess access) -> AstNodeExpr* {
        AstNodeExpr* const refp = new AstVarRef{flp, snapshotRef.m_snapshotVscp, access};
        if (!snapshotRef.m_isBundle) return refp;
        return new AstArraySel{flp, refp, static_cast<int>(snapshotRef.m_elemIndex)};
    };
    const auto newSnapshotHelperArg
        = [](AstCFunc* funcp, AstNodeDType* dtypep, const std::string& name,
             VDirection direction) -> AstVarScope* {
        FileLine* const flp = funcp->fileline();
        AstScope* const scopep = funcp->scopep();
        AstVar* const varp = new AstVar{flp, VVarType::BLOCKTEMP, name, dtypep};
        varp->funcLocal(true);
        varp->direction(direction);
        funcp->addArgsp(varp);
        AstVarScope* const vscp = new AstVarScope{flp, scopep, varp};
        scopep->addVarsp(vscp);
        return vscp;
    };
    const auto isUnderBoundaryScope = [](AstScope* scopep, AstScope* boundaryScopep) {
        for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
            if (scanp == boundaryScopep) return true;
        }
        return false;
    };
    const auto needsCrossBoundarySnapshot
        = [&](AstScope* boundaryScopep, AstVarScope* sourceVscp) -> bool {
        AstScope* const sourceScopep = sourceVscp->scopep();
        const bool boundaryInput = sourceScopep == boundaryScopep && sourceVscp->varp()->isIO()
                                   && sourceVscp->varp()->direction().isNonOutput();
        const bool externalRead = !isUnderBoundaryScope(sourceScopep, boundaryScopep);
        if (!boundaryInput && !externalRead) return false;

        // Reads of another subgraph's scoped state or outputs can change when its wrapper runs.
        if (sourceScopep != boundaryScopep && boundaryScopeFor(sourceScopep)) return true;

        // Otherwise only snapshot sources that are rewritten by non-subgraph logic in this region.
        return regionWrittenVars.count(sourceVscp);
    };
    const auto collectCrossBoundaryReads = [&](AstNode* nodep, AstScope* boundaryScopep,
                                               LogicByScope* ownerp, AstSenTree* senTreep) {
        nodep->foreach([&](AstVarRef* refp) {
            if (refp->access() != VAccess::READ) return;
            AstVarScope* const sourceVscp = refp->varScopep();
            if (!needsCrossBoundarySnapshot(boundaryScopep, sourceVscp)) return;
            addSnapshotRequirement(ownerp, senTreep, sourceVscp);
        });
    };
    const auto collectCrossBoundaryReadsInLogic
        = [&](LogicByScope& subgraphLogic, AstScope* boundaryScopep, LogicByScope* ownerp,
              AstSenTree* senTreep) {
              subgraphLogic.foreachLogic([&](AstNode* logicp) {
                  collectCrossBoundaryReads(logicp, boundaryScopep, ownerp, senTreep);
              });
          };
    const auto rewriteCrossBoundaryReads = [&](AstNode* nodep, AstScope* boundaryScopep,
                                               LogicByScope* ownerp, AstSenTree* senTreep) {
        if (!snapshotCrossBoundaryReads) return;
        nodep->foreach([&](AstVarRef* refp) {
            if (refp->access() != VAccess::READ) return;
            AstVarScope* const sourceVscp = refp->varScopep();
            if (!needsCrossBoundarySnapshot(boundaryScopep, sourceVscp)) return;
            const SnapshotRef& snapshotRef = getSnapshotRef(ownerp, senTreep, sourceVscp);
            refp->replaceWith(makeSnapshotExpr(snapshotRef, refp->fileline(), VAccess::READ));
            VL_DO_DANGLING(refp->deleteTree(), refp);
        });
    };
    const auto rewriteCrossBoundaryReadsInLogic
        = [&](LogicByScope& subgraphLogic, AstScope* boundaryScopep, LogicByScope* ownerp,
              AstSenTree* senTreep) {
              subgraphLogic.foreachLogic([&](AstNode* logicp) {
                  rewriteCrossBoundaryReads(logicp, boundaryScopep, ownerp, senTreep);
              });
          };
    const auto cloneTailFuncForNba = [&](AstCFunc* tailFuncp, AstScope* boundaryScopep,
                                         LogicByScope* ownerp, AstSenTree* senTreep) -> AstCFunc* {
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
        return clonep;
    };
    const auto tailNeedsNbaClone = [&](AstCFunc* tailFuncp, AstScope* boundaryScopep) {
        bool needClone = false;
        tailFuncp->foreach([&](AstVarRef* refp) {
            if (needClone || refp->access() != VAccess::READ) return;
            if (needsCrossBoundarySnapshot(boundaryScopep, refp->varScopep())) needClone = true;
        });
        return needClone;
    };
    const auto computeDomainShape = [&](const LogicByScope& logic, AstScope* boundaryScopep) {
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
    };
    const auto buildLogicSig = [](const LogicByScope& logic) {
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
    };
    const auto buildTemplateVarScopeMap
        = [](const SubgraphLogicSig& templateSig, const LogicByScope& currentLogic,
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
          };
    const auto canShareSubgraphLogic = [&](const LogicByScope& logic, AstScope* boundaryScopep) {
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
    };
    const auto cloneOrderedFuncGraph
        = [&](AstCFunc* funcp, AstScope* destBoundaryScopep,
              const std::unordered_map<const AstVarScope*, AstVarScope*>& templateVarMap)
        -> AstCFunc* {
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
                if (resolvedVarMap.find(sourceVscp) == resolvedVarMap.end()) { failed = true; }
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
    };
    const auto buildSubgraphCallUsageSummary = [&](AstCFunc* funcp, AstScope* boundaryScopep) {
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
    };
    const auto registerSubgraphCallUsageSummary = [&](AstCFunc* funcp, AstScope* boundaryScopep) {
        s_subgraphCallUsageSummaries[funcp] = buildSubgraphCallUsageSummary(funcp, boundaryScopep);
    };

    std::vector<SubgraphGroup> groups;
    const auto findGroup
        = [&](LogicByScope* ownerp, AstScope* scopep, AstSenTree* senTreep) -> SubgraphGroup& {
        for (SubgraphGroup& group : groups) {
            if (group.m_scopep == scopep && group.m_senTreep == senTreep) return group;
        }
        groups.emplace_back();
        SubgraphGroup& group = groups.back();
        group.m_ownerp = ownerp;
        group.m_scopep = scopep;
        group.m_senTreep = senTreep;
        return group;
    };

    for (LogicByScope* const lbsp : logic) {
        LogicByScope lowered;

        for (const auto& pair : *lbsp) {
            AstScope* const scopep = pair.first;
            AstScope* const boundaryScopep = boundaryScopeFor(scopep);
            if (!boundaryScopep) {
                lowered.emplace_back(pair);
                continue;
            }

            AstActive* const activep = pair.second;
            AstSenTree* const senTreep = activep->sentreep();
            SubgraphGroup& group = findGroup(lbsp, boundaryScopep, senTreep);
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

    if (snapshotCrossBoundaryReads) {
        for (LogicByScope* const lbsp : logic) {
            lbsp->foreachLogic([&](AstNode* logicp) {
                logicp->foreach([&](AstVarRef* refp) {
                    if (!refp->access().isWriteOrRW()) return;
                    regionWrittenVars.insert(refp->varScopep());
                });
            });
        }
    }

    if (snapshotCrossBoundaryReads) {
        for (SubgraphGroup& group : groups) {
            collectCrossBoundaryReadsInLogic(group.m_earlyLogic, group.m_scopep, group.m_ownerp,
                                             group.m_senTreep);
            collectCrossBoundaryReadsInLogic(group.m_lateLogic, group.m_scopep, group.m_ownerp,
                                             group.m_senTreep);
            if (tag != "stl" && tag != "ico") {
                const auto it = s_stlSubgraphFuncs.find(group.m_scopep);
                if (it != s_stlSubgraphFuncs.end()) {
                    for (AstCFunc* const tailFuncp : it->second) {
                        if (!tailNeedsNbaClone(tailFuncp, group.m_scopep)
                            || !tailFuncp->stmtsp()) {
                            continue;
                        }
                        collectCrossBoundaryReads(tailFuncp->stmtsp(), group.m_scopep,
                                                  group.m_ownerp, group.m_senTreep);
                    }
                }
            }
        }
        for (SnapshotBucket& bucket : snapshotBuckets) {
            std::unordered_map<AstNodeDType*, std::vector<AstVarScope*>> dtypeGroups;
            for (AstVarScope* const sourceVscp : bucket.m_sourceVars) {
                dtypeGroups[sourceVscp->dtypep()].push_back(sourceVscp);
            }
            unsigned bundleIndex = 0;
            for (const auto& pair : dtypeGroups) {
                const std::vector<AstVarScope*>& groupedVars = pair.second;
                if (groupedVars.size() == 1) {
                    AstVarScope* const sourceVscp = groupedVars.front();
                    const string name = "__VsubgraphSnapshot__"
                                        + sourceVscp->scopep()->nameDotless() + "__"
                                        + sourceVscp->varp()->shortName();
                    AstVarScope* const snapshotp
                        = sourceVscp->scopep()->createTempLike(name, sourceVscp);
                    bucket.m_snapshotRefs.emplace(sourceVscp, SnapshotRef{snapshotp, 0, false});
                    continue;
                }

                FileLine* const flp = groupedVars.front()->fileline();
                AstRange* const rangep
                    = new AstRange{flp, static_cast<int>(groupedVars.size() - 1), 0};
                AstNodeDType* const bundleDTypep
                    = new AstUnpackArrayDType{flp, pair.first, rangep};
                v3Global.rootp()->typeTablep()->addTypesp(bundleDTypep);
                const string bundleName = "__VsubgraphSnapshot__"
                                          + groupedVars.front()->scopep()->nameDotless()
                                          + "__bundle" + cvtToStr(bundleIndex++);
                AstVarScope* const bundleVscp
                    = groupedVars.front()->scopep()->createTemp(bundleName, bundleDTypep);
                for (uint32_t i = 0; i < groupedVars.size(); ++i) {
                    bucket.m_snapshotRefs.emplace(groupedVars[i],
                                                  SnapshotRef{bundleVscp, i, true});
                }
            }
        }
        for (SubgraphGroup& group : groups) {
            rewriteCrossBoundaryReadsInLogic(group.m_earlyLogic, group.m_scopep, group.m_ownerp,
                                             group.m_senTreep);
            rewriteCrossBoundaryReadsInLogic(group.m_lateLogic, group.m_scopep, group.m_ownerp,
                                             group.m_senTreep);
        }
    }

    unsigned subgraphIndex = 0;
    for (SubgraphGroup& group : groups) {
        FileLine* const flp = group.m_flp;
        AstActive* const wrapperActivep = new AstActive{flp, "subgraph", group.m_senTreep};
        const auto lowerActiveGroup = [&](LogicByScope& subgraphLogic,
                                          const SubgraphWrapper& wrapper, bool isEarly,
                                          const std::vector<AstCFunc*>* tailFuncps = nullptr) {
            if (subgraphLogic.empty()) return;
            const bool canShare = canShareSubgraphLogic(subgraphLogic, group.m_scopep);
            SubgraphOrderCacheKey cacheKey;
            cacheKey.m_domainShape = computeDomainShape(subgraphLogic, group.m_scopep);
            cacheKey.m_modp = group.m_scopep->modp();
            cacheKey.m_senTreep = group.m_senTreep;
            cacheKey.m_isEarly = isEarly;
            AstCFunc* funcp = nullptr;
            if (canShare) {
                const auto cacheIt = subgraphOrderCache.find(cacheKey);
                if (cacheIt != subgraphOrderCache.end()) {
                    std::unordered_map<const AstVarScope*, AstVarScope*> templateVarMap;
                    if (buildTemplateVarScopeMap(cacheIt->second.m_logicSig, subgraphLogic,
                                                 templateVarMap)) {
                        funcp = cloneOrderedFuncGraph(cacheIt->second.m_funcp, group.m_scopep,
                                                      templateVarMap);
                        if (funcp) discardLogic(subgraphLogic);
                    }
                }
            }
            if (!funcp) {
                SubgraphLogicSig logicSig;
                if (canShare) logicSig = buildLogicSig(subgraphLogic);
                funcp = V3Order::order(netlistp, {&subgraphLogic}, trigToSen,
                                       tag + "_subgraph_" + cvtToStr(subgraphIndex++), false, slow,
                                       externalDomains, group.m_scopep);
                if (funcp) {
                    util::splitCheck(funcp);
                    registerSubgraphCallUsageSummary(funcp, group.m_scopep);
                    if (canShare) {
                        if (subgraphOrderCache.find(cacheKey) == subgraphOrderCache.end()) {
                            subgraphOrderCache.emplace(
                                cacheKey, SubgraphOrderCacheEntry{funcp, std::move(logicSig)});
                        }
                    }
                }
            }
            if (funcp) {
                AstCFunc* callFuncp = funcp;
                if (tag == "stl") {
                    AstCFunc* const tailFuncp
                        = cloneUnguardedFuncBody(funcp, group.m_scopep, "__tail", slow);
                    registerSubgraphCallUsageSummary(tailFuncp, group.m_scopep);
                    s_stlSubgraphFuncs[group.m_scopep].push_back(tailFuncp);
                    callFuncp = tailFuncp;
                }
                AstNodeStmt* stmtsp = util::callVoidFunc(callFuncp);
                if (tailFuncps) {
                    for (AstCFunc* const tailFuncp : *tailFuncps) {
                        stmtsp->addNext(util::callVoidFunc(tailFuncp));
                    }
                }
                AstNodeStmt* const subgraphp
                    = new AstSubgraphInstance{flp, group.m_scopep, stmtsp};
                wrapperActivep->addStmtsp(makeWrapperLogic(flp, wrapper, subgraphp));
            }
        };
        if (!group.m_earlyLogic.empty()) {
            lowerActiveGroup(group.m_earlyLogic,
                             wrapperFromLogic(group.m_earlyLogic.front().second->stmtsp()), true);
        }
        if (!group.m_lateLogic.empty()) {
            SubgraphWrapper wrapper;
            if (group.m_hasNonPostLate) {
                wrapper = group.m_lateWrapper;
            } else if (group.m_hasPost) {
                wrapper.m_kind = SubgraphWrapperKind::AlwaysPost;
            } else {
                wrapper = wrapperFromLogic(group.m_lateLogic.front().second->stmtsp());
            }
            disableLifePostForExternalReads(group.m_lateLogic, group.m_scopep);
            const std::vector<AstCFunc*>* tailFuncps = nullptr;
            std::vector<AstCFunc*> activeTailFuncps;
            if (tag != "stl" && tag != "ico") {
                const auto it = s_stlSubgraphFuncs.find(group.m_scopep);
                if (it != s_stlSubgraphFuncs.end()) {
                    if (snapshotCrossBoundaryReads) {
                        activeTailFuncps.reserve(it->second.size());
                        for (AstCFunc* const tailFuncp : it->second) {
                            AstCFunc* const activeTailFuncp
                                = tailNeedsNbaClone(tailFuncp, group.m_scopep)
                                      ? cloneTailFuncForNba(tailFuncp, group.m_scopep,
                                                            group.m_ownerp, group.m_senTreep)
                                      : tailFuncp;
                            activeTailFuncps.push_back(activeTailFuncp);
                            if (activeTailFuncp != tailFuncp) {
                                registerSubgraphCallUsageSummary(activeTailFuncp, group.m_scopep);
                            }
                        }
                        tailFuncps = &activeTailFuncps;
                    } else {
                        tailFuncps = &it->second;
                    }
                }
            }
            lowerActiveGroup(group.m_lateLogic, wrapper, false, tailFuncps);
        }
        if (wrapperActivep->stmtsp()) {
            group.m_ownerp->emplace_back(group.m_scopep, wrapperActivep);
        } else {
            wrapperActivep->deleteTree();
        }
    }

    for (SnapshotBucket& bucket : snapshotBuckets) {
        static unsigned s_snapshotHelperIndex = 0;
        AstScope* const topScopep = v3Global.rootp()->topScopep()->scopep();
        if (bucket.m_sourceVars.empty()) continue;
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
                    = getSnapshotRef(bucket.m_ownerp, bucket.m_senTreep, sourceVscp);
                AstVarScope* const outArgVscp = newSnapshotHelperArg(
                    funcp, sourceVscp->dtypep(), "out" + cvtToStr(i), VDirection::OUTPUT);
                AstVarScope* const inArgVscp = newSnapshotHelperArg(
                    funcp, sourceVscp->dtypep(), "in" + cvtToStr(i), VDirection::CONSTREF);
                funcp->addStmtsp(new AstAssign{flp, new AstVarRef{flp, outArgVscp, VAccess::WRITE},
                                               new AstVarRef{flp, inArgVscp, VAccess::READ}});
                callp->addArgsp(makeSnapshotExpr(snapshotRef, flp, VAccess::WRITE));
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
                = getSnapshotRef(bucket.m_ownerp, bucket.m_senTreep, sourceVscp);
            procp->addStmtsp(new AstAssign{
                sourceVscp->fileline(),
                makeSnapshotExpr(snapshotRef, sourceVscp->fileline(), VAccess::WRITE),
                new AstVarRef{sourceVscp->fileline(), sourceVscp, VAccess::READ}});
        }
        s_subgraphSnapshotProcedures.insert(procp);
        bucket.m_ownerp->add(topScopep, bucket.m_senTreep, procp);
    }
}

// Order the combinational logic to create the settle loop
AstCFunc* createSettle(AstNetlist* netlistp, AstCFunc* const initFuncp,
                       SenExprBuilder& senExprBulider, LogicClasses& logicClasses) {
    AstCFunc* const funcp = util::makeTopFunction(netlistp, "_eval_settle", true);

    // Clone, because ordering is destructive, but we still need them for "_eval"
    LogicByScope comb = logicClasses.m_comb.clone();
    LogicByScope hybrid = logicClasses.m_hybrid.clone();

    // Nothing to do if there is no logic.
    // While this is rare in real designs, it reduces noise in small tests.
    if (comb.empty() && hybrid.empty()) return nullptr;

    // We have an extra trigger denoting this is the first iteration of the settle loop
    TriggerKit::ExtraTriggers extraTriggers;
    const uint32_t firstIterationTrigger = extraTriggers.allocate("first iteration");

    // Gather the relevant sensitivity expressions and create the trigger kit
    const auto& senTreeps = getSenTreesUsedBy({&comb, &hybrid});
    const TriggerKit trigKit = TriggerKit::create(netlistp, initFuncp, senExprBulider, {},
                                                  senTreeps, "stl", extraTriggers, true, false);

    // Remap sensitivities (comb has none, so only do the hybrid)
    remapSensitivities(hybrid, trigKit.mapVec());

    // Create the inverse map from trigger ref AstSenTree to original AstSenTree
    V3Order::TrigToSenMap trigToSen;
    invertAndMergeSenTreeMap(trigToSen, trigKit.mapVec());

    // First trigger is for pure combinational triggers (first iteration)
    AstSenTree* const inputChanged
        = trigKit.newExtraTriggerSenTree(trigKit.vscp(), firstIterationTrigger);

    lowerSubgraphLogic(
        netlistp, {&comb, &hybrid}, trigToSen, "stl", true,
        [=](const AstVarScope*, std::vector<AstSenTree*>& out) { out.push_back(inputChanged); });

    // Create and the body function
    AstCFunc* const stlFuncp = V3Order::order(
        netlistp, {&comb, &hybrid}, trigToSen, "stl", false, true,
        [=](const AstVarScope*, std::vector<AstSenTree*>& out) { out.push_back(inputChanged); });
    util::splitCheck(stlFuncp);
    AstCFunc* const stlRefreshFuncp
        = v3Global.opt.subgraphSchedule()
              ? cloneUnguardedFuncBody(stlFuncp, netlistp->topScopep()->scopep(),
                                       "_refresh__subgraph", true)
              : nullptr;

    // Create the eval loop
    const EvalLoop stlLoop = createEvalLoop(  //
        netlistp, "stl", "Settle", /* slow: */ true, trigKit,
        // Use trigger
        trigKit.vscp(), nullptr,
        // Explicit condition
        // Inner loop statements
        nullptr,
        // Prep statements: Compute the current 'stl' triggers
        [&trigKit] {
            AstNodeStmt* const stmtp = trigKit.newCompBaseCall();
            if (stmtp) stmtp->addNext(trigKit.newDumpCall(trigKit.vscp(), trigKit.name(), true));
            return stmtp;
        }(),
        // Work statements: Invoke the 'stl' function
        util::callVoidFunc(stlFuncp));

    // Add the first iteration trigger to the trigger computation function
    trigKit.addExtraTriggerAssignment(stlLoop.firstIterp, firstIterationTrigger, false);

    // Add the eval loop to the top function
    funcp->addStmtsp(stlLoop.stmtsp);
    return stlRefreshFuncp;
}

//============================================================================
// Order the replicated combinational logic to create the 'ico' region

AstNode* createInputCombLoop(AstNetlist* netlistp, AstCFunc* const initFuncp,
                             SenExprBuilder& senExprBuilder, LogicByScope& logic,
                             const VirtIfaceTriggers& virtIfaceTriggers) {
    // Nothing to do if no combinational logic is sensitive to top level inputs
    if (logic.empty()) return nullptr;

    // SystemC only: Any top level inputs feeding a combinational logic must be marked,
    // so we can make them sc_sensitive
    if (v3Global.opt.systemC()) {
        logic.foreachLogic([](AstNode* logicp) {
            logicp->foreach([](AstVarRef* refp) {
                if (refp->access().isWriteOnly()) return;
                AstVarScope* const vscp = refp->varScopep();
                if (vscp->scopep()->isTop() && vscp->varp()->isNonOutput()) {
                    vscp->varp()->scSensitive(true);
                }
            });
        });
    }

    // We have some extra trigger denoting external conditions
    AstVarScope* const dpiExportTriggerVscp = netlistp->dpiExportTriggerp();

    TriggerKit::ExtraTriggers extraTriggers;
    const uint32_t firstIterationTrigger = extraTriggers.allocate("first iteration");
    const uint32_t dpiExportTriggerIndex = dpiExportTriggerVscp
                                               ? extraTriggers.allocate("DPI export trigger")
                                               : std::numeric_limits<uint32_t>::max();
    const uint32_t firstVifTriggerIndex = extraTriggers.size();
    for (const auto& entry : virtIfaceTriggers.m_triggers) {
        extraTriggers.allocate("virtual interface member: " + entry.m_ifacep->name() + "."
                               + entry.m_memberp->name());
    }

    // Gather the relevant sensitivity expressions and create the trigger kit
    const auto& senTreeps = getSenTreesUsedBy({&logic});
    const TriggerKit trigKit = TriggerKit::create(netlistp, initFuncp, senExprBuilder, {},
                                                  senTreeps, "ico", extraTriggers, false, false);
    std::ignore = senExprBuilder.getAndClearResults();

    if (dpiExportTriggerVscp) {
        trigKit.addExtraTriggerAssignment(dpiExportTriggerVscp, dpiExportTriggerIndex);
    }
    addVirtIfaceTriggerAssignments(netlistp, initFuncp, virtIfaceTriggers, firstVifTriggerIndex,
                                   trigKit);

    // Remap sensitivities
    remapSensitivities(logic, trigKit.mapVec());

    // Create the inverse map from trigger ref AstSenTree to original AstSenTree
    V3Order::TrigToSenMap trigToSen;
    invertAndMergeSenTreeMap(trigToSen, trigKit.mapVec());

    // The trigger top level inputs (first iteration)
    AstSenTree* const inputChanged
        = trigKit.newExtraTriggerSenTree(trigKit.vscp(), firstIterationTrigger);

    // The DPI Export trigger
    AstSenTree* const dpiExportTriggered
        = dpiExportTriggerVscp
              ? trigKit.newExtraTriggerSenTree(trigKit.vscp(), dpiExportTriggerIndex)
              : nullptr;
    const auto& vifVscpToSensIco
        = virtIfaceTriggers.makeVscpToSensMap(trigKit, firstVifTriggerIndex, trigKit.vscp());

    const V3Order::ExternalDomainsProvider icoExternalDomains
        = [&](const AstVarScope* vscp, std::vector<AstSenTree*>& out) {
              AstVar* const varp = vscp->varp();
              if (varp->isPrimaryInish() || varp->isSigUserRWPublic()) {
                  out.push_back(inputChanged);
              }
              if (varp->isWrittenByDpi()) out.push_back(dpiExportTriggered);
              if (vscp->varp()->sensIfacep() || vscp->varp()->isVirtIface()) {
                  const auto& ifaceTriggered
                      = findTriggeredIface(vscp, vifVscpToSensIco, virtIfaceTriggers);
                  out.insert(out.end(), ifaceTriggered.begin(), ifaceTriggered.end());
              }
          };

    lowerSubgraphLogic(netlistp, {&logic}, trigToSen, "ico", false, icoExternalDomains);

    // Create and Order the body function
    AstCFunc* const icoFuncp
        = V3Order::order(netlistp, {&logic}, trigToSen, "ico", false, false, icoExternalDomains);
    util::splitCheck(icoFuncp);

    // Create the eval loop
    const EvalLoop icoLoop = createEvalLoop(  //
        netlistp, "ico", "Input combinational", /* slow: */ false, trigKit,
        // Use trigger
        trigKit.vscp(), nullptr,
        // Inner loop statements
        nullptr,
        // Prep statements: Compute the current 'ico' triggers
        [&trigKit] {
            AstNodeStmt* const stmtp = trigKit.newCompBaseCall();
            if (stmtp) stmtp->addNext(trigKit.newDumpCall(trigKit.vscp(), trigKit.name(), true));
            return stmtp;
        }(),
        // Work statements: Invoke the 'ico' function
        util::callVoidFunc(icoFuncp));

    // Add the first iteration trigger to the trigger computation function
    trigKit.addExtraTriggerAssignment(icoLoop.firstIterp, firstIterationTrigger, false);

    return icoLoop.stmtsp;
}

//============================================================================
// EvalKit groups items that have to be passed to createEval() for a given eval region

struct EvalKit final {
    // The AstVarScope representing the region's trigger vector
    AstVarScope* const m_vscp = nullptr;
    // The AstCFunc that evaluates the region's logic
    AstCFunc* const m_funcp = nullptr;
    // Is this kit used/required?
    bool empty() const { return !m_funcp; }
};

//============================================================================
// Bolt together parts to create the top level _eval function

void createEval(AstNetlist* netlistp,  //
                AstNode* icoLoop,  //
                AstCFunc* settleRefreshFuncp,  //
                const TriggerKit& trigKit,  //
                const EvalKit& actKit,  //
                const EvalKit& nbaKit,  //
                const EvalKit& obsKit,  //
                const EvalKit& reactKit,  //
                AstCFunc* postponedFuncp,  //
                TimingKit& timingKit  //
) {
    FileLine* const flp = netlistp->fileline();

    // Grab the delay scheduler variable, if any
    AstVarScope* const delaySchedVscp = timingKit.getDelayScheduler(netlistp);

    // 'createResume' consumes the contents that 'createReady' needs, so do the right order
    AstCCall* const timingReadyp = timingKit.createReady(netlistp);
    AstCCall* const timingResumep = timingKit.createResume(netlistp);

    // Create the active eval loop
    EvalLoop topLoop = createEvalLoop(  //
        netlistp, "act", "Active", /* slow: */ false, trigKit,
        // Use trigger
        actKit.m_vscp, nullptr,
        // Inner loop statements
        nullptr,
        // Prep statements
        [&]() {
            // Compute the current 'act' triggers - the NBA triggers are the latched value
            AstNodeStmt* stmtsp = trigKit.newCompBaseCall();
            AstNodeStmt* const dumpp
                = stmtsp ? trigKit.newDumpCall(trigKit.vscp(), trigKit.name(), true) : nullptr;
            // Mark as ready for triggered awaits
            if (timingReadyp) stmtsp = AstNode::addNext(stmtsp, timingReadyp->makeStmt());
            if (AstVarScope* const vscAccp = trigKit.vscAccp()) {
                stmtsp = AstNode::addNext(stmtsp, trigKit.newOrIntoCall(actKit.m_vscp, vscAccp));
            }
            stmtsp = AstNode::addNext(stmtsp, trigKit.newCompExtCall(nbaKit.m_vscp));
            stmtsp = AstNode::addNext(stmtsp, dumpp);
            // Latch the 'act' triggers under the 'nba' triggers
            stmtsp = AstNode::addNext(stmtsp, trigKit.newOrIntoCall(nbaKit.m_vscp, actKit.m_vscp));
            //
            return stmtsp;
        }(),
        // Work statements
        [&]() {
            AstNodeStmt* workp = nullptr;
            if (AstVarScope* const actAccp = trigKit.vscAccp()) {
                AstCMethodHard* const cCallp = new AstCMethodHard{
                    flp, new AstVarRef{flp, actAccp, VAccess::WRITE}, VCMethod::UNPACKED_FILL,
                    new AstConst{flp, AstConst::Unsized64{}, 0}};
                cCallp->dtypeSetVoid();
                workp = AstNode::addNext(workp, cCallp->makeStmt());
            }
            // Resume triggered timing schedulers
            if (timingResumep) workp = AstNode::addNext(workp, timingResumep->makeStmt());
            // Invoke the 'act' function
            workp = AstNode::addNext(workp, util::callVoidFunc(actKit.m_funcp));
            //
            return workp;
        }());

    // Create if there are any delays, so we can check at runtime if a #0 is unexpected
    if (delaySchedVscp) {
        topLoop = createEvalLoop(  //
            netlistp, "inact", "Inactive", /* slow: */ false, trigKit,
            // Use explicit condition
            nullptr,
            [&]() {
                // Run if any zero delays are pending
                AstNodeExpr* const callp
                    = new AstCMethodHard{flp, new AstVarRef{flp, delaySchedVscp, VAccess::READ},
                                         VCMethod::SCHED_AWAITING_ZERO_DELAY};
                callp->dtypeSetBit();
                return callp;
            }(),
            // Inner loop statements
            topLoop.stmtsp,
            // Prep statements
            nullptr,
            // Work statements
            [&]() -> AstNodeStmt* {
                if (v3Global.usesZeroDelay()) {
                    // Resume processes watiting for #0 delay
                    AstCMethodHard* const callp = new AstCMethodHard{
                        flp, new AstVarRef{flp, delaySchedVscp, VAccess::READWRITE},
                        VCMethod::SCHED_RESUME_ZERO_DELAY};
                    callp->dtypeSetVoid();
                    return callp->makeStmt();
                } else {
                    // Assumption was that the design doesn't use #0 delays.
                    // Die at run-time if it does.
                    AstCStmt* const stmtp = new AstCStmt{flp};
                    const FileLine* const locp = netlistp->topModulep()->fileline();
                    const std::string& file = VIdProtect::protect(locp->filename());
                    const std::string& line = std::to_string(locp->lineno());
                    stmtp->add(
                        "VL_FATAL_MT(\"" + V3OutFormatter::quoteNameControls(file) + "\", " + line
                        + ", \"\", \"ZERODLY: Design Verilated with '--no-sched-zero-delay', "
                        + "but #0 delay executed at runtime\");");
                    return stmtp;
                }
            }());
    }

    // Create the NBA eval loop, which is the default top level loop.
    topLoop = createEvalLoop(  //
        netlistp, "nba", "NBA", /* slow: */ false, trigKit,
        // Use trigger
        nbaKit.m_vscp, nullptr,
        // Inner loop statements
        topLoop.stmtsp,
        // Prep statements
        nullptr,
        // Work statements
        [&]() {
            AstNodeStmt* workp = nullptr;
            // Latch the 'nba' trigger flags under the following region's trigger flags
            if (!obsKit.empty()) {
                workp = trigKit.newOrIntoCall(obsKit.m_vscp, nbaKit.m_vscp);
            } else if (!reactKit.empty()) {
                workp = trigKit.newOrIntoCall(reactKit.m_vscp, nbaKit.m_vscp);
            }
            // Invoke the 'nba' function
            workp = AstNode::addNext(workp, util::callVoidFunc(nbaKit.m_funcp));
            if (settleRefreshFuncp) {
                workp = AstNode::addNext(workp, util::callVoidFunc(settleRefreshFuncp));
            }
            // Clear the 'nba' triggers
            workp = AstNode::addNext(workp, trigKit.newClearCall(nbaKit.m_vscp));
            //
            return workp;
        }(),
        // Extra work (not conditional on having had a fired trigger)
        [&](AstVarScope* continuep) -> AstNodeStmt* {
            // Check if any dynamic NBAs are pending, if there are any in the design
            if (!netlistp->nbaEventp()) return nullptr;
            AstVarScope* const nbaEventp = netlistp->nbaEventp();
            AstVarScope* const nbaEventTriggerp = netlistp->nbaEventTriggerp();
            UASSERT(nbaEventTriggerp, "NBA event trigger var should exist");
            netlistp->nbaEventp(nullptr);
            netlistp->nbaEventTriggerp(nullptr);

            // If a dynamic NBA is pending, clear the pending flag and fire the ready event
            AstIf* const ifp = new AstIf{flp, new AstVarRef{flp, nbaEventTriggerp, VAccess::READ}};
            ifp->addThensp(util::setVar(continuep, 1));
            ifp->addThensp(util::setVar(nbaEventTriggerp, 0));
            AstCMethodHard* const firep = new AstCMethodHard{
                flp, new AstVarRef{flp, nbaEventp, VAccess::WRITE}, VCMethod::EVENT_FIRE};
            firep->dtypeSetVoid();
            ifp->addThensp(firep->makeStmt());
            return ifp;
        });

    if (!obsKit.empty()) {
        // Create the Observed eval loop, which becomes the top level loop.
        topLoop = createEvalLoop(  //
            netlistp, "obs", "Observed", /* slow: */ false, trigKit,
            // Use trigger
            obsKit.m_vscp, nullptr,
            // Inner loop statements
            topLoop.stmtsp,
            // Prep statements
            nullptr,
            // Work statements
            [&]() {
                AstNodeStmt* workp = nullptr;
                // Latch the Observed trigger flags under the Reactive trigger flags
                if (!reactKit.empty()) {
                    workp = trigKit.newOrIntoCall(reactKit.m_vscp, obsKit.m_vscp);
                }
                // Invoke the 'obs' function
                workp = AstNode::addNext(workp, util::callVoidFunc(obsKit.m_funcp));
                // Clear the 'obs' triggers
                workp = AstNode::addNext(workp, trigKit.newClearCall(obsKit.m_vscp));
                //
                return workp;
            }());
    }

    if (!reactKit.empty()) {
        // Create the Reactive eval loop, which becomes the top level loop.
        topLoop = createEvalLoop(  //
            netlistp, "react", "Reactive", /* slow: */ false, trigKit,
            // Use trigger
            reactKit.m_vscp, nullptr,
            // Inner loop statements
            topLoop.stmtsp,
            // Prep statements
            nullptr,
            // Work statements
            [&]() {
                // Invoke the 'react' function
                AstNodeStmt* workp = util::callVoidFunc(reactKit.m_funcp);
                // Clear the 'react' triggers
                workp = AstNode::addNext(workp, trigKit.newClearCall(reactKit.m_vscp));
                return workp;
            }());
    }

    // Now that we have build the loops, create the main 'eval' function
    AstCFunc* const funcp = util::makeTopFunction(netlistp, "_eval", false);
    netlistp->evalp(funcp);

    if (v3Global.opt.profExec()) funcp->addStmtsp(AstCStmt::profExecSectionPush(flp, "eval"));

    // Start with the ico loop, if any
    if (icoLoop) funcp->addStmtsp(icoLoop);

    // Execute the top level eval loop
    funcp->addStmtsp(topLoop.stmtsp);

    // Add the Postponed eval call
    if (postponedFuncp) funcp->addStmtsp(util::callVoidFunc(postponedFuncp));

    if (v3Global.opt.profExec()) funcp->addStmtsp(AstCStmt::profExecSectionPop(flp, "eval"));
}

}  // namespace

const std::vector<SubgraphCallUsageSummary>* getSubgraphCallUsageSummary(const AstCFunc* funcp) {
    const auto it = s_subgraphCallUsageSummaries.find(funcp);
    return it == s_subgraphCallUsageSummaries.end() ? nullptr : &it->second;
}

void clearSubgraphCallUsageSummaries() { s_subgraphCallUsageSummaries.clear(); }

bool isSubgraphSnapshotProcedure(const AstNodeProcedure* procp) {
    return s_subgraphSnapshotProcedures.count(procp);
}

//============================================================================
// Helper that builds virtual interface trigger sentrees

VirtIfaceTriggers::VscpSensMap VirtIfaceTriggers::makeVscpToSensMap(const TriggerKit& trigKit,
                                                                    uint32_t firstIndex,
                                                                    AstVarScope* trigVscp) const {
    VscpSensMap map;
    uint32_t index = firstIndex;
    for (const auto& entry : m_triggers) {
        map.emplace(entry.m_vscp, trigKit.newExtraTriggerSenTree(trigVscp, index));
        ++index;
    }
    return map;
}

std::unordered_map<const AstSenTree*, AstSenTree*>
cloneMapWithNewTriggerReferences(const std::unordered_map<const AstSenTree*, AstSenTree*>& map,
                                 AstVarScope* vscp) {
    AstTopScope* const topScopep = v3Global.rootp()->topScopep();
    // Label global SenTrees by the order they are in the Ast
    const VNUser1InUse user1InUse;
    int n = 0;
    for (AstNode* nodep = topScopep->senTreesp(); nodep; nodep = nodep->nextp()) nodep->user1(++n);
    // Sort map by key order for determinism
    using Pair = std::pair<const AstSenTree*, AstSenTree*>;
    std::vector<Pair> pairs{map.begin(), map.end()};
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {  //
        return a.first->user1() < b.first->user1();
    });
    // Replace references in each mapped value with a reference to the given vscp
    for (Pair& pair : pairs) {
        pair.second = pair.second->cloneTree(false);
        pair.second->foreach([&](AstVarRef* refp) {
            UASSERT_OBJ(refp->access() == VAccess::READ, refp, "Should be read ref");
            refp->replaceWith(new AstVarRef{refp->fileline(), vscp, VAccess::READ});
            VL_DO_DANGLING(refp->deleteTree(), refp);
        });
        topScopep->addSenTreesp(pair.second);
    }
    // Convert back to map
    return std::unordered_map<const AstSenTree*, AstSenTree*>{pairs.begin(), pairs.end()};
}

//============================================================================
// Top level entry-point to scheduling

void schedule(AstNetlist* netlistp) {
    clearSubgraphCallUsageSummaries();
    s_subgraphSnapshotProcedures.clear();
    const auto addSizeStat = [](const string& name, const LogicByScope& lbs) {
        uint64_t size = 0;
        lbs.foreachLogic([&](AstNode* nodep) { size += nodep->nodeCount(); });
        V3Stats::addStat("Scheduling, " + name, size);
    };

    // Step 0. Prepare external domains for timing and virtual interfaces
    // Create extra triggers for virtual interfaces
    const auto& virtIfaceTriggers = makeVirtIfaceTriggers(netlistp);
    // Prepare timing-related logic and external domains
    TimingKit timingKit = prepareTiming(netlistp);

    // Step 1. Gather and classify all logic in the design
    LogicClasses logicClasses = gatherLogicClasses(netlistp);

    if (v3Global.opt.stats()) {
        V3Stats::statsStage("sched-gather");
        addSizeStat("size of class: static", logicClasses.m_static);
        addSizeStat("size of class: initial", logicClasses.m_initial);
        addSizeStat("size of class: final", logicClasses.m_final);
    }

    // Step 2. Schedule static, initial and final logic classes in source order
    AstCFunc* const staticp = createStatic(netlistp, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-static");

    createInitial(netlistp, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-initial");

    createFinal(netlistp, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-final");

    // Step 3: Break combinational cycles by introducing hybrid logic
    // Note: breakCycles also removes corresponding logic from logicClasses.m_comb;
    logicClasses.m_hybrid = breakCycles(netlistp, logicClasses.m_comb);
    if (v3Global.opt.stats()) {
        addSizeStat("size of class: clocked", logicClasses.m_clocked);
        addSizeStat("size of class: combinational", logicClasses.m_comb);
        addSizeStat("size of class: hybrid", logicClasses.m_hybrid);
        V3Stats::statsStage("sched-break-cycles");
    }

    // We pass around a single SenExprBuilder instance, as we only need one set of 'prev' variables
    // for edge/change detection in sensitivity expressions, which this keeps track of.
    AstTopScope* const topScopep = netlistp->topScopep();
    AstScope* const scopeTopp = topScopep->scopep();
    SenExprBuilder senExprBuilder{scopeTopp};

    // Step 4: Create 'settle' region that restores the combinational invariant
    AstCFunc* const settleRefreshFuncp
        = createSettle(netlistp, staticp, senExprBuilder, logicClasses);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-settle");

    // Step 5: Partition the clocked and combinational (including hybrid) logic into pre/act/nba.
    // All clocks (signals referenced in an AstSenTree) generated via a blocking assignment
    // (including combinationally generated signals) are computed within the act region.
    LogicRegions logicRegions
        = partition(logicClasses.m_clocked, logicClasses.m_comb, logicClasses.m_hybrid);
    logicRegions.m_obs = logicClasses.m_observed;
    logicRegions.m_react = logicClasses.m_reactive;
    if (v3Global.opt.stats()) {
        addSizeStat("size of region: Active Pre", logicRegions.m_pre);
        addSizeStat("size of region: Active", logicRegions.m_act);
        addSizeStat("size of region: NBA", logicRegions.m_nba);
        addSizeStat("size of region: Observed", logicRegions.m_obs);
        addSizeStat("size of region: Reactive", logicRegions.m_react);
        V3Stats::statsStage("sched-partition");
    }

    // Step 6: Replicate combinational logic
    LogicReplicas logicReplicas = replicateLogic(logicRegions);
    if (v3Global.opt.stats()) {
        addSizeStat("size of replicated logic: Input", logicReplicas.m_ico);
        addSizeStat("size of replicated logic: Active", logicReplicas.m_act);
        addSizeStat("size of replicated logic: NBA", logicReplicas.m_nba);
        addSizeStat("size of replicated logic: Observed", logicReplicas.m_obs);
        addSizeStat("size of replicated logic: Reactive", logicReplicas.m_react);
        V3Stats::statsStage("sched-replicate");
    }

    // Step 7: Create input combinational logic loop
    AstNode* const icoLoopp = createInputCombLoop(netlistp, staticp, senExprBuilder,
                                                  logicReplicas.m_ico, virtIfaceTriggers);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-ico");

    // Step 8: Create the triggers
    AstVarScope* const dpiExportTriggerVscp = netlistp->dpiExportTriggerp();
    netlistp->dpiExportTriggerp(nullptr);  // Finished with this here

    // We may have an extra trigger for variable updated in DPI exports
    TriggerKit::ExtraTriggers extraTriggers;
    const uint32_t dpiExportTriggerIndex = dpiExportTriggerVscp
                                               ? extraTriggers.allocate("DPI export trigger")
                                               : std::numeric_limits<uint32_t>::max();
    const uint32_t firstVifTriggerIndex = extraTriggers.size();
    for (const auto& entry : virtIfaceTriggers.m_triggers) {
        extraTriggers.allocate("virtual interface member: " + entry.m_ifacep->name() + "."
                               + entry.m_memberp->name());
    }

    const auto& preTreeps = getSenTreesUsedBy({&logicRegions.m_pre});
    const auto& senTreeps = getSenTreesUsedBy({&logicRegions.m_act,  //
                                               &logicRegions.m_nba,  //
                                               &logicRegions.m_obs,  //
                                               &logicRegions.m_react,  //
                                               &timingKit.m_lbs});
    const TriggerKit trigKit
        = TriggerKit::create(netlistp, staticp, senExprBuilder, preTreeps, senTreeps, "act",
                             extraTriggers, false, v3Global.usesTiming());

    // Add post updates from the timing kit
    if (timingKit.m_postUpdates) trigKit.compBasep()->addStmtsp(timingKit.m_postUpdates);

    if (dpiExportTriggerVscp) {
        trigKit.addExtraTriggerAssignment(dpiExportTriggerVscp, dpiExportTriggerIndex);
    }
    addVirtIfaceTriggerAssignments(netlistp, staticp, virtIfaceTriggers, firstVifTriggerIndex,
                                   trigKit);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-triggers");

    // Note: Experiments so far show that running the Act (or Ico) regions on
    // multiple threads is always a net loss, so only use multi-threading for
    // NBA for now. This can be revised if evidence is available that it would
    // be beneficial

    // Step 9: Create the 'act' region evaluation function

    // Remap sensitivities of the input logic to the triggers
    remapSensitivities(logicRegions.m_pre, trigKit.mapPre());
    remapSensitivities(logicRegions.m_act, trigKit.mapVec());
    remapSensitivities(logicReplicas.m_act, trigKit.mapVec());
    remapSensitivities(timingKit.m_lbs, trigKit.mapVec());
    const std::map<const AstVarScope*, std::vector<AstSenTree*>> actTimingDomains
        = timingKit.remapDomains(trigKit.mapVec());

    // Create the inverse map from trigger ref AstSenTree to original AstSenTree
    V3Order::TrigToSenMap trigToSenAct;
    invertAndMergeSenTreeMap(trigToSenAct, trigKit.mapPre());
    invertAndMergeSenTreeMap(trigToSenAct, trigKit.mapVec());

    // The DPI Export trigger AstSenTree
    AstSenTree* const dpiExportTriggeredAct
        = dpiExportTriggerVscp
              ? trigKit.newExtraTriggerSenTree(trigKit.vscp(), dpiExportTriggerIndex)
              : nullptr;

    const auto& vifVscpToSensAct
        = virtIfaceTriggers.makeVscpToSensMap(trigKit, firstVifTriggerIndex, trigKit.vscp());

    const V3Order::ExternalDomainsProvider actExternalDomains
        = [&](const AstVarScope* vscp, std::vector<AstSenTree*>& out) {
              auto it = actTimingDomains.find(vscp);
              if (it != actTimingDomains.end()) out = it->second;
              if (vscp->varp()->isWrittenByDpi()) out.push_back(dpiExportTriggeredAct);
              if (vscp->varp()->sensIfacep() || vscp->varp()->isVirtIface()) {
                  const auto& ifaceTriggered
                      = findTriggeredIface(vscp, vifVscpToSensAct, virtIfaceTriggers);
                  out.insert(out.end(), ifaceTriggered.begin(), ifaceTriggered.end());
              }
          };

    lowerSubgraphLogic(netlistp, {&logicRegions.m_pre, &logicRegions.m_act, &logicReplicas.m_act},
                       trigToSenAct, "act", false, actExternalDomains);

    AstCFunc* const actFuncp = V3Order::order(
        netlistp, {&logicRegions.m_pre, &logicRegions.m_act, &logicReplicas.m_act}, trigToSenAct,
        "act", false, false, actExternalDomains);
    util::splitCheck(actFuncp);
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-act");

    const EvalKit actKit{trigKit.vscp(), actFuncp};

    // Orders a region's logic and creates the region eval function
    const auto order = [&](const std::string& name,
                           const std::vector<V3Sched::LogicByScope*>& logic) -> EvalKit {
        UINFO(2, "Scheduling " << name << " #logic = " << logic.size());
        AstVarScope* const trigVscp = trigKit.newTrigVec(name);
        const auto trigMap = cloneMapWithNewTriggerReferences(trigKit.mapVec(), trigVscp);
        // Remap sensitivities of the input logic to the triggers
        for (LogicByScope* lbs : logic) remapSensitivities(*lbs, trigMap);

        // Create the inverse map from trigger ref AstSenTree to original AstSenTree
        V3Order::TrigToSenMap trigToSen;
        invertAndMergeSenTreeMap(trigToSen, trigMap);

        AstSenTree* const dpiExportTriggered
            = dpiExportTriggerVscp
                  ? trigKit.newExtraTriggerSenTree(trigVscp, dpiExportTriggerIndex)
                  : nullptr;
        const auto& vifVscpToSens
            = virtIfaceTriggers.makeVscpToSensMap(trigKit, firstVifTriggerIndex, trigVscp);

        const auto& timingDomains = timingKit.remapDomains(trigMap);
        const V3Order::ExternalDomainsProvider externalDomains
            = [&](const AstVarScope* vscp, std::vector<AstSenTree*>& out) {
                  auto it = timingDomains.find(vscp);
                  if (it != timingDomains.end()) out = it->second;
                  if (vscp->varp()->isWrittenByDpi()) out.push_back(dpiExportTriggered);
                  if (vscp->varp()->sensIfacep() || vscp->varp()->isVirtIface()) {
                      const auto& ifaceTriggered
                          = findTriggeredIface(vscp, vifVscpToSens, virtIfaceTriggers);
                      out.insert(out.end(), ifaceTriggered.begin(), ifaceTriggered.end());
                  }
              };

        lowerSubgraphLogic(netlistp, logic, trigToSen, name, false, externalDomains);

        AstCFunc* const funcp
            = V3Order::order(netlistp, logic, trigToSen, name,
                             name == "nba" && v3Global.opt.mtasks(), false, externalDomains);

        return {trigVscp, funcp};
    };

    // Step 10: Create the 'nba' region evaluation function
    const EvalKit nbaKit = order("nba", {&logicRegions.m_nba, &logicReplicas.m_nba});
    util::splitCheck(nbaKit.m_funcp);
    netlistp->evalNbap(nbaKit.m_funcp);  // Remember for V3LifePost
    if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-nba");

    // Orders a region's logic and creates the region eval function (only if there is any logic in
    // the region)
    const auto orderIfNonEmpty
        = [&](const std::string& name, const std::vector<LogicByScope*>& logic) -> EvalKit {
        if (logic[0]->empty())
            return {};  // if region is empty, replica is supposed to be empty as well
        const auto& kit = order(name, logic);
        if (v3Global.opt.stats()) V3Stats::statsStage("sched-create-" + name);
        return kit;
    };

    // Step 11: Create the 'obs' region evaluation function
    const EvalKit obsKit = orderIfNonEmpty("obs", {&logicRegions.m_obs, &logicReplicas.m_obs});

    // Step 12: Create the 're' region evaluation function
    const EvalKit reactKit
        = orderIfNonEmpty("react", {&logicRegions.m_react, &logicReplicas.m_react});

    // Step 13: Create the 'postponed' region evaluation function
    auto* const postponedFuncp = createPostponed(netlistp, logicClasses);

    // Step 14: Bolt it all together to create the '_eval' function
    createEval(netlistp, icoLoopp, settleRefreshFuncp, trigKit, actKit, nbaKit, obsKit, reactKit,
               postponedFuncp, timingKit);

    // Step 15: Add neccessary evaluation before awaits
    if (AstCCall* const readyp = timingKit.createReady(netlistp)) {
        staticp->addStmtsp(readyp->makeStmt());
        beforeTrigVisitor(netlistp, senExprBuilder, trigKit);
    } else {
        // beforeTrigVisitor clears Sentree pointers in AstCAwaits (as these sentrees will get
        // deleted later) if there was no need to call it, SenTrees have to be cleaned manually
        netlistp->foreach([](AstCAwait* const cAwaitp) { cAwaitp->clearSentreep(); });
    }
    if (AstVarScope* const trigAccp = trigKit.vscAccp()) {
        // Copy trigger vector to accumulator at the end of static initialziation so,
        // triggers fired during initialization persist to the first resume.
        const AstUnpackArrayDType* const trigAccDTypep
            = VN_AS(trigAccp->dtypep(), UnpackArrayDType);
        UASSERT_OBJ(
            trigAccDTypep->right() == 0, trigAccp,
            "Expected that trigger vector and accumulator start elements enumeration from 0");
        UASSERT_OBJ(trigAccDTypep->left() >= 0, trigAccp,
                    "Expected that trigger vector and accumulator has no negative indexes");
        FileLine* const flp = trigAccp->fileline();
        AstVarScope* const vscp = netlistp->topScopep()->scopep()->createTemp("__Vi", 32);
        AstLoop* const loopp = new AstLoop{flp};
        loopp->addStmtsp(
            new AstAssign{flp,
                          new AstArraySel{flp, new AstVarRef{flp, trigAccp, VAccess::WRITE},
                                          new AstVarRef{flp, vscp, VAccess::READ}},
                          new AstArraySel{flp, new AstVarRef{flp, actKit.m_vscp, VAccess::READ},
                                          new AstVarRef{flp, vscp, VAccess::READ}}});
        loopp->addStmtsp(util::incrementVar(vscp));
        loopp->addStmtsp(new AstLoopTest{
            flp, loopp,
            new AstLte{flp, new AstVarRef{flp, vscp, VAccess::READ},
                       new AstConst{flp, AstConst::WidthedValue{}, 32,
                                    static_cast<uint32_t>(trigAccDTypep->left())}}});
        staticp->addStmtsp(loopp);
    }

    // Step 16: Clean up
    netlistp->clearStlFirstIterationp();

    // Haven't split static initializer yet
    util::splitCheck(staticp);

    // Dump
    V3Global::dumpCheckGlobalTree("sched", 0, dumpTreeEitherLevel() >= 3);
}

}  // namespace V3Sched
