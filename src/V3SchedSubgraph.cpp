// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Experimental subgraph scheduling helpers
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
// This pass is the first scheduling experiment for subgraph boundaries. It
// removes eligible per-instance logic before the parent scheduling analyses,
// schedules each child independently, and exposes boundary writes to parent
// replication through an output-port contract. Child logic is materialized as
// callable helpers before parent Order. Ineligible boundaries use the late
// lowering path and materialize receiver logic before parent analysis.
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SchedSubgraph.h"

#include "V3Stats.h"

#include <map>
#include <unordered_map>
#include <unordered_set>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace V3Sched {

namespace {

bool isUnderScope(const AstScope* scopep, const AstScope* basep) {
    for (const AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
        if (scanp == basep) return true;
    }
    return false;
}

struct SubgraphGroup final {
    struct Capture final {
        AstVarScope* m_sourcep = nullptr;
        AstVarScope* m_savedp = nullptr;
    };
    AstScope* m_boundaryScopep = nullptr;
    AstSenTree* m_senTreep = nullptr;
    FileLine* m_filelinep = nullptr;
    LogicByScope* m_ownerp = nullptr;
    LogicByScope m_preLogic;
    LogicByScope m_postLogic;
    std::vector<Capture> m_captures;
    std::unordered_set<const AstVar*> m_delayedVars;
};

struct CaptureVars final {
    std::map<AstNodeModule*, std::map<AstVar*, std::vector<AstVar*>>> m_slots;
    std::map<AstNodeModule*, size_t> m_nextNameIndex;
};

AstScope* findBoundaryScope(AstScope* scopep) {
    for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
        if (scanp->modp()->subgraphBoundary()) return scanp;
    }
    return nullptr;
}

AstVarScope* findVarScope(AstScope* scopep, const AstVar* varp) {
    for (AstVarScope* vscp = scopep->varsp(); vscp; vscp = VN_AS(vscp->nextp(), VarScope)) {
        if (vscp->varp() == varp) return vscp;
    }
    return nullptr;
}

bool isPublishStatement(const AstNode* stmtp) {
    const AstAlways* const alwaysp = VN_CAST(stmtp, Always);
    if (!alwaysp) return false;
    const AstAssignW* const assp = VN_CAST(alwaysp->stmtsp(), AssignW);
    if (!assp || assp->nextp()) return false;
    const AstVarRef* const lhsp = VN_CAST(assp->lhsp(), VarRef);
    return lhsp && lhsp->varp()->subgraphPublished();
}

bool isPublishActive(const AstActive* activep) {
    return activep->sentreep()->hasCombo() && activep->stmtsp() && !activep->stmtsp()->nextp()
           && isPublishStatement(activep->stmtsp());
}

// Port connections execute in the parent schedule even when Inst places them in the child
// scope. Their writes establish the input values observed by the local scheduler.
bool isBoundaryInputStatement(const AstScope* boundaryScopep, const AstNode* stmtp) {
    const AstAlways* const alwaysp = VN_CAST(stmtp, Always);
    if (!alwaysp) return false;
    const AstAssignW* const assp = VN_CAST(alwaysp->stmtsp(), AssignW);
    if (!assp || assp->nextp()) return false;
    const AstVarRef* const lhsp = VN_CAST(assp->lhsp(), VarRef);
    if (!lhsp || lhsp->varScopep()->scopep() != boundaryScopep || !lhsp->varp()->isNonOutput()
        || !lhsp->varp()->subgraphPortId()) {
        return false;
    }
    bool externalInputs = true;
    assp->rhsp()->foreach([&](const AstNodeVarRef* refp) {
        if (isUnderScope(refp->varScopep()->scopep(), boundaryScopep)) externalInputs = false;
    });
    return externalInputs;
}

bool isBoundaryInputActive(const AstScope* boundaryScopep, const AstActive* activep) {
    return activep->sentreep()->hasCombo() && activep->stmtsp() && !activep->stmtsp()->nextp()
           && isBoundaryInputStatement(boundaryScopep, activep->stmtsp());
}

void splitBoundaryCombinationalLogic(AstNetlist* netlistp) {
    std::vector<std::pair<AstScope*, AstActive*>> actives;
    netlistp->foreach([&](AstScope* scopep) {
        AstScope* const boundaryScopep = findBoundaryScope(scopep);
        if (!boundaryScopep) return;
        scopep->foreach([&](AstActive* activep) {
            if (activep->sentreep()->hasCombo()) actives.emplace_back(scopep, activep);
        });
    });
    for (const auto& pair : actives) {
        AstScope* const scopep = pair.first;
        AstActive* const activep = pair.second;
        AstScope* const boundaryScopep = findBoundaryScope(scopep);
        if (!activep->stmtsp() || !activep->stmtsp()->nextp()) continue;
        std::vector<AstNode*> boundaryStatements;
        for (AstNode* stmtp = activep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (isPublishStatement(stmtp) || isBoundaryInputStatement(boundaryScopep, stmtp)) {
                boundaryStatements.push_back(stmtp);
            }
        }
        for (AstNode* const stmtp : boundaryStatements) {
            AstActive* const separatep
                = new AstActive{stmtp->fileline(), "subgraph-boundary", activep->sentreep()};
            separatep->addStmtsp(stmtp->unlinkFrBack());
            scopep->addBlocksp(separatep);
        }
        if (!activep->stmtsp()) activep->unlinkFrBack()->deleteTree();
    }
}

using SharedReceivers = std::unordered_map<AstScope*, std::vector<AstScope*>>;

SharedReceivers prepareSharedReceiverState(AstNetlist* netlistp) {
    SharedReceivers receivers;
    netlistp->foreach([&](AstScope* scopep) {
        if (AstScope* const implementationp = scopep->subgraphImplementationScopep()) {
            receivers[implementationp].push_back(scopep);
        }
    });
    uint64_t lateVarScopes = 0;
    for (const auto& entry : receivers) {
        AstScope* const implementationp = entry.first;
        for (AstScope* const receiverp : entry.second) {
            UASSERT_OBJ(receiverp->modp() == implementationp->modp(), receiverp,
                        "Subgraph receiver has a different specialization");
            std::unordered_set<const AstVar*> receiverVars;
            for (AstVarScope* vscp = receiverp->varsp(); vscp;
                 vscp = VN_AS(vscp->nextp(), VarScope)) {
                receiverVars.insert(vscp->varp());
            }
            for (AstVarScope* vscp = implementationp->varsp(); vscp;
                 vscp = VN_AS(vscp->nextp(), VarScope)) {
                if (!receiverVars.insert(vscp->varp()).second) continue;
                AstVarScope* const newp
                    = new AstVarScope{vscp->fileline(), receiverp, vscp->varp()};
                receiverp->addVarsp(newp);
                ++lateVarScopes;
            }
        }
    }
    V3Stats::addStat("Scheduling, Subgraph receiver late VarScopes", lateVarScopes);
    V3Stats::addStat("Scheduling, Subgraph receiver late VarScope bytes",
                     lateVarScopes * sizeof(AstVarScope));
    return receivers;
}

