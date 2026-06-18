// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Subgraph boundary validation
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

#include "V3PchAstNoMT.h"  // VL_MT_DISABLED_CODE_UNIT

#include "V3SubgraphCheck.h"

#include "V3Error.h"

#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

VL_DEFINE_DEBUG_FUNCTIONS;

namespace {

class SubgraphConstraintVisitor final {
    using ArgBindings = std::unordered_map<string, AstNodeExpr*>;
    using Assignments = std::vector<std::pair<string, AstNodeExpr*>>;
    using DrivenNames = std::set<string>;
    using FunctionNameSeen = std::set<std::pair<AstNodeFTask*, string>>;
    using SafeContextCache = std::unordered_map<string, std::unordered_set<string>>;
    using SafeInputNames = std::set<string>;
    using TraceSeen = std::set<std::pair<AstNodeModule*, string>>;

    class ModuleScanVisitor final : public VNVisitorConst {
        // STATE
        Assignments& m_assignments;
        std::vector<AstCell*>& m_cellps;
        std::unordered_set<string>& m_safeNames;
        std::vector<AstNodeFTaskRef*>& m_taskRefps;
        int m_ftaskDepth = 0;

        // METHODS
        void addAssignment(AstNodeExpr* lhsp, AstNodeExpr* rhsp) {
            const DrivenNames names = lhsNames(lhsp, true);
            for (const string& name : names) m_assignments.emplace_back(name, rhsp);
        }
        void addSafeLhs(AstNodeExpr* lhsp) {
            const DrivenNames names = lhsNames(lhsp, true);
            for (const string& name : names) m_safeNames.insert(name);
        }

        // VISITORS
        void visit(AstAssign* nodep) override { addAssignment(nodep->lhsp(), nodep->rhsp()); }
        void visit(AstAssignDly* nodep) override { addSafeLhs(nodep->lhsp()); }
        void visit(AstAssignW* nodep) override { addAssignment(nodep->lhsp(), nodep->rhsp()); }
        void visit(AstCell* nodep) override { m_cellps.push_back(nodep); }
        void visit(AstNodeFTask* nodep) override {
            ++m_ftaskDepth;
            iterateChildrenConst(nodep);
            --m_ftaskDepth;
        }
        void visit(AstNodeFTaskRef* nodep) override {
            if (m_ftaskDepth == 0 && VN_IS(nodep, TaskRef)) m_taskRefps.push_back(nodep);
            iterateChildrenConst(nodep);
        }
        void visit(AstVar* nodep) override {
            if (m_ftaskDepth == 0 && isCompileTimeConstVar(nodep)) {
                m_safeNames.insert(nodep->name());
            }
        }
        void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    public:
        ModuleScanVisitor(AstNodeModule* modp, Assignments& assignments,
                          std::vector<AstCell*>& cellps, std::unordered_set<string>& safeNames,
                          std::vector<AstNodeFTaskRef*>& taskRefps)
            : m_assignments{assignments}
            , m_cellps{cellps}
            , m_safeNames{safeNames}
            , m_taskRefps{taskRefps} {
            iterateAndNextConstNull(modp->inlinesp());
            iterateAndNextConstNull(modp->stmtsp());
        }
        ~ModuleScanVisitor() override = default;
    };

    struct ModuleInfo final {
        Assignments m_assignments;
        bool m_busy = false;
        std::vector<AstCell*> m_cellps;
        bool m_done = false;
        std::unordered_set<string> m_inputNames;
        std::unordered_set<string> m_safeNames;
        std::vector<AstNodeFTaskRef*> m_taskRefps;
    };

    static bool isCompileTimeConstVar(const AstVar* varp) {
        return varp && (varp->isParam() || varp->isGenVar());
    }

    static bool isCompileTimeConstRef(const AstNodeVarRef* refp) {
        return refp && isCompileTimeConstVar(refp->varp());
    }

    class FunctionScanVisitor final : public VNVisitorConst {
        AstNodeFTask* const m_rootp;
        Assignments& m_assignments;
        std::vector<AstVar*>& m_formalp;
        std::unordered_set<string>& m_localNames;

        void addAssignment(AstNodeExpr* lhsp, AstNodeExpr* rhsp) {
            const DrivenNames names = lhsNames(lhsp, true);
            for (const string& name : names) m_assignments.emplace_back(name, rhsp);
        }

        void visit(AstAssign* nodep) override { addAssignment(nodep->lhsp(), nodep->rhsp()); }
        void visit(AstAssignW* nodep) override { addAssignment(nodep->lhsp(), nodep->rhsp()); }
        void visit(AstNodeFTask* nodep) override {
            if (nodep != m_rootp) return;
            iterateChildrenConst(nodep);
        }
        void visit(AstVar* nodep) override {
            if (!nodep->isFuncLocal() && !nodep->isFuncReturn()) return;
            m_localNames.insert(nodep->name());
            if (nodep->isIO() && !nodep->isFuncReturn()) m_formalp.push_back(nodep);
        }
        void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

