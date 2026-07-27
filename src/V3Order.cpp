// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Block code ordering
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
// V3Order's Transformations:
//
//  Compute near optimal scheduling of always/wire statements
//  Make a graph of the entire netlist
//
//      For seq logic
//          Add logic_sensitive_vertex for this list of SenItems
//              Add edge for each sensitive_var->logic_sensitive_vertex
//          For AlwaysPre's
//              Add vertex for this logic
//                  Add edge logic_sensitive_vertex->logic_vertex
//                  Add edge logic_consumed_var_PREVAR->logic_vertex
//                  Add edge logic_vertex->logic_generated_var (same as if comb)
//                  Add edge logic_vertex->generated_var_PREORDER
//                      Cutable dependency to attempt to order dlyed
//                      assignments to avoid saving state, thus we prefer
//                              a <= b ...      As the opposite order would
//                              b <= c ...      require the old value of b.
//                  Add edge consumed_var_POST->logic_vertex
//                      This prevents a consumer of the "early" value to be
//                      scheduled after we've changed to the next-cycle value
//          For Logic
//              Add vertex for this logic
//                  Add edge logic_sensitive_vertex->logic_vertex
//                  Add edge logic_generated_var_PREORDER->logic_vertex
//                      This ensures the AlwaysPre gets scheduled before this logic
//                  Add edge logic_vertex->consumed_var_PREVAR
//                  Add edge logic_vertex->consumed_var_POSTVAR
//                  Add edge logic_vertex->logic_generated_var (same as if comb)
//          For AlwaysPost's
//              Add vertex for this logic
//                  Add edge logic_sensitive_vertex->logic_vertex
//                  Add edge logic_consumed_var->logic_vertex (same as if comb)
//                  Add edge logic_vertex->logic_generated_var (same as if comb)
//                  Add edge consumed_var_POST->logic_vertex (same as if comb)
//
//      For comb logic
//          For comb logic
//              Add vertex for this logic
//              Add edge logic_consumed_var->logic_vertex
//              Add edge logic_vertex->logic_generated_var
//                  Mark it cutable, as circular logic may require
//                  the generated signal to become a primary input again.
//
//
//
//   Rank the graph starting at INPUTS (see V3Graph)
//
//   Visit the graph's logic vertices in ranked order
//      For all logic vertices with all inputs already ordered
//         Make ordered block for this module
//         For all ^^ in same domain
//              Move logic to ordered activation
//      When we have no more choices, we move to the next module
//      and make a new block.  Add that new activation block to the list of calls to make.
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3OrderInternal.h"
#include "V3Os.h"
#include "V3Sched.h"
#include "V3SchedSubgraph.h"
#include "V3Stats.h"

#include <memory>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {
AstScope* orderGraphSubgraphBoundaryScope(AstScope* scopep) {
    for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
        if (scanp->modp()->subgraphBoundary()) return scanp;
    }
    return nullptr;
}

string orderGraphFilelineSummary(FileLine* flp) {
    if (!flp) return "";
    return " file='" + flp->filebasename() + ":" + cvtToStr(flp->lineno()) + "'";
}

string orderGraphSubgraphVarKind(const string& name) {
    if (0 == name.rfind("__VsubgraphSnapshot__", 0)) return "subgraph-snapshot";
    if (0 == name.rfind("__Vdly__", 0)) return "delayed-shadow";
    return "";
}

uint64_t statStartUsecs() {
    if (!v3Global.opt.stats()) return 0;
    return V3Os::timeUsecs();
}

void addElapsedStat(const string& prefix, const string& name, uint64_t startUsecs) {
    if (!startUsecs) return;
    const uint64_t elapsedUsecs = V3Os::timeUsecs() - startUsecs;
    V3Stats::addStat(prefix + "time " + name + " sec", elapsedUsecs / 1.0e6, 6);
}

AstCFunc* makeResultFunction(AstNetlist* netlistp, AstNodeStmt* stmtsp, const string& tag,
                             bool slow, AstScope* resultScopep) {
    if (!stmtsp) return nullptr;
    FileLine* const flp = netlistp->fileline();
    AstScope* const scopep = resultScopep ? resultScopep : netlistp->topScopep()->scopep();
    AstCFunc* const funcp = new AstCFunc{flp, "_eval_" + tag, scopep, ""};
    funcp->dontCombine(true);
    funcp->isStatic(false);
    funcp->isLoose(true);
    funcp->slow(slow);
    funcp->isConst(false);
    funcp->declPrivate(true);
    scopep->addBlocksp(funcp);

    if (v3Global.opt.profExec()) {
        funcp->addStmtsp(AstCStmt::profExecSectionPush(flp, "func " + tag));
    }
    funcp->addStmtsp(stmtsp);
    if (v3Global.opt.profExec()) {
        funcp->addStmtsp(AstCStmt::profExecSectionPop(flp, "func " + tag));
    }
    return funcp;
}

