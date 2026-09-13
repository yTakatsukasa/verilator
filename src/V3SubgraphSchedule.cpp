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
// Build one coarse schedule directly over the detached V3Ast. Processes in the same event domain
// share PRE and COMMIT entries, and combinational processes share a REFRESH entry. Expressions and
// statements remain V3Ast nodes owned by V3SubgraphAst; the schedule only records phase,
// dependency, event, and ABI metadata. Parent connections and global trigger numbers therefore
// cannot specialize it.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphSchedule.h"

#include "V3Graph.h"

#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

using Ast = V3SubgraphAst;

void rejectUnsupported(const AstNode* nodep, const Ast::Template& item, std::string& rejection,
                       const std::string& reason) {
    if (!rejection.empty()) return;
    rejection = reason;
    nodep->v3warn(SUBGRAPH, "Subgraph scheduling does not support '"
                                << reason << "' in boundary module " << item.m_treep->prettyNameQ()
                                << "; using fallback scheduling.");
}

class ScheduleVertex final : public V3GraphVertex {
public:
    explicit ScheduleVertex(V3Graph* graphp)
        : V3GraphVertex{graphp} {}
};

struct FTaskAccess final {
    bool m_eligible = false;
    bool m_noInline = false;
    std::set<AstVar*> m_reads;
    std::set<AstVar*> m_writes;
    std::vector<AstNodeFTask*> m_callees;
};

class FTaskAccessVisitor final : public VNVisitorConst {
    const std::unordered_set<const AstVar*>& m_templateVariables;
    std::unordered_set<const AstVar*> m_ftaskVariables;
    std::set<AstVar*> m_reads;
    std::set<AstVar*> m_writes;
    std::vector<AstNodeFTask*> m_callees;
    bool m_eligible = true;
    bool m_noInline = false;

    void reject() { m_eligible = false; }

    void visit(AstNodeVarRef* nodep) override {
        AstVar* const varp = nodep->varp();
        if (!varp || m_ftaskVariables.count(varp)) return;
        if (!m_templateVariables.count(varp)) {
            reject();
            return;
        }
        if (nodep->access().isReadOrRW()) m_reads.insert(varp);
        if (nodep->access().isWriteOrRW()) m_writes.insert(varp);
    }
    void visit(AstNodeFTaskRef* nodep) override {
        AstNodeFTask* const taskp = nodep->taskp();
        if (!taskp) {
            reject();
        } else {
            m_callees.push_back(taskp);
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstAssignDly*) override { reject(); }
    void visit(AstNodeAssign* nodep) override {
        if (nodep->timingControlp()) {
            reject();
            return;
        }
        iterateChildrenConst(nodep);
    }
    void visit(AstDelay*) override { reject(); }
    void visit(AstEventControl*) override { reject(); }
    void visit(AstFork*) override { reject(); }
    void visit(AstPragma* nodep) override {
        if (nodep->pragType() == VPragmaType::NO_INLINE_TASK) m_noInline = true;
    }
    void visit(AstStmtExpr* nodep) override { iterateChildrenConst(nodep); }
    void visit(AstNode* nodep) override {
        if (!nodep->isPure()) {
            reject();
            return;
        }
        iterateChildrenConst(nodep);
    }

public:
    FTaskAccessVisitor(AstNodeFTask* taskp,
                       const std::unordered_set<const AstVar*>& templateVariables,
                       bool instanceLocal)
        : m_templateVariables{templateVariables} {
        if (!taskp || taskp->dpiImport() || taskp->recursive() || taskp->classMethod()
            || !taskp->stmtsp()) {
            reject();
            return;
        }
        // A static external task can retain package-global argument state between calls.
        if (!instanceLocal && !taskp->isFunction() && !taskp->lifetime().isAutomatic()) {
            reject();
            return;
        }
        const AstVar* const fvarp = VN_CAST(taskp->fvarp(), Var);
        if (taskp->isFunction()) {
            UASSERT_OBJ(fvarp, taskp, "Function has no return variable");
            m_ftaskVariables.insert(fvarp);
        }
        taskp->foreach([&](const AstVar* varp) { m_ftaskVariables.insert(varp); });
        for (const AstVar* const varp : m_ftaskVariables) {
            if (taskp->isFunction() && varp->isIO() && !varp->isFuncReturn()
                && varp->isWritable()) {
                reject();
            }
            if (!varp->lifetime().isAutomatic() && !varp->isIO() && !varp->isFuncReturn()
                && (taskp->isFunction() || !instanceLocal)) {
                reject();
            }
        }
        iterateAndNextConstNull(taskp->stmtsp());
    }
    FTaskAccess take() {
        FTaskAccess result;
        result.m_eligible = m_eligible;
        result.m_noInline = m_noInline;
        result.m_reads = std::move(m_reads);
        result.m_writes = std::move(m_writes);
        result.m_callees = std::move(m_callees);
        return result;
    }
};

class AccessVisitor final : public VNVisitorConst {
    const Ast::Template& m_item;
    const std::unordered_set<const AstVar*>& m_locals;
    const std::unordered_map<const AstNodeFTask*, FTaskAccess>& m_ftasks;
    const bool m_clocked;
    std::set<AstVar*>& m_reads;
    std::set<AstVar*>& m_writes;
    std::set<AstVar*>& m_immediateWrites;
    std::set<AstVar*>& m_delayedWrites;
    std::string& m_rejection;
    bool m_delayedLhs = false;

