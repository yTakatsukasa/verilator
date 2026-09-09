// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Capture independent subgraph templates before scoping
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
// Capture a leaf module once after elaboration, but before V3Inst converts its ports to parent
// assignments and V3Scope clones its procedures. Scalar declarations, expressions, event lists,
// and procedures become owning records with local slot/node IDs. No AST or dtype pointer escapes
// capture. Unsupported constructs reject the entire template, never a portion of its behavior.
// The existing scheduling path remains active until independent scheduling and binding are ready.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphTemplates.h"

#include "V3File.h"
#include "V3Stats.h"
#include "V3SubgraphSchedule.h"

#include <map>
#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

using Templates = V3SubgraphTemplates;
using Id = Templates::Id;

class TemplateBuilder final {
    Templates::Module m_module;
    std::map<const AstVar*, Id> m_slots;
    std::string m_rejection;
    bool m_binding = false;
    std::vector<Templates::Instance::Connection> m_connections;

    Id reject(const std::string& reason) {
        if (m_rejection.empty()) m_rejection = reason;
        return Templates::NONE;
    }

    Templates::Type type(const AstNodeDType* dtypep) {
        Templates::Type result;
        if (!dtypep) return result;
        // Do not flatten enums, typedefs, aggregates, or opaque types into a matching bit width.
        const AstBasicDType* const basicp = VN_CAST(dtypep, BasicDType);
        if (!basicp || !basicp->keyword().isIntNumeric()) {
            reject("dtype");
            return result;
        }
        result.m_keyword = basicp->keyword().ascii();
        result.m_width = basicp->width();
        result.m_signed = basicp->isSigned();
        result.m_ranged = basicp->isRanged();
        result.m_left = basicp->left();
        result.m_right = basicp->right();
        return result;
    }

    Id add(Templates::Node&& node) {
        UASSERT(m_module.m_nodes.size() < std::numeric_limits<Id>::max(),
                "Subgraph template node limit exceeded");
        m_module.m_nodes.push_back(std::move(node));
        return static_cast<Id>(m_module.m_nodes.size());
    }