struct OrderGraphStats final {
    uint64_t m_edges = 0;
    uint64_t m_hardEdges = 0;
    uint64_t m_logicVertices = 0;
    uint64_t m_phaseVertices = 0;
    uint64_t m_pordVertices = 0;
    uint64_t m_postVertices = 0;
    uint64_t m_preVertices = 0;
    uint64_t m_softEdges = 0;
    uint64_t m_stdVertices = 0;
    uint64_t m_subgraphLogicVertices = 0;
    uint64_t m_varVertices = 0;
    uint64_t m_vertices = 0;
};

OrderGraphStats collectOrderGraphStats(const V3Graph& graph) {
    OrderGraphStats stats;
    for (const V3GraphVertex& vertex : graph.vertices()) {
        ++stats.m_vertices;
        for (const V3GraphEdge& edge : vertex.outEdges()) {
            ++stats.m_edges;
            if (edge.cutable()) {
                ++stats.m_softEdges;
            } else {
                ++stats.m_hardEdges;
            }
        }
        const auto* const logicp = dynamic_cast<const OrderLogicVertex*>(&vertex);
        if (logicp) {
            ++stats.m_logicVertices;
            if (VN_IS(logicp->nodep(), SubgraphInstance)) ++stats.m_subgraphLogicVertices;
            continue;
        }
        if (dynamic_cast<const OrderVarStdVertex*>(&vertex)) {
            ++stats.m_stdVertices;
        } else if (dynamic_cast<const OrderVarPreVertex*>(&vertex)) {
            ++stats.m_preVertices;
        } else if (dynamic_cast<const OrderVarPostVertex*>(&vertex)) {
            ++stats.m_postVertices;
        } else if (dynamic_cast<const OrderVarPordVertex*>(&vertex)) {
            ++stats.m_pordVertices;
        } else if (dynamic_cast<const OrderSubgraphPhaseVertex*>(&vertex)) {
            ++stats.m_phaseVertices;
        }
        if (dynamic_cast<const OrderVarVertex*>(&vertex)) ++stats.m_varVertices;
    }
    return stats;
}

void reportOrderGraphStats(const string& tag, const string& stage, const V3Graph& graph) {
    if (!v3Global.opt.stats()) return;
    const OrderGraphStats stats = collectOrderGraphStats(graph);
    const string prefix = "Scheduling, Order " + tag + " " + stage + ", ";
    V3Stats::addStat(prefix + "edges", stats.m_edges);
    V3Stats::addStat(prefix + "edges hard", stats.m_hardEdges);
    V3Stats::addStat(prefix + "edges soft", stats.m_softEdges);
    V3Stats::addStat(prefix + "vertices", stats.m_vertices);
    V3Stats::addStat(prefix + "vertices logic", stats.m_logicVertices);
    V3Stats::addStat(prefix + "vertices logic subgraph", stats.m_subgraphLogicVertices);
    V3Stats::addStat(prefix + "vertices var", stats.m_varVertices);
    V3Stats::addStat(prefix + "vertices var phase", stats.m_phaseVertices);
    V3Stats::addStat(prefix + "vertices var pord", stats.m_pordVertices);
    V3Stats::addStat(prefix + "vertices var post", stats.m_postVertices);
    V3Stats::addStat(prefix + "vertices var pre", stats.m_preVertices);
    V3Stats::addStat(prefix + "vertices var std", stats.m_stdVertices);
}

void markSubgraphInputRefreshes(OrderGraph& graph) {
    // Acyclic preserves the pre-cut SCC colors. Refresh a subgraph input if its soft dependency
    // participated in a cycle, even when acyclic chose another edge in that cycle to cut.
    for (V3GraphVertex& vertex : graph.vertices()) {
        auto* const logicp = dynamic_cast<OrderLogicVertex*>(&vertex);
        if (!logicp) continue;
        AstSubgraphInstance* const subgraphp = VN_CAST(logicp->nodep(), SubgraphInstance);
        if (!subgraphp) continue;
        for (const V3GraphEdge& edge : vertex.inEdges()) {
            if (vertex.color() && edge.cutable() && edge.fromp()->color() == vertex.color()) {
                V3Sched::rememberSubgraphInputRefreshInstance(subgraphp);
                break;
            }
        }
    }
}
}  // namespace

