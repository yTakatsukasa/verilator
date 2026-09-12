// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Specialization-local subgraph AST ownership
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
// Clone each elaborated subgraph module specialization before V3Inst lowers its port connections
// and V3Scope replicates procedures for every instance. The detached trees retain the complete
// V3Ast type and operation nodes; no second expression or statement representation is introduced.
// Parent passes continue to use the original modules until execution is switched to these trees.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphAst.h"

#include "V3Stats.h"
#include "V3SubgraphSchedule.h"

#include <map>
#include <unordered_map>
#include <unordered_set>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

using TemplateIdMap = std::unordered_map<const AstNodeModule*, uint32_t>;

const AstPin* findPin(const AstCell* cellp, const AstVar* formalp) {
    for (const AstPin* pinp = cellp->pinsp(); pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
        if (pinp->modVarp() == formalp) return pinp;
    }
    return nullptr;
}

void collectInstances(const AstNodeModule* modp, const AstCell* aboveCellp,
                      const std::string& path, const TemplateIdMap& ids,
                      std::vector<V3SubgraphAst::Template>& templates,
                      std::vector<V3SubgraphAst::Instance>& instances) {
    const auto it = ids.find(modp);
    if (it != ids.end()) {
        UASSERT_OBJ(aboveCellp, modp, "Top module cannot be a subgraph template instance");
        V3SubgraphAst::Template& item = templates[it->second - 1];
        V3SubgraphAst::Instance instance;
        instance.m_template = item.m_id;
        instance.m_path = path;
        instance.m_bindings.reserve(item.m_ports.size());
        for (const V3SubgraphAst::Template::Port& port : item.m_ports) {
            const AstPin* const pinp = findPin(aboveCellp, port.m_sourcep);
            AstNodeExpr* const actualp = pinp && pinp->exprp()
                                             ? VN_AS(pinp->exprp(), NodeExpr)->cloneTree(false)
                                             : nullptr;
            instance.m_bindings.push_back(V3SubgraphAst::Instance::Binding{port.m_id, actualp});
        }
        ++item.m_instances;
        instances.push_back(std::move(instance));
    }
    for (const AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
        if (const AstCell* const cellp = VN_CAST(nodep, Cell)) {
            collectInstances(cellp->modp(), cellp, path + "." + cellp->name(), ids, templates,
                             instances);
        }
    }
}

uint64_t nodeCount(const AstNode* nodep) {
    uint64_t nodes = 1;
    nodep->foreach([&](const AstNode*) { ++nodes; });
    return nodes;
}

std::map<std::string, uint64_t> callCategories(const V3SubgraphAst::Template& item) {
    std::unordered_set<const AstNodeFTask*> localTasks;
    item.m_treep->foreach([&](const AstNodeFTask* taskp) { localTasks.insert(taskp); });

    std::map<std::string, uint64_t> result;
    for (AstNode* nodep = item.m_treep->stmtsp(); nodep; nodep = nodep->nextp()) {
        AstNodeProcedure* const procedurep = VN_CAST(nodep, NodeProcedure);
        if (!procedurep || !procedurep->stmtsp()) continue;
        procedurep->stmtsp()->foreachAndNext([&](AstNodeFTaskRef* refp) {
            AstNodeFTask* const taskp = refp->taskp();
            if (!taskp) {
                ++result["unresolved"];
                return;
            }
            const std::string target = localTasks.count(taskp) ? "local" : "external";
            const std::string lifetime = taskp->lifetime().isAutomatic() ? "automatic"
                                         : taskp->lifetime().isStatic()  ? "static"
                                                                         : "unknown";
            const std::string purity = refp->isPure() ? "pure" : "impure";
            const std::string kind = taskp->isFunction() ? "function" : "task";
            const std::string dpi = taskp->dpiImport() ? " DPI" : "";
            const std::string recursive = taskp->recursive() ? " recursive" : "";
            ++result[target + " " + lifetime + " " + purity + dpi + recursive + " " + kind];
        });
    }
    return result;
}

}  // namespace