    Id expression(const AstNodeExpr* nodep) {
        if (!nodep || !m_rejection.empty()) return Templates::NONE;
        Templates::Node node;
        node.m_kind = nodep->typeName();
        node.m_type = type(nodep->dtypep());
        if (const AstConst* const constp = VN_CAST(nodep, Const)) {
            if (constp->num().isOpaque()) return reject("constant");
            node.m_value = constp->num().ascii();
        } else if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            if (m_binding && !refp->classOrPackagep() && !m_slots.count(refp->varp())) {
                Templates::Slot slot;
                slot.m_name = refp->varp()->name();
                slot.m_type = type(refp->varp()->dtypep());
                m_module.m_slots.push_back(std::move(slot));
                m_slots.emplace(refp->varp(), static_cast<Id>(m_module.m_slots.size()));
            }
            const auto it = m_slots.find(refp->varp());
            if (it == m_slots.end() || refp->classOrPackagep())
                return reject("external reference");
            node.m_slot = it->second;
            node.m_value = refp->access().ascii();
        } else if (const AstSel* const selp = VN_CAST(nodep, Sel)) {
            node.m_operands = {expression(selp->fromp()), expression(selp->lsbp())};
            node.m_selectWidth = selp->widthConst();
        } else if (const AstCond* const condp = VN_CAST(nodep, Cond)) {
            node.m_operands = {expression(condp->condp()), expression(condp->thenp()),
                               expression(condp->elsep())};
        } else if (VN_IS(nodep, Not) || VN_IS(nodep, LogNot) || VN_IS(nodep, Negate)
                   || VN_IS(nodep, Extend) || VN_IS(nodep, ExtendS) || VN_IS(nodep, RedAnd)
                   || VN_IS(nodep, RedOr) || VN_IS(nodep, RedXor)) {
            node.m_operands = {expression(VN_AS(nodep, NodeUniop)->lhsp())};
        } else if (VN_IS(nodep, Add) || VN_IS(nodep, Sub) || VN_IS(nodep, Mul) || VN_IS(nodep, And)
                   || VN_IS(nodep, Or) || VN_IS(nodep, Xor) || VN_IS(nodep, LogAnd)
                   || VN_IS(nodep, LogOr) || VN_IS(nodep, Eq) || VN_IS(nodep, Neq)
                   || VN_IS(nodep, Lt) || VN_IS(nodep, Lte) || VN_IS(nodep, Gt)
                   || VN_IS(nodep, Gte) || VN_IS(nodep, ShiftL) || VN_IS(nodep, ShiftR)
                   || VN_IS(nodep, ShiftRS) || VN_IS(nodep, Concat)) {
            const AstNodeBiop* const opp = VN_AS(nodep, NodeBiop);
            node.m_operands = {expression(opp->lhsp()), expression(opp->rhsp())};
        } else {
            return reject(std::string{"expression "} + nodep->typeName());
        }
        return add(std::move(node));
    }

    Id sensitivity(const AstSenTree* treep) {
        if (!treep) return Templates::NONE;
        Templates::Node tree;
        tree.m_kind = "SENTREE";
        if (treep->isMulti()) return reject("merged sensitivity");
        for (const AstSenItem* itemp = treep->sensesp(); itemp;
             itemp = VN_AS(itemp->nextp(), SenItem)) {
            Templates::Node item;
            item.m_kind = "SENITEM";
            item.m_value = itemp->edgeType().ascii();
            item.m_operands = {expression(itemp->sensp()), expression(itemp->condp())};
            tree.m_operands.push_back(add(std::move(item)));
        }
        return add(std::move(tree));
    }

    Id statement(const AstNode* nodep) {
        Templates::Node node;
        node.m_kind = nodep->typeName();
        if (const AstNodeAssign* const assignp = VN_CAST(nodep, NodeAssign)) {
            if (!(VN_IS(nodep, Assign) || VN_IS(nodep, AssignDly) || VN_IS(nodep, AssignW))
                || assignp->timingControlp()) {
                return reject("assignment");
            }
            node.m_operands = {expression(assignp->lhsp()), expression(assignp->rhsp())};
        } else if (const AstIf* const ifp = VN_CAST(nodep, If)) {
            if (ifp->uniquePragma() || ifp->unique0Pragma() || ifp->priorityPragma()) {
                return reject("qualified if");
            }
            node.m_operands
                = {expression(ifp->condp()), statements(ifp->thensp()), statements(ifp->elsesp())};
        } else if (const AstAlways* const alwaysp = VN_CAST(nodep, Always)) {
            node.m_value = alwaysp->keyword().ascii();
            node.m_operands = {sensitivity(alwaysp->sentreep()), statements(alwaysp->stmtsp())};
        } else if (VN_IS(nodep, Initial) || VN_IS(nodep, InitialStatic)) {
            node.m_operands = {statements(VN_AS(nodep, NodeProcedure)->stmtsp())};
        } else {
            return reject(std::string{"statement "} + nodep->typeName());
        }
        return add(std::move(node));
    }

    Id statements(const AstNode* nodep) {
        if (!nodep) return Templates::NONE;
        Templates::Node node;
        node.m_kind = "BLOCK";
        for (; nodep && m_rejection.empty(); nodep = nodep->nextp()) {
            if (const AstVar* const varp = VN_CAST(nodep, Var)) {
                if (!m_slots.count(varp)) return reject("local declaration");
                continue;
            }
            node.m_operands.push_back(statement(nodep));
        }
        return add(std::move(node));
    }

