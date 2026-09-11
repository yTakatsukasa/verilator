// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Build schedules from specialization-local subgraph V3Ast
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
// Build one schedule directly over the detached V3Ast. Expressions and statements remain V3Ast
// nodes owned by V3SubgraphAst; the schedule only records phase, dependency, event, and ABI
// metadata. Parent connections and global trigger numbers therefore cannot specialize it.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphSchedule.h"

#include "V3Graph.h"

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

using Ast = V3SubgraphAst;

class ScheduleVertex final : public V3GraphVertex {
public:
    explicit ScheduleVertex(V3Graph* graphp)
        : V3GraphVertex{graphp} {}
};

class AccessVisitor final : public VNVisitorConst {
    const std::unordered_set<const AstVar*>& m_locals;
    const bool m_nba;
    std::set<AstVar*>& m_reads;
    std::set<AstVar*>& m_writes;
    std::string& m_rejection;

    void reject(const char* reason) {
        if (m_rejection.empty()) m_rejection = reason;
    }
    void visit(AstNodeAssign* nodep) override {
        if (static_cast<bool>(VN_IS(nodep, AssignDly)) != m_nba) reject("assignment phase");
        if (nodep->timingControlp()) reject("assignment timing control");
        iterateChildrenConst(nodep);
    }
    void visit(AstNodeVarRef* nodep) override {
        AstVar* const varp = nodep->varp();
        if (!varp || !m_locals.count(varp)) {
            reject("nonlocal variable");
            return;
        }
        if (nodep->access().isReadOrRW()) m_reads.insert(varp);
        if (nodep->access().isWriteOrRW()) m_writes.insert(varp);
    }
    void visit(AstNodeFTaskRef*) override { reject("task call"); }
    void visit(AstDelay*) override { reject("timing control"); }
    void visit(AstEventControl*) override { reject("timing control"); }
    void visit(AstFork*) override { reject("fork"); }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    AccessVisitor(AstNode* nodep, const std::unordered_set<const AstVar*>& locals, bool nba,
                  std::set<AstVar*>& reads, std::set<AstVar*>& writes, std::string& rejection)
        : m_locals{locals}
        , m_nba{nba}
        , m_reads{reads}
        , m_writes{writes}
        , m_rejection{rejection} {
        iterateAndNextConstNull(nodep);
    }
};

class ScheduleBuilder final {
    const Ast::Template& m_item;
    Ast::Schedule m_schedule;
    std::unordered_map<const AstVar*, uint32_t> m_varIds;
    std::unordered_set<const AstVar*> m_locals;
    std::map<std::pair<const AstVar*, bool>, uint32_t> m_triggerIds;
    std::unordered_set<const AstVar*> m_driven;
    std::string m_rejection;

    void reject(const char* reason) {
        if (m_rejection.empty()) m_rejection = reason;
    }

    std::vector<AstVar*> ordered(const std::set<AstVar*>& vars) const {
        std::vector<AstVar*> result{vars.begin(), vars.end()};
        std::sort(result.begin(), result.end(), [&](const AstVar* lhs, const AstVar* rhs) {
            return m_varIds.at(lhs) < m_varIds.at(rhs);
        });
        return result;
    }

    Ast::Process process(AstNodeProcedure* procedurep, bool nba, bool steady) {
        std::set<AstVar*> reads;
        std::set<AstVar*> writes;
        const AccessVisitor visitor{procedurep->stmtsp(), m_locals, nba, reads, writes,
                                    m_rejection};
        Ast::Process result;
        result.m_procedurep = procedurep;
        result.m_reads = ordered(reads);
        result.m_writes = ordered(writes);
        if (steady) {
            for (AstVar* const varp : result.m_writes) {
                if (!m_driven.insert(varp).second) reject("multiple drivers");
                if (varp->isInput()) reject("input write");
            }
        }
        return result;
    }

    std::vector<uint32_t> triggers(AstSenTree* treep) {
        std::set<uint32_t> result;
        for (AstSenItem* itemp = treep->sensesp(); itemp; itemp = VN_AS(itemp->nextp(), SenItem)) {
            const bool posedge = itemp->edgeType() == VEdgeType::ET_POSEDGE;
            if ((!posedge && itemp->edgeType() != VEdgeType::ET_NEGEDGE) || itemp->condp()) {
                reject("event expression");
                break;
            }
            AstNodeVarRef* const refp = VN_CAST(itemp->sensp(), NodeVarRef);
            AstVar* const varp = refp ? refp->varp() : nullptr;
            if (!varp || !m_locals.count(varp) || !varp->isInput()) {
                reject("internal trigger");
                break;
            }
            const uint32_t next = static_cast<uint32_t>(m_schedule.m_triggers.size() + 1);
            const auto inserted = m_triggerIds.emplace(std::make_pair(varp, posedge), next);
            if (inserted.second) m_schedule.m_triggers.push_back(Ast::Trigger{varp, posedge});
            result.insert(inserted.first->second);
        }
        return {result.begin(), result.end()};
    }