    const FTaskAccess* ftaskAccess(AstNodeFTaskRef* refp) const {
        AstNodeFTask* const taskp = refp->taskp();
        const auto it = taskp ? m_ftasks.find(taskp) : m_ftasks.end();
        return it == m_ftasks.end() || !it->second.m_eligible ? nullptr : &it->second;
    }

    void reject(const AstNode* nodep, const std::string& reason) {
        rejectUnsupported(nodep, m_item, m_rejection, reason);
    }
    void visit(AstAssignDly* nodep) override {
        if (!m_clocked) reject(nodep, "assignment phase");
        if (nodep->timingControlp()) reject(nodep, "assignment timing control");
        iterateConst(nodep->rhsp());
        VL_RESTORER(m_delayedLhs);
        m_delayedLhs = true;
        iterateConst(nodep->lhsp());
    }
    void visit(AstNodeAssign* nodep) override {
        if (nodep->timingControlp()) reject(nodep, "assignment timing control");
        iterateChildrenConst(nodep);
    }
    void visit(AstNodeVarRef* nodep) override {
        AstVar* const varp = nodep->varp();
        if (!varp || !m_locals.count(varp)) {
            reject(nodep, "nonlocal variable");
            return;
        }
        if (nodep->access().isReadOrRW()) m_reads.insert(varp);
        if (nodep->access().isWriteOrRW()) {
            m_writes.insert(varp);
            (m_delayedLhs ? m_delayedWrites : m_immediateWrites).insert(varp);
        }
    }
    void visit(AstNodeFTaskRef* nodep) override {
        const FTaskAccess* const accessp = ftaskAccess(nodep);
        if (!accessp) {
            reject(nodep, "task call");
            return;
        }
        iterateChildrenConst(nodep);
        m_reads.insert(accessp->m_reads.begin(), accessp->m_reads.end());
        m_writes.insert(accessp->m_writes.begin(), accessp->m_writes.end());
        m_immediateWrites.insert(accessp->m_writes.begin(), accessp->m_writes.end());
    }
    void visit(AstDelay* nodep) override { reject(nodep, "timing control"); }
    void visit(AstEventControl* nodep) override { reject(nodep, "timing control"); }
    void visit(AstFork* nodep) override { reject(nodep, "fork"); }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    AccessVisitor(AstNode* nodep, const Ast::Template& item,
                  const std::unordered_set<const AstVar*>& locals,
                  const std::unordered_map<const AstNodeFTask*, FTaskAccess>& ftasks, bool clocked,
                  std::set<AstVar*>& reads, std::set<AstVar*>& writes,
                  std::set<AstVar*>& immediateWrites, std::set<AstVar*>& delayedWrites,
                  std::string& rejection)
        : m_item{item}
        , m_locals{locals}
        , m_ftasks{ftasks}
        , m_clocked{clocked}
        , m_reads{reads}
        , m_writes{writes}
        , m_immediateWrites{immediateWrites}
        , m_delayedWrites{delayedWrites}
        , m_rejection{rejection} {
        iterateAndNextConstNull(nodep);
    }
};

class ScheduleBuilder final {
    const Ast::Template& m_item;
    Ast::Schedule m_schedule;
    std::unordered_map<const AstVar*, uint32_t> m_varIds;
    std::unordered_set<const AstVar*> m_locals;
    std::unordered_set<const AstNodeFTask*> m_localFTasks;
    std::unordered_map<const AstNodeFTask*, FTaskAccess> m_ftasks;
    std::unordered_set<const AstNodeFTask*> m_ftasksVisiting;
    std::map<std::pair<const AstVar*, bool>, uint32_t> m_triggerIds;
    std::string m_rejection;

