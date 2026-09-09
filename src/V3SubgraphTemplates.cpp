// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Capture independent subgraph templates before scoping
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
// Capture a leaf module once after elaboration, but before V3Inst converts its ports to parent
// assignments and V3Scope clones its procedures. Scalar declarations, expressions, event lists,
// and procedures become owning records with local slot/node IDs. No AST or dtype pointer escapes
// capture. Unsupported constructs reject the entire template, never a portion of its behavior.
// The existing scheduling path remains active until independent scheduling and binding are ready.
//*************************************************************************

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphTemplates.h"

#include "V3File.h"
#include "V3Stats.h"

#include <map>
#include <unordered_map>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

using Templates = V3SubgraphTemplates;
using Id = Templates::Id;

class TemplateBuilder final {
    Templates::Module m_module;
    std::map<const AstVar*, Id> m_slots;
    std::string m_rejection;

    Id reject(const std::string& reason) {
        if (m_rejection.empty()) m_rejection = reason;
        return Templates::NONE;
    }

    Templates::Type type(const AstNodeDType* dtypep) {
        Templates::Type result;
        if (!dtypep) return result;
        // Do not flatten enums, typedefs, aggregates, or opaque types into a matching bit width.
        const AstBasicDType* const basicp = VN_CAST(dtypep, BasicDType);
        if (!basicp || !basicp->keyword().isIntNumeric()) {
            reject("dtype");
            return result;
        }
        result.m_keyword = basicp->keyword().ascii();
        result.m_width = basicp->width();
        result.m_signed = basicp->isSigned();
        result.m_ranged = basicp->isRanged();
        result.m_left = basicp->left();
        result.m_right = basicp->right();
        return result;
    }

    Id add(Templates::Node&& node) {
        UASSERT(m_module.m_nodes.size() < std::numeric_limits<Id>::max(),
                "Subgraph template node limit exceeded");
        m_module.m_nodes.push_back(std::move(node));
        return static_cast<Id>(m_module.m_nodes.size());
    }

    Id expression(const AstNodeExpr* nodep) {
        if (!nodep || !m_rejection.empty()) return Templates::NONE;
        Templates::Node node;
        node.m_kind = nodep->typeName();
        node.m_type = type(nodep->dtypep());
        if (const AstConst* const constp = VN_CAST(nodep, Const)) {
            if (constp->num().isOpaque()) return reject("constant");
            node.m_value = constp->num().ascii();
        } else if (const AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            const auto it = m_slots.find(refp->varp());
            if (it == m_slots.end() || refp->classOrPackagep())
                return reject("external reference");
            node.m_slot = it->second;
            node.m_value = refp->access().ascii();
        } else if (const AstSel* const selp = VN_CAST(nodep, Sel)) {
            node.m_operands = {expression(selp->fromp()), expression(selp->lsbp())};
            node.m_selectWidth = selp->widthConst();
        } else if (const AstCond* const condp = VN_CAST(nodep, Cond)) {
            node.m_operands = {expression(condp->condp()), expression(condp->thenp()),
                               expression(condp->elsep())};
        } else if (VN_IS(nodep, Not) || VN_IS(nodep, LogNot) || VN_IS(nodep, Negate)
                   || VN_IS(nodep, Extend) || VN_IS(nodep, ExtendS) || VN_IS(nodep, RedAnd)
                   || VN_IS(nodep, RedOr) || VN_IS(nodep, RedXor)) {
            node.m_operands = {expression(VN_AS(nodep, NodeUniop)->lhsp())};
        } else if (VN_IS(nodep, Add) || VN_IS(nodep, Sub) || VN_IS(nodep, Mul) || VN_IS(nodep, And)
                   || VN_IS(nodep, Or) || VN_IS(nodep, Xor) || VN_IS(nodep, LogAnd)
                   || VN_IS(nodep, LogOr) || VN_IS(nodep, Eq) || VN_IS(nodep, Neq)
                   || VN_IS(nodep, Lt) || VN_IS(nodep, Lte) || VN_IS(nodep, Gt)
                   || VN_IS(nodep, Gte) || VN_IS(nodep, ShiftL) || VN_IS(nodep, ShiftR)
                   || VN_IS(nodep, ShiftRS) || VN_IS(nodep, Concat)) {
            const AstNodeBiop* const opp = VN_AS(nodep, NodeBiop);
            node.m_operands = {expression(opp->lhsp()), expression(opp->rhsp())};
        } else {
            return reject(std::string{"expression "} + nodep->typeName());
        }
        return add(std::move(node));
    }

