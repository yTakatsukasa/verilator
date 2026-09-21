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
// replication through a variable contract. Child logic is materialized as
// callable helpers before parent Order. Ineligible boundaries use the late
// lowering path. Scope expansion remains unchanged.
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
    AstScope* m_boundaryScopep = nullptr;
    AstSenTree* m_senTreep = nullptr;
    FileLine* m_filelinep = nullptr;
    LogicByScope* m_ownerp = nullptr;
    LogicByScope m_preLogic;
    LogicByScope m_postLogic;
};

AstScope* findBoundaryScope(AstScope* scopep) {
    for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
        if (scanp->modp()->subgraphBoundary()) return scanp;
    }
    return nullptr;
}

struct EarlyCandidate final {
    AstScope* m_scopep = nullptr;
    std::vector<std::pair<AstScope*, AstActive*>> m_clocked;
    std::vector<std::pair<AstScope*, AstActive*>> m_comb;
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
        LogicByScope& phaseLogic = VN_IS(nodep, AlwaysPost) ? group.m_postLogic : group.m_preLogic;
        phaseLogic.add(scopep, senTreep, nodep);
    }
    if (activep->backp()) activep->unlinkFrBack();
    activep->deleteTree();
}

}  // namespace

struct SubgraphPlan::Impl final {
    struct Group final {
        AstScope* m_scopep = nullptr;
        LogicByScope m_clocked;
        LogicByScope m_comb;
        LogicByScope m_hybrid;
        LogicRegions m_regions;
        LogicReplicas m_replicas;
        std::vector<Use> m_uses;
    };

    std::vector<Group> m_groups;
};

SubgraphPlan::SubgraphPlan(LogicClasses& logicClasses)
    : m_impl{new Impl} {
    if (!v3Global.opt.subgraphSchedule()) return;

    std::vector<EarlyCandidate> candidates;
    std::map<AstScope*, size_t> candidateIndex;
    const auto addCandidates = [&](const LogicByScope& lbs, bool clocked) {
        for (const auto& pair : lbs) {
            AstScope* const boundaryScopep = findBoundaryScope(pair.first);
            if (!boundaryScopep) continue;
            const auto inserted = candidateIndex.emplace(boundaryScopep, candidates.size());
            if (inserted.second) {
                candidates.emplace_back();
                candidates.back().m_scopep = boundaryScopep;
            }
            EarlyCandidate& candidate = candidates[inserted.first->second];
            (clocked ? candidate.m_clocked : candidate.m_comb).push_back(pair);
        }
    };
    addCandidates(logicClasses.m_clocked, true);
    addCandidates(logicClasses.m_comb, false);

    const auto rejectUnsupportedClass = [&](const LogicByScope& lbs) {
        for (const auto& pair : lbs) {
            AstScope* const boundaryScopep = findBoundaryScope(pair.first);
            const auto it = candidateIndex.find(boundaryScopep);
            if (it != candidateIndex.end()) reject(candidates[it->second], "unsupported region");
        }
    };
    rejectUnsupportedClass(logicClasses.m_postponed);
    rejectUnsupportedClass(logicClasses.m_observed);
    rejectUnsupportedClass(logicClasses.m_reactive);
    rejectUnsupportedClass(logicClasses.m_hybrid);

    for (EarlyCandidate& candidate : candidates) {
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
            if (isUnderScope(clockVscp->scopep(), candidate.m_scopep)) {
                reject(candidate, "clock is inside boundary");
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
    }

    // A boundary value used as a parent clock would require the boundary write contract during
    // partitioning. Keep that case on the established late path in this experiment. Follow
    // combinational assignments so an output alias or a short combinational chain cannot hide
    // the dependency.
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

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& pair : logicClasses.m_comb) {
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

        for (const auto& pair : logicClasses.m_clocked) {
            if (findBoundaryScope(pair.first) == candidate.m_scopep) continue;
            pair.second->sentreep()->foreach([&](AstNodeVarRef* refp) {
                if (tainted.count(refp->varScopep())) {
                    reject(candidate, "boundary value used as clock");
                }
            });
        }
    }

    std::map<AstScope*, size_t> accepted;
    uint64_t rejected = 0;
    for (EarlyCandidate& candidate : candidates) {
        if (!candidate.m_rejection.empty()) {
            ++rejected;
            UINFO(4, "Subgraph early scheduling fallback for " << candidate.m_scopep->name()
                                                               << ": " << candidate.m_rejection);
            continue;
        }
        const size_t groupIndex = m_impl->m_groups.size();
        m_impl->m_groups.emplace_back();
        Impl::Group& group = m_impl->m_groups[groupIndex];
        group.m_scopep = candidate.m_scopep;
        accepted.emplace(candidate.m_scopep, groupIndex);
    }

    const auto extract = [&](LogicByScope& source, bool clocked) {
        LogicByScope parent;
        parent.reserve(source.size());
        for (const auto& pair : source) {
            const auto it = accepted.find(findBoundaryScope(pair.first));
            if (it == accepted.end()) {
                parent.emplace_back(pair);
                continue;
            }
            Impl::Group& group = m_impl->m_groups[it->second];
            LogicByScope& target = clocked ? group.m_clocked : group.m_comb;
            target.emplace_back(pair);
        }
        source = std::move(parent);
    };
    extract(logicClasses.m_clocked, true);
    extract(logicClasses.m_comb, false);

    uint64_t clocked = 0;
    uint64_t combinational = 0;
    for (const Impl::Group& group : m_impl->m_groups) {
        clocked += group.m_clocked.size();
        combinational += group.m_comb.size();
    }
    V3Stats::addStat("Scheduling, Subgraph early candidates", candidates.size());
    V3Stats::addStat("Scheduling, Subgraph early groups", m_impl->m_groups.size());
    V3Stats::addStat("Scheduling, Subgraph early fallbacks", rejected);
    V3Stats::addStat("Scheduling, Subgraph early clocked actives", clocked);
    V3Stats::addStat("Scheduling, Subgraph early combinational actives", combinational);
}

