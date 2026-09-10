// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Materialize independent subgraph calls
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
// Replace supported module procedures by thin entry calls before scope replication. The call
// arguments keep real variable references in the AST, so port lowering and scoping relink them
// normally. Resolve the calls immediately after scoping, constructing each shared body once
// from the immutable schedule. Parent optimization can change actual arguments, never formals.
// Support input-clock events and unsigned scalar operations. Unsupported specializations
// keep their original procedures. NBA commit calls become AstAlwaysPost in V3Active.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphLower.h"

#include "V3Stats.h"

#include <map>
#include <set>

namespace {

using Templates = V3SubgraphTemplates;
using Id = Templates::Id;

VAccess access(const Templates::Schedule::Use& use) {
    return use.m_write ? (use.m_read ? VAccess::READWRITE : VAccess::WRITE) : VAccess::READ;
}

std::string eligibility(const Templates::Module& module) {
    if (v3Global.opt.debugSubgraphFreshOrder()) return "fresh order";
    if (v3Global.opt.trace() || v3Global.opt.coverage() || v3Global.opt.threads() != 1)
        return "instrumentation or threads";
    if (!module.m_schedule.m_rejection.empty()) return "schedule";
    if (module.m_schedule.m_triggers.empty() || module.m_schedule.m_pre.empty())
        return "clock shape";
    const std::set<std::string> supported{
        "ADD",   "ALWAYS", "AND",     "ASSIGN",  "ASSIGNDLY",     "ASSIGNW", "BLOCK", "COND",
        "CONST", "EQ",     "IF",      "INITIAL", "INITIALSTATIC", "LOGNOT",  "NEQ",   "NOT",
        "OR",    "SEL",    "SENITEM", "SENTREE", "VARREF",        "XOR"};
    for (const Templates::Node& node : module.m_nodes) {
        if (!supported.count(node.m_kind)) return "expression";
        if (node.m_type.m_width > 64 || (node.m_type.m_signed && node.m_kind != "CONST"))
            return "numeric type";
    }
    for (const Templates::Schedule::Storage& storage : module.m_schedule.m_storage) {
        const Templates::Type& type = module.m_slots[storage.m_slot - 1].m_type;
        if (type.m_width > 64 || type.m_signed) return "storage type";
    }
    return "";
}

class PrepareVisitor final : public VNVisitor {
    const Templates& m_templates;
    std::map<std::string, Id> m_ids;
    std::set<Id> m_rejectedBindings;
    std::map<std::string, uint64_t> m_rejections;
    uint64_t m_activated = 0;

