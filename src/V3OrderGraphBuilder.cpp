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
//
//  Initial graph dependency builder for ordering
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3AstUserAllocator.h"
#include "V3Graph.h"
#include "V3OrderGraph.h"
#include "V3OrderInternal.h"
#include "V3Sched.h"
#include "V3Stats.h"

#include <unordered_set>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// Order information stored under each AstNode::user1p()...

class OrderUser final {
    // Stored in AstVarScope::user1p, a list of all the various vertices
    // that can exist for one given scoped variable
public:
    // TYPES
    enum class VarVertexType : uint8_t {  // Types of vertices we can create
        STD = 0,
        PRE = 1,
        PORD = 2,
        POST = 3
    };

private:
    // Vertex of each type (if non-nullptr)
    std::array<OrderVarVertex*, static_cast<size_t>(VarVertexType::POST) + 1> m_vertexps;

public:
    // METHODS
    OrderVarVertex* getVarVertex(OrderGraph* graphp, AstVarScope* varscp, VarVertexType type) {
        const unsigned idx = static_cast<unsigned>(type);
        OrderVarVertex* vertexp = m_vertexps[idx];
        if (!vertexp) {
            switch (type) {
            case VarVertexType::STD: vertexp = new OrderVarStdVertex{graphp, varscp}; break;
            case VarVertexType::PRE: vertexp = new OrderVarPreVertex{graphp, varscp}; break;
            case VarVertexType::PORD: vertexp = new OrderVarPordVertex{graphp, varscp}; break;
            case VarVertexType::POST: vertexp = new OrderVarPostVertex{graphp, varscp}; break;
            }
            m_vertexps[idx] = vertexp;
        }
        return vertexp;
    }

    // CONSTRUCTORS
    OrderUser() { m_vertexps.fill(nullptr); }
    ~OrderUser() = default;
};

//######################################################################
// OrderBuildVisitor builds the ordering graph of the entire netlist, and
// removes any nodes that are no longer required once the graph is built

class OrderGraphBuilder final : public VNVisitor {
    // TYPES
    enum VarUsage : uint8_t { VU_CON = 0x1, VU_GEN = 0x2 };
    enum VarAccess : uint8_t { VA_READ = 0x1, VA_WRITE = 0x2 };
    using VarVertexType = OrderUser::VarVertexType;

    // NODE STATE
    //  AstVarScope::user1    -> OrderUser instance for variable (via m_orderUser)
    //  AstVarScope::user2    -> VarUsage within logic blocks
    //  AstVarScope::user3    -> bool: Hybrid sensitivity
    //  AstVarScope::user4    -> VarAccess within logic blocks
    const VNUser1InUse user1InUse;
    const VNUser2InUse user2InUse;
    const VNUser3InUse user3InUse;
    const VNUser4InUse user4InUse;
    AstUser1Allocator<AstVarScope, OrderUser> m_orderUser;

    // STATE
    OrderGraph* const m_graphp = new OrderGraph;  // The ordering graph built by this visitor
    OrderLogicVertex* m_logicVxp = nullptr;  // Current logic block being analyzed
    std::vector<AstVarScope*> m_accessedVscps;  // Variables accessed by the current logic block
    std::unordered_set<const AstVarScope*> m_parentAccessedVscps;
    uint64_t m_subgraphContractNodes = 0;
    uint64_t m_subgraphContractUses = 0;
    uint64_t m_subgraphContractCuttableUses = 0;

    // Map from Trigger reference AstSenItem to the original AstSenTree
    const V3Order::TrigToSenMap& m_trigToSen;

    // Current AstScope being processed
    AstScope* m_scopep = nullptr;
    // Sensitivity list for clocked logic, nullptr for combinational and hybrid logic
    AstSenTree* m_domainp = nullptr;
    // Sensitivity list for hybrid logic, nullptr for everything else
    AstSenTree* m_hybridp = nullptr;