    public:
        FunctionScanVisitor(AstNodeFTask* rootp, Assignments& assignments,
                            std::vector<AstVar*>& formalp, std::unordered_set<string>& localNames)
            : m_rootp{rootp}
            , m_assignments{assignments}
            , m_formalp{formalp}
            , m_localNames{localNames} {
            iterateConst(rootp);
        }
        ~FunctionScanVisitor() override = default;
    };

    struct FunctionInfo final {
        Assignments m_assignments;
        bool m_done = false;
        std::vector<AstVar*> m_formalp;
        std::unordered_set<string> m_localNames;
    };

    class ExternalHierAccessVisitor final : public VNVisitor {
        SubgraphConstraintVisitor& m_parent;
        AstNodeModule* const m_boundaryModp;
        AstNodeModule* m_modp = nullptr;
        bool m_reported = false;

        static AstScope* boundaryScope(AstScope* scopep) {
            for (AstScope* scanp = scopep; scanp; scanp = scanp->aboveScopep()) {
                if (scanp->modp()->subgraphBoundary()) return scanp;
            }
            return nullptr;
        }
        static std::vector<string> dottedComponents(string dotted) {
            std::vector<string> components;
            size_t pos = 0;
            while (true) {
                const size_t dotPos = dotted.find("__DOT__", pos);
                const size_t plainPos = dotted.find('.', pos);
                size_t nextPos = string::npos;
                size_t step = 0;
                if (dotPos == string::npos) {
                    nextPos = plainPos;
                    step = 1;
                } else if (plainPos == string::npos || dotPos < plainPos) {
                    nextPos = dotPos;
                    step = 7;
                } else {
                    nextPos = plainPos;
                    step = 1;
                }
                components.push_back(dotted.substr(pos, nextPos - pos));
                if (nextPos == string::npos) break;
                pos = nextPos + step;
            }
            return components;
        }
        AstNodeModule* boundaryModuleFromDotted(const string& dotted) const {
            if (!m_modp) return nullptr;
            AstNodeModule* modp = m_modp;
            const std::vector<string> components = dottedComponents(dotted);
            for (const string& component : components) {
                if (component.empty()) continue;
                if (component == modp->name() || component == modp->prettyName()) continue;
                AstCell* cellp = nullptr;
                for (AstCell* const scanp : m_parent.info(modp).m_cellps) {
                    if (scanp->name() == component) {
                        cellp = scanp;
                        break;
                    }
                }
                if (!cellp) return nullptr;
                modp = cellp->modp();
                if (modp->subgraphBoundary()) return modp;
            }
            return nullptr;
        }

        void visit(AstNodeModule* nodep) override {
            if (m_reported) return;
            VL_RESTORER(m_modp);
            m_modp = nodep;
            iterateChildren(nodep);
        }
        void visit(AstVarXRef* nodep) override {
            if (m_reported || !m_modp) return;
            AstNodeModule* const boundaryModp = boundaryModuleFromDotted(nodep->dotted());
            if (boundaryModp != m_boundaryModp) return;
            nodep->v3error("Subgraph boundary module '"
                           << m_boundaryModp->prettyName() << "' variable '"
                           << nodep->varp()->prettyName()
                           << "' is accessed hierarchically from outside the subgraph");
            m_reported = true;
        }
        void visit(AstNodeVarRef* nodep) override {
            if (m_reported || !m_modp || !nodep->varScopep()) return;
            AstScope* const boundaryp = boundaryScope(nodep->varScopep()->scopep());
            if (!boundaryp || boundaryp->modp() != m_boundaryModp || m_modp == m_boundaryModp) {
                return;
            }
            nodep->v3error("Subgraph boundary module '"
                           << m_boundaryModp->prettyName() << "' variable '"
                           << nodep->varp()->prettyName()
                           << "' is accessed hierarchically from outside the subgraph");
            m_reported = true;
        }
        void visit(AstNode* nodep) override {
            if (m_reported) return;
            iterateChildren(nodep);
        }

    public:
        explicit ExternalHierAccessVisitor(SubgraphConstraintVisitor& parent, AstNetlist* rootp,
                                           AstNodeModule* boundaryModp)
            : m_parent{parent}
            , m_boundaryModp{boundaryModp} {
            iterate(rootp);
        }
        ~ExternalHierAccessVisitor() override = default;
    };

    AstNetlist* const m_rootp;
    std::unordered_map<AstNodeModule*, SafeContextCache> m_contextualSafeCache;
    std::unordered_map<AstNodeFTask*, FunctionInfo> m_functions;
    std::unordered_map<AstNodeModule*, ModuleInfo> m_infos;

    static string safeInputKey(const SafeInputNames& safeInputs) {
        string key;
        for (const string& name : safeInputs) key += name + '\n';
        return key;
    }

    static AstVar* formalByName(const FunctionInfo& finfo, const string& name) {
        for (AstVar* const formalp : finfo.m_formalp) {
            if (formalp->name() == name) return formalp;
        }
        return nullptr;
    }