V3SubgraphAst::V3SubgraphAst(AstNetlist* netlistp) {
    const VlOs::DeltaWallTime timer{v3Global.opt.stats()};
    TemplateIdMap ids;
    uint64_t candidates = 0;
    uint64_t nodes = 0;
    for (AstNodeModule* modp = netlistp->modulesp(); modp;
         modp = VN_AS(modp->nextp(), NodeModule)) {
        if (!modp->subgraphBoundary()) continue;
        ++candidates;
        AstNodeModule* const treep = modp->cloneTree(false);
        const uint32_t id = static_cast<uint32_t>(m_templates.size() + 1);
        Template captured;
        captured.m_id = id;
        captured.m_sourcep = modp;
        captured.m_treep = treep;
        m_templates.push_back(std::move(captured));
        Template& item = m_templates.back();
        for (AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
            AstVar* const sourcep = VN_CAST(nodep, Var);
            if (!sourcep) continue;
            AstVar* const formalp = sourcep->clonep();
            UASSERT_OBJ(formalp, sourcep, "Subgraph V3Ast variable declaration was not cloned");
            const uint32_t variableId = static_cast<uint32_t>(item.m_variables.size() + 1);
            item.m_variables.push_back(Template::Variable{variableId, sourcep, formalp});
            if (sourcep->isIO()) {
                const uint32_t portId = static_cast<uint32_t>(item.m_ports.size() + 1);
                item.m_ports.push_back(Template::Port{portId, sourcep, formalp});
            }
        }
        item.m_schedule = V3SubgraphSchedule::build(item);
        ids.emplace(modp, id);
        nodes += nodeCount(treep);
    }
    if (!ids.empty()) {
        collectInstances(netlistp->topModulep(), nullptr, "TOP", ids, m_templates, m_instances);
    }

    V3Stats::addStat("Scheduling, Subgraph V3Ast candidates", candidates);
    V3Stats::addStat("Scheduling, Subgraph V3Ast templates", m_templates.size());
    V3Stats::addStat("Scheduling, Subgraph V3Ast nodes", nodes);
    uint64_t schedules = 0;
    uint64_t triggers = 0;
    uint64_t entries = 0;
    uint64_t coalescedEntries = 0;
    uint64_t coalescedCalls = 0;
    std::map<std::string, uint64_t> scheduleRejections;
    std::map<std::string, uint64_t> scheduleRejectedInstances;
    std::map<std::string, uint64_t> callSites;
    std::map<std::string, uint64_t> callTemplates;
    std::map<std::string, uint64_t> callInstances;
    for (const Template& item : m_templates) {
        if (v3Global.opt.stats()) {
            for (const auto& pair : callCategories(item)) {
                callSites[pair.first] += pair.second;
                ++callTemplates[pair.first];
                callInstances[pair.first] += item.m_instances;
            }
        }
        if (item.m_schedule.m_rejection.empty()) {
            ++schedules;
            triggers += item.m_schedule.m_triggers.size();
            entries += item.m_schedule.m_entries.size();
            const uint64_t processEntries
                = item.m_schedule.m_static.size() + item.m_schedule.m_initial.size()
                  + 2 * item.m_schedule.m_pre.size() + item.m_schedule.m_refresh.size();
            const uint64_t coalesced = processEntries - item.m_schedule.m_entries.size();
            coalescedEntries += coalesced;
            coalescedCalls += coalesced * item.m_instances;
        } else {
            ++scheduleRejections[item.m_schedule.m_rejection];
            scheduleRejectedInstances[item.m_schedule.m_rejection] += item.m_instances;
        }
    }
    V3Stats::addStat("Scheduling, Subgraph V3Ast schedule builds", m_templates.size());
    V3Stats::addStat("Scheduling, Subgraph V3Ast schedules built", schedules);
    V3Stats::addStat("Scheduling, Subgraph V3Ast schedules rejected",
                     m_templates.size() - schedules);
    V3Stats::addStat("Scheduling, Subgraph V3Ast local triggers", triggers);
    V3Stats::addStat("Scheduling, Subgraph V3Ast phase entries", entries);
    V3Stats::addStat("Scheduling, Subgraph V3Ast phase entries coalesced", coalescedEntries);
    V3Stats::addStat("Scheduling, Subgraph V3Ast instance entry calls avoided", coalescedCalls);
    for (const auto& pair : scheduleRejections) {
        V3Stats::addStat("Scheduling, Subgraph V3Ast schedule rejection, " + pair.first,
                         pair.second);
    }
    for (const auto& pair : scheduleRejectedInstances) {
        V3Stats::addStat("Scheduling, Subgraph V3Ast schedule rejection instances, " + pair.first,
                         pair.second);
    }
    for (const auto& pair : callSites) {
        V3Stats::addStat("Scheduling, Subgraph V3Ast call sites, " + pair.first, pair.second);
        V3Stats::addStat("Scheduling, Subgraph V3Ast call templates, " + pair.first,
                         callTemplates.at(pair.first));
        V3Stats::addStat("Scheduling, Subgraph V3Ast call instances, " + pair.first,
                         callInstances.at(pair.first));
    }
    uint64_t instances = 0;
    for (const Template& item : m_templates) instances += item.m_instances;
    V3Stats::addStat("Scheduling, Subgraph V3Ast instance memberships", instances);
    uint64_t bindings = 0;
    uint64_t constants = 0;
    uint64_t open = 0;
    for (const Instance& instance : m_instances) {
        bindings += instance.m_bindings.size();
        for (const Instance::Binding& binding : instance.m_bindings) {
            if (!binding.m_actualp) {
                ++open;
            } else if (VN_IS(binding.m_actualp, Const)) {
                ++constants;
            }
        }
    }
    V3Stats::addStat("Scheduling, Subgraph V3Ast port bindings", bindings);
    V3Stats::addStat("Scheduling, Subgraph V3Ast constant bindings", constants);
    V3Stats::addStat("Scheduling, Subgraph V3Ast open ports", open);
    V3Stats::addStatPerf("Scheduling, Subgraph V3Ast capture time (sec)", timer.deltaTime());
    check();
}