SubgraphPlan::~SubgraphPlan() = default;

void SubgraphPlan::breakCycles(AstNetlist* netlistp) {
    for (Impl::Group& group : m_impl->m_groups) {
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
        std::unordered_map<AstVarScope*, size_t> useIndex;
        const auto collect = [&](const LogicByScope& lbs) {
            lbs.foreachLogic([&](AstNode* nodep) {
                nodep->foreach([&](AstNodeVarRef* refp) {
                    AstVarScope* const vscp = refp->varScopep();
                    const auto inserted = useIndex.emplace(vscp, group.m_uses.size());
                    if (inserted.second) group.m_uses.emplace_back(Use{vscp, false, false});
                    Use& use = group.m_uses[inserted.first->second];
                    use.m_read |= refp->access().isReadOrRW();
                    use.m_write |= refp->access().isWriteOrRW();
                });
            });
        };
        collect(group.m_regions.m_nba);
        collect(group.m_replicas.m_nba);
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
    }
}

void lowerSubgraphNbaLogic(AstNetlist* netlistp, const std::vector<LogicByScope*>& logic,
                           const V3Order::TrigToSenMap& trigToSen,
                           const CovergroupRefBindings& cgRefBindings, bool slow,
                           const V3Order::ExternalDomainsProvider& externalDomains) {
    if (!v3Global.opt.subgraphSchedule()) return;

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
    unsigned groupIndex = 0;
    for (SubgraphGroup& group : groups) {
        orderedLogic += group.m_preLogic.size() + group.m_postLogic.size();
        UASSERT_OBJ(group.m_senTreep, group.m_boundaryScopep,
                    "Subgraph NBA logic has no clocked sensitivity");

        const auto orderPhase = [&](LogicByScope& phaseLogic, const string& phase, bool post) {
            if (phaseLogic.empty()) return;
            const string tag = "nba_subgraph_" + phase + "_" + cvtToStr(groupIndex);
            AstCFunc* const funcp
                = V3Order::order(netlistp, {&phaseLogic}, trigToSen, cgRefBindings, tag, false,
                                 slow, externalDomains, group.m_boundaryScopep);
            if (!funcp) return;
            util::splitCheck(funcp);

            AstActive* const wrapperp
                = new AstActive{group.m_filelinep, "subgraph", group.m_senTreep};
            AstNode* const callp = util::callVoidFunc(funcp);
            if (post) {
                AstAlwaysPost* const postp = new AstAlwaysPost{group.m_filelinep};
                postp->addStmtsp(callp);
                wrapperp->addStmtsp(postp);
            } else {
                wrapperp->addStmtsp(callp);
            }
            group.m_ownerp->emplace_back(group.m_boundaryScopep, wrapperp);
        };

        orderPhase(group.m_preLogic, "pre", false);
        orderPhase(group.m_postLogic, "post", true);
        ++groupIndex;
    }

    V3Stats::addStat("Scheduling, Subgraph NBA groups", groups.size());
    V3Stats::addStat("Scheduling, Subgraph NBA internal actives", orderedLogic);
}

}  // namespace V3Sched
