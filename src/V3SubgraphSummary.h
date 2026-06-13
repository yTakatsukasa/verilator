// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Subgraph summary builder
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

#ifndef VERILATOR_V3SUBGRAPHSUMMARY_H_
#define VERILATOR_V3SUBGRAPHSUMMARY_H_

#include "config_build.h"
#include "verilatedos.h"

#include <vector>

class AstNetlist;
class AstScope;
class AstVarScope;

class V3SubgraphSummary final {
public:
    struct ScopeSummary final {
        std::vector<AstVarScope*> m_nonOutputPorts;
        std::vector<AstVarScope*> m_writablePorts;
    };

    static void bindScopes(AstNetlist* nodep) VL_MT_DISABLED;
    static void buildModules(AstNetlist* nodep) VL_MT_DISABLED;
    static void clear() VL_MT_DISABLED;
    static const ScopeSummary* getScopeSummary(const AstScope* scopep) VL_MT_DISABLED;
    static bool isDerivedBoundaryInput(const AstVarScope* vscp) VL_MT_DISABLED;
};

#endif