string OrderGraph::loopsVertexCb(V3GraphVertex* vertexp) {
    if (const auto* const logicp = dynamic_cast<const OrderLogicVertex*>(vertexp)) {
        string msg = "-Loop-Logic: type='" + string{logicp->nodep()->typeName()} + "'";
        if (const auto* const ccallp = VN_CAST(logicp->nodep(), CCall)) {
            msg += " func='" + ccallp->funcp()->name() + "'";
        }
        msg += " scope='" + logicp->scopep()->prettyName() + "'";
        if (AstScope* const boundaryp = orderGraphSubgraphBoundaryScope(logicp->scopep())) {
            msg += " subgraph='" + boundaryp->modp()->prettyName() + "'";
        }
        msg += orderGraphFilelineSummary(logicp->nodep()->fileline());
        return msg + "\n";
    }
    if (const auto* const varp = dynamic_cast<const OrderVarVertex*>(vertexp)) {
        const string role = varp->nameSuffix().empty() ? "STD" : varp->nameSuffix();
        string msg = "-Loop-Var: name='" + varp->debugName() + "' role='" + role + "'";
        if (const auto* const phasep = dynamic_cast<const OrderSubgraphPhaseVertex*>(varp)) {
            switch (phasep->kind()) {
            case OrderSubgraphPhaseVertex::Kind::CLOCKED:
                msg += " kind='subgraph-clocked-phase'";
                break;
            case OrderSubgraphPhaseVertex::Kind::POST: msg += " kind='subgraph-post-phase'"; break;
            case OrderSubgraphPhaseVertex::Kind::SNAPSHOT:
                msg += " kind='subgraph-snapshot-phase'";
                break;
            }
        } else if (varp->hasVarScope()) {
            const string kind = orderGraphSubgraphVarKind(varp->vscp()->varp()->name());
            if (!kind.empty()) { msg += " kind='" + kind + "'"; }
            msg += " scope='" + varp->vscp()->scopep()->prettyName() + "'";
            if (AstScope* const boundaryp
                = orderGraphSubgraphBoundaryScope(varp->vscp()->scopep())) {
                msg += " subgraph='" + boundaryp->modp()->prettyName() + "'";
            }
            msg += orderGraphFilelineSummary(varp->vscp()->fileline());
        }
        if (!varp->hasVarScope()) {
            for (const V3GraphEdge& edge : vertexp->inEdges()) {
                const auto* const logicEdgep = &edge;
                if (const auto* const logicp
                    = dynamic_cast<const OrderLogicVertex*>(logicEdgep->fromp())) {
                    msg += " scope='" + logicp->scopep()->prettyName() + "'";
                    if (AstScope* const boundaryp
                        = orderGraphSubgraphBoundaryScope(logicp->scopep())) {
                        msg += " subgraph='" + boundaryp->modp()->prettyName() + "'";
                    }
                    msg += orderGraphFilelineSummary(logicp->nodep()->fileline());
                    break;
                }
            }
        }
        return msg + "\n";
    }
    return V3Graph::loopsVertexCb(vertexp);
}

void OrderGraph::loopsMessageCb(V3GraphVertex* vertexp, V3EdgeFuncP edgeFuncp) {
    const string loops = reportLoops(edgeFuncp, vertexp);
    const bool subgraphRelated = loops.find("subgraph='") != string::npos
                                 || loops.find("kind='subgraph-") != string::npos;
    vertexp->v3fatalSrc(
        "Loops detected in graph: "
        << vertexp << "\n"
        << (subgraphRelated
                ? "Possible false circularity through subgraph scheduling; loop trace follows.\n"
                : "")
        << loops);
}

void V3Order::orderOrderGraph(OrderGraph& graph, const std::string& tag) {
    // Dump data
    if (dumpGraphLevel()) graph.dumpDotFilePrefixed(tag + "_orderg_pre");

    // Break cycles. Note that the OrderGraph only contains cuttable cycles
    // (soft constraints). Actual logic loops must have been eliminated by
    // the introduction of Hybid sensitivity expressions, before invoking
    // ordering (e.g. in V3SchedAcyclic).
    graph.acyclic(&V3GraphEdge::followAlwaysTrue);
    if (v3Global.opt.subgraphSchedule()) markSubgraphInputRefreshes(graph);
    if (dumpGraphLevel()) graph.dumpDotFilePrefixed(tag + "_orderg_acyc");

    // Assign ranks so we know what to follow, then sort vertices and edges by that ordering
    graph.order();
    if (dumpGraphLevel()) graph.dumpDotFilePrefixed(tag + "_orderg_order");
}

//######################################################################