    void visit(AstNodeModule* nodep) override {
        const auto it = m_ids.find(nodep->name());
        if (it == m_ids.end()) return;
        const Id templateId = it->second;
        const Templates::Module& module = m_templates.modules()[templateId - 1];
        const std::string reason = eligibility(module);
        if (!reason.empty() || m_rejectedBindings.count(templateId)) {
            ++m_rejections[reason.empty() ? "binding" : reason];
            return;
        }
        std::map<std::string, AstVar*> vars;
        for (AstNode* stmtp = nodep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (AstVar* const varp = VN_CAST(stmtp, Var)) vars.emplace(varp->name(), varp);
        }
        std::vector<AstVar*> storage;
        std::vector<AstVar*> current(module.m_slots.size() + 1, nullptr);
        for (const Templates::Schedule::Storage& item : module.m_schedule.m_storage) {
            AstVar* varp = vars.at(module.m_slots[item.m_slot - 1].m_name);
            if (item.m_pending) {
                varp = new AstVar{nodep->fileline(), VVarType::MODULETEMP,
                                  "__VsubgraphPending" + cvtToStr(item.m_slot), varp->dtypep()};
                varp->subgraphPending(true);
                nodep->addStmtsp(varp);
            } else {
                current[item.m_slot] = varp;
            }
            storage.push_back(varp);
        }
        // The original body is now owned by the IR. Only wrappers will be replicated by V3Scope.
        for (AstNode* stmtp = nodep->stmtsp(); stmtp;) {
            AstNode* const nextp = stmtp->nextp();
            if (VN_IS(stmtp, NodeProcedure)) pushDeletep(stmtp->unlinkFrBack());
            stmtp = nextp;
        }
        FileLine* const flp = nodep->fileline();
        for (size_t index = 0; index < module.m_schedule.m_entries.size(); ++index) {
            const Templates::Schedule::Entry& entry = module.m_schedule.m_entries[index];
            AstSubgraphCall* const callp
                = new AstSubgraphCall{flp, templateId, static_cast<Id>(index + 1)};
            for (const Templates::Schedule::Use& use : entry.m_uses) {
                callp->addArgsp(new AstVarRef{flp, storage[use.m_storage - 1], access(use)});
            }
            if (entry.m_phase == "static") {
                nodep->addStmtsp(new AstInitialStatic{flp, callp});
            } else if (entry.m_phase == "initial") {
                nodep->addStmtsp(new AstInitial{flp, callp});
            } else {
                AstSenTree* treep = nullptr;
                for (const Id triggerId : entry.m_triggers) {
                    const Templates::Trigger& trigger
                        = module.m_schedule.m_triggers[triggerId - 1];
                    AstSenItem* const itemp = new AstSenItem{
                        flp,
                        trigger.m_edge == "POS" ? VEdgeType::ET_POSEDGE : VEdgeType::ET_NEGEDGE,
                        new AstVarRef{flp, current[trigger.m_slot], VAccess::READ}};
                    if (!treep)
                        treep = new AstSenTree{flp, itemp};
                    else
                        treep->addSensesp(itemp);
                }
                AstAlways* const alwaysp = new AstAlways{
                    flp, treep ? VAlwaysKwd::ALWAYS : VAlwaysKwd::CONT_ASSIGN, treep, callp};
                alwaysp->subgraphPost(entry.m_phase == "commit");
                nodep->addStmtsp(alwaysp);
            }
        }
        ++m_activated;
    }
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    PrepareVisitor(AstNetlist* netlistp, const Templates& templates)
        : m_templates{templates} {
        for (size_t index = 0; index < templates.modules().size(); ++index)
            m_ids.emplace(templates.modules()[index].m_name, static_cast<Id>(index + 1));
        for (const Templates::Instance& instance : templates.instances()) {
            if (!instance.m_abiRejection.empty()) m_rejectedBindings.insert(instance.m_template);
        }
        iterate(netlistp);
        V3Stats::addStat("Scheduling, Subgraph template schedules activated", m_activated);
        for (const auto& pair : m_rejections)
            V3Stats::addStat("Scheduling, Subgraph template activation rejection, " + pair.first,
                             pair.second);
    }
};

class BodyBuilder final {
    const Templates::Module& m_module;
    FileLine* const m_flp;
    AstCFunc* const m_funcp;
    std::vector<AstVarScope*> m_current;
    std::vector<AstVarScope*> m_pending;

