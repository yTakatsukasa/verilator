// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Materialize shared subgraph V3Ast entry functions
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
// Replace an eligible specialization's procedures with thin phase calls before V3Scope replicates
// them. After scoping, clone each phase body directly from the detached template V3Ast into one
// shared AstCFunc and bind every instance's state through ordinary AstVarRef arguments.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphLower.h"

#include "V3Stats.h"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

using Ast = V3SubgraphAst;

VAccess access(const Ast::Schedule::Use& use) {
    return use.m_write ? (use.m_read ? VAccess::READWRITE : VAccess::WRITE) : VAccess::READ;
}

const Ast::Process& process(const Ast::Schedule& schedule, const Ast::Schedule::Entry& entry) {
    const std::vector<Ast::Process>* processesp = nullptr;
    switch (entry.m_phase) {
    case Ast::Phase::STATIC: processesp = &schedule.m_static; break;
    case Ast::Phase::INITIAL: processesp = &schedule.m_initial; break;
    case Ast::Phase::PRE:
    case Ast::Phase::COMMIT: processesp = &schedule.m_pre; break;
    case Ast::Phase::REFRESH: processesp = &schedule.m_refresh; break;
    }
    UASSERT(processesp && entry.m_process && entry.m_process <= processesp->size(),
            "Invalid subgraph V3Ast process ID");
    return processesp->at(entry.m_process - 1);
}

std::string eligibility(const Ast::Template& item) {
    static constexpr size_t kMaxEntryArgs = 8;
    if (v3Global.opt.debugSubgraphFreshOrder()) return "fresh order";
    if (v3Global.opt.trace() || v3Global.opt.coverage() || v3Global.opt.threads() != 1)
        return "instrumentation or threads";
    if (!item.m_schedule.m_rejection.empty()) return "schedule";
    if (item.m_schedule.m_triggers.empty() || item.m_schedule.m_pre.empty()) return "clock shape";
    for (const Ast::Schedule::Entry& entry : item.m_schedule.m_entries) {
        if (entry.m_uses.size() > kMaxEntryArgs) return "oversized ABI";
    }
    return "";
}

AstVar* sourceVar(const Ast::Template& item, const AstVar* formalp) {
    for (const Ast::Template::Variable& variable : item.m_variables) {
        if (variable.m_formalp == formalp) return variable.m_sourcep;
    }
    return nullptr;
}

class PrepareVisitor final : public VNVisitor {
    const Ast& m_ast;
    std::unordered_map<const AstNodeModule*, uint32_t> m_ids;
    std::map<std::string, uint64_t> m_rejections;
    std::map<std::string, uint64_t> m_rejectedInstances;
    uint64_t m_activated = 0;
    uint64_t m_activatedInstances = 0;