    static bool formalUsesBinding(const FunctionInfo& finfo, const string& name) {
        AstVar* const formalp = formalByName(finfo, name);
        return formalp && formalp->isInput() && !formalp->isWritable();
    }

    const FunctionInfo& functionInfo(AstNodeFTask* taskp) {
        FunctionInfo& info = m_functions[taskp];
        if (info.m_done) return info;

        FunctionScanVisitor{taskp, info.m_assignments, info.m_formalp, info.m_localNames};
        info.m_done = true;
        return info;
    }

    static ArgBindings bindActualArgs(AstNodeFTaskRef* refp, const FunctionInfo& finfo) {
        ArgBindings bindings;
        size_t positional = 0;
        for (AstArg* argp = refp->argsp(); argp; argp = VN_AS(argp->nextp(), Arg)) {
            if (!argp->name().empty()) {
                bindings[argp->name()] = argp->exprp();
                continue;
            }
            while (positional < finfo.m_formalp.size()
                   && bindings.count(finfo.m_formalp[positional]->name())) {
                ++positional;
            }
            if (positional < finfo.m_formalp.size()) {
                bindings[finfo.m_formalp[positional]->name()] = argp->exprp();
                ++positional;
            }
        }
        for (AstVar* const formalp : finfo.m_formalp) {
            if (!formalp->isInput() || formalp->isWritable()) continue;
            if (bindings.count(formalp->name())) continue;
            if (AstNodeExpr* const defaultp = VN_CAST(formalp->valuep(), NodeExpr)) {
                bindings[formalp->name()] = defaultp;
            }
        }
        return bindings;
    }

    static void collectLhsNames(AstNode* nodep, bool requireWrite, DrivenNames& names) {
        if (AstVarRef* const refp = VN_CAST(nodep, VarRef)) {
            if (!requireWrite || refp->access().isWriteOrRW()) names.insert(refp->varp()->name());
            return;
        }
        if (AstArraySel* const selp = VN_CAST(nodep, ArraySel)) {
            collectLhsNames(selp->fromp(), requireWrite, names);
            return;
        }
        if (AstConcat* const concatp = VN_CAST(nodep, Concat)) {
            collectLhsNames(concatp->lhsp(), requireWrite, names);
            collectLhsNames(concatp->rhsp(), requireWrite, names);
            return;
        }
        if (AstConcatN* const concatp = VN_CAST(nodep, ConcatN)) {
            collectLhsNames(concatp->lhsp(), requireWrite, names);
            collectLhsNames(concatp->rhsp(), requireWrite, names);
            return;
        }
        if (AstMemberSel* const memberselp = VN_CAST(nodep, MemberSel)) {
            collectLhsNames(memberselp->fromp(), requireWrite, names);
            return;
        }
        if (AstReplicate* const replicatep = VN_CAST(nodep, Replicate)) {
            collectLhsNames(replicatep->srcp(), requireWrite, names);
            return;
        }
        if (AstSel* const selp = VN_CAST(nodep, Sel)) {
            collectLhsNames(selp->fromp(), requireWrite, names);
            return;
        }
        if (AstSelBit* const selbitp = VN_CAST(nodep, SelBit)) {
            collectLhsNames(selbitp->fromp(), requireWrite, names);
            return;
        }
        if (AstSelMinus* const selminusp = VN_CAST(nodep, SelMinus)) {
            collectLhsNames(selminusp->fromp(), requireWrite, names);
            return;
        }
        if (AstSelPlus* const selplusp = VN_CAST(nodep, SelPlus)) {
            collectLhsNames(selplusp->fromp(), requireWrite, names);
            return;
        }
    }

    static DrivenNames lhsNames(AstNodeExpr* nodep, bool requireWrite) {
        DrivenNames names;
        collectLhsNames(nodep, requireWrite, names);
        return names;
    }

    void collectTaskRefWrittenNames(AstNodeFTaskRef* refp, std::set<string>& names) {
        AstNodeFTask* const taskp = refp->taskp();
        if (!taskp) return;
        const FunctionInfo& finfo = functionInfo(taskp);
        const ArgBindings bindings = bindActualArgs(refp, finfo);
        for (AstVar* const formalp : finfo.m_formalp) {
            if (!formalp->isWritable()) continue;
            const auto it = bindings.find(formalp->name());
            if (it == bindings.end() || !it->second) continue;
            const DrivenNames written = lhsNames(it->second, false);
            names.insert(written.begin(), written.end());
        }
    }