    const Templates::Node& node(Id id) const { return m_module.m_nodes.at(id - 1); }
    AstNodeExpr* expression(Id id) {
        const Templates::Node& expr = node(id);
        if (expr.m_kind == "VARREF") {
            AstVarScope* const vscp = m_current[expr.m_slot];
            if (!vscp) return expression(m_module.m_slots[expr.m_slot - 1].m_initializer);
            return new AstVarRef{m_flp, vscp, VAccess::READ};
        }
        AstNodeExpr* resultp = nullptr;
        if (expr.m_kind == "CONST") {
            resultp = new AstConst{
                m_flp, V3Number{m_flp, V3Number::VerilogNumberLiteral{}, expr.m_value.c_str()}};
        } else if (expr.m_kind == "COND") {
            resultp = new AstCond{m_flp, expression(expr.m_operands[0]),
                                  expression(expr.m_operands[1]), expression(expr.m_operands[2])};
        } else if (expr.m_kind == "SEL") {
            resultp = new AstSel{m_flp, expression(expr.m_operands[0]),
                                 expression(expr.m_operands[1]), expr.m_selectWidth};
        } else if (expr.m_kind == "LOGNOT") {
            resultp = new AstLogNot{m_flp, expression(expr.m_operands[0])};
        } else if (expr.m_kind == "NOT") {
            resultp = new AstNot{m_flp, expression(expr.m_operands[0])};
        } else {
            AstNodeExpr* const lhsp = expression(expr.m_operands[0]);
            AstNodeExpr* const rhsp = expression(expr.m_operands[1]);
            if (expr.m_kind == "ADD")
                resultp = new AstAdd{m_flp, lhsp, rhsp};
            else if (expr.m_kind == "AND")
                resultp = new AstAnd{m_flp, lhsp, rhsp};
            else if (expr.m_kind == "OR")
                resultp = new AstOr{m_flp, lhsp, rhsp};
            else if (expr.m_kind == "XOR")
                resultp = new AstXor{m_flp, lhsp, rhsp};
            else if (expr.m_kind == "EQ")
                resultp = new AstEq{m_flp, lhsp, rhsp};
            else if (expr.m_kind == "NEQ")
                resultp = new AstNeq{m_flp, lhsp, rhsp};
            else
                m_funcp->v3fatalSrc("Unexpected independent template expression");
        }
        resultp->dtypeSetLogicSized(expr.m_type.m_width,
                                    expr.m_type.m_signed ? VSigning::SIGNED : VSigning::UNSIGNED);
        return resultp;
    }

    AstNode* statements(Id id, bool nba) {
        if (!id) return nullptr;
        const Templates::Node& stmt = node(id);
        if (stmt.m_kind == "BLOCK") {
            AstNode* resultp = nullptr;
            for (const Id child : stmt.m_operands) {
                AstNode* const childp = statements(child, nba);
                resultp = AstNode::addNextNull(resultp, childp);
            }
            return resultp;
        }
        if (stmt.m_kind == "IF")
            return new AstIf{m_flp, expression(stmt.m_operands[0]),
                             statements(stmt.m_operands[1], nba),
                             statements(stmt.m_operands[2], nba)};
        const Id slot = node(stmt.m_operands[0]).m_slot;
        AstVarScope* const destp = nba ? m_pending[slot] : m_current[slot];
        UASSERT_OBJ(destp, m_funcp, "Template destination has no ABI argument");
        return new AstAssign{m_flp, new AstVarRef{m_flp, destp, VAccess::WRITE},
                             expression(stmt.m_operands[1])};
    }

public:
    BodyBuilder(const Templates::Module& module, const Templates::Schedule::Entry& entry,
                AstCFunc* funcp, AstSubgraphCall* callp)
        : m_module{module}
        , m_flp{funcp->fileline()}
        , m_funcp{funcp}
        , m_current{module.m_slots.size() + 1, nullptr}
        , m_pending{m_current.size(), nullptr} {
        AstNodeExpr* actualp = callp->argsp();
        for (const Templates::Schedule::Use& use : entry.m_uses) {
            UASSERT_OBJ(actualp, callp, "Missing template argument");
            AstVar* const varp = new AstVar{m_flp, VVarType::BLOCKTEMP,
                                            funcp->name() + "__Varg" + cvtToStr(use.m_storage),
                                            actualp->dtypep()};
            varp->direction(use.m_write ? (use.m_read ? VDirection::INOUT : VDirection::OUTPUT)
                                        : VDirection::CONSTREF);
            varp->funcLocal(true);
            funcp->addArgsp(varp);
            AstVarScope* const vscp = new AstVarScope{m_flp, funcp->scopep(), varp};
            funcp->scopep()->addVarsp(vscp);
            const Templates::Schedule::Storage& storage
                = module.m_schedule.m_storage[use.m_storage - 1];
            (storage.m_pending ? m_pending : m_current)[storage.m_slot] = vscp;
            actualp = VN_CAST(actualp->nextp(), NodeExpr);
        }
        const std::vector<Templates::Process>& processes
            = entry.m_phase == "static"    ? module.m_schedule.m_static
              : entry.m_phase == "initial" ? module.m_schedule.m_initial
              : entry.m_phase == "refresh" ? module.m_schedule.m_refresh
                                           : module.m_schedule.m_pre;
        const Templates::Process& process = processes[entry.m_process - 1];
        if (entry.m_phase == "pre" || entry.m_phase == "commit") {
            for (const Id slot : process.m_writes) {
                const bool pre = entry.m_phase == "pre";
                funcp->addStmtsp(new AstAssign{
                    m_flp,
                    new AstVarRef{m_flp, pre ? m_pending[slot] : m_current[slot], VAccess::WRITE},
                    new AstVarRef{m_flp, pre ? m_current[slot] : m_pending[slot], VAccess::READ}});
            }
        }
        if (entry.m_phase != "commit")
            funcp->addStmtsp(statements(process.m_body, entry.m_phase == "pre"));
    }
};

class ResolveVisitor final : public VNVisitor {
    const Templates& m_templates;
    AstScope* const m_topScopep;
    std::map<std::pair<Id, Id>, AstCFunc*> m_funcs;
    uint64_t m_calls = 0;