    void procedures() {
        for (AstNode* nodep = m_item.m_treep->stmtsp(); nodep; nodep = nodep->nextp()) {
            AstNodeProcedure* const procedurep = VN_CAST(nodep, NodeProcedure);
            if (!procedurep) continue;
            if (VN_IS(procedurep, InitialStatic)) {
                if (procedurep->stmtsp())
                    m_schedule.m_static.push_back(process(procedurep, false, false));
            } else if (VN_IS(procedurep, Initial)) {
                if (procedurep->stmtsp())
                    m_schedule.m_initial.push_back(process(procedurep, false, false));
            } else if (AstAlways* const alwaysp = VN_CAST(procedurep, Always)) {
                if (!alwaysp->stmtsp()) continue;
                if (alwaysp->sentreep()) {
                    Ast::Process item = process(alwaysp, true, true);
                    item.m_triggers = triggers(alwaysp->sentreep());
                    m_schedule.m_pre.push_back(std::move(item));
                } else if (alwaysp->keyword() == VAlwaysKwd::ALWAYS_COMB
                           || alwaysp->keyword() == VAlwaysKwd::CONT_ASSIGN) {
                    m_schedule.m_refresh.push_back(process(alwaysp, false, true));
                } else {
                    reject("combinational procedure");
                }
            } else {
                reject("procedure kind");
            }
            if (!m_rejection.empty()) return;
        }
    }

    void orderRefresh() {
        V3Graph graph;
        std::unordered_map<const AstVar*, ScheduleVertex*> variables;
        const auto variableVertex = [&](const AstVar* varp) {
            ScheduleVertex*& vertexp = variables[varp];
            if (!vertexp) vertexp = new ScheduleVertex{&graph};
            return vertexp;
        };
        std::vector<ScheduleVertex*> processes;
        for (const Ast::Process& process : m_schedule.m_refresh) {
            ScheduleVertex* const vertexp = new ScheduleVertex{&graph};
            processes.push_back(vertexp);
            for (const AstVar* const varp : process.m_reads)
                new V3GraphEdge{&graph, variableVertex(varp), vertexp, 1};
            for (const AstVar* const varp : process.m_writes)
                new V3GraphEdge{&graph, vertexp, variableVertex(varp), 1};
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
        std::vector<Ast::Process> orderedProcesses;
        for (const size_t index : order)
            orderedProcesses.push_back(std::move(m_schedule.m_refresh[index]));
        m_schedule.m_refresh = std::move(orderedProcesses);
    }

    void makeAbi() {
        std::unordered_map<const AstVar*, uint32_t> current;
        std::unordered_map<const AstVar*, uint32_t> pending;
        for (const Ast::Template::Variable& variable : m_item.m_variables) {
            m_schedule.m_storage.push_back(Ast::Schedule::Storage{variable.m_formalp, false});
            current.emplace(variable.m_formalp,
                            static_cast<uint32_t>(m_schedule.m_storage.size()));
        }
        for (const Ast::Process& process : m_schedule.m_pre) {
            for (AstVar* const varp : process.m_writes) {
                if (pending.count(varp)) continue;
                m_schedule.m_storage.push_back(Ast::Schedule::Storage{varp, true});
                pending.emplace(varp, static_cast<uint32_t>(m_schedule.m_storage.size()));
            }
        }
        const auto addEntries = [&](Ast::Phase phase, const std::vector<Ast::Process>& processes) {
            for (size_t index = 0; index < processes.size(); ++index) {
                const Ast::Process& process = processes[index];
                Ast::Schedule::Entry entry;
                entry.m_phase = phase;
                entry.m_process = static_cast<uint32_t>(index + 1);
                entry.m_triggers = process.m_triggers;
                std::map<uint32_t, Ast::Schedule::Use> uses;
                const auto read = [&](uint32_t storage) {
                    if (storage) uses[storage].m_read = true;
                };
                const auto write = [&](uint32_t storage) {
                    UASSERT(storage, "Attempt to write missing subgraph storage");
                    uses[storage].m_write = true;
                };
                if (phase == Ast::Phase::COMMIT) {
                    for (const AstVar* const varp : process.m_writes) {
                        read(pending.at(varp));
                        write(current.at(varp));
                    }
                } else {
                    for (const AstVar* const varp : process.m_reads) read(current.at(varp));
                    for (const AstVar* const varp : process.m_writes) {
                        if (phase == Ast::Phase::PRE) {
                            read(current.at(varp));
                            write(pending.at(varp));
                        } else {
                            write(current.at(varp));
                        }
                    }
                }
                for (auto& pair : uses) {
                    pair.second.m_storage = pair.first;
                    entry.m_uses.push_back(pair.second);
                }
                m_schedule.m_entries.push_back(std::move(entry));
            }
        };
        addEntries(Ast::Phase::STATIC, m_schedule.m_static);
        addEntries(Ast::Phase::INITIAL, m_schedule.m_initial);
        addEntries(Ast::Phase::PRE, m_schedule.m_pre);
        addEntries(Ast::Phase::COMMIT, m_schedule.m_pre);
        addEntries(Ast::Phase::REFRESH, m_schedule.m_refresh);
    }

public:
    explicit ScheduleBuilder(const Ast::Template& item)
        : m_item{item} {
        for (const Ast::Template::Variable& variable : item.m_variables) {
            m_varIds.emplace(variable.m_formalp, variable.m_id);
            m_locals.insert(variable.m_formalp);
        }
        procedures();
        if (m_rejection.empty()) orderRefresh();
        if (m_rejection.empty()) makeAbi();
    }
    Ast::Schedule take() {
        if (!m_rejection.empty()) {
            Ast::Schedule rejected;
            rejected.m_rejection = m_rejection;
            return rejected;
        }
        return std::move(m_schedule);
    }
};

}  // namespace

V3SubgraphAst::Schedule V3SubgraphSchedule::build(const V3SubgraphAst::Template& item) {
    return ScheduleBuilder{item}.take();
}
