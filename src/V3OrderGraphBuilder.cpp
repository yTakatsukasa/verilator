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
#include "V3SubgraphSummary.h"

#include <unordered_map>
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
    using VarVertexType = OrderUser::VarVertexType;
    struct SubgraphCallUsage final {
        AstVarScope* m_varscp = nullptr;
        bool m_read = false;
        bool m_write = false;
    };
    using SubgraphCallUsageCache = std::unordered_map<AstCFunc*, std::vector<SubgraphCallUsage>>;

    // NODE STATE
    //  AstVarScope::user1    -> OrderUser instance for variable (via m_orderUser)
    //  AstVarScope::user2    -> VarUsage within logic blocks
    //  AstVarScope::user3    -> bool: Hybrid sensitivity
    const VNUser1InUse user1InUse;
    const VNUser2InUse user2InUse;
    const VNUser3InUse user3InUse;
    AstUser1Allocator<AstVarScope, OrderUser> m_orderUser;

    // STATE
    OrderGraph* const m_graphp = new OrderGraph;  // The ordering graph built by this visitor
    OrderLogicVertex* m_logicVxp = nullptr;  // Current logic block being analyzed

    // Map from Trigger reference AstSenItem to the original AstSenTree
    const V3Order::TrigToSenMap& m_trigToSen;

    // Current AstScope being processed
    AstScope* m_scopep = nullptr;
    // Sensitivity list for clocked logic, nullptr for combinational and hybrid logic
    AstSenTree* m_domainp = nullptr;
    // Sensitivity list for hybrid logic, nullptr for everything else
    AstSenTree* m_hybridp = nullptr;
    std::unordered_map<const AstSenTree*, AstVarScope*> m_subgraphClockedBarrierVscps;
    std::unordered_map<const AstSenTree*, AstVarScope*> m_subgraphPostBarrierVscps;
    std::unordered_map<const AstSenTree*, AstVarScope*> m_subgraphPreBarrierVscps;
    std::unordered_map<const AstSenTree*, AstVarScope*> m_subgraphSnapshotBarrierVscps;

    bool m_inClocked = false;  // Underneath clocked AstActive
    bool m_inPre = false;  // Underneath AlwaysPre
    bool m_inPost = false;  // Underneath AstAlwaysPost
    bool m_isSubgraphCommitPostLogic = false;  // Post logic commits delayed top state
    bool m_isSubgraphSnapshotLogic = false;  // Procedure snapshots cross-boundary values
    bool m_isSubgraphWrapperLogic = false;  // Procedure wraps a subgraph eval call
    std::function<bool(const AstVarScope*)> m_readTriggersCombLogic;
    SubgraphCallUsageCache m_subgraphCallUsageCache;
    V3Sched::util::VarScopeSet m_forceReadEdgeIgnores;

    // METHODS

    void iterateLogic(AstNode* nodep) {
        UASSERT_OBJ(!m_logicVxp, nodep, "Should not nest");
        // Reset VarUsage
        AstNode::user2ClearTree();
        m_forceReadEdgeIgnores.clear();
        if (!m_inClocked)
            V3Sched::util::collectForceReadEdgeIgnores(nodep, m_forceReadEdgeIgnores);
        // Create LogicVertex for this logic node
        m_logicVxp = new OrderLogicVertex{m_graphp, m_scopep, m_domainp, m_hybridp, nodep};
        // Gather variable dependencies based on usage
        iterateChildren(nodep);
        const AstSenTree* const barrierKeyp = m_domainp ? m_domainp : m_hybridp;
        // Finished with this logic
        if (v3Global.opt.subgraphSchedule() && barrierKeyp) {
            if (m_inClocked && !m_inPost && !m_isSubgraphSnapshotLogic
                && !m_isSubgraphWrapperLogic) {
                addCoarseVarUsage(getSubgraphClockedBarrierVscp(barrierKeyp), false, true, nodep);
            }
            if (m_inPost && m_isSubgraphCommitPostLogic) {
                addCoarseVarUsage(getSubgraphPostBarrierVscp(barrierKeyp), true, false, nodep);
            }
            if (m_inPre && m_isSubgraphWrapperLogic && m_inClocked) {
                addCoarseVarUsage(getSubgraphPreBarrierVscp(barrierKeyp), false, true, nodep);
            }
            if (m_isSubgraphSnapshotLogic) {
                addCoarseVarUsage(getSubgraphSnapshotBarrierVscp(barrierKeyp), false, true, nodep);
            }
            if (m_isSubgraphWrapperLogic && m_inClocked && !m_inPost) {
                if (!m_inPre)
                    addCoarseVarUsage(getSubgraphPostBarrierVscp(barrierKeyp), false, true, nodep);
                if (!m_inPre)
                    addCoarseVarUsage(getSubgraphClockedBarrierVscp(barrierKeyp), true, false,
                                      nodep);
                if (!m_inPre)
                    addCoarseVarUsage(getSubgraphPreBarrierVscp(barrierKeyp), true, false, nodep);
                addCoarseVarUsage(getSubgraphSnapshotBarrierVscp(barrierKeyp), true, false, nodep);
            }
        }
        m_logicVxp = nullptr;
        m_forceReadEdgeIgnores.clear();
    }

    OrderVarVertex* getVarVertex(AstVarScope* varscp, VarVertexType type) {
        return m_orderUser(varscp).getVarVertex(m_graphp, varscp, type);
    }

    AstVarScope* getSubgraphClockedBarrierVscp(const AstSenTree* barrierKeyp) {
        const auto it = m_subgraphClockedBarrierVscps.find(barrierKeyp);
        if (it != m_subgraphClockedBarrierVscps.end()) return it->second;
        AstScope* const topScopep = v3Global.rootp()->topScopep()->scopep();
        AstVarScope* const vscp = topScopep->createTemp(
            "__VsubgraphClockedBarrier__" + cvtToStr(m_subgraphClockedBarrierVscps.size()), 1);
        m_subgraphClockedBarrierVscps.emplace(barrierKeyp, vscp);
        return vscp;
    }

    AstVarScope* getSubgraphPostBarrierVscp(const AstSenTree* barrierKeyp) {
        const auto it = m_subgraphPostBarrierVscps.find(barrierKeyp);
        if (it != m_subgraphPostBarrierVscps.end()) return it->second;
        AstScope* const topScopep = v3Global.rootp()->topScopep()->scopep();
        AstVarScope* const vscp = topScopep->createTemp(
            "__VsubgraphPostBarrier__" + cvtToStr(m_subgraphPostBarrierVscps.size()), 1);
        m_subgraphPostBarrierVscps.emplace(barrierKeyp, vscp);
        return vscp;
    }

    AstVarScope* getSubgraphPreBarrierVscp(const AstSenTree* barrierKeyp) {
        const auto it = m_subgraphPreBarrierVscps.find(barrierKeyp);
        if (it != m_subgraphPreBarrierVscps.end()) return it->second;
        AstScope* const topScopep = v3Global.rootp()->topScopep()->scopep();
        AstVarScope* const vscp = topScopep->createTemp(
            "__VsubgraphPreBarrier__" + cvtToStr(m_subgraphPreBarrierVscps.size()), 1);
        m_subgraphPreBarrierVscps.emplace(barrierKeyp, vscp);
        return vscp;
    }

    AstVarScope* getSubgraphSnapshotBarrierVscp(const AstSenTree* barrierKeyp) {
        const auto it = m_subgraphSnapshotBarrierVscps.find(barrierKeyp);
        if (it != m_subgraphSnapshotBarrierVscps.end()) return it->second;
        AstScope* const topScopep = v3Global.rootp()->topScopep()->scopep();
        AstVarScope* const vscp = topScopep->createTemp(
            "__VsubgraphSnapshotBarrier__" + cvtToStr(m_subgraphSnapshotBarrierVscps.size()), 1);
        m_subgraphSnapshotBarrierVscps.emplace(barrierKeyp, vscp);
        return vscp;
    }

    bool isSubgraphInternalOrderVar(AstVarScope* vscp, AstScope* subgraphScopep) const {
        return isUnderScope(vscp->scopep(), subgraphScopep)
               && 0 == vscp->varp()->name().rfind("__Vdly", 0);
    }

    AstScope* subgraphBoundaryScope(AstScope* scopep) const {
        for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
            if (scanp->modp()->subgraphBoundary()) return scanp;
        }
        return nullptr;
    }

    bool hasDelayedShadowVar(AstVarScope* vscp) const {
        AstScope* const topScopep = v3Global.rootp()->topScopep()->scopep();
        const std::string shadowName = "__Vdly__" + vscp->varp()->name();
        for (AstVarScope* scanp = topScopep->varsp(); scanp;
             scanp = VN_AS(scanp->nextp(), VarScope)) {
            if (scanp->varp()->name() == shadowName) return true;
        }
        return false;
    }

    bool isSubgraphWrapperCall(AstCCall* nodep) const {
        AstCFunc* const funcp = nodep->funcp();
        AstScope* const scopep = funcp->scopep();
        return scopep && scopep->modp()->subgraphBoundary()
               && 0 == funcp->name().rfind("_eval_", 0);
    }

    bool isUnderScope(AstScope* scopep, AstScope* basep) const {
        for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
            if (scanp == basep) return true;
        }
        return false;
    }

    void addVarUsage(AstVarScope* varscp, bool isRead, bool isWrite, AstNode* nodep,
                     bool forcePost = false, bool forceNotPost = false) {
        UASSERT_OBJ(m_scopep, nodep, "Var usage not under scope");
        UASSERT_OBJ(m_logicVxp, nodep, "Var usage not under logic");
        UASSERT_OBJ(varscp, nodep, "Var didn't get varscoped in V3Scope.cpp");

        // Variable reference in logic. Add data dependency.

        // Check whether this variable was already generated/consumed in the same logic. We
        // don't want to add extra edges if the logic has many usages of the same variable,
        // so only proceed on first encounter.
        const bool prevGen = varscp->user2() & VU_GEN;
        const bool prevCon = varscp->user2() & VU_CON;

        // Compute whether the variable is produced (written) here
        const bool gen = !prevGen && isWrite && !varscp->varp()->ignoreSchedWrite();
        const bool inPost = !forceNotPost && (m_inPost || forcePost);

        // Compute whether the value is consumed (read) here
        bool con = false;
        if (!prevCon && isRead) {
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
            if (inPost) {
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
            if (inPost) {
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

    void addCoarseVarUsage(AstVarScope* varscp, bool isRead, bool isWrite, AstNode* nodep) {
        UASSERT_OBJ(m_logicVxp, nodep, "Var usage not under logic");
        UASSERT_OBJ(varscp, nodep, "Var didn't get varscoped in V3Scope.cpp");

        const bool prevGen = varscp->user2() & VU_GEN;
        const bool prevCon = varscp->user2() & VU_CON;
        const bool gen = !prevGen && isWrite && !varscp->varp()->ignoreSchedWrite();

        bool con = false;
        if (!prevCon && isRead) {
            con = true;
            if (m_forceReadEdgeIgnores.count(varscp) || !m_readTriggersCombLogic(varscp)) {
                con = false;
            }
        }

        if (gen) {
            varscp->user2Or(VU_GEN);
            OrderVarVertex* const varVxp = getVarVertex(varscp, VarVertexType::STD);
            m_graphp->addHardEdge(m_logicVxp, varVxp, WEIGHT_NORMAL);
        }
        if (con) {
            varscp->user2Or(VU_CON);
            OrderVarVertex* const varVxp = getVarVertex(varscp, VarVertexType::STD);
            m_graphp->addHardEdge(varVxp, m_logicVxp, WEIGHT_MEDIUM);
        }
    }

    void addSubgraphCallPortUsage(AstCCall* nodep) {
        if (!isSubgraphWrapperCall(nodep)) return;
        AstCFunc* const funcp = nodep->funcp();
        AstScope* const scopep = funcp->scopep();
        const V3SubgraphSummary::ScopeSummary* const summaryp
            = V3SubgraphSummary::getScopeSummary(scopep);
        UASSERT_OBJ(summaryp, nodep, "Missing subgraph scope summary");
        const bool hideClockedBoundaryContract = m_inClocked;
        const bool publishBoundaryWrites = !m_inPre;
        if (!hideClockedBoundaryContract) {
            for (AstVarScope* const vscp : summaryp->m_nonOutputPorts) {
                if (V3SubgraphSummary::isDerivedBoundaryInput(vscp)) {
                    addCoarseVarUsage(vscp, true, false, nodep);
                } else {
                    addVarUsage(vscp, true, false, nodep, false, true);
                }
            }
        }
        if (publishBoundaryWrites) {
            for (AstVarScope* const vscp : summaryp->m_writablePorts) {
                addVarUsage(vscp, false, true, nodep, true);
            }
        }
        for (const SubgraphCallUsage& use : getSubgraphCallUsage(funcp)) {
            AstVarScope* const vscp = use.m_varscp;
            const bool internalOrderVar = isSubgraphInternalOrderVar(vscp, scopep);
            const bool externalToSubgraph = !isUnderScope(vscp->scopep(), scopep);
            if (isUnderScope(vscp->scopep(), scopep) && !internalOrderVar) { continue; }
            AstScope* const sourceBoundaryp = subgraphBoundaryScope(vscp->scopep());
            if (hideClockedBoundaryContract && internalOrderVar) continue;
            if (hideClockedBoundaryContract && sourceBoundaryp == scopep
                && vscp->scopep() == scopep && vscp->varp()->isIO()
                && vscp->varp()->direction().isNonOutput()) {
                continue;
            }
            if (hideClockedBoundaryContract && externalToSubgraph && use.m_read && !use.m_write) {
                continue;
            }
            if (internalOrderVar) {
                addVarUsage(vscp, use.m_read, use.m_write, nodep);
            } else if (sourceBoundaryp && sourceBoundaryp != scopep) {
                const bool coarseRead = use.m_read;
                const bool coarseWrite = publishBoundaryWrites && use.m_write;
                if (coarseRead || coarseWrite) {
                    addCoarseVarUsage(vscp, coarseRead, coarseWrite, nodep);
                }
            } else {
                addVarUsage(vscp, use.m_read, use.m_write, nodep, false, use.m_read);
            }
        }
    }

    const std::vector<SubgraphCallUsage>& getSubgraphCallUsage(AstCFunc* funcp) {
        const auto cacheIt = m_subgraphCallUsageCache.find(funcp);
        if (cacheIt != m_subgraphCallUsageCache.end()) return cacheIt->second;

        if (const auto* const summaryp = V3Sched::getSubgraphCallUsageSummary(funcp)) {
            std::vector<SubgraphCallUsage> uses;
            uses.reserve(summaryp->size());
            for (const V3Sched::SubgraphCallUsageSummary& summary : *summaryp) {
                uses.push_back(
                    SubgraphCallUsage{summary.m_varscp, summary.m_read, summary.m_write});
            }
            return m_subgraphCallUsageCache.emplace(funcp, std::move(uses)).first->second;
        }

        std::vector<SubgraphCallUsage> uses;
        std::unordered_map<AstVarScope*, size_t> useIndices;
        std::unordered_set<AstCFunc*> seen;
        std::function<void(AstCFunc*)> gather = [&](AstCFunc* scanFuncp) {
            if (!seen.insert(scanFuncp).second) return;
            scanFuncp->foreach([&](AstNodeVarRef* refp) {
                AstVarScope* const vscp = refp->varScopep();
                const auto pair = useIndices.emplace(vscp, uses.size());
                if (pair.second) uses.push_back(SubgraphCallUsage{vscp, false, false});
                SubgraphCallUsage& use = uses[pair.first->second];
                use.m_read |= refp->access().isReadOrRW();
                use.m_write |= refp->access().isWriteOrRW();
            });
            scanFuncp->foreach([&](AstCCall* callp) {
                AstCFunc* const calledFuncp = callp->funcp();
                if (calledFuncp->entryPoint()) return;
                gather(calledFuncp);
            });
        };
        gather(funcp);

        return m_subgraphCallUsageCache.emplace(funcp, std::move(uses)).first->second;
    }

    bool shouldGroupSubgraphWrapperActive(AstActive* nodep) const {
        if (!m_scopep->modp()->subgraphBoundary()) return false;
        bool sawCall = false;
        for (AstNode* stmtp = nodep->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            if (VN_IS(stmtp, NodeProcedure)) return false;
            stmtp->foreach([&](AstCCall* callp) {
                if (isSubgraphWrapperCall(callp)) sawCall = true;
            });
        }
        return sawCall;
    }

    bool isSubgraphWrapperProcedure(AstNodeProcedure* nodep) const {
        bool sawCall = false;
        nodep->foreach([&](AstCCall* callp) {
            if (isSubgraphWrapperCall(callp)) sawCall = true;
        });
        return sawCall;
    }

    bool isSubgraphCommitPostProcedure(AstNodeProcedure* nodep) const {
        bool commitsDelayedState = false;
        nodep->foreach([&](AstVarRef* refp) {
            if (commitsDelayedState) return;
            AstVarScope* const vscp = refp->varScopep();
            if (refp->access().isReadOrRW() && 0 == vscp->varp()->name().rfind("__Vdly__", 0)) {
                commitsDelayedState = true;
                return;
            }
            if (refp->access().isWriteOrRW() && hasDelayedShadowVar(vscp)) {
                commitsDelayedState = true;
            }
        });
        return commitsDelayedState;
    }

    bool isSubgraphSnapshotProcedure(AstNodeProcedure* nodep) const {
        bool snapshotsBoundaryValue = false;
        nodep->foreach([&](AstVarRef* refp) {
            if (snapshotsBoundaryValue || !refp->access().isWriteOrRW()) return;
            if (0 == refp->varScopep()->varp()->name().rfind("__VsubgraphSnapshot__", 0)) {
                snapshotsBoundaryValue = true;
            }
        });
        return snapshotsBoundaryValue;
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

        // Analyze logic underneath
        if (shouldGroupSubgraphWrapperActive(nodep)) {
            iterateLogic(nodep);
        } else {
            iterateChildren(nodep);
        }
    }
    void visit(AstNodeVarRef* nodep) override {
        AstVarScope* const varscp = nodep->varScopep();
        if (m_isSubgraphSnapshotLogic) {
            if (0 == varscp->varp()->name().rfind("__VsubgraphSnapshot__", 0)) {
                addCoarseVarUsage(varscp, nodep->access().isReadOrRW(),
                                  nodep->access().isWriteOrRW(), nodep);
            }
            return;
        }
        addVarUsage(varscp, nodep->access().isReadOrRW(), nodep->access().isWriteOrRW(), nodep);
    }
    void visit(AstCCall* nodep) override {
        if (isSubgraphWrapperCall(nodep)) {
            addSubgraphCallPortUsage(nodep);
            return;
        }
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
        if (m_logicVxp) return iterateChildren(nodep);
        VL_RESTORER(m_isSubgraphSnapshotLogic);
        VL_RESTORER(m_isSubgraphWrapperLogic);
        m_isSubgraphSnapshotLogic = isSubgraphSnapshotProcedure(nodep);
        m_isSubgraphWrapperLogic = isSubgraphWrapperProcedure(nodep);
        iterateLogic(nodep);
    }
    void visit(AstAlways* nodep) override {  //
        if (m_logicVxp) return iterateChildren(nodep);
        VL_RESTORER(m_isSubgraphSnapshotLogic);
        VL_RESTORER(m_isSubgraphWrapperLogic);
        m_isSubgraphSnapshotLogic = isSubgraphSnapshotProcedure(nodep);
        m_isSubgraphWrapperLogic = isSubgraphWrapperProcedure(nodep);
        iterateLogic(nodep);
    }
    void visit(AstAlwaysPre* nodep) override {
        if (m_logicVxp) return iterateChildren(nodep);
        UASSERT_OBJ(!m_inPre, nodep, "Should not nest");
        VL_RESTORER(m_inPre);
        VL_RESTORER(m_isSubgraphSnapshotLogic);
        VL_RESTORER(m_isSubgraphWrapperLogic);
        m_inPre = true;
        m_isSubgraphSnapshotLogic = isSubgraphSnapshotProcedure(nodep);
        m_isSubgraphWrapperLogic = isSubgraphWrapperProcedure(nodep);
        iterateLogic(nodep);
    }
    void visit(AstAlwaysPost* nodep) override {
        if (m_logicVxp) return iterateChildren(nodep);
        UASSERT_OBJ(!m_inPost, nodep, "Should not nest");
        VL_RESTORER(m_inPost);
        VL_RESTORER(m_isSubgraphCommitPostLogic);
        VL_RESTORER(m_isSubgraphSnapshotLogic);
        VL_RESTORER(m_isSubgraphWrapperLogic);
        m_inPost = true;
        m_isSubgraphCommitPostLogic = isSubgraphCommitPostProcedure(nodep);
        m_isSubgraphSnapshotLogic = isSubgraphSnapshotProcedure(nodep);
        m_isSubgraphWrapperLogic = isSubgraphWrapperProcedure(nodep);
        iterateLogic(nodep);
    }
    void visit(AstAlwaysObserved* nodep) override {  //
        if (m_logicVxp) return iterateChildren(nodep);
        VL_RESTORER(m_isSubgraphSnapshotLogic);
        VL_RESTORER(m_isSubgraphWrapperLogic);
        m_isSubgraphSnapshotLogic = isSubgraphSnapshotProcedure(nodep);
        m_isSubgraphWrapperLogic = isSubgraphWrapperProcedure(nodep);
        iterateLogic(nodep);
    }
    void visit(AstAlwaysReactive* nodep) override {  //
        if (m_logicVxp) return iterateChildren(nodep);
        VL_RESTORER(m_isSubgraphSnapshotLogic);
        VL_RESTORER(m_isSubgraphWrapperLogic);
        m_isSubgraphSnapshotLogic = isSubgraphSnapshotProcedure(nodep);
        m_isSubgraphWrapperLogic = isSubgraphWrapperProcedure(nodep);
        iterateLogic(nodep);
    }
    void visit(AstFinal* nodep) override {  // LCOV_EXCL_START
        nodep->v3fatalSrc("AstFinal should not need ordering");
    }  // LCOV_EXCL_STOP

    //--- Verilator concoctions
    void visit(AstCoverToggle* nodep) override {  //
        if (m_logicVxp) return iterateChildren(nodep);
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
                      const V3Order::TrigToSenMap& trigToSen)
        : m_trigToSen{trigToSen} {
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
                                             const V3Order::TrigToSenMap& trigToSen) {
        return std::unique_ptr<OrderGraph>{OrderGraphBuilder{nodep, coll, trigToSen}.m_graphp};
    }
};

std::unique_ptr<OrderGraph>
V3Order::buildOrderGraph(AstNetlist* netlistp,  //
                         const std::vector<V3Sched::LogicByScope*>& coll,  //
                         const V3Order::TrigToSenMap& trigToSen) {
    return OrderGraphBuilder::apply(netlistp, coll, trigToSen);
}