    void visit(AstSubgraphCall* nodep) override {
        const Templates::Module& module = m_templates.modules()[nodep->templateId() - 1];
        const Templates::Schedule::Entry& entry
            = module.m_schedule.m_entries[nodep->entryId() - 1];
        AstCFunc*& funcp = m_funcs[std::make_pair(nodep->templateId(), nodep->entryId())];
        if (!funcp) {
            funcp = new AstCFunc{nodep->fileline(),
                                 "__VsubgraphTemplate" + cvtToStr(nodep->templateId()) + "__"
                                     + cvtToStr(nodep->entryId()),
                                 m_topScopep};
            funcp->isStatic(true);
            funcp->isLoose(true);
            funcp->dontCombine(true);
            funcp->subgraphTemplate(true);
            // Each call binds different storage. Do not trace formal references across calls.
            funcp->noLife(true);
            funcp->slow(entry.m_phase == "static" || entry.m_phase == "initial");
            m_topScopep->addBlocksp(funcp);
            const BodyBuilder builder{module, entry, funcp, nodep};
        }
        AstCCall* const callp = new AstCCall{nodep->fileline(), funcp};
        callp->dtypeSetVoid();
        if (nodep->argsp()) callp->addArgsp(nodep->argsp()->unlinkFrBackWithNext());
        nodep->replaceWith(new AstStmtExpr{nodep->fileline(), callp});
        pushDeletep(nodep);
        ++m_calls;
    }
    void visit(AstCFunc*) override {}
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

public:
    ResolveVisitor(AstNetlist* netlistp, const Templates& templates)
        : m_templates{templates}
        , m_topScopep{netlistp->topScopep()->scopep()} {
        iterate(netlistp);
        V3Stats::addStat("Scheduling, Subgraph template shared bodies materialized",
                         m_funcs.size());
        V3Stats::addStat("Scheduling, Subgraph template entry calls materialized", m_calls);
    }
};

}  // namespace

void V3SubgraphLower::prepare(AstNetlist* netlistp, const V3SubgraphTemplates& templates) {
    const PrepareVisitor visitor{netlistp, templates};
}

void V3SubgraphLower::resolve(AstNetlist* netlistp, const V3SubgraphTemplates& templates) {
    const ResolveVisitor visitor{netlistp, templates};
}