uint64_t materializeSharedReceiverLogic(const SharedReceivers& receivers,
                                        const std::unordered_set<AstScope*>& accepted) {
    uint64_t materialized = 0;
    for (const auto& entry : receivers) {
        AstScope* const implementationp = entry.first;
        const bool sharedClocked = accepted.count(implementationp);
        for (AstScope* const receiverp : entry.second) {
            std::unordered_map<const AstVar*, AstVarScope*> receiverVars;
            for (AstVarScope* vscp = receiverp->varsp(); vscp;
                 vscp = VN_AS(vscp->nextp(), VarScope)) {
                receiverVars.emplace(vscp->varp(), vscp);
            }
            for (AstNode* blockp = implementationp->blocksp(); blockp; blockp = blockp->nextp()) {
                AstActive* const activep = VN_CAST(blockp, Active);
                if (!activep || activep->sentreep()->hasCombo() || isPublishActive(activep))
                    continue;
                if (sharedClocked && activep->sentreep()->hasClocked()) continue;
                AstActive* const clonep = activep->cloneTree(false);
                clonep->foreach([&](AstNodeVarRef* refp) {
                    AstVarScope* const vscp = refp->varScopep();
                    if (vscp->scopep() != implementationp) return;
                    const auto it = receiverVars.find(vscp->varp());
                    UASSERT_OBJ(it != receiverVars.end(), refp,
                                "Shared subgraph state missing from receiver scope");
                    refp->varScopep(it->second);
                });
                receiverp->addBlocksp(clonep);
                ++materialized;
            }
            if (!sharedClocked) receiverp->subgraphImplementationScopep(nullptr);
        }
    }
    V3Stats::addStat("Scheduling, Subgraph receiver actives", materialized);
    return materialized;
}

struct EarlyCandidate final {
    struct OutputBinding final {
        uint32_t m_portId = 0;
        AstVarScope* m_statep = nullptr;
        AstVar* m_publishedVarp = nullptr;
        AstVarScope* m_publishedp = nullptr;
    };
    AstScope* m_scopep = nullptr;
    AstVarScope* m_clockp = nullptr;
    std::vector<std::pair<AstScope*, AstActive*>> m_clocked;
    std::vector<std::pair<AstScope*, AstActive*>> m_comb;
    std::vector<OutputBinding> m_outputs;
    std::string m_rejection;
};

void reject(EarlyCandidate& candidate, const char* reason) {
    if (candidate.m_rejection.empty()) candidate.m_rejection = reason;
}

bool hasCallOrSuspendable(AstActive* activep) {
    bool unsupported = false;
    activep->foreach([&](AstCCall*) { unsupported = true; });
    activep->foreach([&](AstNodeProcedure* procp) {
        if (procp->isSuspendable()) unsupported = true;
    });
    return unsupported;
}

AstVarScope* posedgeClock(AstActive* activep) {
    AstSenItem* const itemp = activep->sentreep()->sensesp();
    if (!itemp || itemp->nextp() || itemp->condp() || itemp->edgeType() != VEdgeType::ET_POSEDGE) {
        return nullptr;
    }
    AstNodeVarRef* const refp = itemp->varrefp();
    return refp ? refp->varScopep() : nullptr;
}

bool writesExternalValue(AstActive* activep, AstScope* boundaryScopep) {
    bool writesExternal = false;
    activep->foreach([&](AstNodeVarRef* refp) {
        if (refp->access().isWriteOrRW()
            && !isUnderScope(refp->varScopep()->scopep(), boundaryScopep)) {
            writesExternal = true;
        }
    });
    return writesExternal;
}

SubgraphGroup& findOrCreateGroup(std::vector<SubgraphGroup>& groups, LogicByScope* ownerp,
                                 AstScope* boundaryScopep, FileLine* filelinep) {
    for (SubgraphGroup& group : groups) {
        if (group.m_boundaryScopep == boundaryScopep) return group;
    }
    groups.emplace_back();
    SubgraphGroup& group = groups.back();
    group.m_boundaryScopep = boundaryScopep;
    group.m_filelinep = filelinep;
    group.m_ownerp = ownerp;
    return group;
}

void addSubgraphLogic(SubgraphGroup& group, AstScope* scopep, AstActive* activep) {
    AstSenTree* const senTreep = activep->sentreep();
    if (!group.m_senTreep) group.m_senTreep = senTreep;

    for (AstNode *nodep = activep->stmtsp(), *nextp; nodep; nodep = nextp) {
        nextp = nodep->nextp();
        nodep->unlinkFrBack();
        if (VN_IS(nodep, AlwaysPre)) {
            nodep->foreach([&](AstNodeAssign* assp) {
                AstVarRef* const lhsp = VN_CAST(assp->lhsp(), VarRef);
                if (lhsp && lhsp->varp()->isTemp()) group.m_delayedVars.insert(lhsp->varp());
            });
        }
        LogicByScope& phaseLogic = VN_IS(nodep, AlwaysPost) ? group.m_postLogic : group.m_preLogic;
        phaseLogic.add(scopep, senTreep, nodep);
    }
    if (activep->backp()) activep->unlinkFrBack();
    activep->deleteTree();
}

