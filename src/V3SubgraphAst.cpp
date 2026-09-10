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
        m_templates.push_back(Template{id, modp, treep, 0});
        Template& item = m_templates.back();
        for (AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
            AstVar* const sourcep = VN_CAST(nodep, Var);
            if (!sourcep || !sourcep->isIO()) continue;
            AstVar* const formalp = sourcep->clonep();
            UASSERT_OBJ(formalp, sourcep, "Subgraph V3Ast port declaration was not cloned");
            const uint32_t portId = static_cast<uint32_t>(item.m_ports.size() + 1);
            item.m_ports.push_back(Template::Port{portId, sourcep, formalp});
        }
        ids.emplace(modp, id);
        nodes += nodeCount(treep);
    }
    if (!ids.empty()) {
        collectInstances(netlistp->topModulep(), nullptr, "TOP", ids, m_templates, m_instances);
    }

    V3Stats::addStat("Scheduling, Subgraph V3Ast candidates", candidates);
    V3Stats::addStat("Scheduling, Subgraph V3Ast templates", m_templates.size());
    V3Stats::addStat("Scheduling, Subgraph V3Ast nodes", nodes);
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