public:
    TemplateBuilder(const AstCell* cellp, const Templates::Module& module)
        : m_binding{true} {
        std::map<std::string, size_t> ports;
        for (size_t index = 0; index < module.m_slots.size(); ++index) {
            const Templates::Slot& slot = module.m_slots[index];
            if (slot.m_direction != "INPUT" && slot.m_direction != "OUTPUT") continue;
            ports.emplace(slot.m_name, m_connections.size());
            m_connections.push_back({static_cast<Id>(index + 1), Templates::NONE});
        }
        if (!cellp) {
            reject("top boundary");
            return;
        }
        for (const AstPin* pinp = cellp->pinsp(); pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
            const auto it = ports.find(pinp->modVarp()->name());
            if (it == ports.end()) {
                reject("port declaration");
                break;
            }
            const AstNodeExpr* const exprp = VN_CAST(pinp->exprp(), NodeExpr);
            if (pinp->exprp() && !exprp) {
                reject("port expression");
                break;
            }
            m_connections[it->second].m_actual = expression(exprp);
        }
    }

    explicit TemplateBuilder(const AstNodeModule* modp) {
        m_module.m_name = modp->name();
        // Declaration collection precedes expression capture, including forward references.
        for (const AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
            const AstVar* const varp = VN_CAST(nodep, Var);
            if (!varp) continue;
            if (varp->isSigPublic() || varp->isForceable() || varp->isInout() || varp->delayp()
                || varp->attrsp() || varp->childDTypep()) {
                reject("declaration");
                break;
            }
            Templates::Slot slot;
            slot.m_name = varp->name();
            slot.m_direction = varp->direction().ascii();
            slot.m_varType = varp->varType().ascii();
            slot.m_type = type(varp->dtypep());
            m_module.m_slots.push_back(std::move(slot));
            m_slots.emplace(varp, static_cast<Id>(m_module.m_slots.size()));
        }
        if (!m_rejection.empty()) return;
        for (const AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
            if (const AstVar* const varp = VN_CAST(nodep, Var)) {
                if (!varp->valuep()) continue;
                const AstNodeExpr* const valuep = VN_CAST(varp->valuep(), NodeExpr);
                if (!valuep) {
                    reject("initializer");
                    return;
                }
                m_module.m_slots.at(m_slots.at(varp) - 1).m_initializer = expression(valuep);
            }
        }
        m_module.m_body = statements(modp->stmtsp());
    }

    const std::string& rejection() const { return m_rejection; }
    Templates::Module take() { return std::move(m_module); }
    void bind(Templates::Instance& instance) {
        instance.m_bindingRejection = m_rejection;
        // A partial binding must never be mistaken for an executable instance.
        if (!m_rejection.empty()) return;
        instance.m_parentSlots = std::move(m_module.m_slots);
        instance.m_nodes = std::move(m_module.m_nodes);
        instance.m_connections = std::move(m_connections);
    }
};

class TemplateCaptureVisitor final : public VNVisitorConst {
    std::vector<Templates::Module>& m_modules;
    std::unordered_map<const AstNodeModule*, Id>& m_ids;
    std::map<std::string, uint64_t> m_rejections;
    uint64_t m_candidates = 0;