void captureSubgraphInputs(SubgraphGroup& group, CaptureVars& savedVars) {
    AstScope* const boundaryScopep = group.m_boundaryScopep;
    std::map<AstVarScope*, AstVarScope*> savedScopes;
    std::map<AstVar*, size_t> sourceOccurrences;
    const auto rewrite = [&](LogicByScope& lbs) {
        lbs.foreachLogic([&](AstNode* nodep) {
            nodep->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const sourcep = refp->varScopep();
                const bool portInput
                    = sourcep->scopep() == boundaryScopep && sourcep->varp()->isNonOutput();
                const bool external = !isUnderScope(sourcep->scopep(), boundaryScopep);
                if (!refp->access().isReadOnly() || (!portInput && !external)) { return; }
                const auto inserted = savedScopes.emplace(sourcep, nullptr);
                if (inserted.second) {
                    AstVar* const sourceVarp = sourcep->varp();
                    AstNodeModule* const modp = boundaryScopep->modp();
                    const size_t occurrence = sourceOccurrences[sourceVarp]++;
                    std::vector<AstVar*>& slots = savedVars.m_slots[modp][sourceVarp];
                    if (slots.size() <= occurrence) slots.resize(occurrence + 1);
                    AstVar*& savedVarp = slots[occurrence];
                    if (!savedVarp) {
                        const string name
                            = "__VsubgraphCapture__" + cvtToStr(savedVars.m_nextNameIndex[modp]++);
                        savedVarp = new AstVar{sourcep->fileline(), VVarType::BLOCKTEMP, name,
                                               sourcep->dtypep()};
                        savedVarp->subgraphCaptured(true);
                        modp->addStmtsp(savedVarp);
                    }
                    UASSERT_OBJ(savedVarp->width() == sourcep->width(), sourcep,
                                "Capture slot has inconsistent widths across instances");
                    AstVarScope* const savedp
                        = new AstVarScope{sourcep->fileline(), boundaryScopep, savedVarp};
                    boundaryScopep->addVarsp(savedp);
                    inserted.first->second = savedp;
                    group.m_captures.push_back({sourcep, savedp});
                }
                AstVarScope* const savedp = inserted.first->second;
                refp->varScopep(savedp);
                refp->varp(savedp->varp());
            });
        });
    };
    // NBA right-hand sides are evaluated in the pre phase. Post-phase reads must retain
    // their normal commit-time semantics rather than being silently sampled early.
    rewrite(group.m_preLogic);

    for (const SubgraphGroup::Capture& capture : group.m_captures) {
        FileLine* const flp = capture.m_sourcep->fileline();
        AstActive* const activep = new AstActive{flp, "subgraph-capture", group.m_senTreep};
        activep->addStmtsp(
            new AstAlways{flp, VAlwaysKwd::ALWAYS, nullptr,
                          new AstAssign{flp, new AstVarRef{flp, capture.m_savedp, VAccess::WRITE},
                                        new AstVarRef{flp, capture.m_sourcep, VAccess::READ}}});
        group.m_ownerp->emplace_back(boundaryScopep, activep);
    }
}

}  // namespace

struct SubgraphPlan::Impl final {
    struct Group final {
        AstScope* m_scopep = nullptr;
        AstVarScope* m_clockp = nullptr;
        std::vector<EarlyCandidate::OutputBinding> m_outputs;
        AstSenTree* m_senTreep = nullptr;
        LogicByScope m_clocked;
        LogicByScope m_comb;
        LogicByScope m_hybrid;
        LogicRegions m_regions;
        LogicReplicas m_replicas;
        std::vector<Use> m_uses;
        std::vector<AstScope*> m_receivers;
        std::vector<std::vector<Use>> m_receiverUses;
    };

    std::vector<Group> m_groups;
    std::map<AstScope*, size_t> m_accepted;
};

