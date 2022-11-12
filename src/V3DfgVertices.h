// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: DfgVertex sub-classes
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2003-2022 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************
//
// This is a data-flow graph based representation of combinational logic,
// the main difference from a V3Graph is that DfgVertex owns the storage
// of it's input edges (operands/sources/arguments), and can access each
// input edge directly by indexing, making modifications more efficient
// than the linked list based structures used by V3Graph.
//
// A bulk of the DfgVertex sub-types are generated by astgen, and are
// analogous to the corresponding AstNode sub-types.
//
// See also the internals documentation docs/internals.rst
//
//*************************************************************************

#ifndef VERILATOR_V3DFGVERTICES_H_
#define VERILATOR_V3DFGVERTICES_H_

#ifndef VERILATOR_V3DFG_H_
#error "Use V3Dfg.h as the include"
#include "V3Dfg.h"  // This helps code analysis tools pick up symbols in V3Dfg.h
#define VL_NOT_FINAL  // This #define fixes broken code folding in the CLion IDE
#endif

// === Abstract base node types (DfgVertex*) ===================================

class DfgVertexVar VL_NOT_FINAL : public DfgVertexVariadic {
    AstVar* const m_varp;  // The AstVar associated with this vertex (not owned by this vertex)
    bool m_hasModRefs = false;  // This AstVar is referenced outside the DFG, but in the module
    bool m_hasExtRefs = false;  // This AstVar is referenced from outside the module

    bool selfEquals(const DfgVertex& that) const final;
    V3Hash selfHash() const final;

public:
    DfgVertexVar(DfgGraph& dfg, VDfgType type, AstVar* varp, uint32_t initialCapacity)
        : DfgVertexVariadic{dfg, type, varp->fileline(), dtypeFor(varp), initialCapacity}
        , m_varp{varp} {}
    ASTGEN_MEMBERS_DfgVertexVar;

    DfgVertexVar* verticesNext() const {
        return static_cast<DfgVertexVar*>(DfgVertex::verticesNext());
    }
    DfgVertexVar* verticesPrev() const {
        return static_cast<DfgVertexVar*>(DfgVertex::verticesPrev());
    }

    bool isDrivenByDfg() const { return arity() > 0; }

    AstVar* varp() const { return m_varp; }
    bool hasModRefs() const { return m_hasModRefs; }
    void setHasModRefs() { m_hasModRefs = true; }
    bool hasExtRefs() const { return m_hasExtRefs; }
    void setHasExtRefs() { m_hasExtRefs = true; }
    bool hasRefs() const { return m_hasModRefs || m_hasExtRefs; }

    // Variable cannot be removed, even if redundant in the DfgGraph (might be used externally)
    bool keep() const {
        // Keep if referenced outside this module
        if (hasExtRefs()) return true;
        // Keep if traced
        if (v3Global.opt.trace() && varp()->isTrace()) return true;
        // Keep if public
        if (varp()->isSigPublic()) return true;
        // Otherwise it can be removed
        return false;
    }
};

// === Concrete node types =====================================================

// === DfgVertex ===
class DfgConst final : public DfgVertex {
    friend class DfgVertex;
    friend class DfgVisitor;

    V3Number m_num;  // Constant value

    bool selfEquals(const DfgVertex& that) const override;
    V3Hash selfHash() const override;

public:
    DfgConst(DfgGraph& dfg, FileLine* flp, const V3Number& num)
        : DfgVertex{dfg, dfgType(), flp, dtypeForWidth(num.width())}
        , m_num{num} {}
    DfgConst(DfgGraph& dfg, FileLine* flp, uint32_t width, uint32_t value = 0)
        : DfgVertex{dfg, dfgType(), flp, dtypeForWidth(width)}
        , m_num{flp, static_cast<int>(width), value} {}
    ASTGEN_MEMBERS_DfgConst;

    DfgConst* verticesNext() const { return static_cast<DfgConst*>(DfgVertex::verticesNext()); }
    DfgConst* verticesPrev() const { return static_cast<DfgConst*>(DfgVertex::verticesPrev()); }

    V3Number& num() { return m_num; }
    const V3Number& num() const { return m_num; }

    size_t toSizeT() const {
        if VL_CONSTEXPR_CXX17 (sizeof(size_t) > sizeof(uint32_t)) {
            return static_cast<size_t>(num().toUQuad());
        }
        return static_cast<size_t>(num().toUInt());
    }

    bool isZero() const { return num().isEqZero(); }
    bool isOnes() const { return num().isEqAllOnes(width()); }

    // Does this DfgConst have the given value? Note this is not easy to answer if wider than 32.
    bool hasValue(uint32_t value) const {
        return !num().isFourState() && num().edataWord(0) == value && num().mostSetBitP1() <= 32;
    }

    std::pair<DfgEdge*, size_t> sourceEdges() override { return {nullptr, 0}; }
    std::pair<const DfgEdge*, size_t> sourceEdges() const override { return {nullptr, 0}; }
    const string srcName(size_t) const override {  // LCOV_EXCL_START
        VL_UNREACHABLE;
        return "";
    }  // LCOV_EXCL_STOP
};

// === DfgVertexBinary ===
class DfgMux final : public DfgVertexBinary {
    // AstSel is ternary, but the 'widthp' is always constant and is hence redundant, and
    // 'lsbp' is very often constant. As AstSel is fairly common, we special case as a DfgSel for
    // the constant 'lsbp', and as 'DfgMux` for the non-constant 'lsbp'.
public:
    DfgMux(DfgGraph& dfg, FileLine* flp, AstNodeDType* dtypep)
        : DfgVertexBinary{dfg, dfgType(), flp, dtypep} {}
    ASTGEN_MEMBERS_DfgMux;