    void visit(AstNetlist* nodep) override { iterateAndNextConstNull(nodep->modulesp()); }
    void visit(AstNodeModule* nodep) override {
        if (!nodep->subgraphBoundary()) return;
        ++m_candidates;
        TemplateBuilder builder{nodep};
        if (!builder.rejection().empty()) {
            ++m_rejections[builder.rejection()];
            return;
        }
        m_modules.push_back(builder.take());
        m_ids.emplace(nodep, static_cast<Id>(m_modules.size()));
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    TemplateCaptureVisitor(AstNetlist* netlistp, std::vector<Templates::Module>& modules,
                           std::unordered_map<const AstNodeModule*, Id>& ids)
        : m_modules{modules}
        , m_ids{ids} {
        iterateConstNull(netlistp);
        V3Stats::addStat("Scheduling, Subgraph template candidates", m_candidates);
        V3Stats::addStat("Scheduling, Subgraph templates captured", modules.size());
        V3Stats::addStat("Scheduling, Subgraph templates rejected", m_candidates - modules.size());
        for (const auto& pair : m_rejections) {
            V3Stats::addStat("Scheduling, Subgraph template rejection, " + pair.first,
                             pair.second);
        }
    }
};

// Bind local events without merging formal triggers that happen to alias in the parent.
void bindTriggers(const Templates::Module& module, Templates::Instance& instance) {
    if (!instance.m_bindingRejection.empty()) {
        instance.m_abiRejection = "port " + instance.m_bindingRejection;
        return;
    }
    if (!module.m_schedule.m_rejection.empty()) {
        instance.m_abiRejection = "schedule " + module.m_schedule.m_rejection;
        return;
    }
    std::map<Id, Id> actuals;
    for (const Templates::Instance::Connection& connection : instance.m_connections) {
        actuals.emplace(connection.m_formal, connection.m_actual);
    }
    for (const Templates::Trigger& trigger : module.m_schedule.m_triggers) {
        const auto it = actuals.find(trigger.m_slot);
        UASSERT(it != actuals.end(), "Local trigger is not an input port");
        if (!it->second) {
            instance.m_abiRejection = "open trigger";
            instance.m_triggerActuals.clear();
            return;
        }
        instance.m_triggerActuals.push_back(it->second);
    }
}

// Traverse physical instance membership without copying template bodies. Cell/module pointers
// exist only during capture; later passes may freely replace or delete their AST nodes.
void collectInstances(const AstNodeModule* modp, const AstCell* cellp, const std::string& path,
                      const std::unordered_map<const AstNodeModule*, Id>& ids,
                      const std::vector<Templates::Module>& modules,
                      std::vector<Templates::Instance>& instances) {
    const auto it = ids.find(modp);
    if (it != ids.end()) {
        Templates::Instance instance;
        instance.m_path = path;
        instance.m_template = it->second;
        TemplateBuilder builder{cellp, modules[it->second - 1]};
        builder.bind(instance);
        bindTriggers(modules[it->second - 1], instance);
        instances.push_back(std::move(instance));
    }
    for (const AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
        if (const AstCell* const cellp = VN_CAST(nodep, Cell)) {
            collectInstances(cellp->modp(), cellp, path + "." + cellp->name(), ids, modules,
                             instances);
        }
    }
}

void dumpType(V3OutJsonFile& out, const Templates::Type& type) {
    out.begin("dtype")
        .put("keyword", type.m_keyword)
        .put("width", type.m_width)
        .put("signed", type.m_signed)
        .put("ranged", type.m_ranged)
        .put("left", type.m_left)
        .put("right", type.m_right)
        .end();
}

void dumpSlots(V3OutJsonFile& out, const char* name, const std::vector<Templates::Slot>& slots) {
    out.begin(name, '[');
    for (const Templates::Slot& slot : slots) {
        out.begin()
            .put("name", VIdProtect::protect(slot.m_name))
            .put("direction", slot.m_direction)
            .put("varType", slot.m_varType)
            .put("initializer", static_cast<int>(slot.m_initializer));
        dumpType(out, slot.m_type);
        out.end();
    }
    out.end();
}

void dumpNodes(V3OutJsonFile& out, const std::vector<Templates::Node>& nodes) {
    out.begin("nodes", '[');
    for (const Templates::Node& node : nodes) {
        out.begin()
            .put("kind", node.m_kind)
            .put("value", node.m_value)
            .put("slot", static_cast<int>(node.m_slot))
            .put("selectWidth", node.m_selectWidth);
        dumpType(out, node.m_type);
        out.begin("operands", '[');
        for (const Id operand : node.m_operands) out.put(static_cast<int>(operand));
        out.end();
        out.end();
    }
    out.end();
}

void dumpIds(V3OutJsonFile& out, const char* name, const std::vector<Id>& ids) {
    out.begin(name, '[');
    for (const Id id : ids) out.put(static_cast<int>(id));
    out.end();
}

void dumpProcesses(V3OutJsonFile& out, const char* name,
                   const std::vector<Templates::Process>& processes) {
    out.begin(name, '[');
    for (const Templates::Process& process : processes) {
        out.begin().put("body", static_cast<int>(process.m_body));
        dumpIds(out, "triggers", process.m_triggers);
        dumpIds(out, "reads", process.m_reads);
        dumpIds(out, "writes", process.m_writes);
        out.end();
    }
    out.end();
}

void dumpSchedule(V3OutJsonFile& out, const Templates::Schedule& schedule) {
    out.begin("schedule").put("rejection", schedule.m_rejection);
    dumpIds(out, "constants", schedule.m_constants);
    out.begin("triggers", '[');
    for (const Templates::Trigger& trigger : schedule.m_triggers) {
        out.begin()
            .put("slot", static_cast<int>(trigger.m_slot))
            .put("edge", trigger.m_edge)
            .end();
    }
    out.end();
    dumpProcesses(out, "static", schedule.m_static);
    dumpProcesses(out, "initial", schedule.m_initial);
    dumpProcesses(out, "pre", schedule.m_pre);
    dumpIds(out, "commit", schedule.m_commitSlots);
    dumpProcesses(out, "refresh", schedule.m_refresh);
    out.begin("storage", '[');
    for (const Templates::Schedule::Storage& storage : schedule.m_storage) {
        out.begin()
            .put("slot", static_cast<int>(storage.m_slot))
            .put("pending", storage.m_pending)
            .end();
    }
    out.end().begin("entries", '[');
    for (const Templates::Schedule::Entry& entry : schedule.m_entries) {
        out.begin().put("phase", entry.m_phase).put("process", static_cast<int>(entry.m_process));
        dumpIds(out, "triggers", entry.m_triggers);
        out.begin("uses", '[');
        for (const Templates::Schedule::Use& use : entry.m_uses) {
            out.begin()
                .put("storage", static_cast<int>(use.m_storage))
                .put("read", use.m_read)
                .put("write", use.m_write)
                .end();
        }
        out.end();
        out.end();
    }
    out.end();
    out.end();
}

}  // namespace