SubgraphPlan::SubgraphPlan(AstNetlist* netlistp)
    : m_impl{new Impl} {
    if (!v3Global.opt.subgraphSchedule()) return;
    const SharedReceivers sharedReceivers = prepareSharedReceiverState(netlistp);
    splitBoundaryCombinationalLogic(netlistp);

    std::vector<EarlyCandidate> candidates;
    std::map<AstScope*, size_t> candidateIndex;
    LogicByScope allActives;
    LogicByScope allClocked;
    LogicByScope allComb;
    netlistp->foreach([&](AstScope* scopep) {
        scopep->foreach([&](AstActive* activep) {
            allActives.emplace_back(scopep, activep);
            AstSenTree* const senTreep = activep->sentreep();
            const bool clocked = senTreep->hasClocked();
            const bool comb = senTreep->hasCombo();
            if (clocked) allClocked.emplace_back(scopep, activep);
            if (comb) allComb.emplace_back(scopep, activep);
            AstScope* const boundaryScopep = findBoundaryScope(scopep);
            if (!boundaryScopep || (!clocked && !comb)) return;
            if (comb
                && (isPublishActive(activep) || isBoundaryInputActive(boundaryScopep, activep))) {
                return;
            }
            const auto inserted = candidateIndex.emplace(boundaryScopep, candidates.size());
            if (inserted.second) {
                candidates.emplace_back();
                candidates.back().m_scopep = boundaryScopep;
            }
            EarlyCandidate& candidate = candidates[inserted.first->second];
            if (clocked && !VN_IS(activep->stmtsp(), AlwaysObserved)
                && !VN_IS(activep->stmtsp(), AlwaysReactive)) {
                candidate.m_clocked.emplace_back(scopep, activep);
            } else if (comb && !VN_IS(activep->stmtsp(), AlwaysPostponed)) {
                candidate.m_comb.emplace_back(scopep, activep);
            } else {
                reject(candidate, "unsupported region");
            }
        });
    });

    // A published output can name an FF directly or an alias optimized to its
    // backing FF. The binding is the port contract, independent of child names.
    for (const auto& pair : allComb) {
        AstScope* const boundaryScopep = findBoundaryScope(pair.first);
        if (!boundaryScopep || !isPublishActive(pair.second)) continue;
        AstScope* const implementationp = boundaryScopep->subgraphImplementationScopep()
                                              ? boundaryScopep->subgraphImplementationScopep()
                                              : boundaryScopep;
        const auto it = candidateIndex.find(implementationp);
        if (it == candidateIndex.end()) continue;
        EarlyCandidate& candidate = candidates[it->second];
        const AstAssignW* const assp
            = VN_AS(VN_AS(pair.second->stmtsp(), Always)->stmtsp(), AssignW);
        const AstVarRef* const lhsp = VN_AS(assp->lhsp(), VarRef);
        const AstVarRef* const rhsp = VN_CAST(assp->rhsp(), VarRef);
        if (!rhsp || !isUnderScope(rhsp->varScopep()->scopep(), boundaryScopep)) {
            reject(candidate, "output is not FF state");
            continue;
        }
        AstVarScope* const statep = findVarScope(implementationp, rhsp->varp());
        if (!statep) {
            reject(candidate, "output state missing from representative");
            continue;
        }
        const uint32_t portId = lhsp->varp()->subgraphPortId();
        bool found = false;
        for (const EarlyCandidate::OutputBinding& output : candidate.m_outputs) {
            if (output.m_portId != portId) continue;
            UASSERT_OBJ(output.m_statep == statep && output.m_publishedVarp == lhsp->varp(),
                        pair.second, "Inconsistent publication source across receivers");
            found = true;
        }
        if (!found) candidate.m_outputs.push_back({portId, statep, lhsp->varp(), nullptr});
    }

    // Index accesses that bypass a boundary's ports once for the entire design.
    // Shared receivers are mapped to their representative candidate.
    std::unordered_set<AstScope*> externallyAccessed;
    for (const auto& pair : allActives) {
        pair.second->foreach([&](AstNodeVarRef* refp) {
            AstScope* const boundaryScopep = findBoundaryScope(refp->varScopep()->scopep());
            if (!boundaryScopep || isUnderScope(pair.first, boundaryScopep)) return;
            const AstVar* const varp = refp->varp();
            const bool inputWrite = varp->isInput() && refp->access().isWriteOnly();
            const bool captureWrite = varp->subgraphCaptured() && refp->access().isWriteOnly();
            const bool outputRead = varp->subgraphPublished() && refp->access().isReadOnly();
            if (inputWrite || captureWrite || outputRead) return;
            AstScope* const implementationp = boundaryScopep->subgraphImplementationScopep()
                                                  ? boundaryScopep->subgraphImplementationScopep()
                                                  : boundaryScopep;
            externallyAccessed.insert(implementationp);
        });
    }

    for (EarlyCandidate& candidate : candidates) {
        if (v3Global.usesZeroDelay()) reject(candidate, "zero-delay design");
        if (candidate.m_clocked.empty()) {
            reject(candidate, "no clocked logic");
            continue;
        }
        // The settle region still uses the parent combinational logic. Supporting child
        // combinational logic requires a separate child settle path during initialization.
        if (!candidate.m_comb.empty()) reject(candidate, "child combinational logic");
        AstVarScope* clockVscp = nullptr;
        for (const auto& pair : candidate.m_clocked) {
            AstActive* const activep = pair.second;
            AstVarScope* const activeClockVscp = posedgeClock(activep);
            if (!activeClockVscp) reject(candidate, "not a single posedge clock");
            if (!clockVscp) clockVscp = activeClockVscp;
            if (activeClockVscp != clockVscp) reject(candidate, "multiple clocks");
            if (hasCallOrSuspendable(activep)) reject(candidate, "call or timing control");
            if (writesExternalValue(activep, candidate.m_scopep)) {
                reject(candidate, "clocked write outside boundary");
            }
        }
        if (clockVscp) {
            candidate.m_clockp = clockVscp;
            if (isUnderScope(clockVscp->scopep(), candidate.m_scopep)) {
                const bool boundaryClockInput = clockVscp->scopep() == candidate.m_scopep
                                                && clockVscp->varp()->isNonOutput()
                                                && clockVscp->varp()->subgraphPortId();
                if (!boundaryClockInput) reject(candidate, "clock is inside boundary");
            }
            const auto checkClockWrite = [&](const auto& pairs) {
                for (const auto& pair : pairs) {
                    pair.second->foreach([&](AstNodeVarRef* refp) {
                        if (refp->varScopep() == clockVscp && refp->access().isWriteOrRW()) {
                            reject(candidate, "internally driven clock");
                        }
                    });
                }
            };
            checkClockWrite(candidate.m_clocked);
            checkClockWrite(candidate.m_comb);
        }
        std::unordered_set<AstVarScope*> clockedWrites;
        for (const auto& pair : candidate.m_clocked) {
            pair.second->foreach([&](AstNodeVarRef* refp) {
                if (refp->access().isWriteOrRW()) clockedWrites.insert(refp->varScopep());
            });
        }
        for (const EarlyCandidate::OutputBinding& output : candidate.m_outputs) {
            if (!clockedWrites.count(output.m_statep)) {
                reject(candidate, "output is not FF state");
            }
        }
        if (externallyAccessed.count(candidate.m_scopep)) {
            reject(candidate, "external access to child state");
        }
    }

    // A boundary value used as a parent clock can move the boundary into the Active region,
    // which this NBA-only experiment cannot schedule. Follow combinational assignments so an
    // output alias or a short combinational chain cannot hide that dependency.
    for (EarlyCandidate& candidate : candidates) {
        std::unordered_set<AstVarScope*> tainted;
        const auto seedInternal = [&](const auto& pairs) {
            for (const auto& pair : pairs) {
                pair.second->foreach([&](AstNodeVarRef* refp) {
                    if (isUnderScope(refp->varScopep()->scopep(), candidate.m_scopep)) {
                        tainted.insert(refp->varScopep());
                    }
                });
            }
        };
        seedInternal(candidate.m_clocked);
        seedInternal(candidate.m_comb);
        // Receiver procedures have not been cloned. Include their state in the parent-clock
        // check so an instance-specific generated clock cannot bypass admission.
        const auto receivers = sharedReceivers.find(candidate.m_scopep);
        if (receivers != sharedReceivers.end()) {
            for (AstScope* const receiverp : receivers->second) {
                for (AstVarScope* vscp = receiverp->varsp(); vscp;
                     vscp = VN_AS(vscp->nextp(), VarScope)) {
                    tainted.insert(vscp);
                }
            }
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& pair : allComb) {
                bool readsTainted = false;
                pair.second->foreach([&](AstNodeVarRef* refp) {
                    if (refp->access().isReadOrRW() && tainted.count(refp->varScopep())) {
                        readsTainted = true;
                    }
                });
                if (!readsTainted) continue;
                pair.second->foreach([&](AstNodeVarRef* refp) {
                    if (refp->access().isWriteOrRW() && tainted.insert(refp->varScopep()).second) {
                        changed = true;
                    }
                });
            }
        }

        for (const auto& pair : allClocked) {
            if (findBoundaryScope(pair.first) == candidate.m_scopep) continue;
            pair.second->sentreep()->foreach([&](AstNodeVarRef* refp) {
                if (tainted.count(refp->varScopep())) {
                    reject(candidate, "boundary value used as clock");
                }
            });
        }
    }

    uint64_t rejected = 0;
    uint64_t clocked = 0;
    uint64_t combinational = 0;
    uint64_t acceptedInstances = 0;
    std::unordered_set<AstScope*> acceptedScopes;
    for (EarlyCandidate& candidate : candidates) {
        if (!candidate.m_rejection.empty()) {
            ++rejected;
            UINFO(4, "Subgraph early scheduling fallback for " << candidate.m_scopep->name()
                                                               << ": " << candidate.m_rejection);
            continue;
        }
        const size_t groupIndex = m_impl->m_groups.size();
        // Scope may omit an unconnected output in the representative or another
        // receiver. The shared post function needs a state slot in every receiver.
        for (EarlyCandidate::OutputBinding& output : candidate.m_outputs) {
            const auto ensurePublished = [&](AstScope* scopep) {
                if (AstVarScope* const vscp = findVarScope(scopep, output.m_publishedVarp)) {
                    return vscp;
                }
                AstVarScope* const vscp = new AstVarScope{output.m_publishedVarp->fileline(),
                                                          scopep, output.m_publishedVarp};
                scopep->addVarsp(vscp);
                return vscp;
            };
            output.m_publishedp = ensurePublished(candidate.m_scopep);
            output.m_publishedVarp->subgraphSharedState(true);
            const auto receivers = sharedReceivers.find(candidate.m_scopep);
            if (receivers != sharedReceivers.end()) {
                for (AstScope* const receiverp : receivers->second) ensurePublished(receiverp);
            }
        }
        m_impl->m_groups.emplace_back();
        Impl::Group& group = m_impl->m_groups[groupIndex];
        group.m_scopep = candidate.m_scopep;
        group.m_clockp = candidate.m_clockp;
        group.m_outputs = candidate.m_outputs;
        m_impl->m_accepted.emplace(candidate.m_scopep, groupIndex);
        acceptedScopes.insert(candidate.m_scopep);
        const auto receivers = sharedReceivers.find(candidate.m_scopep);
        if (receivers != sharedReceivers.end()) group.m_receivers = receivers->second;
        acceptedInstances += 1 + group.m_receivers.size();
        clocked += candidate.m_clocked.size();
        combinational += candidate.m_comb.size();
    }
    materializeSharedReceiverLogic(sharedReceivers, acceptedScopes);
    V3Stats::addStat("Scheduling, Subgraph early candidates", candidates.size());
    V3Stats::addStat("Scheduling, Subgraph early groups", acceptedInstances);
    V3Stats::addStat("Scheduling, Subgraph early fallbacks", rejected);
    V3Stats::addStat("Scheduling, Subgraph early clocked actives", clocked);
    V3Stats::addStat("Scheduling, Subgraph early combinational actives", combinational);
}

