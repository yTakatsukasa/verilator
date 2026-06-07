// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Replace return/continue with jumps
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
// V3LinkJump's Transformations:
//
// Each module:
//   Look for BEGINs
//      BEGIN(VAR...) -> VAR ... {renamed}
//   FOR -> WHILEs
//
//   Add JumpLabel which branches to after statements within JumpLabel
//      RETURN -> JUMPBLOCK(statements with RETURN changed to JUMPGO, ..., JUMPLABEL)
//      WHILE(... BREAK) -> JUMPBLOCK(WHILE(... statements with BREAK changed to JUMPGO),
//                                    ... JUMPLABEL)
//      WHILE(... CONTINUE) -> WHILE(JUMPBLOCK(... statements with CONTINUE changed to JUMPGO,
//                                    ... JUMPPABEL))
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3LinkJump.h"

#include "V3AstUserAllocator.h"
#include "V3Error.h"
#include "V3UniqueNames.h"

#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################

class LinkJumpVisitor final : public VNVisitor {
    // NODE STATE
    //  AstBegin/etc::user1()  -> AstJumpBlock*, for body of this loop
    //  AstFinish::user1()     -> bool, processed
    //  AstNode::user2()       -> AstJumpBlock*, for this block
    //  AstNodeBegin::user3()  -> bool, true if contains a fork
    const VNUser1InUse m_user1InUse;
    const VNUser2InUse m_user2InUse;
    const VNUser3InUse m_user3InUse;

    // STATE
    AstNodeModule* m_modp = nullptr;  // Current module
    AstNodeFTask* m_ftaskp = nullptr;  // Current function/task
    AstNode* m_loopp = nullptr;  // Current loop
    AstRandSequence* m_randsequencep = nullptr;  // Current randsequence
    bool m_loopInc = false;  // In loop increment
    bool m_inFork = false;  // Under fork
    int m_modRepeatNum = 0;  // Repeat counter
    VOptionBool m_unrollFull;  // Pragma full, disable, or default unrolling
    std::vector<AstNodeBlock*> m_blockStack;  // All begin blocks above current node
    V3UniqueNames m_queueNames{
        "__VprocessQueue"};  // Names for queues needed for 'disable' handling
    std::unordered_map<const AstTask*, AstVar*> m_taskDisableQueues;  // Per-task process queues
    std::unordered_map<const AstBegin*, AstVar*> m_beginDisableQueues;  // Per-begin process queues
    std::unordered_map<const AstTask*, AstBegin*>
        m_taskDisableBegins;  // Per-task process wrappers
    std::unordered_map<const AstBegin*, AstBegin*>
        m_beginDisableBegins;  // Per-begin process wrappers

