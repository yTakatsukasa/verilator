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

#ifndef VERILATOR_V3SUBGRAPHAST_H_
#define VERILATOR_V3SUBGRAPHAST_H_

#include "config_build.h"
#include "verilatedos.h"

#include <string>
#include <vector>

class AstNetlist;
class AstNodeExpr;
class AstNodeModule;
class AstVar;

// Own one detached V3Ast tree for each elaborated subgraph module specialization. The trees are
// captured before port lowering and scope replication, so parent connectivity cannot specialize
// their contents. They remain V3Ast trees and are the future inputs to ordinary lowering and
// scheduling passes.
class V3SubgraphAst final {
public:
    struct Template final {
        struct Port final {
            uint32_t m_id = 0;
            AstVar* m_sourcep = nullptr;  // Declaration in m_sourcep
            AstVar* m_formalp = nullptr;  // Declaration in m_treep
        };

        uint32_t m_id = 0;
        AstNodeModule* m_sourcep = nullptr;  // Main-tree specialization; owned by AstNetlist
        AstNodeModule* m_treep = nullptr;  // Detached specialization-local V3Ast; owned here
        uint64_t m_instances = 0;
        std::vector<Port> m_ports;
    };
    struct Instance final {
        struct Binding final {
            uint32_t m_formal = 0;  // One-based Template::m_ports ID
            AstNodeExpr* m_actualp = nullptr;  // Detached parent-side V3Ast; nullptr is open
        };

        uint32_t m_template = 0;
        std::string m_path;
        std::vector<Binding> m_bindings;
    };

private:
    std::vector<Template> m_templates;
    std::vector<Instance> m_instances;

public:
    explicit V3SubgraphAst(AstNetlist* netlistp) VL_MT_DISABLED;
    ~V3SubgraphAst();
    VL_UNCOPYABLE(V3SubgraphAst);

    const std::vector<Template>& templates() const { return m_templates; }
    const std::vector<Instance>& instances() const { return m_instances; }
    void check() const VL_MT_DISABLED;
};

#endif  // Guard