SubgraphPlan::~SubgraphPlan() = default;

bool SubgraphPlan::extract(AstScope* scopep, AstActive* activep) {
    if (isPublishActive(activep)) return false;
    AstScope* const boundaryScopep = findBoundaryScope(scopep);
    if (boundaryScopep && isBoundaryInputActive(boundaryScopep, activep)) return false;
    const auto it = m_impl->m_accepted.find(boundaryScopep);
    if (it == m_impl->m_accepted.end()) return false;
    Impl::Group& group = m_impl->m_groups[it->second];
    AstSenTree* const senTreep = activep->sentreep();
    if (senTreep->hasClocked()) {
        if (!group.m_senTreep) group.m_senTreep = senTreep;
        group.m_clocked.emplace_back(scopep, activep);
    } else if (senTreep->hasCombo()) {
        group.m_comb.emplace_back(scopep, activep);
    } else {
        return false;
    }
    return true;
}

bool SubgraphPlan::isAccepted(const AstScope* scopep) const {
    return m_impl->m_accepted.count(const_cast<AstScope*>(scopep));
}

AstVarScope* SubgraphPlan::clockPort(const AstScope* scopep) const {
    const auto it = m_impl->m_accepted.find(const_cast<AstScope*>(scopep));
    UASSERT_OBJ(it != m_impl->m_accepted.end(), scopep, "Missing early subgraph clock");
    return m_impl->m_groups[it->second].m_clockp;
}

void SubgraphPlan::foreachPublished(const AstScope* scopep,
                                    const std::function<void(AstVarScope*)>& callback) const {
    const auto it = m_impl->m_accepted.find(const_cast<AstScope*>(scopep));
    UASSERT_OBJ(it != m_impl->m_accepted.end(), scopep, "Missing early subgraph outputs");
    for (const EarlyCandidate::OutputBinding& output : m_impl->m_groups[it->second].m_outputs) {
        callback(output.m_publishedp);
    }
}

void SubgraphPlan::appendPublications(const AstScope* scopep, AstCFunc* funcp) const {
    const auto it = m_impl->m_accepted.find(const_cast<AstScope*>(scopep));
    UASSERT_OBJ(it != m_impl->m_accepted.end(), scopep, "Missing early subgraph outputs");
    for (const EarlyCandidate::OutputBinding& output : m_impl->m_groups[it->second].m_outputs) {
        FileLine* const flp = output.m_publishedp->fileline();
        funcp->addStmtsp(new AstAssign{flp,
                                       new AstVarRef{flp, output.m_publishedp, VAccess::WRITE},
                                       new AstVarRef{flp, output.m_statep, VAccess::READ}});
    }
}

void SubgraphPlan::movePublications(LogicByScope& comb, LogicByScope& hybrid) {
    const auto remove = [&](LogicByScope& lbs) {
        const auto newEnd = std::remove_if(lbs.begin(), lbs.end(), [&](const auto& pair) {
            if (!isPublishActive(pair.second)) return false;
            AstScope* const boundaryScopep = findBoundaryScope(pair.first);
            if (!boundaryScopep) return false;
            AstScope* const implementationp = boundaryScopep->subgraphImplementationScopep()
                                                  ? boundaryScopep->subgraphImplementationScopep()
                                                  : boundaryScopep;
            if (!m_impl->m_accepted.count(implementationp)) return false;
            pair.second->unlinkFrBack()->deleteTree();
            return true;
        });
        lbs.erase(newEnd, lbs.end());
    };
    remove(comb);
    remove(hybrid);
}

