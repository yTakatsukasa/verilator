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

void countInstances(const AstNodeModule* modp, const TemplateIdMap& ids,
                    std::vector<V3SubgraphAst::Template>& templates) {
    const auto it = ids.find(modp);
    if (it != ids.end()) ++templates[it->second - 1].m_instances;
    for (const AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
        if (const AstCell* const cellp = VN_CAST(nodep, Cell)) {
            countInstances(cellp->modp(), ids, templates);
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
        ids.emplace(modp, id);
        nodes += nodeCount(treep);
    }
    if (!ids.empty()) countInstances(netlistp->topModulep(), ids, m_templates);

    V3Stats::addStat("Scheduling, Subgraph V3Ast candidates", candidates);
    V3Stats::addStat("Scheduling, Subgraph V3Ast templates", m_templates.size());
    V3Stats::addStat("Scheduling, Subgraph V3Ast nodes", nodes);
    uint64_t instances = 0;
    for (const Template& item : m_templates) instances += item.m_instances;
    V3Stats::addStat("Scheduling, Subgraph V3Ast instance memberships", instances);
    V3Stats::addStatPerf("Scheduling, Subgraph V3Ast capture time (sec)", timer.deltaTime());
    check();
}

V3SubgraphAst::~V3SubgraphAst() {
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
}