    DfgVertex* fromp() const { return source<0>(); }
    void fromp(DfgVertex* vtxp) { relinkSource<0>(vtxp); }
    DfgVertex* lsbp() const { return source<1>(); }
    void lsbp(DfgVertex* vtxp) { relinkSource<1>(vtxp); }

    const string srcName(size_t idx) const override { return idx ? "lsbp" : "fromp"; }
};

// === DfgVertexUnary ===
class DfgSel final : public DfgVertexUnary {
    // AstSel is ternary, but the 'widthp' is always constant and is hence redundant, and
    // 'lsbp' is very often constant. As AstSel is fairly common, we special case as a DfgSel for
    // the constant 'lsbp', and as 'DfgMux` for the non-constant 'lsbp'.
    uint32_t m_lsb = 0;  // The LSB index

    bool selfEquals(const DfgVertex& that) const override;
    V3Hash selfHash() const override;

public:
    DfgSel(DfgGraph& dfg, FileLine* flp, AstNodeDType* dtypep)
        : DfgVertexUnary{dfg, dfgType(), flp, dtypep} {}
    ASTGEN_MEMBERS_DfgSel;

    DfgVertex* fromp() const { return source<0>(); }
    void fromp(DfgVertex* vtxp) { relinkSource<0>(vtxp); }
    uint32_t lsb() const { return m_lsb; }
    void lsb(uint32_t value) { m_lsb = value; }

    const string srcName(size_t) const override { return "fromp"; }
};

// === DfgVertexVar ===
class DfgVarArray final : public DfgVertexVar {
    friend class DfgVertex;
    friend class DfgVisitor;

    using DriverData = std::pair<FileLine*, uint32_t>;

    std::vector<DriverData> m_driverData;  // Additional data associate with each driver

public:
    DfgVarArray(DfgGraph& dfg, AstVar* varp)
        : DfgVertexVar{dfg, dfgType(), varp, 4u} {
        UASSERT_OBJ(VN_IS(varp->dtypeSkipRefp(), UnpackArrayDType), varp, "Non array DfgVarArray");
    }
    ASTGEN_MEMBERS_DfgVarArray;

    void addDriver(FileLine* flp, uint32_t index, DfgVertex* vtxp) {
        m_driverData.emplace_back(flp, index);
        DfgVertexVariadic::addSource()->relinkSource(vtxp);
    }

    void resetSources() {
        m_driverData.clear();
        DfgVertexVariadic::resetSources();
    }

    // Remove undriven sources
    void packSources() {
        // Grab and reset the driver data
        std::vector<DriverData> driverData{std::move(m_driverData)};

        // Grab and unlink the sources
        std::vector<DfgVertex*> sources{arity()};
        forEachSourceEdge([&](DfgEdge& edge, size_t idx) {
            sources[idx] = edge.sourcep();
            edge.unlinkSource();
        });
        DfgVertexVariadic::resetSources();

        // Add back the driven sources
        for (size_t i = 0; i < sources.size(); ++i) {
            if (!sources[i]) continue;
            addDriver(driverData[i].first, driverData[i].second, sources[i]);
        }
    }

    FileLine* driverFileLine(size_t idx) const { return m_driverData[idx].first; }
    uint32_t driverIndex(size_t idx) const { return m_driverData[idx].second; }

    DfgVertex* driverAt(size_t idx) const {
        const DfgEdge* const edgep = findSourceEdge([=](const DfgEdge&, size_t i) {  //
            return driverIndex(i) == idx;
        });
        return edgep ? edgep->sourcep() : nullptr;
    }

    const string srcName(size_t idx) const override { return cvtToStr(driverIndex(idx)); }
};
class DfgVarPacked final : public DfgVertexVar {
    friend class DfgVertex;
    friend class DfgVisitor;

    using DriverData = std::pair<FileLine*, uint32_t>;

    std::vector<DriverData> m_driverData;  // Additional data associate with each driver

public:
    DfgVarPacked(DfgGraph& dfg, AstVar* varp)
        : DfgVertexVar{dfg, dfgType(), varp, 1u} {}
    ASTGEN_MEMBERS_DfgVarPacked;

    bool isDrivenFullyByDfg() const { return arity() == 1 && source(0)->dtypep() == dtypep(); }

    void addDriver(FileLine* flp, uint32_t lsb, DfgVertex* vtxp) {
        m_driverData.emplace_back(flp, lsb);
        DfgVertexVariadic::addSource()->relinkSource(vtxp);
    }

    void resetSources() {
        m_driverData.clear();
        DfgVertexVariadic::resetSources();
    }

    // Remove undriven sources
    void packSources() {
        // Grab and reset the driver data
        std::vector<DriverData> driverData{std::move(m_driverData)};

        // Grab and unlink the sources
        std::vector<DfgVertex*> sources{arity()};
        forEachSourceEdge([&](DfgEdge& edge, size_t idx) {
            sources[idx] = edge.sourcep();
            edge.unlinkSource();
        });
        DfgVertexVariadic::resetSources();

        // Add back the driven sources
        for (size_t i = 0; i < sources.size(); ++i) {
            if (!sources[i]) continue;
            addDriver(driverData[i].first, driverData[i].second, sources[i]);
        }
    }

    FileLine* driverFileLine(size_t idx) const { return m_driverData[idx].first; }
    uint32_t driverLsb(size_t idx) const { return m_driverData[idx].second; }

    const string srcName(size_t idx) const override {
        return isDrivenFullyByDfg() ? "" : cvtToStr(driverLsb(idx));
    }
};

#endif