V3SubgraphTemplates::V3SubgraphTemplates(AstNetlist* netlistp) {
    const VlOs::DeltaWallTime timer{v3Global.opt.stats()};
    std::unordered_map<const AstNodeModule*, Id> ids;
    const TemplateCaptureVisitor visitor{netlistp, m_modules, ids};
    {
        const VlOs::DeltaWallTime scheduleTimer{v3Global.opt.stats()};
        uint64_t schedules = 0;
        uint64_t triggers = 0;
        uint64_t shadows = 0;
        uint64_t storage = 0;
        uint64_t entries = 0;
        std::map<std::string, uint64_t> rejections;
        for (Module& module : m_modules) {
            module.m_schedule = V3SubgraphSchedule::build(module);
            if (module.m_schedule.m_rejection.empty()) {
                ++schedules;
                triggers += module.m_schedule.m_triggers.size();
                shadows += module.m_schedule.m_commitSlots.size();
                storage += module.m_schedule.m_storage.size();
                entries += module.m_schedule.m_entries.size();
            } else {
                ++rejections[module.m_schedule.m_rejection];
            }
        }
        V3Stats::addStat("Scheduling, Subgraph template schedule builds", m_modules.size());
        V3Stats::addStat("Scheduling, Subgraph template schedules built", schedules);
        V3Stats::addStat("Scheduling, Subgraph template schedules rejected",
                         m_modules.size() - schedules);
        V3Stats::addStat("Scheduling, Subgraph template local triggers", triggers);
        V3Stats::addStat("Scheduling, Subgraph template NBA shadow slots", shadows);
        V3Stats::addStat("Scheduling, Subgraph template ABI storage slots", storage);
        V3Stats::addStat("Scheduling, Subgraph template ABI entry points", entries);
        // Plan construction is not yet a replacement for the ordinary runtime scheduling path.
        V3Stats::addStat("Scheduling, Subgraph template schedules activated", 0);
        for (const auto& pair : rejections) {
            V3Stats::addStat("Scheduling, Subgraph template schedule rejection, " + pair.first,
                             pair.second);
        }
        V3Stats::addStatPerf("Scheduling, Subgraph template schedule time (sec)",
                             scheduleTimer.deltaTime());
    }
    if (!ids.empty()) {
        collectInstances(netlistp->topModulep(), nullptr, "TOP", ids, m_modules, m_instances);
    }
    uint64_t bound = 0;
    uint64_t connections = 0;
    uint64_t open = 0;
    uint64_t abiBindings = 0;
    uint64_t triggerBindings = 0;
    std::map<std::string, uint64_t> rejections;
    std::map<std::string, uint64_t> abiRejections;
    for (const Instance& instance : m_instances) {
        if (instance.m_abiRejection.empty()) {
            ++abiBindings;
            triggerBindings += instance.m_triggerActuals.size();
        } else {
            ++abiRejections[instance.m_abiRejection];
        }
        if (!instance.m_bindingRejection.empty()) {
            ++rejections[instance.m_bindingRejection];
            continue;
        }
        ++bound;
        connections += instance.m_connections.size();
        for (const Instance::Connection& connection : instance.m_connections) {
            if (!connection.m_actual) ++open;
        }
    }
    V3Stats::addStat("Scheduling, Subgraph template instance bindings", bound);
    V3Stats::addStat("Scheduling, Subgraph template instance bindings rejected",
                     m_instances.size() - bound);
    V3Stats::addStat("Scheduling, Subgraph template port bindings", connections);
    V3Stats::addStat("Scheduling, Subgraph template open ports", open);
    V3Stats::addStat("Scheduling, Subgraph template ABI instance bindings", abiBindings);
    V3Stats::addStat("Scheduling, Subgraph template local trigger bindings", triggerBindings);
    for (const auto& pair : abiRejections) {
        V3Stats::addStat("Scheduling, Subgraph template ABI rejection, " + pair.first,
                         pair.second);
    }
    for (const auto& pair : rejections) {
        V3Stats::addStat("Scheduling, Subgraph template binding rejection, " + pair.first,
                         pair.second);
    }
    uint64_t nodes = 0;
    for (const Module& module : m_modules) nodes += module.m_nodes.size();
    V3Stats::addStat("Scheduling, Subgraph template nodes stored", nodes);
    V3Stats::addStat("Scheduling, Subgraph template instance memberships", m_instances.size());
    check();
    V3Stats::addStatPerf("Scheduling, Subgraph template capture time (sec)", timer.deltaTime());
}

