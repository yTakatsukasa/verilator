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
#include "V3Sched.h"

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
    if (0 == name.rfind("__VsubgraphClockedBarrier__", 0)) return "subgraph-clocked-barrier";
    if (0 == name.rfind("__VsubgraphPostBarrier__", 0)) return "subgraph-post-barrier";
    if (0 == name.rfind("__VsubgraphSnapshotBarrier__", 0)) return "subgraph-snapshot-barrier";
    if (0 == name.rfind("__VsubgraphSnapshot__", 0)) return "subgraph-snapshot";
    if (0 == name.rfind("__Vdly__", 0)) return "delayed-shadow";
    return "";
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
        string msg = "-Loop-Var: name='" + varp->vscp()->name() + "' role='" + role + "'";
        if (const string kind = orderGraphSubgraphVarKind(varp->vscp()->varp()->name());
            !kind.empty()) {
            msg += " kind='" + kind + "'";
        }
        msg += " scope='" + varp->vscp()->scopep()->prettyName() + "'";
        if (AstScope* const boundaryp = orderGraphSubgraphBoundaryScope(varp->vscp()->scopep())) {
            msg += " subgraph='" + boundaryp->modp()->prettyName() + "'";
        }
        msg += orderGraphFilelineSummary(varp->vscp()->fileline());
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
                         AstScope* resultScopep) {
    // Build the OrderGraph
    const std::unique_ptr<OrderGraph> graph = buildOrderGraph(netlistp, logic, trigToSen);
    // Order it
    orderOrderGraph(*graph, tag);
    // Assign sensitivity domains to combinational logic
    processDomains(netlistp, *graph, tag, externalDomains);
    // Build the move graph
    OrderMoveDomScope::clear();
    const std::unique_ptr<OrderMoveGraph> moveGraphp = OrderMoveGraph::build(*graph, trigToSen);
    if (dumpGraphLevel() >= 9) moveGraphp->dumpDotFilePrefixed(tag + "_ordermv");

    // The ordered statements, if there are any
    AstNodeStmt* stmtsp = nullptr;
    if (!moveGraphp->empty()) {
        if (parallel) {
            stmtsp = createParallel(*graph, *moveGraphp, tag, slow);
        } else {
            stmtsp = createSerial(*moveGraphp, tag, slow);
        }
        // Should have consumed all vertices
        UASSERT(moveGraphp->empty(), "Unconsumed vertices remain in OrderMoveGraph");
    }
    OrderMoveDomScope::clear();

    // Dump data
    if (dumpGraphLevel()) graph->dumpDotFilePrefixed(tag + "_orderg_done");

    // Dispose of the remnants of the inputs
    for (auto* const lbsp : logic) lbsp->deleteActives();

    // If there is no resulting logic, then don't create an empty function
    if (!stmtsp) return nullptr;

    // Create the result function
    FileLine* const flp = netlistp->fileline();
    AstCFunc* const funcp = [&]() {
        AstScope* const scopep = resultScopep ? resultScopep : netlistp->topScopep()->scopep();
        AstCFunc* const resp = new AstCFunc{flp, "_eval_" + tag, scopep, ""};
        resp->dontCombine(true);
        resp->isStatic(false);
        resp->isLoose(true);
        resp->slow(slow);
        resp->isConst(false);
        resp->declPrivate(true);
        scopep->addBlocksp(resp);
        return resp;
    }();

    // Assemble the body
    if (v3Global.opt.profExec()) {
        funcp->addStmtsp(AstCStmt::profExecSectionPush(flp, "func " + tag));
    }
    funcp->addStmtsp(stmtsp);
    if (v3Global.opt.profExec()) {  //
        funcp->addStmtsp(AstCStmt::profExecSectionPop(flp, "func " + tag));
    }

    // Done
    return funcp;
}
