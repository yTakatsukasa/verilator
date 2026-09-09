// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Instance-independent subgraph templates
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

#ifndef VERILATOR_V3SUBGRAPHTEMPLATES_H_
#define VERILATOR_V3SUBGRAPHTEMPLATES_H_

#include "config_build.h"
#include "verilatedos.h"

#include <string>
#include <vector>

class AstNetlist;

// An immutable, owning checkpoint taken before port lowering and scope replication. Node and
// variable references are local indices, never pointers into the subsequently optimized AST.
// This is the input to independent scheduling, not a cache of instance-specific ordered code.
class V3SubgraphTemplates final {
public:
    using Id = uint32_t;
    static constexpr Id NONE = 0;

    struct Type final {
        std::string m_keyword;
        int m_width = 0;
        int m_left = 0;
        int m_right = 0;
        bool m_signed = false;
        bool m_ranged = false;
    };
    struct Node final {
        std::string m_kind;
        Type m_type;
        std::vector<Id> m_operands;
        std::string m_value;  // Literal, access mode, or event/always keyword
        Id m_slot = NONE;  // Variable reference, one-based in Module::m_slots
        int m_selectWidth = 0;
    };
    struct Slot final {
        std::string m_name;
        std::string m_direction;
        std::string m_varType;
        Type m_type;
        Id m_initializer = NONE;
    };
    struct Module final {
        std::string m_name;
        std::vector<Slot> m_slots;
        std::vector<Node> m_nodes;  // Postorder; references are one-based indices
        Id m_body = NONE;
    };
    struct Instance final {
        std::string m_path;
        Id m_template = NONE;  // One-based in m_modules
        struct Connection final {
            Id m_formal = NONE;  // Template storage slot, never aliased with another formal
            Id m_actual = NONE;  // Instance expression node; NONE denotes an open port
        };
        std::vector<Slot> m_parentSlots;  // Referenced declarations in the enclosing instance
        std::vector<Node> m_nodes;  // Parent expressions, separate from the template body
        std::vector<Connection> m_connections;
        std::string m_bindingRejection;  // Nonempty means the whole instance needs fallback
    };

private:
    std::vector<Module> m_modules;
    std::vector<Instance> m_instances;

public:
    explicit V3SubgraphTemplates(AstNetlist* netlistp) VL_MT_DISABLED;
    const std::vector<Module>& modules() const { return m_modules; }
    const std::vector<Instance>& instances() const { return m_instances; }
    void check() const VL_MT_DISABLED;
    void dump(const std::string& filename) const VL_MT_DISABLED;
};

#endif  // Guard