    Id sensitivity(const AstSenTree* treep) {
        if (!treep) return Templates::NONE;
        Templates::Node tree;
        tree.m_kind = "SENTREE";
        if (treep->isMulti()) return reject("merged sensitivity");
        for (const AstSenItem* itemp = treep->sensesp(); itemp;
             itemp = VN_AS(itemp->nextp(), SenItem)) {
            Templates::Node item;
            item.m_kind = "SENITEM";
            item.m_value = itemp->edgeType().ascii();
            item.m_operands = {expression(itemp->sensp()), expression(itemp->condp())};
            tree.m_operands.push_back(add(std::move(item)));
        }
        return add(std::move(tree));
    }

    Id statement(const AstNode* nodep) {
        Templates::Node node;
        node.m_kind = nodep->typeName();
        if (const AstNodeAssign* const assignp = VN_CAST(nodep, NodeAssign)) {
            if (!(VN_IS(nodep, Assign) || VN_IS(nodep, AssignDly) || VN_IS(nodep, AssignW))
                || assignp->timingControlp()) {
                return reject("assignment");
            }
            node.m_operands = {expression(assignp->lhsp()), expression(assignp->rhsp())};
        } else if (const AstIf* const ifp = VN_CAST(nodep, If)) {
            if (ifp->uniquePragma() || ifp->unique0Pragma() || ifp->priorityPragma()) {
                return reject("qualified if");
            }
            node.m_operands
                = {expression(ifp->condp()), statements(ifp->thensp()), statements(ifp->elsesp())};
        } else if (const AstAlways* const alwaysp = VN_CAST(nodep, Always)) {
            node.m_value = alwaysp->keyword().ascii();
            node.m_operands = {sensitivity(alwaysp->sentreep()), statements(alwaysp->stmtsp())};
        } else if (VN_IS(nodep, Initial) || VN_IS(nodep, InitialStatic)) {
            node.m_operands = {statements(VN_AS(nodep, NodeProcedure)->stmtsp())};
        } else {
            return reject(std::string{"statement "} + nodep->typeName());
        }
        return add(std::move(node));
    }

    Id statements(const AstNode* nodep) {
        if (!nodep) return Templates::NONE;
        Templates::Node node;
        node.m_kind = "BLOCK";
        for (; nodep && m_rejection.empty(); nodep = nodep->nextp()) {
            if (const AstVar* const varp = VN_CAST(nodep, Var)) {
                if (!m_slots.count(varp)) return reject("local declaration");
                continue;
            }
            node.m_operands.push_back(statement(nodep));
        }
        return add(std::move(node));
    }

public:
    explicit TemplateBuilder(const AstNodeModule* modp) {
        m_module.m_name = modp->name();
        // Declaration collection precedes expression capture, including forward references.
        for (const AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
            const AstVar* const varp = VN_CAST(nodep, Var);
            if (!varp) continue;
            if (varp->isSigPublic() || varp->isForceable() || varp->isInout() || varp->delayp()
                || varp->attrsp() || varp->childDTypep()) {
                reject("declaration");
                break;
            }
            Templates::Slot slot;
            slot.m_name = varp->name();
            slot.m_direction = varp->direction().ascii();
            slot.m_varType = varp->varType().ascii();
            slot.m_type = type(varp->dtypep());
            m_module.m_slots.push_back(std::move(slot));
            m_slots.emplace(varp, static_cast<Id>(m_module.m_slots.size()));
        }
        if (!m_rejection.empty()) return;
        for (const AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
            if (const AstVar* const varp = VN_CAST(nodep, Var)) {
                if (!varp->valuep()) continue;
                const AstNodeExpr* const valuep = VN_CAST(varp->valuep(), NodeExpr);
                if (!valuep) {
                    reject("initializer");
                    return;
                }
                m_module.m_slots.at(m_slots.at(varp) - 1).m_initializer = expression(valuep);
            }
        }
        m_module.m_body = statements(modp->stmtsp());
    }

    const std::string& rejection() const { return m_rejection; }
    Templates::Module take() { return std::move(m_module); }
};

class TemplateCaptureVisitor final : public VNVisitorConst {
    std::vector<Templates::Module>& m_modules;
    std::unordered_map<const AstNodeModule*, Id>& m_ids;
    std::map<std::string, uint64_t> m_rejections;
    uint64_t m_candidates = 0;

    void visit(AstNetlist* nodep) override { iterateAndNextConstNull(nodep->modulesp()); }
    void visit(AstNodeModule* nodep) override {
        if (!nodep->subgraphBoundary()) return;
        ++m_candidates;
        TemplateBuilder builder{nodep};
        if (!builder.rejection().empty()) {
            ++m_rejections[builder.rejection()];
            return;
        }
        m_modules.push_back(builder.take());
        m_ids.emplace(nodep, static_cast<Id>(m_modules.size()));
    }
    void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

public:
    TemplateCaptureVisitor(AstNetlist* netlistp, std::vector<Templates::Module>& modules,
                           std::unordered_map<const AstNodeModule*, Id>& ids)
        : m_modules{modules}
        , m_ids{ids} {
        iterateConstNull(netlistp);
        V3Stats::addStat("Scheduling, Subgraph template candidates", m_candidates);
        V3Stats::addStat("Scheduling, Subgraph templates captured", modules.size());
        V3Stats::addStat("Scheduling, Subgraph templates rejected", m_candidates - modules.size());
        for (const auto& pair : m_rejections) {
            V3Stats::addStat("Scheduling, Subgraph template rejection, " + pair.first,
                             pair.second);
        }
    }
};