    // METHODS
    // Get (and create if necessary) the JumpBlock for this statement
    AstJumpBlock* getJumpBlock(AstNode* nodep, bool endOfIter) {
        // Wrap 'nodep' in JumpBlock. If loop, wrap the body instead if endOfIter is true
        UINFO(4, "Create JumpBlock for " << nodep);

        // Made it previously?  We always jump to the end, so this works out
        if (endOfIter) {
            if (nodep->user1p()) return VN_AS(nodep->user1p(), JumpBlock);
        } else {
            if (nodep->user2p()) return VN_AS(nodep->user2p(), JumpBlock);
        }

        AstNode* underp = nullptr;
        bool under_and_next = true;
        if (AstBegin* const blockp = VN_CAST(nodep, Begin)) {
            UASSERT_OBJ(!endOfIter, nodep, "No endOfIter for Begin");
            underp = blockp->stmtsp();
        } else if (AstNodeFTask* const fTaskp = VN_CAST(nodep, NodeFTask)) {
            UASSERT_OBJ(!endOfIter, nodep, "No endOfIter for FTask");
            underp = fTaskp->stmtsp();
        } else if (AstForeach* const foreachp = VN_CAST(nodep, Foreach)) {
            if (endOfIter) {
                underp = foreachp->bodyp();
                // Keep a LoopTest **at the front** outside the jump block
                if (VN_IS(underp, LoopTest)) underp = underp->nextp();
            } else {
                underp = nodep;
                under_and_next = false;  // IE we skip the entire foreach
            }
        } else if (AstLoop* const loopp = VN_CAST(nodep, Loop)) {
            if (endOfIter) {
                underp = loopp->stmtsp();
            } else {
                underp = nodep;
                under_and_next = false;  // IE we skip the entire loop
            }
        } else {
            nodep->v3fatalSrc("Unknown jump point for break/disable/continue");
            return nullptr;
        }
        // Skip over variables as we'll just move them in a moment
        // Also this would otherwise prevent us from using a label twice
        // see t_func_return test.
        while (underp && VN_IS(underp, Var)) underp = underp->nextp();
        UASSERT_OBJ(underp, nodep, "Break/disable/continue not under expected statement");
        UINFO(5, "  Underpoint is " << underp);

        // If already wrapped, we are done ...
        if (!underp->nextp() || !under_and_next) {
            if (AstJumpBlock* const blockp = VN_CAST(underp, JumpBlock)) return blockp;
        }

        // Move underp stuff to be under a new AstJumpBlock
        VNRelinker repHandle;
        if (under_and_next) {
            underp->unlinkFrBackWithNext(&repHandle);
        } else {
            underp->unlinkFrBack(&repHandle);
        }
        AstJumpBlock* const blockp = new AstJumpBlock{nodep->fileline(), underp};
        if (endOfIter) {
            nodep->user1p(blockp);
        } else {
            nodep->user2p(blockp);
        }
        repHandle.relink(blockp);

        // Keep any AstVars under the function not under the new JumpLabel
        for (AstNode *nextp, *varp = underp; varp; varp = nextp) {
            nextp = varp->nextp();
            if (VN_IS(varp, Var)) blockp->addHereThisAsNext(varp->unlinkFrBack());
        }
        return blockp;
    }
    void addPrefixToBlocksRecurse(const std::string& prefix, AstNode* const nodep) {
        // Add a prefix to blocks
        // Used to not have blocks with duplicated names
        if (AstBegin* const beginp = VN_CAST(nodep, Begin)) {
            if (beginp->name() != "") beginp->name(prefix + beginp->name());
        }

        if (AstNode* const refp = nodep->op1p()) addPrefixToBlocksRecurse(prefix, refp);
        if (AstNode* const refp = nodep->op2p()) addPrefixToBlocksRecurse(prefix, refp);
        if (AstNode* const refp = nodep->op3p()) addPrefixToBlocksRecurse(prefix, refp);
        if (AstNode* const refp = nodep->op4p()) addPrefixToBlocksRecurse(prefix, refp);
        if (AstNode* const refp = nodep->nextp()) addPrefixToBlocksRecurse(prefix, refp);
    }
    bool existsBlockAbove(const std::string& name) const {
        for (const AstNodeBlock* const stackp : vlstd::reverse_view(m_blockStack)) {
            if (stackp->name() == name) return true;
        }
        return false;
    }
    static AstStmtExpr* getQueuePushProcessSelfp(AstVarRef* const queueRefp) {
        // Constructs queue.push_back(std::process::self()) statement
        FileLine* const flp = queueRefp->fileline();
        return new AstStmtExpr{
            flp,
            new AstMethodCall{flp, queueRefp, "push_back",
                              new AstArg{flp, "", v3Global.rootp()->stdPackageProcessSelfp(flp)}}};
    }
    static AstStmtExpr* getQueuePushProcessSelfp(FileLine* const fl, AstVar* const processQueuep) {
        AstPackage* const topPkgp = v3Global.rootp()->dollarUnitPkgAddp();
        AstVarRef* const queueWriteRefp
            = new AstVarRef{fl, topPkgp, processQueuep, VAccess::WRITE};
        return getQueuePushProcessSelfp(queueWriteRefp);
    }
    static AstStmtExpr* getQueueKillStmtp(FileLine* const fl, AstVar* const processQueuep) {
        AstPackage* const topPkgp = v3Global.rootp()->dollarUnitPkgAddp();
        AstVarRef* const queueRefp = new AstVarRef{fl, topPkgp, processQueuep, VAccess::READWRITE};
        AstTaskRef* killQueueCall = nullptr;
        for (AstNode* itemp = v3Global.rootp()->stdPackageProcessp()->stmtsp(); itemp;
             itemp = itemp->nextp()) {
            if (itemp->name() == "killQueue") {
                killQueueCall
                    = new AstTaskRef{fl, VN_AS(itemp, Task), new AstArg{fl, "", queueRefp}};
                break;
            }
        }
        UASSERT(killQueueCall, "Should be found");
        killQueueCall->classOrPackagep(v3Global.rootp()->stdPackageProcessp());
        return new AstStmtExpr{fl, killQueueCall};
    }
    static void prependStmtsp(AstNodeFTask* const nodep, AstNode* const stmtp) {
        if (AstNode* const origStmtsp = nodep->stmtsp()) {
            origStmtsp->unlinkFrBackWithNext();
            stmtp->addNext(origStmtsp);
        }
        nodep->addStmtsp(stmtp);
    }
    static void prependStmtsp(AstNodeBlock* const nodep, AstNode* const stmtp) {
        if (AstNode* const origStmtsp = nodep->stmtsp()) {
            origStmtsp->unlinkFrBackWithNext();
            stmtp->addNext(origStmtsp);
        }
        nodep->addStmtsp(stmtp);
    }
    static bool directlyUnderFork(const AstNode* const nodep) {
        if (nodep->backp()->nextp() == nodep) return directlyUnderFork(nodep->backp());
        return VN_IS(nodep->backp(), Fork);
    }
    AstBegin* getOrCreateTaskDisableBeginp(AstTask* const taskp, FileLine* const fl) {
        const auto it = m_taskDisableBegins.find(taskp);
        if (it != m_taskDisableBegins.end()) return it->second;

        AstBegin* const taskBodyp = new AstBegin{fl, "", nullptr, false};
        // Disable-by-name rewrites kill this detached task-body process, so mark it as process
        // backed to ensure fork/join kill-accounting hooks are always emitted.
        taskBodyp->setNeedProcess();
        if (taskp->stmtsp()) taskBodyp->addStmtsp(taskp->stmtsp()->unlinkFrBackWithNext());

        AstFork* const forkp = new AstFork{fl, VJoinType::JOIN};
        forkp->addForksp(taskBodyp);
        taskp->addStmtsp(forkp);

        m_taskDisableBegins.emplace(taskp, taskBodyp);
        return taskBodyp;
    }
    AstVar* getProcessQueuep(AstNode* const nodep, FileLine* const fl) {
        AstPackage* const topPkgp = v3Global.rootp()->dollarUnitPkgAddp();
        AstVar* const processQueuep = new AstVar{
            fl, VVarType::VAR, m_queueNames.get(nodep->name()), VFlagChildDType{},
            new AstQueueDType{
                fl, VFlagChildDType{},
                new AstClassRefDType{fl, v3Global.rootp()->stdPackageProcessp(), nullptr},
                nullptr}};
        processQueuep->lifetime(VLifetime::STATIC_EXPLICIT);
        processQueuep->processQueue(true);
        processQueuep->setIgnoreSchedWrite();
        topPkgp->addStmtsp(processQueuep);
        return processQueuep;
    }
    AstVar* getOrCreateTaskDisableQueuep(AstTask* const taskp, FileLine* const fl) {
        const auto it = m_taskDisableQueues.find(taskp);
        if (it != m_taskDisableQueues.end()) return it->second;

        AstVar* const processQueuep = getProcessQueuep(taskp, fl);
        AstStmtExpr* const pushCurrentProcessp = getQueuePushProcessSelfp(fl, processQueuep);
        AstBegin* const taskBodyp = getOrCreateTaskDisableBeginp(taskp, fl);
        prependStmtsp(taskBodyp, pushCurrentProcessp);
        m_taskDisableQueues.emplace(taskp, processQueuep);
        return processQueuep;
    }
    AstBegin* getOrCreateBeginDisableBeginp(AstBegin* const beginp, FileLine* const fl) {
        const auto it = m_beginDisableBegins.find(beginp);
        if (it != m_beginDisableBegins.end()) return it->second;

        AstBegin* const beginBodyp = new AstBegin{fl, "", nullptr, false};
        // Disable-by-name rewrites kill this detached block-body process, so mark it as process
        // backed to ensure fork/join kill-accounting hooks are always emitted.
        beginBodyp->setNeedProcess();
        if (beginp->stmtsp()) beginBodyp->addStmtsp(beginp->stmtsp()->unlinkFrBackWithNext());

        AstFork* const forkp = new AstFork{fl, VJoinType::JOIN};
        forkp->addForksp(beginBodyp);
        beginp->addStmtsp(forkp);

        m_beginDisableBegins.emplace(beginp, beginBodyp);
        return beginBodyp;
    }
    AstVar* getOrCreateBeginDisableQueuep(AstBegin* const beginp, FileLine* const fl) {
        const auto it = m_beginDisableQueues.find(beginp);
        if (it != m_beginDisableQueues.end()) return it->second;

        AstVar* const processQueuep = getProcessQueuep(beginp, fl);
        AstStmtExpr* const pushCurrentProcessp = getQueuePushProcessSelfp(fl, processQueuep);
        AstBegin* const beginBodyp = getOrCreateBeginDisableBeginp(beginp, fl);
        prependStmtsp(beginBodyp, pushCurrentProcessp);

        // Named-block disable must also terminate detached descendants created by forks
        // under the block, so track each fork branch process in the same queue.
        beginBodyp->foreach([&](AstFork* const forkp) {
            for (AstBegin* branchp = forkp->forksp(); branchp;
                 branchp = VN_AS(branchp->nextp(), Begin)) {
                AstStmtExpr* const pushBranchProcessp
                    = getQueuePushProcessSelfp(fl, processQueuep);
                prependStmtsp(branchp, pushBranchProcessp);
            }
        });
        m_beginDisableQueues.emplace(beginp, processQueuep);
        return processQueuep;
    }
    void handleDisableOnFork(AstDisable* const nodep, const std::vector<AstBegin*>& forks) {
        // The support utilizes the process::kill()` method. For each `disable` a queue of
        // processes is declared. At the beginning of each fork that can be disabled, its process
        // handle is pushed to the queue. `disable` statement is replaced with calling `kill()`
        // method on each element of the queue.
        FileLine* const fl = nodep->fileline();
        AstNode* const targetp = nodep->targetp();
        if (m_ftaskp) {
            if (!m_ftaskp->exists(
                    [targetp](const AstNodeBlock* blockp) -> bool { return blockp == targetp; })) {
                // Disabling a fork, which is within the same task, is not a problem
                nodep->v3warn(E_UNSUPPORTED, "Unsupported: disabling fork from task / function");
            }
        }

        AstPackage* const topPkgp = v3Global.rootp()->dollarUnitPkgAddp();
        AstVar* const processQueuep = getProcessQueuep(targetp, fl);
        AstVarRef* const queueWriteRefp
            = new AstVarRef{fl, topPkgp, processQueuep, VAccess::WRITE};
        AstStmtExpr* pushCurrentProcessp = getQueuePushProcessSelfp(queueWriteRefp);

        for (AstBegin* const beginp : forks) {
            if (pushCurrentProcessp->backp()) {
                pushCurrentProcessp = pushCurrentProcessp->cloneTree(false);
            }
            prependStmtsp(beginp, pushCurrentProcessp);
        }
        AstStmtExpr* const killStmtp = getQueueKillStmtp(fl, processQueuep);
        nodep->addNextHere(killStmtp);

        // 'process::kill' does not immediately kill the current process
        // executing the disable statement (because it's in the running state).
        // If the disable statement is indeed executed by a process under the
        // target AstFork, then jump to the end of that fork branch.
        if (VN_IS(targetp, Fork)) {
            AstNodeBlock* forkBranchp = nullptr;
            for (AstNodeBlock* const blockp : vlstd::reverse_view(m_blockStack)) {
                if (blockp == targetp) {
                    AstJumpBlock* const jmpBlockp = getJumpBlock(VN_AS(forkBranchp, Begin), false);
                    killStmtp->addNextHere(new AstJumpGo{fl, jmpBlockp});
                    break;
                }
                forkBranchp = blockp;
            }
        }
    }
    // VISITORS
    void visit(AstNodeModule* nodep) override {
        if (nodep->dead()) return;
        VL_RESTORER(m_modp);
        VL_RESTORER(m_modRepeatNum);
        m_modp = nodep;
        m_modRepeatNum = 0;
        iterateChildren(nodep);
    }
    void visit(AstNodeFTask* nodep) override {
        VL_RESTORER(m_ftaskp);
        m_ftaskp = nodep;
        iterateChildren(nodep);
    }
    void visit(AstBegin* nodep) override {
        UINFO(8, "  " << nodep);
        VL_RESTORER(m_unrollFull);
        m_blockStack.push_back(nodep);
        iterateChildren(nodep);
        m_blockStack.pop_back();
    }
    void visit(AstFork* nodep) override {
        UINFO(8, "  " << nodep);
        VL_RESTORER(m_unrollFull);
        VL_RESTORER(m_inFork);
        m_inFork = true;
        // Mark all upper blocks, can stop once see one set to avoid O(n^2)
        for (AstNodeBlock* const blockp : vlstd::reverse_view(m_blockStack)) {
            if (blockp->user3SetOnce()) break;
        }
        m_blockStack.push_back(nodep);
        iterateChildren(nodep);
        m_blockStack.pop_back();
    }
    void visit(AstStmtPragma* nodep) override {
        if (nodep->pragp()->pragType() == VPragmaType::UNROLL_DISABLE) {
            m_unrollFull = VOptionBool::OPT_FALSE;
            VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
        } else if (nodep->pragp()->pragType() == VPragmaType::UNROLL_FULL) {
            m_unrollFull = VOptionBool::OPT_TRUE;
            VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
        } else {
            iterateChildren(nodep);
        }
    }
    void visit(AstRandSequence* nodep) override {
        VL_RESTORER(m_randsequencep);
        m_randsequencep = nodep;
        iterateChildren(nodep);
    }
    void visit(AstRepeat* nodep) override {
        // So later optimizations don't need to deal with them,
        //    REPEAT(count,body) -> loop=count,WHILE(loop>0) { body, loop-- }
        // Note var can be signed or unsigned based on original number.
        AstNodeExpr* const countp = nodep->countp()->unlinkFrBackWithNext();
        const string name = "__Vrepeat"s + cvtToStr(m_modRepeatNum++);
        AstBegin* const beginp = new AstBegin{nodep->fileline(), "", nullptr, true};
        // Spec says value is integral, if negative is ignored
        AstVar* const varp
            = new AstVar{nodep->fileline(), VVarType::BLOCKTEMP, name, nodep->findIntDType()};
        varp->lifetime(VLifetime::AUTOMATIC_EXPLICIT);
        varp->usedLoopIdx(true);
        beginp->addStmtsp(varp);
        AstNode* initsp = new AstAssign{
            nodep->fileline(), new AstVarRef{nodep->fileline(), varp, VAccess::WRITE}, countp};
        AstNode* const decp = new AstAssign{
            nodep->fileline(), new AstVarRef{nodep->fileline(), varp, VAccess::WRITE},
            new AstSub{nodep->fileline(), new AstVarRef{nodep->fileline(), varp, VAccess::READ},
                       new AstConst{nodep->fileline(), 1}}};
        AstNodeExpr* const zerosp = new AstConst{nodep->fileline(), AstConst::Signed32{}, 0};
        AstNodeExpr* const condp = new AstGtS{
            nodep->fileline(), new AstVarRef{nodep->fileline(), varp, VAccess::READ}, zerosp};
        AstNode* const bodysp = nodep->stmtsp();
        if (bodysp) bodysp->unlinkFrBackWithNext();
        FileLine* const flp = nodep->fileline();
        AstLoop* const loopp = new AstLoop{flp};
        loopp->addStmtsp(new AstLoopTest{flp, loopp, condp});
        loopp->addStmtsp(bodysp);
        loopp->addContsp(decp);
        if (!m_unrollFull.isDefault()) loopp->unroll(m_unrollFull);
        m_unrollFull = VOptionBool::OPT_DEFAULT_FALSE;
        beginp->addStmtsp(initsp);
        beginp->addStmtsp(loopp);
        // Replacement AstBegin will be iterated next
        nodep->replaceWith(beginp);
        VL_DO_DANGLING(nodep->deleteTree(), nodep);
    }
    void visit(AstLoop* nodep) override {
        if (!m_unrollFull.isDefault()) nodep->unroll(m_unrollFull);
        if (m_modp->hasParameterList() || m_modp->hasGParam()) {
            nodep->fileline()->modifyWarnOff(V3ErrorCode::UNUSEDLOOP, true);
        }
        m_unrollFull = VOptionBool::OPT_DEFAULT_FALSE;
        VL_RESTORER(m_loopp);
        VL_RESTORER(m_loopInc);
        m_loopp = nodep;
        m_loopInc = false;
        iterateAndNextNull(nodep->stmtsp());
        m_loopInc = true;
        iterateAndNextNull(nodep->contsp());
        // Move contsp into stmtsp, no longer needed to keep separately
        if (nodep->contsp()) nodep->addStmtsp(nodep->contsp()->unlinkFrBackWithNext());
    }
    void visit(AstNodeForeach* nodep) override {
        VL_RESTORER(m_loopp);
        m_loopp = nodep;
        iterateAndNextNull(nodep->bodyp());
    }
    void visit(AstReturn* nodep) override {
        iterateChildren(nodep);
        const AstFunc* const funcp = VN_CAST(m_ftaskp, Func);
        if (m_randsequencep) {
            nodep->replaceWith(new AstRSReturn{nodep->fileline()});
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
            return;
        } else if (m_inFork) {
            nodep->v3error("Return isn't legal under fork (IEEE 1800-2023 9.2.3)");
            VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
            return;
        } else if (!m_ftaskp) {
            nodep->v3error("Return isn't underneath a task or function");
        } else if (funcp && !nodep->lhsp() && !funcp->isConstructor()) {
            nodep->v3error("Return underneath a function should have return value");
        } else if (!funcp && nodep->lhsp()) {
            nodep->v3error("Return underneath a task shouldn't have return value");
        } else {
            if (funcp && nodep->lhsp()) {
                // Set output variable to return value
                nodep->addHereThisAsNext(new AstAssign{
                    nodep->fileline(),
                    new AstVarRef{nodep->fileline(), VN_AS(funcp->fvarp(), Var), VAccess::WRITE},
                    nodep->lhsp()->unlinkFrBackWithNext()});
            }
            // Jump to the end of the function call
            AstJumpBlock* const blockp = getJumpBlock(m_ftaskp, false);
            nodep->addHereThisAsNext(new AstJumpGo{nodep->fileline(), blockp});
        }
        nodep->unlinkFrBack();
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void visit(AstBreak* nodep) override {
        iterateChildren(nodep);
        if (!m_loopp && m_randsequencep) {
            nodep->replaceWith(new AstRSBreak{nodep->fileline()});
            VL_DO_DANGLING(pushDeletep(nodep), nodep);
            return;
        } else if (!m_loopp) {
            nodep->v3error("break isn't underneath a loop");
        } else {
            // Jump to the end of the loop
            AstJumpBlock* const blockp = getJumpBlock(m_loopp, false);
            nodep->addNextHere(new AstJumpGo{nodep->fileline(), blockp});
        }
        nodep->unlinkFrBack();
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void visit(AstContinue* nodep) override {
        iterateChildren(nodep);
        if (!m_loopp) {
            nodep->v3error("continue isn't underneath a loop");
        } else {
            // Jump to the end of this iteration
            // If a "for" loop then need to still do the post-loop increment
            AstJumpBlock* const blockp = getJumpBlock(m_loopp, true);
            nodep->addNextHere(new AstJumpGo{nodep->fileline(), blockp});
        }
        nodep->unlinkFrBack();
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void visit(AstDisable* nodep) override {
        UINFO(8, "   DISABLE " << nodep);
        AstNode* const targetp = nodep->targetp();
        if (!targetp) {
            // Linking errors on the disable target are already reported upstream.
            // Drop this node to avoid cascading into an internal assertion.
            VL_DO_DANGLING(pushDeletep(nodep->unlinkFrBack()), nodep);
            return;
        }
        if (AstTask* const taskp = VN_CAST(targetp, Task)) {
            AstVar* const processQueuep = getOrCreateTaskDisableQueuep(taskp, nodep->fileline());
            AstStmtExpr* const killStmtp = getQueueKillStmtp(nodep->fileline(), processQueuep);
            nodep->addNextHere(killStmtp);

            // process::kill does not terminate the currently running process immediately.
            // If we disable the current task by name from inside itself, jump to its end.
            if (m_ftaskp == taskp) {
                AstNode* jumpTargetp = taskp;
                const auto it = m_taskDisableBegins.find(taskp);
                if (it != m_taskDisableBegins.end()) jumpTargetp = it->second;
                AstJumpBlock* const blockp = getJumpBlock(jumpTargetp, false);
                killStmtp->addNextHere(new AstJumpGo{nodep->fileline(), blockp});
            }
        } else if (AstFork* const forkp = VN_CAST(targetp, Fork)) {
            std::vector<AstBegin*> forks;
            for (AstBegin* itemp = forkp->forksp(); itemp; itemp = VN_AS(itemp->nextp(), Begin)) {
                forks.push_back(itemp);
            }
            handleDisableOnFork(nodep, forks);
        } else if (AstBegin* const beginp = VN_CAST(targetp, Begin)) {
            if (existsBlockAbove(beginp->name())) {
                if (!beginp->user3()) {
                    // Jump to the end of the named block
                    AstJumpBlock* const blockp = getJumpBlock(beginp, false);
                    nodep->addNextHere(new AstJumpGo{nodep->fileline(), blockp});
                } else {
                    AstVar* const processQueuep
                        = getOrCreateBeginDisableQueuep(beginp, nodep->fileline());
                    AstStmtExpr* const killStmtp
                        = getQueueKillStmtp(nodep->fileline(), processQueuep);
                    nodep->addNextHere(killStmtp);

                    // process::kill does not terminate the currently running process immediately.
                    // If disable executes inside a fork branch of this named block, jump to the
                    // end of that branch to prevent statements after disable from executing.
                    AstBegin* currentBeginp = nullptr;
                    for (AstNodeBlock* const blockp : vlstd::reverse_view(m_blockStack)) {
                        if (VN_IS(blockp, Begin)) {
                            currentBeginp = VN_AS(blockp, Begin);
                            break;
                        }
                    }
                    if (currentBeginp && directlyUnderFork(currentBeginp)) {
                        AstJumpBlock* const blockp = getJumpBlock(currentBeginp, false);
                        killStmtp->addNextHere(new AstJumpGo{nodep->fileline(), blockp});
                    }
                }
            } else {
                AstVar* const processQueuep
                    = getOrCreateBeginDisableQueuep(beginp, nodep->fileline());
                AstStmtExpr* const killStmtp = getQueueKillStmtp(nodep->fileline(), processQueuep);
                nodep->addNextHere(killStmtp);
            }
        } else {
            nodep->v3fatalSrc("Disable linked with node of unhandled type "
                              << targetp->prettyTypeName());
        }
        nodep->unlinkFrBack();
        VL_DO_DANGLING(pushDeletep(nodep), nodep);
    }
    void visit(AstFinish* nodep) override {
        if (nodep->user1SetOnce()) return;  // Process once
        iterateChildren(nodep);
        if (m_inFork) {
            nodep->replaceWith(new AstFinishFork{nodep->fileline()});
            VL_DO_DANGLING(nodep->deleteTree(), nodep);
        } else if (m_loopp) {
            // Jump to the end of the loop (post-finish)
            AstJumpBlock* const blockp = getJumpBlock(m_loopp, false);
            nodep->addNextHere(new AstJumpGo{nodep->fileline(), blockp});
        }
    }
    void visit(AstVarRef* nodep) override {
        if (m_loopInc && nodep->varp()) nodep->varp()->usedLoopIdx(true);
    }
    void visit(AstConst*) override {}
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    // CONSTRUCTORS
    explicit LinkJumpVisitor(AstNetlist* nodep) { iterate(nodep); }
    ~LinkJumpVisitor() override = default;
};

//######################################################################
// SubgraphConstraintVisitor

class SubgraphConstraintVisitor final {
    using ArgBindings = std::unordered_map<string, AstNodeExpr*>;
    using Assignments = std::vector<std::pair<string, AstNodeExpr*>>;
    using DrivenNames = std::set<string>;
    using FunctionNameSeen = std::set<std::pair<AstNodeFTask*, string>>;
    using SafeContextCache = std::unordered_map<string, std::unordered_set<string>>;
    using SafeInputNames = std::set<string>;
    using TraceSeen = std::set<std::pair<AstNodeModule*, string>>;

    class ModuleScanVisitor final : public VNVisitorConst {
        // STATE
        Assignments& m_assignments;
        std::vector<AstCell*>& m_cellps;
        std::unordered_set<string>& m_safeNames;
        std::vector<AstNodeFTaskRef*>& m_taskRefps;
        int m_ftaskDepth = 0;

        // METHODS
        void addAssignment(AstNodeExpr* lhsp, AstNodeExpr* rhsp) {
            const DrivenNames names = lhsNames(lhsp, true);
            for (const string& name : names) m_assignments.emplace_back(name, rhsp);
        }
        void addSafeLhs(AstNodeExpr* lhsp) {
            const DrivenNames names = lhsNames(lhsp, true);
            for (const string& name : names) m_safeNames.insert(name);
        }

        // VISITORS
        void visit(AstAssign* nodep) override { addAssignment(nodep->lhsp(), nodep->rhsp()); }
        void visit(AstAssignDly* nodep) override { addSafeLhs(nodep->lhsp()); }
        void visit(AstAssignW* nodep) override { addAssignment(nodep->lhsp(), nodep->rhsp()); }
        void visit(AstCell* nodep) override { m_cellps.push_back(nodep); }
        void visit(AstNodeFTask* nodep) override {
            ++m_ftaskDepth;
            iterateChildrenConst(nodep);
            --m_ftaskDepth;
        }
        void visit(AstNodeFTaskRef* nodep) override {
            if (m_ftaskDepth == 0 && VN_IS(nodep, TaskRef)) m_taskRefps.push_back(nodep);
            iterateChildrenConst(nodep);
        }
        void visit(AstVar* nodep) override {
            if (m_ftaskDepth == 0 && isCompileTimeConstVar(nodep)) {
                m_safeNames.insert(nodep->name());
            }
        }
        void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    public:
        ModuleScanVisitor(AstNodeModule* modp, Assignments& assignments,
                          std::vector<AstCell*>& cellps, std::unordered_set<string>& safeNames,
                          std::vector<AstNodeFTaskRef*>& taskRefps)
            : m_assignments{assignments}
            , m_cellps{cellps}
            , m_safeNames{safeNames}
            , m_taskRefps{taskRefps} {
            iterateAndNextConstNull(modp->inlinesp());
            iterateAndNextConstNull(modp->stmtsp());
        }
        ~ModuleScanVisitor() override = default;
    };

    struct ModuleInfo final {
        Assignments m_assignments;
        bool m_busy = false;
        std::vector<AstCell*> m_cellps;
        bool m_done = false;
        std::unordered_set<string> m_inputNames;
        std::unordered_set<string> m_safeNames;
        std::vector<AstNodeFTaskRef*> m_taskRefps;
    };

    static bool isCompileTimeConstVar(const AstVar* varp) {
        return varp && (varp->isParam() || varp->isGenVar());
    }

    class FunctionScanVisitor final : public VNVisitorConst {
        AstNodeFTask* const m_rootp;
        Assignments& m_assignments;
        std::vector<AstVar*>& m_formalp;
        std::unordered_set<string>& m_localNames;

        void addAssignment(AstNodeExpr* lhsp, AstNodeExpr* rhsp) {
            const DrivenNames names = lhsNames(lhsp, true);
            for (const string& name : names) m_assignments.emplace_back(name, rhsp);
        }

        void visit(AstAssign* nodep) override { addAssignment(nodep->lhsp(), nodep->rhsp()); }
        void visit(AstAssignW* nodep) override { addAssignment(nodep->lhsp(), nodep->rhsp()); }
        void visit(AstNodeFTask* nodep) override {
            if (nodep != m_rootp) return;
            iterateChildrenConst(nodep);
        }
        void visit(AstVar* nodep) override {
            if (!nodep->isFuncLocal() && !nodep->isFuncReturn()) return;
            m_localNames.insert(nodep->name());
            if (nodep->isIO() && !nodep->isFuncReturn()) m_formalp.push_back(nodep);
        }
        void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    public:
        FunctionScanVisitor(AstNodeFTask* rootp, Assignments& assignments,
                            std::vector<AstVar*>& formalp, std::unordered_set<string>& localNames)
            : m_rootp{rootp}
            , m_assignments{assignments}
            , m_formalp{formalp}
            , m_localNames{localNames} {
            iterateConst(rootp);
        }
        ~FunctionScanVisitor() override = default;
    };

    struct FunctionInfo final {
        Assignments m_assignments;
        bool m_done = false;
        std::vector<AstVar*> m_formalp;
        std::unordered_set<string> m_localNames;
    };

    class ExternalHierAccessVisitor final : public VNVisitor {
        SubgraphConstraintVisitor& m_parent;
        AstNodeModule* const m_boundaryModp;
        AstNodeModule* m_modp = nullptr;
        bool m_reported = false;

        static AstScope* boundaryScope(AstScope* scopep) {
            for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
                if (scanp->modp()->subgraphBoundary()) return scanp;
            }
            return nullptr;
        }
        static std::vector<string> dottedComponents(string dotted) {
            std::vector<string> components;
            size_t pos = 0;
            while (true) {
                const size_t dotPos = dotted.find("__DOT__", pos);
                const size_t plainPos = dotted.find('.', pos);
                size_t nextPos = string::npos;
                size_t step = 0;
                if (dotPos == string::npos) {
                    nextPos = plainPos;
                    step = 1;
                } else if (plainPos == string::npos || dotPos < plainPos) {
                    nextPos = dotPos;
                    step = 7;
                } else {
                    nextPos = plainPos;
                    step = 1;
                }
                components.push_back(dotted.substr(pos, nextPos - pos));
                if (nextPos == string::npos) break;
                pos = nextPos + step;
            }
            return components;
        }
        AstNodeModule* boundaryModuleFromDotted(const string& dotted) const {
            if (!m_modp) return nullptr;
            AstNodeModule* modp = m_modp;
            const std::vector<string> components = dottedComponents(dotted);
            for (const string& component : components) {
                if (component.empty()) continue;
                if (component == modp->name() || component == modp->prettyName()) continue;
                AstCell* cellp = nullptr;
                for (AstCell* const scanp : m_parent.info(modp).m_cellps) {
                    if (scanp->name() == component) {
                        cellp = scanp;
                        break;
                    }
                }
                if (!cellp) return nullptr;
                modp = cellp->modp();
                if (modp->subgraphBoundary()) return modp;
            }
            return nullptr;
        }

        void visit(AstNodeModule* nodep) override {
            if (m_reported) return;
            VL_RESTORER(m_modp);
            m_modp = nodep;
            iterateChildren(nodep);
        }
        void visit(AstVarXRef* nodep) override {
            if (m_reported || !m_modp) return;
            AstNodeModule* const boundaryModp = boundaryModuleFromDotted(nodep->dotted());
            if (boundaryModp != m_boundaryModp) return;
            nodep->v3error("Subgraph boundary module '"
                           << m_boundaryModp->prettyName() << "' variable '"
                           << nodep->varp()->prettyName()
                           << "' is accessed hierarchically from outside the subgraph");
            m_reported = true;
        }
        void visit(AstNodeVarRef* nodep) override {
            if (m_reported || !m_modp || !nodep->varScopep()) return;
            AstScope* const boundaryp = boundaryScope(nodep->varScopep()->scopep());
            if (!boundaryp || boundaryp->modp() != m_boundaryModp || m_modp == m_boundaryModp) {
                return;
            }
            nodep->v3error("Subgraph boundary module '"
                           << m_boundaryModp->prettyName() << "' variable '"
                           << nodep->varp()->prettyName()
                           << "' is accessed hierarchically from outside the subgraph");
            m_reported = true;
        }
        void visit(AstNode* nodep) override {
            if (m_reported) return;
            iterateChildren(nodep);
        }

    public:
        explicit ExternalHierAccessVisitor(SubgraphConstraintVisitor& parent, AstNetlist* rootp,
                                           AstNodeModule* boundaryModp)
            : m_parent{parent}
            , m_boundaryModp{boundaryModp} {
            iterate(rootp);
        }
        ~ExternalHierAccessVisitor() override = default;
    };

    AstNetlist* const m_rootp;
    std::unordered_map<AstNodeModule*, SafeContextCache> m_contextualSafeCache;
    std::unordered_map<AstNodeFTask*, FunctionInfo> m_functions;
    std::unordered_map<AstNodeModule*, ModuleInfo> m_infos;

    static string safeInputKey(const SafeInputNames& safeInputs) {
        string key;
        for (const string& name : safeInputs) key += name + '\n';
        return key;
    }

    static AstVar* formalByName(const FunctionInfo& finfo, const string& name) {
        for (AstVar* const formalp : finfo.m_formalp) {
            if (formalp->name() == name) return formalp;
        }
        return nullptr;
    }

    static bool formalUsesBinding(const FunctionInfo& finfo, const string& name) {
        AstVar* const formalp = formalByName(finfo, name);
        return formalp && formalp->isInput() && !formalp->isWritable();
    }

    const FunctionInfo& functionInfo(AstNodeFTask* taskp) {
        FunctionInfo& info = m_functions[taskp];
        if (info.m_done) return info;

        FunctionScanVisitor{taskp, info.m_assignments, info.m_formalp, info.m_localNames};
        info.m_done = true;
        return info;
    }

    static ArgBindings bindActualArgs(AstNodeFTaskRef* refp, const FunctionInfo& finfo) {
        ArgBindings bindings;
        size_t positional = 0;
        for (AstArg* argp = refp->argsp(); argp; argp = VN_AS(argp->nextp(), Arg)) {
            if (!argp->name().empty()) {
                bindings[argp->name()] = argp->exprp();
                continue;
            }
            while (positional < finfo.m_formalp.size()
                   && bindings.count(finfo.m_formalp[positional]->name())) {
                ++positional;
            }
            if (positional < finfo.m_formalp.size()) {
                bindings[finfo.m_formalp[positional]->name()] = argp->exprp();
                ++positional;
            }
        }
        for (AstVar* const formalp : finfo.m_formalp) {
            if (!formalp->isInput() || formalp->isWritable()) continue;
            if (bindings.count(formalp->name())) continue;
            if (AstNodeExpr* const defaultp = VN_CAST(formalp->valuep(), NodeExpr)) {
                bindings[formalp->name()] = defaultp;
            }
        }
        return bindings;
    }

    static void collectLhsNames(AstNode* nodep, bool requireWrite, DrivenNames& names) {
        if (AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            if (!requireWrite || refp->access().isWriteOrRW()) names.insert(refp->varp()->name());
            return;
        }
        if (AstArraySel* const selp = VN_CAST(nodep, ArraySel)) {
            collectLhsNames(selp->fromp(), requireWrite, names);
            return;
        }
        if (AstConcat* const concatp = VN_CAST(nodep, Concat)) {
            collectLhsNames(concatp->lhsp(), requireWrite, names);
            collectLhsNames(concatp->rhsp(), requireWrite, names);
            return;
        }
        if (AstConcatN* const concatp = VN_CAST(nodep, ConcatN)) {
            collectLhsNames(concatp->lhsp(), requireWrite, names);
            collectLhsNames(concatp->rhsp(), requireWrite, names);
            return;
        }
        if (AstMemberSel* const memberselp = VN_CAST(nodep, MemberSel)) {
            collectLhsNames(memberselp->fromp(), requireWrite, names);
            return;
        }
        if (AstReplicate* const replicatep = VN_CAST(nodep, Replicate)) {
            collectLhsNames(replicatep->srcp(), requireWrite, names);
            return;
        }
        if (AstSel* const selp = VN_CAST(nodep, Sel)) {
            collectLhsNames(selp->fromp(), requireWrite, names);
            return;
        }
        if (AstSelBit* const selbitp = VN_CAST(nodep, SelBit)) {
            collectLhsNames(selbitp->fromp(), requireWrite, names);
            return;
        }
        if (AstSelMinus* const selminusp = VN_CAST(nodep, SelMinus)) {
            collectLhsNames(selminusp->fromp(), requireWrite, names);
            return;
        }
        if (AstSelPlus* const selplusp = VN_CAST(nodep, SelPlus)) {
            collectLhsNames(selplusp->fromp(), requireWrite, names);
            return;
        }
    }

    static DrivenNames lhsNames(AstNodeExpr* nodep, bool requireWrite) {
        DrivenNames names;
        collectLhsNames(nodep, requireWrite, names);
        return names;
    }

    void collectTaskRefWrittenNames(AstNodeFTaskRef* refp, std::set<string>& names) {
        AstNodeFTask* const taskp = refp->taskp();
        if (!taskp) return;
        const FunctionInfo& finfo = functionInfo(taskp);
        const ArgBindings bindings = bindActualArgs(refp, finfo);
        for (AstVar* const formalp : finfo.m_formalp) {
            if (!formalp->isWritable()) continue;
            const auto it = bindings.find(formalp->name());
            if (it == bindings.end() || !it->second) continue;
            const DrivenNames written = lhsNames(it->second, false);
            names.insert(written.begin(), written.end());
        }
    }

    void collectModuleWrittenNames(AstNodeModule* modp, std::set<string>& names) {
        const ModuleInfo& inf = info(modp);
        for (const auto& pair : inf.m_assignments) names.insert(pair.first);
        for (AstNodeFTaskRef* const refp : inf.m_taskRefps)
            collectTaskRefWrittenNames(refp, names);
        for (AstCell* const cellp : inf.m_cellps) {
            for (AstPin* pinp = cellp->pinsp(); pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
                AstVar* const portp = pinp->modVarp();
                AstNodeExpr* const exprp = VN_CAST(pinp->exprp(), NodeExpr);
                if (!portp || !exprp || !portp->isWritable()) continue;
                const DrivenNames written = lhsNames(exprp, false);
                names.insert(written.begin(), written.end());
            }
        }
    }

    bool nameSafeWithCurrent(AstNodeModule* modp, const string& name,
                             const std::unordered_set<string>& safeNames) {
        if (safeNames.count(name)) return true;

        const ModuleInfo& inf = info(modp);
        bool anyWriter = false;

        for (const auto& pair : inf.m_assignments) {
            if (pair.first != name) continue;
            anyWriter = true;
            if (!exprSafe(modp, pair.second, safeNames)) return false;
        }

        for (AstNodeFTaskRef* const refp : inf.m_taskRefps) {
            AstNodeFTask* const taskp = refp->taskp();
            if (!taskp) continue;
            const FunctionInfo& finfo = functionInfo(taskp);
            const ArgBindings bindings = bindActualArgs(refp, finfo);
            for (AstVar* const formalp : finfo.m_formalp) {
                if (!formalp->isWritable()) continue;
                const auto itBinding = bindings.find(formalp->name());
                if (itBinding == bindings.end() || !itBinding->second) continue;
                if (!lhsNames(itBinding->second, false).count(name)) continue;
                anyWriter = true;
                FunctionNameSeen seen;
                if (!funcNameSafe(modp, taskp, formalp->name(), safeNames, &bindings, seen)) {
                    return false;
                }
            }
        }

        for (AstCell* const cellp : inf.m_cellps) {
            SafeInputNames childSafeInputs;
            for (AstPin* pinp = cellp->pinsp(); pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
                AstVar* const portp = pinp->modVarp();
                AstNodeExpr* const exprp = VN_CAST(pinp->exprp(), NodeExpr);
                if (!portp || !exprp || !portp->isInput()) continue;
                if (exprSafe(modp, exprp, safeNames)) childSafeInputs.insert(portp->name());
            }
            const std::unordered_set<string>& childSafe
                = contextualSafeNames(cellp->modp(), childSafeInputs);
            for (AstPin* pinp = cellp->pinsp(); pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
                AstVar* const portp = pinp->modVarp();
                AstNodeExpr* const exprp = VN_CAST(pinp->exprp(), NodeExpr);
                if (!portp || !exprp || !portp->isWritable()) continue;
                if (!lhsNames(exprp, false).count(name)) continue;
                anyWriter = true;
                if (!childSafe.count(portp->name())) return false;
            }
        }

        return anyWriter;
    }

    const std::unordered_set<string>& contextualSafeNames(AstNodeModule* modp,
                                                          const SafeInputNames& safeInputs) {
        SafeContextCache& byKey = m_contextualSafeCache[modp];
        const string key = safeInputKey(safeInputs);
        auto it = byKey.find(key);
        if (it != byKey.end()) return it->second;

        std::unordered_set<string>& safeNames = byKey[key];
        const ModuleInfo& inf = info(modp);
        safeNames = inf.m_safeNames;
        safeNames.insert(safeInputs.begin(), safeInputs.end());

        bool changed = true;
        while (changed) {
            changed = false;
            std::set<string> candidates;
            collectModuleWrittenNames(modp, candidates);
            for (const string& name : candidates) {
                if (safeNames.count(name)) continue;
                if (nameSafeWithCurrent(modp, name, safeNames)) {
                    safeNames.insert(name);
                    changed = true;
                }
            }
        }

        return safeNames;
    }

    bool exprSafe(AstNodeModule* modp, AstNodeExpr* nodep,
                  const std::unordered_set<string>& safeNames, AstNodeFTask* taskp = nullptr,
                  const ArgBindings* argBindingsp = nullptr, FunctionNameSeen* seenp = nullptr) {
        if (AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            if (!refp->access().isReadOrRW()) return true;
            if (taskp && (refp->varp()->isFuncLocal() || refp->varp()->isFuncReturn())) {
                FunctionNameSeen localSeen;
                return funcNameSafe(modp, taskp, refp->varp()->name(), safeNames, argBindingsp,
                                    seenp ? *seenp : localSeen);
            }
            return safeNames.count(refp->varp()->name());
        }

        bool safe = true;
        nodep->foreach([&](AstVarRef* refp) {
            if (!safe || !refp->access().isReadOrRW()) return;
            if (taskp && (refp->varp()->isFuncLocal() || refp->varp()->isFuncReturn())) {
                FunctionNameSeen localSeen;
                if (!funcNameSafe(modp, taskp, refp->varp()->name(), safeNames, argBindingsp,
                                  seenp ? *seenp : localSeen)) {
                    safe = false;
                }
            } else if (!safeNames.count(refp->varp()->name())) {
                safe = false;
            }
        });
        nodep->foreach([&](AstNodeFTaskRef* refp) {
            if (!safe) return;
            AstNodeFTask* const calledTaskp = refp->taskp();
            if (!calledTaskp || !calledTaskp->isFunction()) return;
            const FunctionInfo& finfo = functionInfo(calledTaskp);
            const ArgBindings bindings = bindActualArgs(refp, finfo);
            FunctionNameSeen localSeen;
            if (!funcNameSafe(modp, calledTaskp, calledTaskp->name(), safeNames, &bindings,
                              seenp ? *seenp : localSeen)) {
                safe = false;
            }
        });
        return safe;
    }

    void collectExprInputs(AstNodeModule* modp, AstNodeExpr* exprp, std::set<string>& inputs,
                           std::set<string>& nonstates, TraceSeen& seen,
                           AstNodeFTask* taskp = nullptr,
                           const ArgBindings* argBindingsp = nullptr,
                           FunctionNameSeen* funcSeenp = nullptr) {
        if (AstVarRef* const refp = VN_CAST(exprp, VarRef)) {
            if (!refp->access().isReadOrRW()) return;
            if (taskp && (refp->varp()->isFuncLocal() || refp->varp()->isFuncReturn())) {
                FunctionNameSeen localSeen;
                collectFuncNameInputs(modp, taskp, refp->varp()->name(), inputs, nonstates, seen,
                                      argBindingsp, funcSeenp ? *funcSeenp : localSeen);
            } else {
                collectNameInputs(modp, refp->varp()->name(), inputs, nonstates, seen);
            }
            return;
        }
        exprp->foreach([&](AstVarRef* refp) {
            if (!refp->access().isReadOrRW()) return;
            if (taskp && (refp->varp()->isFuncLocal() || refp->varp()->isFuncReturn())) {
                FunctionNameSeen localSeen;
                collectFuncNameInputs(modp, taskp, refp->varp()->name(), inputs, nonstates, seen,
                                      argBindingsp, funcSeenp ? *funcSeenp : localSeen);
            } else {
                collectNameInputs(modp, refp->varp()->name(), inputs, nonstates, seen);
            }
        });
        exprp->foreach([&](AstNodeFTaskRef* refp) {
            AstNodeFTask* const calledTaskp = refp->taskp();
            if (!calledTaskp || !calledTaskp->isFunction()) return;
            const FunctionInfo& finfo = functionInfo(calledTaskp);
            const ArgBindings bindings = bindActualArgs(refp, finfo);
            FunctionNameSeen localSeen;
            collectFuncNameInputs(modp, calledTaskp, calledTaskp->name(), inputs, nonstates, seen,
                                  &bindings, funcSeenp ? *funcSeenp : localSeen);
        });
    }

    bool funcNameSafe(AstNodeModule* modp, AstNodeFTask* taskp, const string& name,
                      const std::unordered_set<string>& safeNames, const ArgBindings* argBindingsp,
                      FunctionNameSeen& seen) {
        if (!seen.emplace(taskp, name).second) return true;

        if (argBindingsp) {
            const FunctionInfo& finfo = functionInfo(taskp);
            if (formalUsesBinding(finfo, name)) {
                const auto argIt = argBindingsp->find(name);
                if (argIt != argBindingsp->end() && argIt->second) {
                    return exprSafe(modp, argIt->second, safeNames, taskp, argBindingsp, &seen);
                }
            }
        }

        const FunctionInfo& finfo = functionInfo(taskp);
        if (!finfo.m_localNames.count(name)) return safeNames.count(name);

        bool anyAssign = false;
        for (const auto& pair : finfo.m_assignments) {
            if (pair.first != name) continue;
            anyAssign = true;
            if (!exprSafe(modp, pair.second, safeNames, taskp, argBindingsp, &seen)) return false;
        }
        return anyAssign;
    }

    void collectFuncNameInputs(AstNodeModule* modp, AstNodeFTask* taskp, const string& name,
                               std::set<string>& inputs, std::set<string>& nonstates,
                               TraceSeen& seen, const ArgBindings* argBindingsp,
                               FunctionNameSeen& funcSeen) {
        if (!funcSeen.emplace(taskp, name).second) return;

        if (argBindingsp) {
            const auto argIt = argBindingsp->find(name);
            const FunctionInfo& finfo = functionInfo(taskp);
            if (formalUsesBinding(finfo, name) && argIt != argBindingsp->end() && argIt->second) {
                collectExprInputs(modp, argIt->second, inputs, nonstates, seen, taskp,
                                  argBindingsp, &funcSeen);
                return;
            }
        }

        const FunctionInfo& finfo = functionInfo(taskp);
        if (!finfo.m_localNames.count(name)) {
            collectNameInputs(modp, name, inputs, nonstates, seen);
            return;
        }

        for (const auto& pair : finfo.m_assignments) {
            if (pair.first != name) continue;
            collectExprInputs(modp, pair.second, inputs, nonstates, seen, taskp, argBindingsp,
                              &funcSeen);
        }
    }

    void collectNameInputs(AstNodeModule* modp, const string& name, std::set<string>& inputs,
                           std::set<string>& nonstates, TraceSeen& seen) {
        if (!seen.emplace(modp, name).second) return;

        const ModuleInfo& inf = info(modp);
        if (inf.m_inputNames.count(name)) {
            inputs.insert(name);
            return;
        }
        if (inf.m_safeNames.count(name)) return;

        const size_t inputsBefore = inputs.size();
        const size_t nonstatesBefore = nonstates.size();

        for (const auto& pair : inf.m_assignments) {
            if (pair.first == name) collectExprInputs(modp, pair.second, inputs, nonstates, seen);
        }

        for (AstNodeFTaskRef* const refp : inf.m_taskRefps) {
            AstNodeFTask* const taskp = refp->taskp();
            if (!taskp) continue;
            const FunctionInfo& finfo = functionInfo(taskp);
            const ArgBindings bindings = bindActualArgs(refp, finfo);
            for (AstVar* const formalp : finfo.m_formalp) {
                if (!formalp->isWritable()) continue;
                const auto itBinding = bindings.find(formalp->name());
                if (itBinding == bindings.end() || !itBinding->second) continue;
                if (!lhsNames(itBinding->second, false).count(name)) continue;
                FunctionNameSeen funcSeen;
                collectFuncNameInputs(modp, taskp, formalp->name(), inputs, nonstates, seen,
                                      &bindings, funcSeen);
            }
        }

        for (AstCell* const cellp : inf.m_cellps) {
            for (AstPin* pinp = cellp->pinsp(); pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
                AstVar* const portp = pinp->modVarp();
                AstNodeExpr* const exprp = VN_CAST(pinp->exprp(), NodeExpr);
                if (!portp || !exprp || !portp->isWritable()) continue;
                if (!lhsNames(exprp, false).count(name)) continue;

                std::set<string> childInputs;
                std::set<string> childNonstates;
                TraceSeen childSeen;
                collectNameInputs(cellp->modp(), portp->name(), childInputs, childNonstates,
                                  childSeen);
                for (const string& childInput : childInputs) {
                    for (AstPin* inputPinp = cellp->pinsp(); inputPinp;
                         inputPinp = VN_AS(inputPinp->nextp(), Pin)) {
                        AstVar* const inputPortp = inputPinp->modVarp();
                        AstNodeExpr* const inputExprp = VN_CAST(inputPinp->exprp(), NodeExpr);
                        if (inputPortp && inputExprp && inputPortp->name() == childInput) {
                            collectExprInputs(modp, inputExprp, inputs, nonstates, seen);
                        }
                    }
                }
                for (const string& childNonstate : childNonstates) {
                    nonstates.insert(cellp->name() + "." + childNonstate);
                }
            }
        }

        if (inputs.size() == inputsBefore && nonstates.size() == nonstatesBefore) {
            nonstates.insert(name);
        }
    }

    string feedthroughSourceText(AstNodeModule* modp, const string& name) {
        std::set<string> inputs;
        std::set<string> nonstates;
        TraceSeen seen;
        collectNameInputs(modp, name, inputs, nonstates, seen);
        if (inputs.empty() && nonstates.empty()) return "non-state logic";

        std::ostringstream os;
        if (!inputs.empty()) {
            os << "input port(s): ";
            const char* sep = "";
            for (const string& input : inputs) {
                os << sep << "'" << input << "'";
                sep = ", ";
            }
            if (nonstates.empty()) return os.str();
            os << " (non-state node(s): ";
            const char* nodeSep = "";
            for (const string& nonstate : nonstates) {
                os << nodeSep << "'" << nonstate << "'";
                nodeSep = ", ";
            }
            os << ")";
            return os.str();
        }

        os << "non-state node(s): ";
        const char* sep = "";
        for (const string& nonstate : nonstates) {
            os << sep << "'" << nonstate << "'";
            sep = ", ";
        }
        return os.str();
    }

    void checkNested(AstNodeModule* ownerp, AstNodeModule* modp,
                     std::unordered_set<AstNodeModule*>& seen) {
        if (!seen.insert(modp).second) return;
        const ModuleInfo& inf = info(modp);
        for (AstCell* const cellp : inf.m_cellps) {
            AstNodeModule* const childp = cellp->modp();
            if (childp->subgraphBoundary()) {
                cellp->v3error("Subgraph boundary module '"
                               << ownerp->prettyName()
                               << "' instantiates subgraph boundary module '"
                               << childp->prettyName() << "'");
            } else {
                checkNested(ownerp, childp, seen);
            }
        }
    }

    void checkDpiUsage(AstNodeModule* modp) {
        bool reported = false;
        modp->foreach([&](AstNodeFTask* nodep) {
            if (reported) return;
            if (!nodep->dpiImport() && !nodep->dpiExport()) return;
            nodep->v3error("Subgraph boundary module '" << modp->prettyName()
                                                        << "' uses DPI-C function/task '"
                                                        << nodep->prettyName() << "'");
            reported = true;
        });
        modp->foreach([&](AstNodeFTaskRef* nodep) {
            if (reported) return;
            AstNodeFTask* const taskp = nodep->taskp();
            if (!taskp || (!taskp->dpiImport() && !taskp->dpiExport())) return;
            nodep->v3error("Subgraph boundary module '" << modp->prettyName()
                                                        << "' uses DPI-C function/task '"
                                                        << taskp->prettyName() << "'");
            reported = true;
        });
    }

    void checkExternalHierarchicalAccess(AstNodeModule* modp) {
        ExternalHierAccessVisitor{*this, m_rootp, modp};
    }

    void checkTimingUsage(AstNodeModule* modp) {
        if (!v3Global.opt.timing()) return;

        bool reported = false;
        modp->foreach([&](AstNodeAssign* nodep) {
            if (reported || !nodep->timingControlp()) return;
            nodep->timingControlp()->v3error("Subgraph boundary module '"
                                             << modp->prettyName()
                                             << "' uses timing control or dynamic event logic");
            reported = true;
        });
        modp->foreach([&](AstNode* nodep) {
            if (reported) return;
            if (!VN_IS(nodep, Delay) && !VN_IS(nodep, EventControl) && !VN_IS(nodep, FireEvent)
                && !VN_IS(nodep, Wait) && !VN_IS(nodep, WaitFork) && !VN_IS(nodep, CAwait)) {
                return;
            }
            nodep->v3error("Subgraph boundary module '"
                           << modp->prettyName()
                           << "' uses timing control or dynamic event logic");
            reported = true;
        });
    }

    void checkVpiUsage(AstNodeModule* modp) {
        if (!v3Global.opt.vpi()) return;
        modp->v3error("Subgraph boundary module '" << modp->prettyName()
                                                   << "' is unsupported with --vpi");
    }

    const ModuleInfo& info(AstNodeModule* modp) {
        ModuleInfo& inf = m_infos[modp];
        if (inf.m_done || inf.m_busy) return inf;

        inf.m_busy = true;
        ModuleScanVisitor{modp, inf.m_assignments, inf.m_cellps, inf.m_safeNames, inf.m_taskRefps};
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstVar* const varp = VN_CAST(stmtp, Var);
            if (varp && varp->isInput()) inf.m_inputNames.insert(varp->name());
            if (isCompileTimeConstVar(varp)) inf.m_safeNames.insert(varp->name());
        }

        bool changed = true;
        while (changed) {
            changed = false;
            std::set<string> candidates;
            collectModuleWrittenNames(modp, candidates);
            for (const string& name : candidates) {
                if (inf.m_safeNames.count(name)) continue;
                if (nameSafeWithCurrent(modp, name, inf.m_safeNames)) {
                    inf.m_safeNames.insert(name);
                    changed = true;
                }
            }
        }

        inf.m_busy = false;
        inf.m_done = true;
        return inf;
    }

    void validate(AstNodeModule* modp) {
        if (!modp->subgraphBoundary()) return;

        checkDpiUsage(modp);
        checkExternalHierarchicalAccess(modp);
        std::unordered_set<AstNodeModule*> seen;
        checkNested(modp, modp, seen);
        checkTimingUsage(modp);
        checkVpiUsage(modp);

        const ModuleInfo& inf = info(modp);
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstVar* const varp = VN_CAST(stmtp, Var);
            if (!varp || !varp->isIO() || !varp->isWritable()) continue;
            if (inf.m_safeNames.count(varp->name())) continue;
            const string sourceText = feedthroughSourceText(modp, varp->name());
            varp->v3error("Subgraph boundary module '"
                          << modp->prettyName() << "' output '" << varp->prettyName()
                          << "' has a feedthrough path from " << sourceText);
        }
    }

public:
    explicit SubgraphConstraintVisitor(AstNetlist* rootp)
        : m_rootp{rootp} {
        for (AstNodeModule* modp = m_rootp->modulesp(); modp;
             modp = VN_AS(modp->nextp(), NodeModule)) {
            info(modp);
        }
        for (AstNodeModule* modp = m_rootp->modulesp(); modp;
             modp = VN_AS(modp->nextp(), NodeModule)) {
            validate(modp);
        }
    }
};

//######################################################################
// Task class functions

void V3LinkJump::linkJump(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    LinkJumpVisitor{nodep};
    V3Global::dumpCheckGlobalTree("linkjump", 0, dumpTreeEitherLevel() >= 3);
}

void V3LinkJump::checkSubgraphs(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    if (v3Global.opt.subgraphSchedule()) SubgraphConstraintVisitor{nodep};
    V3Global::dumpCheckGlobalTree("subgraphcheck", 0, dumpTreeEitherLevel() >= 3);
}