    void collectModuleWrittenNames(AstNodeModule* modp, std::set<string>& names) {
        const ModuleInfo& inf = info(modp);
        for (const auto& pair : inf.m_assignments) names.insert(pair.first);
        for (AstNodeFTaskRef* const refp : inf.m_taskRefps)
            collectTaskRefWrittenNames(refp, names);
        for (AstCell* const cellp : inf.m_cellps) {
            for (AstPin* pinp = cellp->pinsp(); pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
                AstVar* const portp = pinp->modVarp();
                AstNodeExpr* const exprp = VN_CAST(pinp->exprp(), NodeExpr);
                if (!portp || !exprp || !portp->isWritable()) continue;
                const DrivenNames written = lhsNames(exprp, false);
                names.insert(written.begin(), written.end());
            }
        }
    }

    bool nameSafeWithCurrent(AstNodeModule* modp, const string& name,
                             const std::unordered_set<string>& safeNames) {
        if (safeNames.count(name)) return true;

        const ModuleInfo& inf = info(modp);
        bool anyWriter = false;

        for (const auto& pair : inf.m_assignments) {
            if (pair.first != name) continue;
            anyWriter = true;
            if (!exprSafe(modp, pair.second, safeNames)) return false;
        }

        for (AstNodeFTaskRef* const refp : inf.m_taskRefps) {
            AstNodeFTask* const taskp = refp->taskp();
            if (!taskp) continue;
            const FunctionInfo& finfo = functionInfo(taskp);
            const ArgBindings bindings = bindActualArgs(refp, finfo);
            for (AstVar* const formalp : finfo.m_formalp) {
                if (!formalp->isWritable()) continue;
                const auto itBinding = bindings.find(formalp->name());
                if (itBinding == bindings.end() || !itBinding->second) continue;
                if (!lhsNames(itBinding->second, false).count(name)) continue;
                anyWriter = true;
                FunctionNameSeen seen;
                if (!funcNameSafe(modp, taskp, formalp->name(), safeNames, &bindings, seen)) {
                    return false;
                }
            }
        }

        for (AstCell* const cellp : inf.m_cellps) {
            SafeInputNames childSafeInputs;
            for (AstPin* pinp = cellp->pinsp(); pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
                AstVar* const portp = pinp->modVarp();
                AstNodeExpr* const exprp = VN_CAST(pinp->exprp(), NodeExpr);
                if (!portp || !exprp || !portp->isInput()) continue;
                if (exprSafe(modp, exprp, safeNames)) childSafeInputs.insert(portp->name());
            }
            const std::unordered_set<string>& childSafe
                = contextualSafeNames(cellp->modp(), childSafeInputs);
            for (AstPin* pinp = cellp->pinsp(); pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
                AstVar* const portp = pinp->modVarp();
                AstNodeExpr* const exprp = VN_CAST(pinp->exprp(), NodeExpr);
                if (!portp || !exprp || !portp->isWritable()) continue;
                if (!lhsNames(exprp, false).count(name)) continue;
                anyWriter = true;
                if (!childSafe.count(portp->name())) return false;
            }
        }

        return anyWriter;
    }

    const std::unordered_set<string>& contextualSafeNames(AstNodeModule* modp,
                                                          const SafeInputNames& safeInputs) {
        SafeContextCache& byKey = m_contextualSafeCache[modp];
        const string key = safeInputKey(safeInputs);
        auto it = byKey.find(key);
        if (it != byKey.end()) return it->second;

        std::unordered_set<string>& safeNames = byKey[key];
        const ModuleInfo& inf = info(modp);
        safeNames = inf.m_safeNames;
        safeNames.insert(safeInputs.begin(), safeInputs.end());

        bool changed = true;
        while (changed) {
            changed = false;
            std::set<string> candidates;
            collectModuleWrittenNames(modp, candidates);
            for (const string& name : candidates) {
                if (safeNames.count(name)) continue;
                if (nameSafeWithCurrent(modp, name, safeNames)) {
                    safeNames.insert(name);
                    changed = true;
                }
            }
        }

        return safeNames;
    }

    static bool isCompileTimeConstAttr(const AstAttrOf* attrp) {
        if (!attrp) return false;
        switch (attrp->attrType()) {
        case VAttrType::DIM_BITS:
        case VAttrType::DIM_HIGH:
        case VAttrType::DIM_LEFT:
        case VAttrType::DIM_LOW:
        case VAttrType::DIM_RIGHT:
        case VAttrType::DIM_INCREMENT:
        case VAttrType::DIM_SIZE: return true;
        default: return false;
        }
    }

