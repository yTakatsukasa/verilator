// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Materialize independent subgraph calls
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

#include "V3SubgraphTemplates.h"

class V3SubgraphLower final {
public:
    static void prepare(AstNetlist* netlistp, const V3SubgraphTemplates& templates) VL_MT_DISABLED;
    static void resolve(AstNetlist* netlistp, const V3SubgraphTemplates& templates) VL_MT_DISABLED;
};

#endif  // Guard
