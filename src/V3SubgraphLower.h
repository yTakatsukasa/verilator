// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Materialize shared subgraph V3Ast entry functions
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

#ifndef VERILATOR_V3SUBGRAPHLOWER_H_
#define VERILATOR_V3SUBGRAPHLOWER_H_

#include "V3SubgraphAst.h"

class V3SubgraphLower final {
public:
    static void prepare(AstNetlist* netlistp, const V3SubgraphAst& ast) VL_MT_DISABLED;
    static void resolve(AstNetlist* netlistp, const V3SubgraphAst& ast) VL_MT_DISABLED;
};

#endif  // Guard