    bool m_inClocked = false;  // Underneath clocked AstActive
    bool m_inPre = false;  // Underneath AlwaysPre
    bool m_inPost = false;  // Underneath AstAlwaysPost
    bool m_softSubgraphRead = false;  // Cuttable read in a coarse subgraph contract
    std::function<bool(const AstVarScope*)> m_readTriggersCombLogic;
    V3Sched::util::VarScopeSet m_forceReadEdgeIgnores;
    const bool m_parallel;  // Ordering for multi-threaded execution (record variable accesses)

    // METHODS

    void iterateLogic(AstNode* nodep) {
        UASSERT_OBJ(!m_logicVxp, nodep, "Should not nest");
        // Reset VarUsage and VarAccess
        AstNode::user2ClearTree();
        AstNode::user4ClearTree();
        m_forceReadEdgeIgnores.clear();
        if (!m_inClocked)
            V3Sched::util::collectForceReadEdgeIgnores(nodep, m_forceReadEdgeIgnores);
        // Create LogicVertex for this logic node
        m_logicVxp = new OrderLogicVertex{m_graphp, m_scopep, m_domainp, m_hybridp, nodep};
        // Gather variable dependencies based on usage
        iterateChildren(nodep);
        if (m_parallel) {
            // Emit one access record for each variable this logic block accessed
            for (AstVarScope* const vscp : m_accessedVscps) {
                const int recorded = vscp->user4();
                const VAccess access = recorded == (VA_READ | VA_WRITE) ? VAccess::READWRITE
                                       : recorded == VA_WRITE           ? VAccess::WRITE
                                                                        : VAccess::READ;
                m_logicVxp->addVarAccess(vscp, access);
            }
            m_accessedVscps.clear();
        }
        // Finished with this logic
        m_logicVxp = nullptr;
        m_forceReadEdgeIgnores.clear();
    }

    OrderVarVertex* getVarVertex(AstVarScope* varscp, VarVertexType type) {
        return m_orderUser(varscp).getVarVertex(m_graphp, varscp, type);
    }

    static bool isSubgraphWrapperCall(const AstCCall* nodep) {
        const AstCFunc* const funcp = nodep->funcp();
        const AstScope* const scopep = funcp->scopep();
        return scopep && scopep->modp()->subgraphBoundary()
               && 0 == funcp->name().rfind("_eval_body__nba_subgraph_", 0);
    }

    static bool containsSubgraphInstance(AstActive* nodep) {
        bool found = false;
        nodep->foreach([&](AstSubgraphInstance*) { found = true; });
        return found;
    }

    bool shouldGroupSubgraphWrapperActive(AstActive* nodep) const {
        for (AstNode* stmtp = nodep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (VN_IS(stmtp, NodeProcedure)) return false;
        }
        return containsSubgraphInstance(nodep);
    }