// Traverse physical instance membership without copying template bodies. Cell/module pointers
// exist only during capture; later passes may freely replace or delete their AST nodes.
void collectInstances(const AstNodeModule* modp, const std::string& path,
                      const std::unordered_map<const AstNodeModule*, Id>& ids,
                      std::vector<Templates::Instance>& instances) {
    const auto it = ids.find(modp);
    if (it != ids.end()) instances.push_back(Templates::Instance{path, it->second});
    for (const AstNode* nodep = modp->stmtsp(); nodep; nodep = nodep->nextp()) {
        if (const AstCell* const cellp = VN_CAST(nodep, Cell)) {
            collectInstances(cellp->modp(), path + "." + cellp->name(), ids, instances);
        }
    }
}

void dumpType(V3OutJsonFile& out, const Templates::Type& type) {
    out.begin("dtype")
        .put("keyword", type.m_keyword)
        .put("width", type.m_width)
        .put("signed", type.m_signed)
        .put("ranged", type.m_ranged)
        .put("left", type.m_left)
        .put("right", type.m_right)
        .end();
}

}  // namespace

V3SubgraphTemplates::V3SubgraphTemplates(AstNetlist* netlistp) {
    const VlOs::DeltaWallTime timer{v3Global.opt.stats()};
    std::unordered_map<const AstNodeModule*, Id> ids;
    const TemplateCaptureVisitor visitor{netlistp, m_modules, ids};
    if (!ids.empty()) collectInstances(netlistp->topModulep(), "TOP", ids, m_instances);
    uint64_t nodes = 0;
    for (const Module& module : m_modules) nodes += module.m_nodes.size();
    V3Stats::addStat("Scheduling, Subgraph template nodes stored", nodes);
    V3Stats::addStat("Scheduling, Subgraph template instance memberships", m_instances.size());
    check();
    V3Stats::addStatPerf("Scheduling, Subgraph template capture time (sec)", timer.deltaTime());
}

void V3SubgraphTemplates::check() const {
    for (const Module& module : m_modules) {
        UASSERT(module.m_body && module.m_body <= module.m_nodes.size(), "Invalid template body");
        for (size_t index = 0; index < module.m_nodes.size(); ++index) {
            const Node& node = module.m_nodes[index];
            UASSERT(node.m_slot <= module.m_slots.size(), "Invalid template variable slot");
            for (const Id operand : node.m_operands) {
                UASSERT(operand <= index, "Template operand is not in postorder");
            }
        }
        for (const Slot& slot : module.m_slots) {
            UASSERT(slot.m_initializer <= module.m_nodes.size(), "Invalid template initializer");
        }
    }
    for (const Instance& instance : m_instances) {
        UASSERT(instance.m_template && instance.m_template <= m_modules.size(),
                "Invalid template instance membership");
    }
}

void V3SubgraphTemplates::dump(const std::string& filename) const {
    V3OutJsonFile out{filename};
    out.put("version", 1).begin("templates", '[');
    for (const Module& module : m_modules) {
        out.begin()
            .put("name", VIdProtect::protect(module.m_name))
            .put("body", static_cast<int>(module.m_body));
        out.begin("slots", '[');
        for (const Slot& slot : module.m_slots) {
            out.begin()
                .put("name", VIdProtect::protect(slot.m_name))
                .put("direction", slot.m_direction)
                .put("varType", slot.m_varType)
                .put("initializer", static_cast<int>(slot.m_initializer));
            dumpType(out, slot.m_type);
            out.end();
        }
        out.end().begin("nodes", '[');
        for (const Node& node : module.m_nodes) {
            out.begin()
                .put("kind", node.m_kind)
                .put("value", node.m_value)
                .put("slot", static_cast<int>(node.m_slot))
                .put("selectWidth", node.m_selectWidth);
            dumpType(out, node.m_type);
            out.begin("operands", '[');
            for (const Id operand : node.m_operands) out.put(static_cast<int>(operand));
            out.end();
            out.end();
        }
        out.end();
        out.end();
    }
    out.end().begin("instances", '[');
    for (const Instance& instance : m_instances) {
        out.begin()
            .put("path", VIdProtect::protect(instance.m_path))
            .put("template", static_cast<int>(instance.m_template))
            .end();
    }
    out.end();
}