V3SubgraphAst::~V3SubgraphAst() {
    for (const Instance& instance : m_instances) {
        for (const Instance::Binding& binding : instance.m_bindings) {
            if (binding.m_actualp) binding.m_actualp->deleteTree();
        }
    }
    for (const Template& item : m_templates) item.m_treep->deleteTree();
}

void V3SubgraphAst::check() const {
    uint32_t expectedId = 1;
    for (const Template& item : m_templates) {
        UASSERT(item.m_id == expectedId++, "Non-contiguous subgraph V3Ast template ID");
        UASSERT(item.m_sourcep && item.m_treep, "Incomplete subgraph V3Ast template");
        UASSERT(!item.m_treep->backp(), "Subgraph V3Ast template is attached to another tree");
        UASSERT(item.m_sourcep != item.m_treep, "Subgraph V3Ast template aliases source module");
        UASSERT(item.m_sourcep->subgraphBoundary() && item.m_treep->subgraphBoundary(),
                "Subgraph V3Ast template lost its boundary attribute");
        UASSERT(item.m_sourcep->name() == item.m_treep->name(),
                "Subgraph V3Ast template changed specialization identity");
        uint32_t expectedPortId = 1;
        uint32_t expectedVariableId = 1;
        for (const Template::Variable& variable : item.m_variables) {
            UASSERT(variable.m_id == expectedVariableId++,
                    "Non-contiguous subgraph V3Ast variable ID");
            UASSERT_OBJ(variable.m_sourcep && variable.m_formalp, item.m_treep,
                        "Incomplete subgraph V3Ast variable mapping");
        }
        for (const Template::Port& port : item.m_ports) {
            UASSERT(port.m_id == expectedPortId++, "Non-contiguous subgraph V3Ast formal ID");
            UASSERT_OBJ(port.m_sourcep && port.m_formalp && port.m_sourcep->isIO()
                            && port.m_formalp->isIO(),
                        item.m_treep, "Subgraph V3Ast formal is not a port declaration");
        }

        std::unordered_set<const AstVar*> sourceVars;
        std::unordered_set<const AstNodeFTask*> sourceTasks;
        item.m_sourcep->foreach([&](const AstVar* varp) { sourceVars.insert(varp); });
        item.m_sourcep->foreach([&](const AstNodeFTask* taskp) { sourceTasks.insert(taskp); });
        item.m_treep->foreach([&](const AstNodeVarRef* refp) {
            UASSERT_OBJ(refp->varp(), refp, "Subgraph V3Ast variable reference is unresolved");
            UASSERT_OBJ(!sourceVars.count(refp->varp()), refp,
                        "Subgraph V3Ast variable reference points into source module");
        });
        item.m_treep->foreach([&](const AstNodeFTaskRef* refp) {
            UASSERT_OBJ(!refp->taskp() || !sourceTasks.count(refp->taskp()), refp,
                        "Subgraph V3Ast task reference points into source module");
        });
        item.m_treep->foreach([&](const AstVarScope* vscp) {
            vscp->v3fatalSrc("Subgraph V3Ast template was captured after scope replication");
        });
        const Schedule& schedule = item.m_schedule;
        if (!schedule.m_rejection.empty()) continue;
        for (const Trigger& trigger : schedule.m_triggers) {
            UASSERT_OBJ(trigger.m_formalp && trigger.m_formalp->isInput(), item.m_treep,
                        "Subgraph V3Ast trigger is not a template input");
        }
        for (const Schedule::Storage& storage : schedule.m_storage) {
            UASSERT_OBJ(storage.m_formalp, item.m_treep, "Subgraph V3Ast storage has no formal");
        }
        for (const Schedule::Entry& entry : schedule.m_entries) {
            UASSERT(!entry.m_processes.empty(), "Subgraph V3Ast schedule entry has no process");
            for (const uint32_t trigger : entry.m_triggers) {
                UASSERT(trigger && trigger <= schedule.m_triggers.size(),
                        "Invalid subgraph V3Ast local trigger ID");
            }
            for (const Schedule::Use& use : entry.m_uses) {
                UASSERT(use.m_storage && use.m_storage <= schedule.m_storage.size(),
                        "Invalid subgraph V3Ast ABI storage ID");
            }
        }
    }
    for (const Instance& instance : m_instances) {
        UASSERT(instance.m_template && instance.m_template <= m_templates.size(),
                "Invalid subgraph V3Ast instance template ID");
        const Template& item = m_templates[instance.m_template - 1];
        UASSERT(instance.m_bindings.size() == item.m_ports.size(),
                "Incomplete subgraph V3Ast port binding");
        uint32_t expectedFormal = 1;
        for (const Instance::Binding& binding : instance.m_bindings) {
            UASSERT(binding.m_formal == expectedFormal++,
                    "Non-contiguous subgraph V3Ast instance formal ID");
            UASSERT(!binding.m_actualp || !binding.m_actualp->backp(),
                    "Subgraph V3Ast actual expression is attached to another tree");
        }
    }
}
