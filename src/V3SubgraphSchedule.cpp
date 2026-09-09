// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Schedule an independent subgraph template
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
// Build one schedule from formal slots only, without instance or parent-trigger information.
// The first supported subset has full-variable NBA writes, input-port edge sensitivities, and
// acyclic continuous assignments. All enabled PRE bodies read current state before any pending
// NBA is committed; refresh then restores combinational consistency in dataflow order. Static
// and initial procedures retain source order. Reject an entire schedule on unsupported behavior.
// This constructs the immutable schedule; parent-region integration is a separate operation.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphSchedule.h"

#include "V3Graph.h"

#include <map>
#include <set>

namespace {

using Templates = V3SubgraphTemplates;
using Id = Templates::Id;

class ScheduleVertex final : public V3GraphVertex {
public:
    explicit ScheduleVertex(V3Graph* graphp)
        : V3GraphVertex{graphp} {}
};

class ScheduleBuilder final {
    const Templates::Module& m_module;
    Templates::Schedule m_schedule;
    std::map<std::pair<Id, std::string>, Id> m_triggerIds;
    std::set<Id> m_driven;  // Enforce a single steady-state process per destination
    std::string m_rejection;

    const Templates::Node& node(Id id) const { return m_module.m_nodes.at(id - 1); }
    void reject(const std::string& reason) {
        if (m_rejection.empty()) m_rejection = reason;
    }

    void reads(Id id, std::set<Id>& result) {
        if (!id) return;
        const Templates::Node& expr = node(id);
        if (expr.m_slot) result.insert(expr.m_slot);
        for (const Id operand : expr.m_operands) reads(operand, result);
    }

    void statement(Id id, bool nba, std::set<Id>& readSlots, std::set<Id>& writeSlots) {
        if (!id || !m_rejection.empty()) return;
        const Templates::Node& stmt = node(id);
        if (stmt.m_kind == "BLOCK") {
            for (const Id operand : stmt.m_operands)
                statement(operand, nba, readSlots, writeSlots);
        } else if (stmt.m_kind == "IF") {
            reads(stmt.m_operands[0], readSlots);
            statement(stmt.m_operands[1], nba, readSlots, writeSlots);
            statement(stmt.m_operands[2], nba, readSlots, writeSlots);
        } else if (stmt.m_kind == "ASSIGNDLY" || stmt.m_kind == "ASSIGN"
                   || stmt.m_kind == "ASSIGNW") {
            if ((stmt.m_kind == "ASSIGNDLY") != nba) {
                reject("assignment phase");
                return;
            }
            const Templates::Node& lhs = node(stmt.m_operands[0]);
            if (lhs.m_kind != "VARREF") {
                reject("partial write");
                return;
            }
            if (m_module.m_slots[lhs.m_slot - 1].m_direction == "INPUT") {
                reject("input write");
                return;
            }
            writeSlots.insert(lhs.m_slot);
            reads(stmt.m_operands[1], readSlots);
        } else {
            reject("procedure statement");
        }
    }

    Templates::Process process(Id body, bool nba, bool steady) {
        Templates::Process result;
        result.m_body = body;
        std::set<Id> readSlots;
        std::set<Id> writeSlots;
        statement(body, nba, readSlots, writeSlots);
        result.m_reads.assign(readSlots.begin(), readSlots.end());
        result.m_writes.assign(writeSlots.begin(), writeSlots.end());
        if (steady) {
            for (const Id slot : writeSlots) {
                if (!m_driven.insert(slot).second) reject("multiple drivers");
            }
        }
        return result;
    }

    std::vector<Id> triggers(Id tree) {
        std::set<Id> result;
        for (const Id itemId : node(tree).m_operands) {
            const Templates::Node& item = node(itemId);
            if (item.m_operands[1] || !item.m_operands[0]
                || (item.m_value != "POS" && item.m_value != "NEG")) {
                reject("event expression");
                break;
            }
            const Templates::Node& expr = node(item.m_operands[0]);
            if (expr.m_kind != "VARREF"
                || m_module.m_slots[expr.m_slot - 1].m_direction != "INPUT") {
                reject("internal trigger");
                break;
            }
            const Id next = static_cast<Id>(m_schedule.m_triggers.size() + 1);
            const auto inserted
                = m_triggerIds.emplace(std::make_pair(expr.m_slot, item.m_value), next);
            if (inserted.second) m_schedule.m_triggers.push_back({expr.m_slot, item.m_value});
            result.insert(inserted.first->second);
        }
        return {result.begin(), result.end()};
    }

    void procedures() {
        for (size_t index = 0; index < m_module.m_slots.size(); ++index) {
            const Templates::Slot& slot = m_module.m_slots[index];
            if (!slot.m_initializer) continue;
            if ((slot.m_varType == "GPARAM" || slot.m_varType == "LPARAM")
                && node(slot.m_initializer).m_kind == "CONST") {
                m_schedule.m_constants.push_back(static_cast<Id>(index + 1));
            } else {
                reject("declaration initializer");
            }
        }
        for (const Id id : node(m_module.m_body).m_operands) {
            if (!m_rejection.empty()) return;
            const Templates::Node& proc = node(id);
            if (proc.m_kind == "INITIALSTATIC") {
                if (!proc.m_operands[0]) continue;
                m_schedule.m_static.push_back(process(proc.m_operands[0], false, false));
            } else if (proc.m_kind == "INITIAL") {
                if (!proc.m_operands[0]) continue;
                m_schedule.m_initial.push_back(process(proc.m_operands[0], false, false));
            } else if (proc.m_kind == "ALWAYS") {
                const Id tree = proc.m_operands[0];
                const Id body = proc.m_operands[1];
                if (!body) continue;
                if (tree) {
                    Templates::Process pre = process(body, true, true);
                    pre.m_triggers = triggers(tree);
                    m_schedule.m_pre.push_back(std::move(pre));
                } else if (proc.m_value == "cont_assign") {
                    m_schedule.m_refresh.push_back(process(body, false, true));
                } else {
                    reject("combinational procedure");
                }
            } else {
                reject("module procedure");
            }
        }
        for (const Templates::Process& pre : m_schedule.m_pre) {
            m_schedule.m_commitSlots.insert(m_schedule.m_commitSlots.end(), pre.m_writes.begin(),
                                            pre.m_writes.end());
        }
    }