    // VISITORS
    void visit(AstActive* nodep) override {
        UASSERT_OBJ(!nodep->senTreeStorep(), nodep,
                    "AstSenTrees should have been made global in V3ActiveTop");
        UASSERT_OBJ(m_scopep, nodep, "AstActive not under AstScope");
        UASSERT_OBJ(!m_logicVxp, nodep, "AstActive under logic");
        UASSERT_OBJ(!m_inClocked && !m_domainp && !m_hybridp, nodep, "Should not nest");

        VL_RESTORER(m_domainp);
        VL_RESTORER(m_hybridp);
        VL_RESTORER(m_inClocked);

        // This is the original sensitivity of the block (i.e.: not the ref into the trigger vec)

        const AstSenTree* const senTreep = nodep->sentreep()->hasCombo()
                                               ? nodep->sentreep()
                                               : m_trigToSen.at(nodep->sentreep());

        m_inClocked = senTreep->hasClocked();

        // Note: We don't need to analyze the sensitivity list, as currently all sensitivity
        // lists simply reference an entry in a trigger vector, which are all set external to
        // the code being ordered.

        // Combinational and hybrid logic will have it's domain assigned based on the driver
        // domains. For clocked logic, we already know its domain.
        if (!senTreep->hasCombo() && !senTreep->hasHybrid()) m_domainp = nodep->sentreep();

        // Hybrid logic also includes additional sensitivities
        if (senTreep->hasHybrid()) {
            m_hybridp = nodep->sentreep();
            // Mark AstVarScopes that are explicit sensitivities
            AstNode::user3ClearTree();
            senTreep->foreach([](const AstVarRef* refp) {  //
                refp->varScopep()->user3(true);
            });
            m_readTriggersCombLogic = [](const AstVarScope* vscp) { return !vscp->user3(); };
        } else {
            // Always triggers
            m_readTriggersCombLogic = [](const AstVarScope*) { return true; };
        }

        // Analyze logic underneath. A subgraph wrapper represents its entire helper as one
        // coarse logic vertex; only its boundary contract is visible to this parent graph.
        if (shouldGroupSubgraphWrapperActive(nodep)) {
            iterateLogic(nodep);
        } else {
            iterateChildren(nodep);
        }
    }
    void addVarUsage(AstNode* nodep, AstVarScope* varscp, bool read, bool write) {
        // As we explicitly not visit (see ignored nodes below) any subtree that is not relevant
        // for ordering, we should be able to assert this:
        UASSERT_OBJ(m_scopep, nodep, "AstVarRef not under scope");
        UASSERT_OBJ(m_logicVxp, nodep, "AstVarRef not under logic");
        UASSERT_OBJ(varscp, nodep, "Var didn't get varscoped in V3Scope.cpp");

        // Variable reference in logic. Add data dependency.

        // Record the raw access for the multi-threaded data hazard fixer
        if (m_parallel) {
            uint8_t recorded = 0;
            if (write) recorded |= VA_WRITE;
            if (read) recorded |= VA_READ;
            UASSERT_OBJ(recorded, nodep, "Unknown variable access type");
            // Accumulate access type, record the variable on first access only
            if (!varscp->user4Or(recorded)) m_accessedVscps.push_back(varscp);
        }

        // Check whether this variable was already generated/consumed in the same logic. We
        // don't want to add extra edges if the logic has many usages of the same variable,
        // so only proceed on first encounter.
        const bool prevGen = varscp->user2() & VU_GEN;
        const bool prevCon = varscp->user2() & VU_CON;

        // Compute whether the variable is produced (written) here
        const bool gen = !prevGen && write && !varscp->varp()->ignoreSchedWrite();

        // Compute whether the value is consumed (read) here
        bool con = false;
        if (!prevCon && read) {
            con = true;
            if (prevGen && !m_inClocked) {
                // Dangerous assumption:
                // If a variable is consumed in the same combinational process that produced it
                // earlier, consider it something like:
                //      foo = 1
                //      foo = foo + 1
                // and still optimize. Note this will break though:
                //      if (sometimes) foo = 1
                //      foo = foo + 1
                // TODO: Do this properly with liveness analysis (i.e.: if live, it's consumed)
                //       Note however that this construct is not nicely synthesizable (yields
                //       latch?).
                con = false;
            }
            if (!m_inClocked && m_forceReadEdgeIgnores.count(varscp)) con = false;
        }

        // Note: See V3OrderGraph.h about the roles of the various vertex types

        // Variable is produced
        if (gen) {
            // Update VarUsage
            varscp->user2Or(VU_GEN);
            // Add edges for produced variables
            if (m_inPost) {
                if (!varscp->varp()->ignorePostWrite()) {
                    // Add edge from producing LogicVertex -> produced VarStdVertex
                    OrderVarVertex* const varVxp = getVarVertex(varscp, VarVertexType::STD);
                    m_graphp->addHardEdge(m_logicVxp, varVxp, WEIGHT_NORMAL);
                }
                OrderVarVertex* const postVxp = getVarVertex(varscp, VarVertexType::POST);
                // Add edge from produced VarPostVertex -> to producing LogicVertex
                m_graphp->addHardEdge(postVxp, m_logicVxp, WEIGHT_POST);
            } else if (!m_inClocked) {  // Combinational logic
                // Add edge from producing LogicVertex -> produced VarStdVertex
                OrderVarVertex* const varVxp = getVarVertex(varscp, VarVertexType::STD);
                m_graphp->addHardEdge(m_logicVxp, varVxp, WEIGHT_NORMAL);
                // Add edge from produced VarPostVertex -> to producing LogicVertex
                OrderVarVertex* const postVxp = getVarVertex(varscp, VarVertexType::POST);
                m_graphp->addHardEdge(postVxp, m_logicVxp, WEIGHT_POST);
            } else if (m_inPre) {  // AstAlwaysPre
                // Add edge from producing LogicVertex -> produced VarPordVertex
                OrderVarVertex* const ordVxp = getVarVertex(varscp, VarVertexType::PORD);
                m_graphp->addHardEdge(m_logicVxp, ordVxp, WEIGHT_NORMAL);
                // Add edge from producing LogicVertex -> produced VarStdVertex
                OrderVarVertex* const varVxp = getVarVertex(varscp, VarVertexType::STD);
                m_graphp->addHardEdge(m_logicVxp, varVxp, WEIGHT_NORMAL);
            } else {
                // Sequential (clocked) logic
                // Add edge from produced VarPordVertex -> to producing LogicVertex
                OrderVarVertex* const ordVxp = getVarVertex(varscp, VarVertexType::PORD);
                m_graphp->addHardEdge(ordVxp, m_logicVxp, WEIGHT_NORMAL);
                // Add edge from producing LogicVertex-> to produced VarStdVertex
                OrderVarVertex* const varVxp = getVarVertex(varscp, VarVertexType::STD);
                m_graphp->addHardEdge(m_logicVxp, varVxp, WEIGHT_NORMAL);
            }
        }

        // Variable is consumed
        if (con) {
            // Update VarUsage
            varscp->user2Or(VU_CON);
            // Add edges
            if (m_softSubgraphRead) {
                OrderVarVertex* const varVxp = getVarVertex(varscp, VarVertexType::STD);
                m_graphp->addSoftEdge(varVxp, m_logicVxp, WEIGHT_MEDIUM);
            } else if (m_inPost) {
                // Combinational logic
                if (!varscp->varp()->ignorePostRead() && m_readTriggersCombLogic(varscp)) {
                    // Ignore explicit sensitivities
                    OrderVarVertex* const varVxp = getVarVertex(varscp, VarVertexType::STD);
                    // Add edge from consumed VarStdVertex -> to consuming LogicVertex
                    m_graphp->addHardEdge(varVxp, m_logicVxp, WEIGHT_MEDIUM);
                }
            } else if (!m_inClocked) {  // Combinational logic
                if (m_readTriggersCombLogic(varscp)) {
                    // Ignore explicit sensitivities
                    OrderVarVertex* const varVxp = getVarVertex(varscp, VarVertexType::STD);
                    // Add edge from consumed VarStdVertex -> to consuming LogicVertex
                    m_graphp->addHardEdge(varVxp, m_logicVxp, WEIGHT_MEDIUM);
                }
            } else if (m_inPre) {
                // AstAlwaysPre logic
                // Add edge from consumed VarPreVertex -> to consuming LogicVertex
                // This one is cutable (vs the producer) as there's only one such consumer,
                // but may be many producers
                OrderVarVertex* const preVxp = getVarVertex(varscp, VarVertexType::PRE);
                m_graphp->addSoftEdge(preVxp, m_logicVxp, WEIGHT_PRE);
            } else {
                // Sequential (clocked) logic
                // Add edge from consuming LogicVertex -> to consumed VarPreVertex
                // Generation of 'pre' because we want to indicate it should be before
                // AstAlwaysPre
                OrderVarVertex* const preVxp = getVarVertex(varscp, VarVertexType::PRE);
                m_graphp->addHardEdge(m_logicVxp, preVxp, WEIGHT_NORMAL);
                // Add edge from consuming LogicVertex -> to consumed VarPostVertex
                OrderVarVertex* const postVxp = getVarVertex(varscp, VarVertexType::POST);
                m_graphp->addHardEdge(m_logicVxp, postVxp, WEIGHT_POST);
            }
        }
    }
    void addSnapshotSourceUsage(AstSubgraphUse* nodep, AstVarScope* varscp) {
        UASSERT_OBJ(m_scopep, nodep, "Snapshot source not under scope");
        UASSERT_OBJ(m_logicVxp, nodep, "Snapshot source not under logic");
        UASSERT_OBJ(nodep->read() && !nodep->write(), nodep,
                    "Snapshot source should be read-only");

        if (m_parallel && !varscp->user4Or(VA_READ)) m_accessedVscps.push_back(varscp);
        if (varscp->user2() & VU_CON) return;
        varscp->user2Or(VU_CON);

        // A snapshot samples the value before any clocked writer commits it. Clocked writers
        // depend on the variable's PORD vertex, and NBA commits depend on the POST vertex, so
        // publish the sample before both. Using the normal AstAlwaysPre read edge would impose
        // the opposite ordering.
        OrderVarVertex* const ordVxp = getVarVertex(varscp, VarVertexType::PORD);
        m_graphp->addHardEdge(m_logicVxp, ordVxp, WEIGHT_NORMAL);
        OrderVarVertex* const postVxp = getVarVertex(varscp, VarVertexType::POST);
        m_graphp->addHardEdge(m_logicVxp, postVxp, WEIGHT_POST);
    }
    void visit(AstNodeVarRef* nodep) override {
        addVarUsage(nodep, nodep->varScopep(), nodep->access().isReadOrRW(),
                    nodep->access().isWriteOrRW());
    }
    void visit(AstSubgraphInstance* nodep) override {
        UASSERT_OBJ(m_logicVxp, nodep, "Subgraph contract not under logic");
        UASSERT_OBJ((nodep->phase() == VSubgraphPhase{VSubgraphPhase::POST}) == m_inPost, nodep,
                    "Subgraph phase does not match its parent procedure");
        UASSERT_OBJ((nodep->phase() == VSubgraphPhase{VSubgraphPhase::SNAPSHOT}) == m_inPre, nodep,
                    "Subgraph snapshot phase does not match its parent procedure");
        ++m_subgraphContractNodes;
        const bool refresh = nodep->phase() == VSubgraphPhase{VSubgraphPhase::REFRESH};
        for (AstSubgraphUse* usep = nodep->materializedsp(); usep;
             usep = VN_AS(usep->nextp(), SubgraphUse)) {
            ++m_subgraphContractUses;
            AstVarScope* const vscp = usep->varScopep();
            UASSERT_OBJ(vscp, usep, "Materialized subgraph use has no variable scope");
            const bool delayedState = usep->kind() == VSubgraphUseKind{VSubgraphUseKind::INTERNAL}
                                      && 0 == vscp->varp()->name().rfind("__Vdly", 0);
            if (usep->kind() == VSubgraphUseKind{VSubgraphUseKind::INTERNAL}) {
                const bool externallyAccessed = m_parentAccessedVscps.count(vscp);
                if (!delayedState && !externallyAccessed) continue;
                // The helper locally orders reads after its own state commits. Publishing those
                // reads would create an artificial parent-level read-after-write cycle; only the
                // commit needs to precede external consumers.
                if (!refresh && externallyAccessed && !delayedState && !usep->write()) continue;
            }
            VL_RESTORER(m_softSubgraphRead);
            m_softSubgraphRead = usep->cuttable() && usep->read() && !delayedState;
            if (m_softSubgraphRead) ++m_subgraphContractCuttableUses;
            if (usep->kind() == VSubgraphUseKind{VSubgraphUseKind::SNAPSHOT_SOURCE}) {
                addSnapshotSourceUsage(usep, vscp);
            } else if (usep->kind() == VSubgraphUseKind{VSubgraphUseKind::SNAPSHOT_STORAGE}) {
                VL_RESTORER(m_inClocked);
                VL_RESTORER(m_inPre);
                m_inClocked = false;
                m_inPre = false;
                addVarUsage(usep, vscp, usep->read(), usep->write());
            } else {
                addVarUsage(usep, vscp, usep->read(), usep->write());
            }
        }
        // Do not visit stmtsp(): the helper body is deliberately opaque to the parent graph.
    }
    void visit(AstCCall* nodep) override {
        UASSERT_OBJ(!isSubgraphWrapperCall(nodep), nodep,
                    "Subgraph helper call escaped its coarse contract node");
        iterateChildren(nodep);
    }