    bool exprSafe(AstNodeModule* modp, AstNodeExpr* nodep,
                  const std::unordered_set<string>& safeNames, AstNodeFTask* taskp = nullptr,
                  const ArgBindings* argBindingsp = nullptr, FunctionNameSeen* seenp = nullptr) {
        if (AstNodeVarRef* const refp = VN_CAST(nodep, NodeVarRef)) {
            if (!refp->access().isReadOrRW()) return true;
            if (isCompileTimeConstRef(refp)) return true;
            if (taskp && (refp->varp()->isFuncLocal() || refp->varp()->isFuncReturn())) {
                FunctionNameSeen localSeen;
                return funcNameSafe(modp, taskp, refp->varp()->name(), safeNames, argBindingsp,
                                    seenp ? *seenp : localSeen);
            }
            return safeNames.count(refp->varp()->name());
        }
        if (AstAttrOf* const attrp = VN_CAST(nodep, AttrOf)) {
            if (isCompileTimeConstAttr(attrp)) {
                return !attrp->dimp()
                       || exprSafe(modp, attrp->dimp(), safeNames, taskp, argBindingsp, seenp);
            }
        }

        class ExprSafeVisitor final : public VNVisitorConst {
            SubgraphConstraintVisitor& m_parent;
            AstNodeModule* const m_modp;
            const std::unordered_set<string>& m_safeNames;
            AstNodeFTask* const m_taskp;
            const ArgBindings* const m_argBindingsp;
            FunctionNameSeen* const m_seenp;
            bool m_safe = true;

            void visit(AstAttrOf* nodep) override {
                if (!m_safe) return;
                if (isCompileTimeConstAttr(nodep)) {
                    iterateAndNextConstNull(nodep->dimp());
                    return;
                }
                iterateChildrenConst(nodep);
            }
            void visit(AstNodeFTaskRef* nodep) override {
                if (!m_safe) return;
                AstNodeFTask* const calledTaskp = nodep->taskp();
                if (calledTaskp && calledTaskp->isFunction()) {
                    const FunctionInfo& finfo = m_parent.functionInfo(calledTaskp);
                    const ArgBindings bindings = bindActualArgs(nodep, finfo);
                    FunctionNameSeen localSeen;
                    if (!m_parent.funcNameSafe(m_modp, calledTaskp, calledTaskp->name(),
                                               m_safeNames, &bindings,
                                               m_seenp ? *m_seenp : localSeen)) {
                        m_safe = false;
                        return;
                    }
                }
                iterateChildrenConst(nodep);
            }
            void visit(AstNodeVarRef* nodep) override {
                if (!m_safe || !nodep->access().isReadOrRW()) return;
                if (isCompileTimeConstRef(nodep)) return;
                if (m_taskp && (nodep->varp()->isFuncLocal() || nodep->varp()->isFuncReturn())) {
                    FunctionNameSeen localSeen;
                    if (!m_parent.funcNameSafe(m_modp, m_taskp, nodep->varp()->name(), m_safeNames,
                                               m_argBindingsp, m_seenp ? *m_seenp : localSeen)) {
                        m_safe = false;
                    }
                } else if (!m_safeNames.count(nodep->varp()->name())) {
                    m_safe = false;
                }
            }
            void visit(AstNode* nodep) override {
                if (!m_safe) return;
                iterateChildrenConst(nodep);
            }

        public:
            ExprSafeVisitor(SubgraphConstraintVisitor& parent, AstNodeModule* modp,
                            AstNodeExpr* nodep, const std::unordered_set<string>& safeNames,
                            AstNodeFTask* taskp, const ArgBindings* argBindingsp,
                            FunctionNameSeen* seenp)
                : m_parent{parent}
                , m_modp{modp}
                , m_safeNames{safeNames}
                , m_taskp{taskp}
                , m_argBindingsp{argBindingsp}
                , m_seenp{seenp} {
                iterateConst(nodep);
            }
            bool safe() const { return m_safe; }
        };

        return ExprSafeVisitor{*this, modp, nodep, safeNames, taskp, argBindingsp, seenp}.safe();
    }

