// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Scheduling subgraph helpers
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

#ifndef VERILATOR_V3SCHEDSUBGRAPH_H_
#define VERILATOR_V3SCHEDSUBGRAPH_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Order.h"
#include "V3Sched.h"

namespace V3Sched {

AstCFunc* cloneUnguardedFuncBody(AstCFunc* funcp, AstScope* scopep, const std::string& nameSuffix,
                                 bool slow);

void registerSubgraphCallUsageSummary(
    const AstCFunc* funcp, std::vector<SubgraphCallUsageSummary>&& summary) VL_MT_DISABLED;
void rememberSubgraphSnapshotProcedure(const AstNodeProcedure* procp) VL_MT_DISABLED;
void clearSubgraphSnapshotProcedures() VL_MT_DISABLED;

}  // namespace V3Sched

#endif  // guard