    //--- Logic akin to SystemVerilog Processes (AstNodeProcedure)
    void visit(AstInitial* nodep) override {  // LCOV_EXCL_START
        nodep->v3fatalSrc("AstInitial should not need ordering");
    }  // LCOV_EXCL_STOP
    void visit(AstInitialStatic* nodep) override {  // LCOV_EXCL_START
        nodep->v3fatalSrc("AstInitialStatic should not need ordering");
    }  // LCOV_EXCL_STOP
    void visit(AstInitialAutomatic* nodep) override {  //
        iterateLogic(nodep);
    }
    void visit(AstAlways* nodep) override {  //
        iterateLogic(nodep);
    }
    void visit(AstAlwaysPre* nodep) override {
        UASSERT_OBJ(!m_inPre, nodep, "Should not nest");
        VL_RESTORER(m_inPre);
        m_inPre = true;
        iterateLogic(nodep);
    }
    void visit(AstAlwaysPost* nodep) override {
        UASSERT_OBJ(!m_inPost, nodep, "Should not nest");
        VL_RESTORER(m_inPost);
        m_inPost = true;
        iterateLogic(nodep);
    }
    void visit(AstAlwaysObserved* nodep) override {  //
        iterateLogic(nodep);
    }
    void visit(AstAlwaysReactive* nodep) override {  //
        iterateLogic(nodep);
    }
    void visit(AstFinal* nodep) override {  // LCOV_EXCL_START
        nodep->v3fatalSrc("AstFinal should not need ordering");
    }  // LCOV_EXCL_STOP