    void collectExprInputs(AstNodeModule* modp, AstNodeExpr* exprp, std::set<string>& inputs,
                           std::set<string>& nonstates, TraceSeen& seen,
                           AstNodeFTask* taskp = nullptr,
                           const ArgBindings* argBindingsp = nullptr,
                           FunctionNameSeen* funcSeenp = nullptr) {
        if (AstNodeVarRef* const refp = VN_CAST(exprp, NodeVarRef)) {
            if (!refp->access().isReadOrRW()) return;
            if (isCompileTimeConstRef(refp)) return;
            if (taskp && (refp->varp()->isFuncLocal() || refp->varp()->isFuncReturn())) {
                FunctionNameSeen localSeen;
                collectFuncNameInputs(modp, taskp, refp->varp()->name(), inputs, nonstates, seen,
                                      argBindingsp, funcSeenp ? *funcSeenp : localSeen);
            } else {
                collectNameInputs(modp, refp->varp()->name(), inputs, nonstates, seen);
            }
            return;
        }
        if (AstAttrOf* const attrp = VN_CAST(exprp, AttrOf)) {
            if (isCompileTimeConstAttr(attrp)) {
                if (attrp->dimp()) {
                    collectExprInputs(modp, attrp->dimp(), inputs, nonstates, seen, taskp,
                                      argBindingsp, funcSeenp);
                }
                return;
            }
        }

        class ExprInputVisitor final : public VNVisitorConst {
            SubgraphConstraintVisitor& m_parent;
            AstNodeModule* const m_modp;
            std::set<string>& m_inputs;
            std::set<string>& m_nonstates;
            TraceSeen& m_seen;
            AstNodeFTask* const m_taskp;
            const ArgBindings* const m_argBindingsp;
            FunctionNameSeen* const m_funcSeenp;

            void visit(AstAttrOf* nodep) override {
                if (isCompileTimeConstAttr(nodep)) {
                    iterateAndNextConstNull(nodep->dimp());
                    return;
                }
                iterateChildrenConst(nodep);
            }
            void visit(AstNodeFTaskRef* nodep) override {
                AstNodeFTask* const calledTaskp = nodep->taskp();
                if (calledTaskp && calledTaskp->isFunction()) {
                    const FunctionInfo& finfo = m_parent.functionInfo(calledTaskp);
                    const ArgBindings bindings = bindActualArgs(nodep, finfo);
                    FunctionNameSeen localSeen;
                    m_parent.collectFuncNameInputs(m_modp, calledTaskp, calledTaskp->name(),
                                                   m_inputs, m_nonstates, m_seen, &bindings,
                                                   m_funcSeenp ? *m_funcSeenp : localSeen);
                }
                iterateChildrenConst(nodep);
            }
            void visit(AstNodeVarRef* nodep) override {
                if (!nodep->access().isReadOrRW()) return;
                if (isCompileTimeConstRef(nodep)) return;
                if (m_taskp && (nodep->varp()->isFuncLocal() || nodep->varp()->isFuncReturn())) {
                    FunctionNameSeen localSeen;
                    m_parent.collectFuncNameInputs(m_modp, m_taskp, nodep->varp()->name(),
                                                   m_inputs, m_nonstates, m_seen, m_argBindingsp,
                                                   m_funcSeenp ? *m_funcSeenp : localSeen);
                } else {
                    m_parent.collectNameInputs(m_modp, nodep->varp()->name(), m_inputs,
                                               m_nonstates, m_seen);
                }
            }
            void visit(AstNode* nodep) override { iterateChildrenConst(nodep); }

        public:
            ExprInputVisitor(SubgraphConstraintVisitor& parent, AstNodeModule* modp,
                             AstNodeExpr* exprp, std::set<string>& inputs,
                             std::set<string>& nonstates, TraceSeen& seen, AstNodeFTask* taskp,
                             const ArgBindings* argBindingsp, FunctionNameSeen* funcSeenp)
                : m_parent{parent}
                , m_modp{modp}
                , m_inputs{inputs}
                , m_nonstates{nonstates}
                , m_seen{seen}
                , m_taskp{taskp}
                , m_argBindingsp{argBindingsp}
                , m_funcSeenp{funcSeenp} {
                iterateConst(exprp);
            }
        };

        ExprInputVisitor{*this, modp,  exprp,        inputs,   nonstates,
                         seen,  taskp, argBindingsp, funcSeenp};
    }

    bool funcNameSafe(AstNodeModule* modp, AstNodeFTask* taskp, const string& name,
                      const std::unordered_set<string>& safeNames, const ArgBindings* argBindingsp,
                      FunctionNameSeen& seen) {
        if (!seen.emplace(taskp, name).second) return true;

        if (argBindingsp) {
            const FunctionInfo& finfo = functionInfo(taskp);
            if (formalUsesBinding(finfo, name)) {
                const auto argIt = argBindingsp->find(name);
                if (argIt != argBindingsp->end() && argIt->second) {
                    return exprSafe(modp, argIt->second, safeNames, taskp, argBindingsp, &seen);
                }
            }
        }

        const FunctionInfo& finfo = functionInfo(taskp);
        if (!finfo.m_localNames.count(name)) return safeNames.count(name);

        bool anyAssign = false;
        for (const auto& pair : finfo.m_assignments) {
            if (pair.first != name) continue;
            anyAssign = true;
            if (!exprSafe(modp, pair.second, safeNames, taskp, argBindingsp, &seen)) return false;
        }
        return anyAssign;
    }

    void collectFuncNameInputs(AstNodeModule* modp, AstNodeFTask* taskp, const string& name,
                               std::set<string>& inputs, std::set<string>& nonstates,
                               TraceSeen& seen, const ArgBindings* argBindingsp,
                               FunctionNameSeen& funcSeen) {
        if (!funcSeen.emplace(taskp, name).second) return;

        if (argBindingsp) {
            const auto argIt = argBindingsp->find(name);
            const FunctionInfo& finfo = functionInfo(taskp);
            if (formalUsesBinding(finfo, name) && argIt != argBindingsp->end() && argIt->second) {
                collectExprInputs(modp, argIt->second, inputs, nonstates, seen, taskp,
                                  argBindingsp, &funcSeen);
                return;
            }
        }

        const FunctionInfo& finfo = functionInfo(taskp);
        if (!finfo.m_localNames.count(name)) {
            collectNameInputs(modp, name, inputs, nonstates, seen);
            return;
        }

        for (const auto& pair : finfo.m_assignments) {
            if (pair.first != name) continue;
            collectExprInputs(modp, pair.second, inputs, nonstates, seen, taskp, argBindingsp,
                              &funcSeen);
        }
    }