    void visit(AstNodeModule* nodep) override {
        const auto idIt = m_ids.find(nodep);
        if (idIt == m_ids.end()) return;
        const uint32_t templateId = idIt->second;
        const Ast::Template& item = m_ast.templates()[templateId - 1];
        const std::string reason = eligibility(item);
        if (!reason.empty()) {
            ++m_rejections[reason];
            m_rejectedInstances[reason] += item.m_instances;
            return;
        }

        const Ast::Schedule& schedule = item.m_schedule;
        std::vector<AstVar*> storage;
        storage.reserve(schedule.m_storage.size());
        std::unordered_map<const AstVar*, AstVar*> current;
        for (const Ast::Schedule::Storage& slot : schedule.m_storage) {
            AstVar* sourcep = sourceVar(item, slot.m_formalp);
            UASSERT_OBJ(sourcep, nodep, "Subgraph V3Ast storage has no source declaration");
            if (slot.m_pending) {
                sourcep = new AstVar{nodep->fileline(), VVarType::MODULETEMP,
                                     "__VsubgraphPending" + cvtToStr(storage.size() + 1),
                                     sourcep->dtypep()};
                sourcep->subgraphPending(true);
                nodep->addStmtsp(sourcep);
            } else {
                current.emplace(slot.m_formalp, sourcep);
            }
            storage.push_back(sourcep);
        }

        for (AstNode* stmtp = nodep->stmtsp(); stmtp;) {
            AstNode* const nextp = stmtp->nextp();
            if (VN_IS(stmtp, NodeProcedure)) pushDeletep(stmtp->unlinkFrBack());
            stmtp = nextp;
        }

        FileLine* const flp = nodep->fileline();
        for (size_t index = 0; index < schedule.m_entries.size(); ++index) {
            const Ast::Schedule::Entry& entry = schedule.m_entries[index];
            AstSubgraphCall* const callp
                = new AstSubgraphCall{flp, templateId, static_cast<uint32_t>(index + 1)};
            for (const Ast::Schedule::Use& use : entry.m_uses) {
                callp->addArgsp(new AstVarRef{flp, storage.at(use.m_storage - 1), access(use)});
            }
            if (entry.m_phase == Ast::Phase::STATIC) {
                nodep->addStmtsp(new AstInitialStatic{flp, callp});
            } else if (entry.m_phase == Ast::Phase::INITIAL) {
                nodep->addStmtsp(new AstInitial{flp, callp});
            } else {
                AstSenTree* treep = nullptr;
                for (const uint32_t triggerId : entry.m_triggers) {
                    const Ast::Trigger& trigger = schedule.m_triggers.at(triggerId - 1);
                    AstVar* const varp = current.at(trigger.m_formalp);
                    AstSenItem* const senp = new AstSenItem{
                        flp, trigger.m_posedge ? VEdgeType::ET_POSEDGE : VEdgeType::ET_NEGEDGE,
                        new AstVarRef{flp, varp, VAccess::READ}};
                    if (!treep)
                        treep = new AstSenTree{flp, senp};
                    else
                        treep->addSensesp(senp);
                }
                AstAlways* const alwaysp = new AstAlways{
                    flp, treep ? VAlwaysKwd::ALWAYS : VAlwaysKwd::CONT_ASSIGN, treep, callp};
                alwaysp->subgraphPost(entry.m_phase == Ast::Phase::COMMIT);
                nodep->addStmtsp(alwaysp);
            }
        }
        ++m_activated;
        m_activatedInstances += item.m_instances;
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    PrepareVisitor(AstNetlist* netlistp, const Ast& ast)
        : m_ast{ast} {
        for (const Ast::Template& item : ast.templates()) m_ids.emplace(item.m_sourcep, item.m_id);
        iterate(netlistp);
        V3Stats::addStat("Scheduling, Subgraph V3Ast schedules activated", m_activated);
        V3Stats::addStat("Scheduling, Subgraph V3Ast instances activated", m_activatedInstances);
        uint64_t fallbackSchedules = 0;
        uint64_t fallbackInstances = 0;
        for (const auto& pair : m_rejections) {
            V3Stats::addStat("Scheduling, Subgraph V3Ast activation rejection, " + pair.first,
                             pair.second);
            fallbackSchedules += pair.second;
        }
        for (const auto& pair : m_rejectedInstances) {
            V3Stats::addStat("Scheduling, Subgraph V3Ast activation rejection instances, "
                                 + pair.first,
                             pair.second);
            fallbackInstances += pair.second;
        }
        V3Stats::addStat("Scheduling, Subgraph V3Ast schedules fallback", fallbackSchedules);
        V3Stats::addStat("Scheduling, Subgraph V3Ast instances fallback", fallbackInstances);
    }
};

class BodyRelinker final : public VNVisitor {
    const std::unordered_map<const AstVar*, AstVarScope*>& m_current;
    const std::unordered_map<const AstVar*, AstVarScope*>& m_pending;
    const bool m_pre;

    void visit(AstVarRef* nodep) override {
        if (nodep->varScopep()) return;  // Already visited through an AstAssignDly replacement
        const AstVar* const formalp = nodep->varp();
        const bool write = nodep->access().isWriteOrRW();
        const auto& vars = m_pre && write ? m_pending : m_current;
        const auto it = vars.find(formalp);
        UASSERT_OBJ(it != vars.end(), nodep, "Template variable is absent from phase ABI");
        nodep->varp(it->second->varp());
        nodep->varScopep(it->second);
        nodep->classOrPackagep(nullptr);
    }
    void visit(AstAssignDly* nodep) override {
        iterateChildren(nodep);
        AstNodeExpr* const lhsp = nodep->lhsp()->unlinkFrBack();
        AstNodeExpr* const rhsp = nodep->rhsp()->unlinkFrBack();
        nodep->replaceWith(new AstAssign{nodep->fileline(), lhsp, rhsp});
        pushDeletep(nodep);
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    BodyRelinker(AstNode* nodep, const std::unordered_map<const AstVar*, AstVarScope*>& current,
                 const std::unordered_map<const AstVar*, AstVarScope*>& pending, bool pre)
        : m_current{current}
        , m_pending{pending}
        , m_pre{pre} {
        iterateAndNextNull(nodep);
    }
};

class BodyBuilder final {
public:
    BodyBuilder(const Ast::Template& item, const Ast::Schedule::Entry& entry, AstCFunc* funcp,
                AstSubgraphCall* callp) {
        const Ast::Schedule& schedule = item.m_schedule;
        const Ast::Process& processItem = process(schedule, entry);
        FileLine* const flp = funcp->fileline();
        std::unordered_map<const AstVar*, AstVarScope*> current;
        std::unordered_map<const AstVar*, AstVarScope*> pending;
        AstNodeExpr* actualp = callp->argsp();
        for (const Ast::Schedule::Use& use : entry.m_uses) {
            UASSERT_OBJ(actualp, callp, "Missing subgraph V3Ast phase argument");
            const Ast::Schedule::Storage& storage = schedule.m_storage.at(use.m_storage - 1);
            AstVar* const varp = new AstVar{flp, VVarType::BLOCKTEMP,
                                            funcp->name() + "__Varg" + cvtToStr(use.m_storage),
                                            actualp->dtypep()};
            varp->direction(use.m_write ? (use.m_read ? VDirection::INOUT : VDirection::OUTPUT)
                                        : VDirection::CONSTREF);
            varp->funcLocal(true);
            funcp->addArgsp(varp);
            AstVarScope* const vscp = new AstVarScope{flp, funcp->scopep(), varp};
            funcp->scopep()->addVarsp(vscp);
            (storage.m_pending ? pending : current).emplace(storage.m_formalp, vscp);
            actualp = VN_CAST(actualp->nextp(), NodeExpr);
        }

        if (entry.m_phase == Ast::Phase::PRE) {
            for (AstVar* const formalp : processItem.m_writes) {
                funcp->addStmtsp(
                    new AstAssign{flp, new AstVarRef{flp, pending.at(formalp), VAccess::WRITE},
                                  new AstVarRef{flp, current.at(formalp), VAccess::READ}});
            }
        }
        if (entry.m_phase == Ast::Phase::COMMIT) {
            for (AstVar* const formalp : processItem.m_writes) {
                funcp->addStmtsp(
                    new AstAssign{flp, new AstVarRef{flp, current.at(formalp), VAccess::WRITE},
                                  new AstVarRef{flp, pending.at(formalp), VAccess::READ}});
            }
            return;
        }

        AstNode* const bodyp = processItem.m_procedurep->stmtsp()->cloneTree(true);
        funcp->addStmtsp(bodyp);
        const BodyRelinker relinker{bodyp, current, pending, entry.m_phase == Ast::Phase::PRE};
    }
};

class ResolveVisitor final : public VNVisitor {
    const Ast& m_ast;
    AstScope* const m_topScopep;
    std::map<std::pair<uint32_t, uint32_t>, AstCFunc*> m_funcs;
    uint64_t m_calls = 0;
    uint64_t m_bodyArguments = 0;
    uint64_t m_callArguments = 0;
    uint64_t m_maxBodyArguments = 0;