void V3SubgraphTemplates::check() const {
    for (const Module& module : m_modules) {
        UASSERT(module.m_body && module.m_body <= module.m_nodes.size(), "Invalid template body");
        for (size_t index = 0; index < module.m_nodes.size(); ++index) {
            const Node& node = module.m_nodes[index];
            UASSERT(node.m_slot <= module.m_slots.size(), "Invalid template variable slot");
            for (const Id operand : node.m_operands) {
                UASSERT(operand <= index, "Template operand is not in postorder");
            }
        }
        for (const Slot& slot : module.m_slots) {
            UASSERT(slot.m_initializer <= module.m_nodes.size(), "Invalid template initializer");
        }
        const Schedule& schedule = module.m_schedule;
        const auto checkSlots = [&](const std::vector<Id>& slots) {
            for (const Id slot : slots) {
                UASSERT(slot && slot <= module.m_slots.size(), "Invalid schedule storage slot");
            }
        };
        for (const Trigger& trigger : schedule.m_triggers) {
            UASSERT(trigger.m_slot && trigger.m_slot <= module.m_slots.size(),
                    "Invalid local trigger slot");
        }
        for (const std::vector<Process>* const processesp :
             {&schedule.m_static, &schedule.m_initial, &schedule.m_pre, &schedule.m_refresh}) {
            for (const Process& process : *processesp) {
                UASSERT(process.m_body && process.m_body <= module.m_nodes.size(),
                        "Invalid schedule body");
                checkSlots(process.m_reads);
                checkSlots(process.m_writes);
                for (const Id trigger : process.m_triggers) {
                    UASSERT(trigger && trigger <= schedule.m_triggers.size(),
                            "Invalid process local trigger");
                }
            }
        }
        checkSlots(schedule.m_commitSlots);
        checkSlots(schedule.m_constants);
        for (const Schedule::Storage& storage : schedule.m_storage) {
            UASSERT(storage.m_slot && storage.m_slot <= module.m_slots.size(),
                    "Invalid ABI storage source slot");
        }
        for (const Schedule::Entry& entry : schedule.m_entries) {
            for (const Schedule::Use& use : entry.m_uses) {
                UASSERT(use.m_storage && use.m_storage <= schedule.m_storage.size(),
                        "Invalid ABI use storage");
            }
        }
    }
    for (const Instance& instance : m_instances) {
        UASSERT(instance.m_template && instance.m_template <= m_modules.size(),
                "Invalid template instance membership");
        const Module& module = m_modules[instance.m_template - 1];
        if (instance.m_abiRejection.empty()) {
            UASSERT(instance.m_triggerActuals.size() == module.m_schedule.m_triggers.size(),
                    "Incomplete local trigger binding");
        }
        for (const Id actual : instance.m_triggerActuals) {
            UASSERT(actual && actual <= instance.m_nodes.size(), "Invalid local trigger actual");
        }
        for (const Instance::Connection& connection : instance.m_connections) {
            UASSERT(connection.m_formal && connection.m_formal <= module.m_slots.size(),
                    "Invalid template formal binding");
            UASSERT(connection.m_actual <= instance.m_nodes.size(),
                    "Invalid template actual binding");
        }
        for (size_t index = 0; index < instance.m_nodes.size(); ++index) {
            const Node& node = instance.m_nodes[index];
            UASSERT(node.m_slot <= instance.m_parentSlots.size(), "Invalid parent binding slot");
            for (const Id operand : node.m_operands) {
                UASSERT(operand <= index, "Binding operand is not in postorder");
            }
        }
    }
}