    void orderRefresh() {
        V3Graph graph;
        std::vector<ScheduleVertex*> slots(m_module.m_slots.size(), nullptr);
        const auto slotVertex = [&](Id slot) {
            ScheduleVertex*& vertexp = slots[slot - 1];
            if (!vertexp) vertexp = new ScheduleVertex{&graph};
            return vertexp;
        };
        std::vector<ScheduleVertex*> processes;
        for (const Templates::Process& process : m_schedule.m_refresh) {
            ScheduleVertex* const vertexp = new ScheduleVertex{&graph};
            processes.push_back(vertexp);
            for (const Id slot : process.m_reads) {
                new V3GraphEdge{&graph, slotVertex(slot), vertexp, 1};
            }
            for (const Id slot : process.m_writes) {
                new V3GraphEdge{&graph, vertexp, slotVertex(slot), 1};
            }
        }
        graph.stronglyConnected(&V3GraphEdge::followAlwaysTrue);
        for (const V3GraphVertex& vertex : graph.vertices()) {
            if (vertex.color()) {
                reject("combinational cycle");
                return;
            }
        }
        graph.rank();
        std::vector<size_t> order;
        for (size_t index = 0; index < processes.size(); ++index) order.push_back(index);
        std::stable_sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) {
            return processes[lhs]->rank() < processes[rhs]->rank();
        });
        std::vector<Templates::Process> ordered;
        for (const size_t index : order) ordered.push_back(std::move(m_schedule.m_refresh[index]));
        m_schedule.m_refresh = std::move(ordered);
    }

    void makeAbi() {
        std::vector<Id> current(m_module.m_slots.size() + 1, Templates::NONE);
        std::vector<Id> pending(current.size(), Templates::NONE);
        const std::set<Id> constants{m_schedule.m_constants.begin(), m_schedule.m_constants.end()};
        for (size_t index = 1; index < current.size(); ++index) {
            const Id slot = static_cast<Id>(index);
            if (constants.count(slot)) continue;
            m_schedule.m_storage.push_back({slot, false});
            current[slot] = static_cast<Id>(m_schedule.m_storage.size());
        }
        for (const Id slot : m_schedule.m_commitSlots) {
            m_schedule.m_storage.push_back({slot, true});
            pending[slot] = static_cast<Id>(m_schedule.m_storage.size());
        }
        for (const std::string phase : {"static", "initial", "pre", "commit", "refresh"}) {
            const std::vector<Templates::Process>& processes
                = phase == "static"    ? m_schedule.m_static
                  : phase == "initial" ? m_schedule.m_initial
                  : phase == "refresh" ? m_schedule.m_refresh
                                       : m_schedule.m_pre;
            for (size_t index = 0; index < processes.size(); ++index) {
                const Templates::Process& process = processes[index];
                Templates::Schedule::Entry entry;
                entry.m_phase = phase;
                entry.m_process = static_cast<Id>(index + 1);
                entry.m_triggers = process.m_triggers;
                std::map<Id, Templates::Schedule::Use> uses;
                const auto read = [&](Id storage) {
                    if (storage) uses[storage].m_read = true;
                };
                const auto write = [&](Id storage) {
                    UASSERT(storage, "Attempt to write a template constant");
                    uses[storage].m_write = true;
                };
                if (phase == "commit") {
                    for (const Id slot : process.m_writes) {
                        read(pending[slot]);
                        write(current[slot]);
                    }
                } else {
                    for (const Id slot : process.m_reads) read(current[slot]);
                    for (const Id slot : process.m_writes) {
                        if (phase == "pre") {
                            // Retain old values on paths which perform no NBA assignment.
                            read(current[slot]);
                            write(pending[slot]);
                        } else {
                            write(current[slot]);
                        }
                    }
                }
                for (auto& pair : uses) {
                    pair.second.m_storage = pair.first;
                    entry.m_uses.push_back(pair.second);
                }
                m_schedule.m_entries.push_back(std::move(entry));
            }
        }
    }

public:
    explicit ScheduleBuilder(const Templates::Module& module)
        : m_module{module} {
        procedures();
        if (m_rejection.empty()) orderRefresh();
        if (m_rejection.empty()) makeAbi();
    }
    Templates::Schedule take() {
        if (!m_rejection.empty()) {
            Templates::Schedule rejected;
            rejected.m_rejection = m_rejection;
            return rejected;
        }
        return std::move(m_schedule);
    }
};

}  // namespace

V3SubgraphTemplates::Schedule
V3SubgraphSchedule::build(const V3SubgraphTemplates::Module& module) {
    return ScheduleBuilder{module}.take();
}
