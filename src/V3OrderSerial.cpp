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
//  Serial code ordering
//
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3OrderCFuncEmitter.h"
#include "V3OrderInternal.h"

#include <memory>

VL_DEFINE_DEBUG_FUNCTIONS;

//######################################################################
// OrderSerial class

AstNodeStmt*
V3Order::createSerial(OrderMoveGraph& moveGraph, const std::string& tag, bool slow,
                      const std::unordered_map<const AstNode*, size_t>* logicIndexp,
                      const std::unordered_map<const AstNode*, AstSenTree*>* sourceDomainp,
                      OrderRecipe* recipep) {

    UINFO(2, "  Constructing serial code for '" + tag + "'");

    // Serializer
    OrderMoveGraphSerializer serializer{moveGraph};

    // Add initially ready vertices (those with no dependencies) to the serializer as seeds
    for (V3GraphVertex& vtx : moveGraph.vertices()) {
        if (vtx.inEmpty()) serializer.addSeed(vtx.as<OrderMoveVertex>());
    }

    // Emit all logic as they become ready
    V3OrderCFuncEmitter emitter{tag, slow};
    OrderMoveDomScope* prevDomScopep = nullptr;
    while (OrderMoveVertex* const mVtxp = serializer.getNext()) {
        // We only really care about logic vertices
        if (OrderLogicVertex* const logicp = mVtxp->logicp()) {
            // Force a new function if the domain or scope changed, for better combining.
            OrderMoveDomScope* const domScopep = &mVtxp->domScope();
            const bool forceNewFunction = domScopep != prevDomScopep;
            if (forceNewFunction) emitter.forceNewFunction();
            prevDomScopep = domScopep;
            if (recipep) {
                const auto indexIt = logicIndexp->find(logicp->nodep());
                const auto domainIt = sourceDomainp->find(logicp->nodep());
                UASSERT_OBJ(indexIt != logicIndexp->end(), logicp->nodep(),
                            "Missing ordered logic recipe index");
                UASSERT_OBJ(domainIt != sourceDomainp->end(), logicp->nodep(),
                            "Missing ordered logic source domain");
                recipep->m_entries.push_back(OrderRecipeEntry{indexIt->second, forceNewFunction});
                if (domainIt->second != logicp->domainp()) recipep->m_replayable = false;
            }
            // Emit the logic under this vertex
            emitter.emitLogic(logicp);
        }
        // Can delete the vertex now
        VL_DO_DANGLING(mVtxp->unlinkDelete(&moveGraph), mVtxp);
    }

    // Delete the remaining variable vertices
    for (V3GraphVertex* const vtxp : moveGraph.vertices().unlinkable()) {
        if (!vtxp->as<OrderMoveVertex>()->logicp()) {
            VL_DO_DANGLING(vtxp->unlinkDelete(&moveGraph), vtxp);
        }
    }

    return emitter.getStmts();
}

AstNodeStmt* V3Order::replaySerial(const std::vector<V3Sched::LogicByScope*>& logic,
                                   const OrderRecipe& recipe, const std::string& tag, bool slow) {
    struct ReplayLogic final {
        AstNode* m_logicp = nullptr;
        AstScope* m_scopep = nullptr;
        AstSenTree* m_domainp = nullptr;
    };

    std::vector<ReplayLogic> replayLogic;
    replayLogic.reserve(recipe.m_logicCount);
    for (const V3Sched::LogicByScope* const lbsp : logic) {
        for (const auto& pair : *lbsp) {
            for (AstNode* nodep = pair.second->stmtsp(); nodep; nodep = nodep->nextp()) {
                replayLogic.push_back(ReplayLogic{nodep, pair.first, pair.second->sentreep()});
            }
        }
    }
    if (replayLogic.size() != recipe.m_logicCount
        || recipe.m_entries.size() != recipe.m_logicCount) {
        return nullptr;
    }

    std::vector<bool> emitted(recipe.m_logicCount, false);
    V3OrderCFuncEmitter emitter{tag, slow};
    for (const OrderRecipeEntry& entry : recipe.m_entries) {
        if (entry.m_logicIndex >= replayLogic.size() || emitted[entry.m_logicIndex])
            return nullptr;
        emitted[entry.m_logicIndex] = true;
        if (entry.m_forceNewFunction) emitter.forceNewFunction();
        const ReplayLogic& item = replayLogic[entry.m_logicIndex];
        emitter.emitLogic(item.m_logicp, item.m_scopep, item.m_domainp);
    }
    return emitter.getStmts();
}