    void collectNameInputs(AstNodeModule* modp, const string& name, std::set<string>& inputs,
                           std::set<string>& nonstates, TraceSeen& seen) {
        if (!seen.emplace(modp, name).second) return;

        const ModuleInfo& inf = info(modp);
        if (inf.m_inputNames.count(name)) {
            inputs.insert(name);
            return;
        }
        if (inf.m_safeNames.count(name)) return;

        const size_t inputsBefore = inputs.size();
        const size_t nonstatesBefore = nonstates.size();

        for (const auto& pair : inf.m_assignments) {
            if (pair.first == name) collectExprInputs(modp, pair.second, inputs, nonstates, seen);
        }

        for (AstNodeFTaskRef* const refp : inf.m_taskRefps) {
            AstNodeFTask* const taskp = refp->taskp();
            if (!taskp) continue;
            const FunctionInfo& finfo = functionInfo(taskp);
            const ArgBindings bindings = bindActualArgs(refp, finfo);
            for (AstVar* const formalp : finfo.m_formalp) {
                if (!formalp->isWritable()) continue;
                const auto itBinding = bindings.find(formalp->name());
                if (itBinding == bindings.end() || !itBinding->second) continue;
                if (!lhsNames(itBinding->second, false).count(name)) continue;
                FunctionNameSeen funcSeen;
                collectFuncNameInputs(modp, taskp, formalp->name(), inputs, nonstates, seen,
                                      &bindings, funcSeen);
            }
        }

        for (AstCell* const cellp : inf.m_cellps) {
            for (AstPin* pinp = cellp->pinsp(); pinp; pinp = VN_AS(pinp->nextp(), Pin)) {
                AstVar* const portp = pinp->modVarp();
                AstNodeExpr* const exprp = VN_CAST(pinp->exprp(), NodeExpr);
                if (!portp || !exprp || !portp->isWritable()) continue;
                if (!lhsNames(exprp, false).count(name)) continue;

                std::set<string> childInputs;
                std::set<string> childNonstates;
                TraceSeen childSeen;
                collectNameInputs(cellp->modp(), portp->name(), childInputs, childNonstates,
                                  childSeen);
                for (const string& childInput : childInputs) {
                    for (AstPin* inputPinp = cellp->pinsp(); inputPinp;
                         inputPinp = VN_AS(inputPinp->nextp(), Pin)) {
                        AstVar* const inputPortp = inputPinp->modVarp();
                        AstNodeExpr* const inputExprp = VN_CAST(inputPinp->exprp(), NodeExpr);
                        if (inputPortp && inputExprp && inputPortp->name() == childInput) {
                            collectExprInputs(modp, inputExprp, inputs, nonstates, seen);
                        }
                    }
                }
                for (const string& childNonstate : childNonstates) {
                    nonstates.insert(cellp->name() + "." + childNonstate);
                }
            }
        }

        if (inputs.size() == inputsBefore && nonstates.size() == nonstatesBefore) {
            nonstates.insert(name);
        }
    }

    string feedthroughSourceText(AstNodeModule* modp, const string& name) {
        std::set<string> inputs;
        std::set<string> nonstates;
        TraceSeen seen;
        collectNameInputs(modp, name, inputs, nonstates, seen);
        if (inputs.empty() && nonstates.empty()) return "non-state logic";

        std::ostringstream os;
        if (!inputs.empty()) {
            os << "input port(s): ";
            const char* sep = "";
            for (const string& input : inputs) {
                os << sep << "'" << input << "'";
                sep = ", ";
            }
            if (nonstates.empty()) return os.str();
            os << " (non-state node(s): ";
            const char* nodeSep = "";
            for (const string& nonstate : nonstates) {
                os << nodeSep << "'" << nonstate << "'";
                nodeSep = ", ";
            }
            os << ")";
            return os.str();
        }

        os << "non-state node(s): ";
        const char* sep = "";
        for (const string& nonstate : nonstates) {
            os << sep << "'" << nonstate << "'";
            sep = ", ";
        }
        return os.str();
    }

    void checkNested(AstNodeModule* ownerp, AstNodeModule* modp,
                     std::unordered_set<AstNodeModule*>& seen) {
        if (!seen.insert(modp).second) return;
        const ModuleInfo& inf = info(modp);
        for (AstCell* const cellp : inf.m_cellps) {
            AstNodeModule* const childp = cellp->modp();
            if (childp->subgraphBoundary()) {
                cellp->v3error("Subgraph boundary module '"
                               << ownerp->prettyName()
                               << "' instantiates subgraph boundary module '"
                               << childp->prettyName() << "'");
            } else {
                checkNested(ownerp, childp, seen);
            }
        }
    }