void SubgraphPlan::breakCycles(AstNetlist* netlistp) {
    for (Impl::Group& group : m_impl->m_groups) {
        // The parent cycle graph has no boundary edges for this NBA-only experiment.
        // Keep that invariant explicit before analyzing the parent combinational logic.
        UASSERT_OBJ(group.m_comb.empty(), group.m_scopep,
                    "Subgraph combinational effects need parent cycle edges");
        group.m_hybrid = V3Sched::breakCycles(netlistp, group.m_comb);
    }
}

void SubgraphPlan::partitionAndReplicate() {
    for (Impl::Group& group : m_impl->m_groups) {
        group.m_regions = V3Sched::partition(group.m_clocked, group.m_comb, group.m_hybrid);
        UASSERT_OBJ(group.m_regions.m_pre.empty() && group.m_regions.m_act.empty(), group.m_scopep,
                    "Early subgraph unexpectedly requires the Active region");
        group.m_replicas = replicateLogic(group.m_regions);
        UASSERT_OBJ(group.m_replicas.m_ico.empty() && group.m_replicas.m_act.empty()
                        && group.m_replicas.m_obs.empty() && group.m_replicas.m_react.empty(),
                    group.m_scopep, "Early subgraph unexpectedly requires a non-NBA replica");
        for (const EarlyCandidate::OutputBinding& output : group.m_outputs) {
            group.m_uses.emplace_back(Use{output.m_publishedp, false, true});
        }
        for (AstScope* const receiverp : group.m_receivers) {
            std::unordered_map<const AstVar*, AstVarScope*> receiverVars;
            for (AstVarScope* vscp = receiverp->varsp(); vscp;
                 vscp = VN_AS(vscp->nextp(), VarScope)) {
                receiverVars.emplace(vscp->varp(), vscp);
            }
            std::vector<Use> uses;
            uses.reserve(group.m_uses.size());
            for (const Use& use : group.m_uses) {
                AstVarScope* vscp = use.m_vscp;
                if (vscp->scopep() == group.m_scopep) {
                    const auto it = receiverVars.find(vscp->varp());
                    UASSERT_OBJ(it != receiverVars.end(), receiverp,
                                "Shared subgraph state missing from receiver scope");
                    vscp = it->second;
                }
                uses.emplace_back(Use{vscp, use.m_read, use.m_write});
            }
            group.m_receiverUses.emplace_back(std::move(uses));
        }
    }
    uint64_t portWrites = 0;
    for (const Impl::Group& group : m_impl->m_groups) portWrites += group.m_uses.size();
    V3Stats::addStat("Scheduling, Subgraph boundary output ports", portWrites);
}

void SubgraphPlan::foreachBoundary(
    const std::function<void(AstScope*, AstSenTree*, const std::vector<Use>&)>& callback) const {
    for (const Impl::Group& group : m_impl->m_groups) {
        UASSERT_OBJ(group.m_senTreep, group.m_scopep, "Missing subgraph clocked sensitivity");
        callback(group.m_scopep, group.m_senTreep, group.m_uses);
        for (size_t i = 0; i < group.m_receivers.size(); ++i) {
            callback(group.m_receivers[i], group.m_senTreep, group.m_receiverUses[i]);
        }
    }
}

void SubgraphPlan::materializeNba(
    const std::unordered_map<const AstSenTree*, AstSenTree*>& senTreeMap,
    const std::vector<LogicByScope*>& parentLogic) {
    LogicByScope* const defaultOwnerp = parentLogic.front();
    for (Impl::Group& group : m_impl->m_groups) {
        const auto append = [&](LogicByScope& lbs) {
            for (const auto& pair : lbs) {
                AstActive* const activep = pair.second;
                if (!activep->sentreep()->hasCombo()) {
                    activep->sentreep(senTreeMap.at(activep->sentreep()));
                }
                defaultOwnerp->emplace_back(pair);
            }
            lbs.clear();
        };
        append(group.m_regions.m_nba);
        append(group.m_replicas.m_nba);
    }
}

void SubgraphPlan::foreachUse(const std::function<void(const Use&)>& callback) const {
    for (const Impl::Group& group : m_impl->m_groups) {
        for (const Use& use : group.m_uses) callback(use);
        for (const std::vector<Use>& uses : group.m_receiverUses) {
            for (const Use& use : uses) callback(use);
        }
    }
}

static bool sameReceiverLogic(const LogicByScope& representative, const LogicByScope& candidate,
                              AstScope* representativeScopep, AstScope* candidateScopep) {
    if (representative.size() != candidate.size()) return false;
    std::map<const AstVar*, AstVarScope*> representativeVars;
    for (AstVarScope* vscp = representativeScopep->varsp(); vscp;
         vscp = VN_AS(vscp->nextp(), VarScope)) {
        representativeVars.emplace(vscp->varp(), vscp);
    }
    for (size_t i = 0; i < representative.size(); ++i) {
        const auto& source = representative[i];
        const auto& target = candidate[i];
        if (source.first != representativeScopep || target.first != candidateScopep
            || !source.second->sentreep()->sameTree(target.second->sentreep())) {
            return false;
        }
        AstNode* const sourcep = source.second->stmtsp();
        AstNode* const targetp = target.second->stmtsp();
        if (!sourcep || !targetp) {
            if (sourcep != targetp) return false;
            continue;
        }
        AstNode* const clonedp = targetp->cloneTree(true);
        bool compatible = true;
        clonedp->foreachAndNext([&](AstNode* nodep) {
            if (VN_IS(nodep, NodeCCall) || VN_IS(nodep, NodeFTaskRef) || VN_IS(nodep, ScopeName)
                || VN_IS(nodep, VarXRef) || VN_IS(nodep, CExpr) || VN_IS(nodep, CExprUser)
                || VN_IS(nodep, CStmt) || VN_IS(nodep, CStmtUser)) {
                compatible = false;
            }
            if (AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
                if (refp->varScopep()->scopep() != candidateScopep) return;
                const auto it = representativeVars.find(refp->varp());
                if (it == representativeVars.end()) {
                    compatible = false;
                } else {
                    refp->varScopep(it->second);
                }
            }
        });
        if (compatible) compatible = sourcep->sameTree(clonedp);
        clonedp->deleteTree();
        if (!compatible) return false;
    }
    return true;
}