    void reject(const AstNode* nodep, const std::string& reason) {
        rejectUnsupported(nodep, m_item, m_rejection, reason);
    }

    std::vector<AstVar*> ordered(const std::set<AstVar*>& vars) const {
        std::vector<AstVar*> result{vars.begin(), vars.end()};
        std::sort(result.begin(), result.end(), [&](const AstVar* lhs, const AstVar* rhs) {
            return m_varIds.at(lhs) < m_varIds.at(rhs);
        });
        return result;
    }

    Ast::Process process(AstNodeProcedure* procedurep, bool clocked, bool steady) {
        std::set<AstVar*> reads;
        std::set<AstVar*> writes;
        std::set<AstVar*> immediateWrites;
        std::set<AstVar*> delayedWrites;
        const AccessVisitor visitor{
            procedurep->stmtsp(), m_item,        m_locals,   m_ftasks, clocked, reads, writes,
            immediateWrites,      delayedWrites, m_rejection};
        Ast::Process result;
        result.m_procedurep = procedurep;
        result.m_reads = ordered(reads);
        result.m_writes = ordered(writes);
        result.m_immediateWrites = ordered(immediateWrites);
        result.m_delayedWrites = ordered(delayedWrites);
        if (steady) {
            for (AstVar* const varp : result.m_writes) {
                if (varp->isInput()) reject(varp, "input write");
            }
        }
        return result;
    }

    std::vector<uint32_t> triggers(AstSenTree* treep) {
        std::set<uint32_t> result;
        for (AstSenItem* itemp = treep->sensesp(); itemp; itemp = VN_AS(itemp->nextp(), SenItem)) {
            const bool posedge = itemp->edgeType() == VEdgeType::ET_POSEDGE;
            if (!posedge && itemp->edgeType() != VEdgeType::ET_NEGEDGE) {
                reject(itemp, "event edge type "s + itemp->edgeType().ascii());
                break;
            }
            if (itemp->condp()) {
                reject(itemp->condp(), "event condition");
                break;
            }
            AstNodeVarRef* const refp = VN_CAST(itemp->sensp(), NodeVarRef);
            AstVar* const varp = refp ? refp->varp() : nullptr;
            if (!varp || !m_locals.count(varp) || !varp->isInput()) {
                reject(itemp, "internal trigger");
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
                    reject(procedurep, "combinational procedure");
                }
            } else {
                reject(procedurep, "procedure kind");
            }
            if (!m_rejection.empty()) return;
        }
    }