AstCFunc* V3Order::order(AstNetlist* netlistp,  //
                         const std::vector<V3Sched::LogicByScope*>& logic,  //
                         const V3Order::TrigToSenMap& trigToSen,
                         const string& tag,  //
                         bool parallel,  //
                         bool slow,  //
                         const ExternalDomainsProvider& externalDomains,  //
                         AstScope* resultScopep,
                         std::shared_ptr<const V3Order::OrderRecipe>* recipepp) {
    std::shared_ptr<OrderRecipe> recipep;
    std::unordered_map<const AstNode*, size_t> logicIndex;
    std::unordered_map<const AstNode*, AstSenTree*> sourceDomain;
    if (recipepp && !parallel) {
        recipep = std::make_shared<OrderRecipe>();
        for (const V3Sched::LogicByScope* const lbsp : logic) {
            for (const auto& pair : *lbsp) {
                for (AstNode* nodep = pair.second->stmtsp(); nodep; nodep = nodep->nextp()) {
                    const size_t index = logicIndex.size();
                    logicIndex.emplace(nodep, index);
                    sourceDomain.emplace(nodep, pair.second->sentreep());
                }
            }
        }
        recipep->m_logicCount = logicIndex.size();
    }
    // Build the OrderGraph
    const string statPrefix = "Scheduling, Order " + tag + ", ";
    uint64_t startUsecs = statStartUsecs();
    const std::unique_ptr<OrderGraph> graph = buildOrderGraph(netlistp, logic, trigToSen, tag);
    addElapsedStat(statPrefix, "build graph", startUsecs);
    reportOrderGraphStats(tag, "built", *graph);
    // Order it
    startUsecs = statStartUsecs();
    orderOrderGraph(*graph, tag);
    addElapsedStat(statPrefix, "order graph", startUsecs);
    reportOrderGraphStats(tag, "ordered", *graph);
    // Assign sensitivity domains to combinational logic
    startUsecs = statStartUsecs();
    processDomains(netlistp, *graph, tag, externalDomains);
    addElapsedStat(statPrefix, "process domains", startUsecs);
    reportOrderGraphStats(tag, "domains", *graph);
    // Build the move graph
    OrderMoveDomScope::clear();
    startUsecs = statStartUsecs();
    const std::unique_ptr<OrderMoveGraph> moveGraphp = OrderMoveGraph::build(*graph, trigToSen);
    addElapsedStat(statPrefix, "build move graph", startUsecs);
    reportOrderGraphStats(tag, "move built", *moveGraphp);
    if (dumpGraphLevel() >= 9) moveGraphp->dumpDotFilePrefixed(tag + "_ordermv");

    // The ordered statements, if there are any
    AstNodeStmt* stmtsp = nullptr;
    if (!moveGraphp->empty()) {
        if (parallel) {
            startUsecs = statStartUsecs();
            stmtsp = createParallel(*graph, *moveGraphp, tag, slow);
        } else {
            startUsecs = statStartUsecs();
            stmtsp = createSerial(*moveGraphp, tag, slow, recipep ? &logicIndex : nullptr,
                                  recipep ? &sourceDomain : nullptr, recipep.get());
        }
        addElapsedStat(statPrefix, parallel ? "create parallel" : "create serial", startUsecs);
        // Should have consumed all vertices
        UASSERT(moveGraphp->empty(), "Unconsumed vertices remain in OrderMoveGraph");
    }
    OrderMoveDomScope::clear();

    // Dump data
    if (dumpGraphLevel()) graph->dumpDotFilePrefixed(tag + "_orderg_done");

    // Dispose of the remnants of the inputs
    for (auto* const lbsp : logic) lbsp->deleteActives();

    if (recipepp) {
        if (stmtsp && recipep && recipep->m_replayable
            && recipep->m_entries.size() == recipep->m_logicCount) {
            *recipepp = recipep;
        } else {
            recipepp->reset();
        }
    }
    return makeResultFunction(netlistp, stmtsp, tag, slow, resultScopep);
}

AstCFunc* V3Order::replay(AstNetlist* netlistp, const std::vector<V3Sched::LogicByScope*>& logic,
                          const V3Order::OrderRecipe& recipe, const string& tag, bool slow,
                          AstScope* resultScopep) {
    if (!recipe.m_replayable) return nullptr;
    AstNodeStmt* const stmtsp = replaySerial(logic, recipe, tag, slow);
    if (!stmtsp) return nullptr;
    for (V3Sched::LogicByScope* const lbsp : logic) lbsp->deleteActives();
    return makeResultFunction(netlistp, stmtsp, tag, slow, resultScopep);
}