static bool sameReceiverGroup(const SubgraphGroup& representative,
                              const SubgraphGroup& candidate) {
    if (representative.m_boundaryScopep->modp() != candidate.m_boundaryScopep->modp()
        || !representative.m_senTreep || !candidate.m_senTreep
        || !representative.m_senTreep->sameTree(candidate.m_senTreep)) {
        return false;
    }
    return sameReceiverLogic(representative.m_preLogic, candidate.m_preLogic,
                             representative.m_boundaryScopep, candidate.m_boundaryScopep)
           && sameReceiverLogic(representative.m_postLogic, candidate.m_postLogic,
                                representative.m_boundaryScopep, candidate.m_boundaryScopep);
}

V3Order::FreshReads lowerSubgraphNbaLogic(AstNetlist* netlistp,
                                          const std::vector<LogicByScope*>& logic,
                                          const V3Order::TrigToSenMap& trigToSen,
                                          const CovergroupRefBindings& cgRefBindings, bool slow,
                                          const V3Order::ExternalDomainsProvider& externalDomains,
                                          const SubgraphPlan& plan,
                                          V3Order::BoundaryUses& boundaryUses) {
    V3Order::FreshReads freshReads;
    if (!v3Global.opt.subgraphSchedule()) return freshReads;

    std::vector<SubgraphGroup> groups;
    for (LogicByScope* const lbsp : logic) {
        LogicByScope parentLogic;
        parentLogic.reserve(lbsp->size());
        for (const auto& pair : *lbsp) {
            AstScope* const scopep = pair.first;
            AstActive* const activep = pair.second;
            AstScope* const boundaryScopep = findBoundaryScope(scopep);
            // Combinational replicas must observe all parent and child NBA commits before they
            // refresh outputs and next-state values, so keep them in the parent scheduler.
            if (!boundaryScopep || activep->sentreep()->hasCombo()) {
                parentLogic.emplace_back(pair);
                continue;
            }
            SubgraphGroup& group
                = findOrCreateGroup(groups, lbsp, boundaryScopep, activep->fileline());
            addSubgraphLogic(group, scopep, activep);
        }
        *lbsp = std::move(parentLogic);
    }

    uint64_t orderedLogic = 0;
    uint64_t shareableFunctions = 0;
    uint64_t sharedOrderSkips = 0;
    unsigned groupIndex = 0;
    CaptureVars savedVars;
    SharedReceivers sharedReceivers;
    netlistp->foreach([&](AstScope* scopep) {
        if (AstScope* const implementationp = scopep->subgraphImplementationScopep()) {
            sharedReceivers[implementationp].push_back(scopep);
        }
    });
    std::map<AstNodeModule*, unsigned> groupsByModule;
    for (const SubgraphGroup& group : groups) ++groupsByModule[group.m_boundaryScopep->modp()];
    for (const auto& entry : sharedReceivers) {
        groupsByModule[entry.first->modp()] += entry.second.size();
    }
    std::vector<size_t> representative(groups.size());
    std::vector<std::array<AstCFunc*, 2>> orderedFunctions(groups.size());
    std::map<AstNodeModule*, size_t> firstByModule;
    for (size_t i = 0; i < groups.size(); ++i) {
        representative[i] = i;
        AstNodeModule* const modp = groups[i].m_boundaryScopep->modp();
        if (groupsByModule[modp] < 2) continue;
        const auto inserted = firstByModule.emplace(modp, i);
        if (!inserted.second && sameReceiverGroup(groups[inserted.first->second], groups[i])) {
            representative[i] = inserted.first->second;
        }
    }
    std::vector<bool> sharedRepresentative(groups.size(), false);
    for (size_t i = 0; i < groups.size(); ++i) {
        if (representative[i] != i) sharedRepresentative[representative[i]] = true;
        if (sharedReceivers.count(groups[i].m_boundaryScopep)) sharedRepresentative[i] = true;
    }
    for (SubgraphGroup& group : groups) {
        orderedLogic += group.m_preLogic.size() + group.m_postLogic.size();
        UASSERT_OBJ(group.m_senTreep, group.m_boundaryScopep,
                    "Subgraph NBA logic has no clocked sensitivity");
        captureSubgraphInputs(group, savedVars);
        for (const SubgraphGroup::Capture& capture : group.m_captures) {
            freshReads[group.m_boundaryScopep].push_back(capture.m_savedp);
        }
        // Inst may have acquired a connected input before Scope shared the child
        // procedure. Those slots are also fresh on this edge, for every receiver.
        const auto addCapturedPorts = [&](AstScope* scopep) {
            for (AstVarScope* vscp = scopep->varsp(); vscp;
                 vscp = VN_AS(vscp->nextp(), VarScope)) {
                if (!vscp->varp()->subgraphCaptured()) continue;
                std::vector<AstVarScope*>& reads = freshReads[scopep];
                if (std::find(reads.begin(), reads.end(), vscp) == reads.end()) {
                    reads.push_back(vscp);
                }
            }
        };
        addCapturedPorts(group.m_boundaryScopep);
        const auto receivers = sharedReceivers.find(group.m_boundaryScopep);
        if (receivers != sharedReceivers.end()) {
            for (AstScope* const receiverp : receivers->second) addCapturedPorts(receiverp);
        }

        const auto orderPhase = [&](LogicByScope& phaseLogic, const string& phase, bool post) {
            if (phaseLogic.empty()) return;
            V3Order::BoundaryContract contract;
            contract.m_portOnly = plan.isAccepted(group.m_boundaryScopep);
            contract.m_post = post;
            if (contract.m_portOnly) {
                contract.m_phasePortp = plan.clockPort(group.m_boundaryScopep);
                if (post) {
                    plan.foreachPublished(group.m_boundaryScopep, [&](AstVarScope* vscp) {
                        contract.m_uses.push_back({vscp, false, true, false});
                    });
                }
                // Derive the parent effect from the port and phase, before local Order
                // replaces these statements with a callable function.
                phaseLogic.foreachLogic([&](AstNode* nodep) {
                    nodep->foreach([&](AstNodeVarRef* refp) {
                        AstVarScope* const vscp = refp->varScopep();
                        if (vscp->varp()->subgraphCaptured()) {
                            contract.m_uses.push_back({vscp, refp->access().isReadOrRW(),
                                                       refp->access().isWriteOrRW(), false});
                        }
                    });
                });
            }
            const bool mayShare
                = groupsByModule[group.m_boundaryScopep->modp()] > 1
                  && std::all_of(phaseLogic.begin(), phaseLogic.end(), [&](const auto& pair) {
                         return pair.first == group.m_boundaryScopep;
                     });
            std::unordered_set<AstCFunc*> oldFunctions;
            if (mayShare) {
                for (AstNode* blockp = group.m_boundaryScopep->blocksp(); blockp;
                     blockp = blockp->nextp()) {
                    if (AstCFunc* const cfuncp = VN_CAST(blockp, CFunc)) {
                        oldFunctions.insert(cfuncp);
                    }
                }
            }
            AstCFunc* funcp = nullptr;
            if (representative[groupIndex] != groupIndex) {
                funcp = orderedFunctions[representative[groupIndex]][post];
                UASSERT_OBJ(funcp, group.m_boundaryScopep,
                            "Shared subgraph has no representative function");
                for (const auto& pair : phaseLogic) pair.second->deleteTree();
                phaseLogic.clear();
                ++sharedOrderSkips;
            } else {
                const string tag = "nba_subgraph_" + phase + "_" + cvtToStr(groupIndex);
                funcp = V3Order::order(netlistp, {&phaseLogic}, trigToSen, cgRefBindings, tag,
                                       false, slow, externalDomains, group.m_boundaryScopep);
                if (!funcp) return;
                if (post && contract.m_portOnly) {
                    plan.appendPublications(group.m_boundaryScopep, funcp);
                }
                funcp->subgraphWrapper(true);
                if (sharedRepresentative[groupIndex]) {
                    funcp->subgraphShareable(true);
                    ++shareableFunctions;
                }
                util::splitCheck(funcp);
                orderedFunctions[groupIndex][post] = funcp;
                if (!contract.m_portOnly) {
                    std::unordered_set<const AstCFunc*> visited;
                    std::function<void(AstCFunc*)> collect = [&](AstCFunc* currentp) {
                        if (!visited.insert(currentp).second) return;
                        currentp->foreach([&](AstNodeVarRef* refp) {
                            contract.m_uses.push_back(
                                {refp->varScopep(), refp->access().isReadOrRW(),
                                 refp->access().isWriteOrRW(),
                                 group.m_delayedVars.count(refp->varp()) != 0});
                        });
                        currentp->foreach([&](AstCCall* callp) {
                            if (!callp->funcp()->entryPoint()) collect(callp->funcp());
                        });
                    };
                    collect(funcp);
                }
                UASSERT_OBJ(boundaryUses.emplace(funcp, std::move(contract)).second, funcp,
                            "Duplicate subgraph boundary contract");
            }
            if (mayShare && representative[groupIndex] == groupIndex) {
                for (AstNode* blockp = group.m_boundaryScopep->blocksp(); blockp;
                     blockp = blockp->nextp()) {
                    AstCFunc* const cfuncp = VN_CAST(blockp, CFunc);
                    if (!cfuncp || oldFunctions.count(cfuncp)) continue;
                    bool hasInstanceContext = false;
                    cfuncp->foreach([&](AstNode* nodep) {
                        if (VN_IS(nodep, NodeCCall) || VN_IS(nodep, NodeFTaskRef)
                            || VN_IS(nodep, CExpr) || VN_IS(nodep, CExprUser)
                            || VN_IS(nodep, CStmt) || VN_IS(nodep, CStmtUser)
                            || VN_IS(nodep, ScopeName) || VN_IS(nodep, VarXRef)) {
                            hasInstanceContext = true;
                        }
                    });
                    if (hasInstanceContext) continue;
                    cfuncp->dontCombine(false);
                    cfuncp->subgraphShareable(true);
                    ++shareableFunctions;
                }
            }

            AstActive* const wrapperp
                = new AstActive{group.m_filelinep, "subgraph", group.m_senTreep};
            AstCCall* const callp = new AstCCall{group.m_filelinep, funcp};
            callp->dtypeSetVoid();
            if (representative[groupIndex] != groupIndex) {
                callp->subgraphReceiverScopep(group.m_boundaryScopep);
            }
            if (post) {
                AstAlwaysPost* const postp = new AstAlwaysPost{group.m_filelinep};
                postp->addStmtsp(callp->makeStmt());
                wrapperp->addStmtsp(postp);
            } else {
                wrapperp->addStmtsp(callp->makeStmt());
            }
            group.m_ownerp->emplace_back(group.m_boundaryScopep, wrapperp);
        };

        orderPhase(group.m_preLogic, "pre", false);
        orderPhase(group.m_postLogic, "post", true);
        ++groupIndex;
    }

    uint64_t directReceivers = 0;
    for (size_t i = 0; i < groups.size(); ++i) {
        const SubgraphGroup& group = groups[i];
        const auto receivers = sharedReceivers.find(group.m_boundaryScopep);
        if (receivers == sharedReceivers.end()) continue;
        for (AstScope* const receiverp : receivers->second) {
            for (bool post : {false, true}) {
                AstCFunc* const funcp = orderedFunctions[representative[i]][post];
                if (!funcp) continue;
                AstActive* const activep
                    = new AstActive{group.m_filelinep, "subgraph", group.m_senTreep};
                AstCCall* const callp = new AstCCall{group.m_filelinep, funcp};
                callp->dtypeSetVoid();
                callp->subgraphReceiverScopep(receiverp);
                if (post) {
                    AstAlwaysPost* const postp = new AstAlwaysPost{group.m_filelinep};
                    postp->addStmtsp(callp->makeStmt());
                    activep->addStmtsp(postp);
                } else {
                    activep->addStmtsp(callp->makeStmt());
                }
                group.m_ownerp->emplace_back(receiverp, activep);
                ++sharedOrderSkips;
            }
            receiverp->subgraphImplementationScopep(nullptr);
            ++directReceivers;
        }
    }

    V3Stats::addStat("Scheduling, Subgraph NBA groups", groups.size() + directReceivers);
    V3Stats::addStat("Scheduling, Subgraph NBA internal actives", orderedLogic);
    V3Stats::addStat("Scheduling, Subgraph shareable CFuncs", shareableFunctions);
    V3Stats::addStat("Scheduling, Subgraph shared Order skips", sharedOrderSkips);
    uint64_t contractUses = 0;
    for (const auto& entry : boundaryUses) contractUses += entry.second.m_uses.size();
    V3Stats::addStat("Scheduling, Subgraph boundary contract uses", contractUses);
    uint64_t capturedInputs = 0;
    for (const auto& pair : freshReads) capturedInputs += pair.second.size();
    V3Stats::addStat("Scheduling, Subgraph captured inputs", capturedInputs);
    return freshReads;
}

}  // namespace V3Sched