    void validateDrivers() {
        std::unordered_map<const AstVar*, std::vector<uint32_t>> clockedDomains;
        for (const Ast::Process& process : m_schedule.m_pre) {
            for (const AstVar* const varp : process.m_writes) {
                const auto inserted = clockedDomains.emplace(varp, process.m_triggers);
                if (!inserted.second && inserted.first->second != process.m_triggers) {
                    reject(varp, "multiple event drivers");
                    return;
                }
            }
        }
        std::unordered_set<const AstVar*> refreshDrivers;
        for (const Ast::Process& process : m_schedule.m_refresh) {
            for (const AstVar* const varp : process.m_writes) {
                if (clockedDomains.count(varp) || !refreshDrivers.insert(varp).second) {
                    reject(varp, "multiple drivers");
                    return;
                }
            }
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
                reject(m_item.m_treep, "combinational cycle");
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
            for (AstVar* const varp : process.m_delayedWrites) {
                if (pending.count(varp)) continue;
                m_schedule.m_storage.push_back(Ast::Schedule::Storage{varp, true});
                pending.emplace(varp, static_cast<uint32_t>(m_schedule.m_storage.size()));
            }
        }
        const auto processes = [&](Ast::Phase phase) -> const std::vector<Ast::Process>& {
            switch (phase) {
            case Ast::Phase::STATIC: return m_schedule.m_static;
            case Ast::Phase::INITIAL: return m_schedule.m_initial;
            case Ast::Phase::PRE:
            case Ast::Phase::COMMIT: return m_schedule.m_pre;
            case Ast::Phase::REFRESH: return m_schedule.m_refresh;
            }
            VL_UNREACHABLE;
        };
        const auto addEntry = [&](Ast::Phase phase, const std::vector<uint32_t>& processIds,
                                  const std::vector<uint32_t>& triggers) {
            if (processIds.empty()) return;
            Ast::Schedule::Entry entry;
            entry.m_phase = phase;
            entry.m_processes = processIds;
            entry.m_triggers = triggers;
            std::map<uint32_t, Ast::Schedule::Use> uses;
            const auto read = [&](uint32_t storage) {
                if (storage) uses[storage].m_read = true;
            };
            const auto write = [&](uint32_t storage) {
                UASSERT(storage, "Attempt to write missing subgraph storage");
                uses[storage].m_write = true;
            };
            for (const uint32_t processId : processIds) {
                const Ast::Process& process = processes(phase).at(processId - 1);
                if (phase == Ast::Phase::COMMIT) {
                    for (const AstVar* const varp : process.m_delayedWrites) {
                        read(pending.at(varp));
                        write(current.at(varp));
                    }
                    continue;
                }
                for (const AstVar* const varp : process.m_reads) read(current.at(varp));
                for (const AstVar* const varp : process.m_immediateWrites) write(current.at(varp));
                for (const AstVar* const varp : process.m_delayedWrites) {
                    UASSERT(phase == Ast::Phase::PRE,
                            "Delayed assignment outside subgraph PRE phase");
                    read(current.at(varp));
                    write(pending.at(varp));
                }
            }
            for (auto& pair : uses) {
                pair.second.m_storage = pair.first;
                entry.m_uses.push_back(pair.second);
            }
            m_schedule.m_entries.push_back(std::move(entry));
        };
        const auto processIds = [](size_t count) {
            std::vector<uint32_t> result;
            result.reserve(count);
            for (size_t index = 0; index < count; ++index)
                result.push_back(static_cast<uint32_t>(index + 1));
            return result;
        };
        addEntry(Ast::Phase::STATIC, processIds(m_schedule.m_static.size()), {});
        addEntry(Ast::Phase::INITIAL, processIds(m_schedule.m_initial.size()), {});
        std::map<std::vector<uint32_t>, std::vector<uint32_t>> clockedDomains;
        for (size_t index = 0; index < m_schedule.m_pre.size(); ++index) {
            const Ast::Process& process = m_schedule.m_pre[index];
            clockedDomains[process.m_triggers].push_back(static_cast<uint32_t>(index + 1));
        }
        for (const auto& pair : clockedDomains) addEntry(Ast::Phase::PRE, pair.second, pair.first);
        for (const auto& pair : clockedDomains)
            addEntry(Ast::Phase::COMMIT, pair.second, pair.first);
        addEntry(Ast::Phase::REFRESH, processIds(m_schedule.m_refresh.size()), {});
    }

public:
    explicit ScheduleBuilder(const Ast::Template& item)
        : m_item{item} {
        for (const Ast::Template::Variable& variable : item.m_variables) {
            m_varIds.emplace(variable.m_formalp, variable.m_id);
            m_locals.insert(variable.m_formalp);
        }
        item.m_treep->foreach([&](const AstNodeFTask* taskp) { m_localFTasks.insert(taskp); });
        const std::function<bool(AstNodeFTask*)> analyzeFTask = [&](AstNodeFTask* taskp) {
            if (!taskp) return false;
            const auto found = m_ftasks.find(taskp);
            if (found != m_ftasks.end()) return found->second.m_eligible;
            if (!m_ftasksVisiting.insert(taskp).second) return false;
            FTaskAccess access
                = FTaskAccessVisitor{taskp, m_locals, m_localFTasks.count(taskp) != 0}.take();
            for (AstNodeFTask* const calleep : access.m_callees) {
                if (!analyzeFTask(calleep)) {
                    access.m_eligible = false;
                    continue;
                }
                const FTaskAccess& callee = m_ftasks.at(calleep);
                access.m_reads.insert(callee.m_reads.begin(), callee.m_reads.end());
                access.m_writes.insert(callee.m_writes.begin(), callee.m_writes.end());
            }
            if (access.m_noInline && (!access.m_reads.empty() || !access.m_writes.empty())) {
                access.m_eligible = false;
            }
            m_ftasksVisiting.erase(taskp);
            const bool eligible = access.m_eligible;
            m_ftasks.emplace(taskp, std::move(access));
            return eligible;
        };
        item.m_treep->foreach([&](AstNodeFTask* taskp) { analyzeFTask(taskp); });
        for (AstNode* nodep = item.m_treep->stmtsp(); nodep; nodep = nodep->nextp()) {
            AstNodeProcedure* const procedurep = VN_CAST(nodep, NodeProcedure);
            if (!procedurep || !procedurep->stmtsp()) continue;
            procedurep->stmtsp()->foreachAndNext(
                [&](AstNodeFTaskRef* refp) { analyzeFTask(refp->taskp()); });
        }
        procedures();
        if (m_rejection.empty()) validateDrivers();
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