    //--- Verilator concoctions
    void visit(AstCoverToggle* nodep) override {  //
        iterateLogic(nodep);
    }

    //--- Ignored nodes
    void visit(AstVar*) override {}
    void visit(AstVarScope* nodep) override { nodep->v3fatalSrc("Should not reach V3Order"); }
    void visit(AstCell* nodep) override { nodep->v3fatalSrc("Should not reach V3Order"); }
    void visit(AstTypeTable* nodep) override { nodep->v3fatalSrc("Should not reach V3Order"); }
    void visit(AstConstPool* nodep) override { nodep->v3fatalSrc("Should not reach V3Order"); }
    void visit(AstClass* nodep) override { nodep->v3fatalSrc("Should not reach V3Order"); }
    void visit(AstCFunc*) override {
        // Calls to DPI exports handled with AstCCall. /* verilator public */ functions are
        // ignored for now (and hence potentially mis-ordered), but could use the same or
        // similar mechanism as DPI exports. Every other impure function (including those
        // that may set a non-local variable) must have been inlined in V3Task.
    }

    //---
    void visit(AstNode* nodep) override { iterateChildren(nodep); }

    // CONSTRUCTOR
    OrderGraphBuilder(AstNetlist* /*nodep*/, const std::vector<V3Sched::LogicByScope*>& coll,
                      const V3Order::TrigToSenMap& trigToSen, bool parallel)
        : m_trigToSen{trigToSen}
        , m_parallel{parallel} {
        // The parent contract includes internal state only when logic outside a subgraph helper
        // also accesses it (for example, an output-port assignment lowered into the parent
        // scope). State passed from POST to REFRESH is also visible so the parent graph can order
        // the two helpers. Other internal variables remain hidden from this graph.
        std::unordered_set<AstVarScope*> postWrittenVscps;
        for (const V3Sched::LogicByScope* const lbsp : coll) {
            for (const auto& pair : *lbsp) {
                AstActive* const activep = pair.second;
                if (!containsSubgraphInstance(activep)) continue;
                activep->foreach([&](AstSubgraphInstance* instancep) {
                    if (instancep->phase() != VSubgraphPhase{VSubgraphPhase::POST}) return;
                    for (AstSubgraphUse* usep = instancep->materializedsp(); usep;
                         usep = VN_AS(usep->nextp(), SubgraphUse)) {
                        if (usep->kind() == VSubgraphUseKind{VSubgraphUseKind::INTERNAL}
                            && usep->write()) {
                            postWrittenVscps.insert(usep->varScopep());
                        }
                    }
                });
            }
        }
        for (const V3Sched::LogicByScope* const lbsp : coll) {
            for (const auto& pair : *lbsp) {
                AstActive* const activep = pair.second;
                if (containsSubgraphInstance(activep)) {
                    activep->foreach([&](AstSubgraphInstance* instancep) {
                        if (instancep->phase() != VSubgraphPhase{VSubgraphPhase::REFRESH}) {
                            return;
                        }
                        for (AstSubgraphUse* usep = instancep->materializedsp(); usep;
                             usep = VN_AS(usep->nextp(), SubgraphUse)) {
                            if (usep->kind() == VSubgraphUseKind{VSubgraphUseKind::INTERNAL}
                                && usep->read() && postWrittenVscps.count(usep->varScopep())) {
                                m_parentAccessedVscps.insert(usep->varScopep());
                            }
                        }
                    });
                    continue;
                }
                activep->foreach(
                    [&](AstNodeVarRef* refp) { m_parentAccessedVscps.insert(refp->varScopep()); });
            }
        }
        // Build the graph
        for (const V3Sched::LogicByScope* const lbsp : coll) {
            for (const auto& pair : *lbsp) {
                m_scopep = pair.first;
                iterate(pair.second);
                m_scopep = nullptr;
            }
        }
    }
    ~OrderGraphBuilder() override = default;

public:
    // Process the netlist and return the constructed ordering graph. It's 'process' because
    // this visitor does change the tree (removes some nodes related to DPI export trigger).
    static std::unique_ptr<OrderGraph> apply(AstNetlist* nodep,
                                             const std::vector<V3Sched::LogicByScope*>& coll,
                                             const V3Order::TrigToSenMap& trigToSen,
                                             bool parallel) {
        OrderGraphBuilder builder{nodep, coll, trigToSen, parallel};
        if (builder.m_subgraphContractNodes) {
            V3Stats::addStat("Scheduling, Subgraph order graph contract nodes",
                             builder.m_subgraphContractNodes);
            V3Stats::addStat("Scheduling, Subgraph order graph contract uses",
                             builder.m_subgraphContractUses);
            V3Stats::addStat("Scheduling, Subgraph order graph contract cuttable uses",
                             builder.m_subgraphContractCuttableUses);
        }
        return std::unique_ptr<OrderGraph>{builder.m_graphp};
    }
};

std::unique_ptr<OrderGraph>
V3Order::buildOrderGraph(AstNetlist* netlistp,  //
                         const std::vector<V3Sched::LogicByScope*>& coll,  //
                         const V3Order::TrigToSenMap& trigToSen,  //
                         bool parallel) {
    return OrderGraphBuilder::apply(netlistp, coll, trigToSen, parallel);
}