void V3SubgraphTemplates::dump(const std::string& filename) const {
    V3OutJsonFile out{filename};
    out.put("version", 1).begin("templates", '[');
    for (const Module& module : m_modules) {
        out.begin()
            .put("name", VIdProtect::protect(module.m_name))
            .put("body", static_cast<int>(module.m_body));
        dumpSlots(out, "slots", module.m_slots);
        dumpNodes(out, module.m_nodes);
        dumpSchedule(out, module.m_schedule);
        out.end();
    }
    out.end().begin("instances", '[');
    for (const Instance& instance : m_instances) {
        out.begin()
            .put("path", VIdProtect::protect(instance.m_path))
            .put("template", static_cast<int>(instance.m_template))
            .put("bindingRejection", instance.m_bindingRejection)
            .put("abiRejection", instance.m_abiRejection);
        dumpIds(out, "triggerActuals", instance.m_triggerActuals);
        dumpSlots(out, "parentSlots", instance.m_parentSlots);
        dumpNodes(out, instance.m_nodes);
        out.begin("connections", '[');
        for (const Instance::Connection& connection : instance.m_connections) {
            out.begin()
                .put("formal", static_cast<int>(connection.m_formal))
                .put("actual", static_cast<int>(connection.m_actual))
                .end();
        }
        out.end();
        out.end();
    }
    out.end();
}