    void visit(AstSubgraphCall* nodep) override {
        const Ast::Template& item = m_ast.templates().at(nodep->templateId() - 1);
        const Ast::Schedule::Entry& entry = item.m_schedule.m_entries.at(nodep->entryId() - 1);
        AstCFunc*& funcp = m_funcs[std::make_pair(nodep->templateId(), nodep->entryId())];
        if (!funcp) {
            funcp = new AstCFunc{nodep->fileline(),
                                 "__VsubgraphV3Ast" + cvtToStr(nodep->templateId()) + "__"
                                     + cvtToStr(nodep->entryId()),
                                 m_topScopep};
            funcp->isStatic(true);
            funcp->isLoose(true);
            funcp->dontCombine(true);
            funcp->noLife(true);
            funcp->subgraphTemplate(true);
            funcp->slow(entry.m_phase == Ast::Phase::STATIC
                        || entry.m_phase == Ast::Phase::INITIAL);
            m_topScopep->addBlocksp(funcp);
            const BodyBuilder builder{item, entry, funcp, nodep};
            m_bodyArguments += entry.m_uses.size();
            m_maxBodyArguments = std::max<uint64_t>(m_maxBodyArguments, entry.m_uses.size());
        }
        AstCCall* const callp = new AstCCall{nodep->fileline(), funcp};
        callp->dtypeSetVoid();
        if (nodep->argsp()) callp->addArgsp(nodep->argsp()->unlinkFrBackWithNext());
        nodep->replaceWith(new AstStmtExpr{nodep->fileline(), callp});
        pushDeletep(nodep);
        ++m_calls;
        m_callArguments += entry.m_uses.size();
    }
    void visit(AstCFunc*) override {}
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    ResolveVisitor(AstNetlist* netlistp, const Ast& ast)
        : m_ast{ast}
        , m_topScopep{netlistp->topScopep()->scopep()} {
        iterate(netlistp);
        V3Stats::addStat("Scheduling, Subgraph V3Ast shared bodies", m_funcs.size());
        V3Stats::addStat("Scheduling, Subgraph V3Ast entry calls", m_calls);
        V3Stats::addStat("Scheduling, Subgraph V3Ast shared body arguments", m_bodyArguments);
        V3Stats::addStat("Scheduling, Subgraph V3Ast shared body max arguments",
                         m_maxBodyArguments);
        V3Stats::addStat("Scheduling, Subgraph V3Ast entry call arguments", m_callArguments);
    }
};

}  // namespace

void V3SubgraphLower::prepare(AstNetlist* netlistp, const V3SubgraphAst& ast) {
    const VlOs::DeltaWallTime timer{v3Global.opt.stats()};
    const PrepareVisitor visitor{netlistp, ast};
    V3Stats::addStatPerf("Scheduling, Subgraph V3Ast prepare time (sec)", timer.deltaTime());
}

void V3SubgraphLower::resolve(AstNetlist* netlistp, const V3SubgraphAst& ast) {
    const VlOs::DeltaWallTime timer{v3Global.opt.stats()};
    const ResolveVisitor visitor{netlistp, ast};
    V3Stats::addStatPerf("Scheduling, Subgraph V3Ast resolve time (sec)", timer.deltaTime());
}