    void checkDpiUsage(AstNodeModule* modp) {
        bool reported = false;
        modp->foreach([&](AstNodeFTask* nodep) {
            if (reported) return;
            if (!nodep->dpiImport() && !nodep->dpiExport()) return;
            nodep->v3error("Subgraph boundary module '" << modp->prettyName()
                                                        << "' uses DPI-C function/task '"
                                                        << nodep->prettyName() << "'");
            reported = true;
        });
        modp->foreach([&](AstNodeFTaskRef* nodep) {
            if (reported) return;
            AstNodeFTask* const taskp = nodep->taskp();
            if (!taskp || (!taskp->dpiImport() && !taskp->dpiExport())) return;
            nodep->v3error("Subgraph boundary module '" << modp->prettyName()
                                                        << "' uses DPI-C function/task '"
                                                        << taskp->prettyName() << "'");
            reported = true;
        });
    }

    void checkExternalHierarchicalAccess(AstNodeModule* modp) {
        ExternalHierAccessVisitor{*this, m_rootp, modp};
    }

    void checkTimingUsage(AstNodeModule* modp) {
        if (!v3Global.opt.timing()) return;

        bool reported = false;
        modp->foreach([&](AstNodeAssign* nodep) {
            if (reported || !nodep->timingControlp()) return;
            nodep->timingControlp()->v3error("Subgraph boundary module '"
                                             << modp->prettyName()
                                             << "' uses timing control or dynamic event logic");
            reported = true;
        });
        modp->foreach([&](AstNode* nodep) {
            if (reported) return;
            if (!VN_IS(nodep, Delay) && !VN_IS(nodep, EventControl) && !VN_IS(nodep, FireEvent)
                && !VN_IS(nodep, Wait) && !VN_IS(nodep, WaitFork) && !VN_IS(nodep, CAwait)) {
                return;
            }
            nodep->v3error("Subgraph boundary module '"
                           << modp->prettyName()
                           << "' uses timing control or dynamic event logic");
            reported = true;
        });
    }

    void checkVpiUsage(AstNodeModule* modp) {
        if (!v3Global.opt.vpi()) return;
        modp->v3error("Subgraph boundary module '" << modp->prettyName()
                                                   << "' is unsupported with --vpi");
    }

    const ModuleInfo& info(AstNodeModule* modp) {
        ModuleInfo& inf = m_infos[modp];
        if (inf.m_done || inf.m_busy) return inf;

        inf.m_busy = true;
        ModuleScanVisitor{modp, inf.m_assignments, inf.m_cellps, inf.m_safeNames, inf.m_taskRefps};
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstVar* const varp = VN_CAST(stmtp, Var);
            if (varp && varp->isInput()) inf.m_inputNames.insert(varp->name());
            if (isCompileTimeConstVar(varp)) inf.m_safeNames.insert(varp->name());
        }

        bool changed = true;
        while (changed) {
            changed = false;
            std::set<string> candidates;
            collectModuleWrittenNames(modp, candidates);
            for (const string& name : candidates) {
                if (inf.m_safeNames.count(name)) continue;
                if (nameSafeWithCurrent(modp, name, inf.m_safeNames)) {
                    inf.m_safeNames.insert(name);
                    changed = true;
                }
            }
        }

        inf.m_busy = false;
        inf.m_done = true;
        return inf;
    }

    void validate(AstNodeModule* modp) {
        if (!modp->subgraphBoundary()) return;

        checkDpiUsage(modp);
        checkExternalHierarchicalAccess(modp);
        std::unordered_set<AstNodeModule*> seen;
        checkNested(modp, modp, seen);
        checkTimingUsage(modp);
        checkVpiUsage(modp);

        const ModuleInfo& inf = info(modp);
        for (AstNode* stmtp = modp->stmtsp(); stmtp; stmtp = stmtp->nextp()) {
            AstVar* const varp = VN_CAST(stmtp, Var);
            if (!varp || !varp->isIO() || !varp->isWritable()) continue;
            if (inf.m_safeNames.count(varp->name())) continue;
            const string sourceText = feedthroughSourceText(modp, varp->name());
            varp->v3error("Subgraph boundary module '"
                          << modp->prettyName() << "' output '" << varp->prettyName()
                          << "' has a feedthrough path from " << sourceText);
        }
    }

public:
    explicit SubgraphConstraintVisitor(AstNetlist* rootp)
        : m_rootp{rootp} {
        for (AstNodeModule* modp = m_rootp->modulesp(); modp;
             modp = VN_AS(modp->nextp(), NodeModule)) {
            info(modp);
        }
        for (AstNodeModule* modp = m_rootp->modulesp(); modp;
             modp = VN_AS(modp->nextp(), NodeModule)) {
            validate(modp);
        }
    }
};

}  // namespace

void V3SubgraphCheck::check(AstNetlist* nodep) {
    UINFO(2, __FUNCTION__ << ":");
    if (v3Global.opt.subgraphSchedule()) SubgraphConstraintVisitor{nodep};
    V3Global::dumpCheckGlobalTree("subgraphcheck", 0, dumpTreeEitherLevel() >= 3);
}
