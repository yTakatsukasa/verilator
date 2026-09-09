// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Schedule an independent subgraph template
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

#ifndef VERILATOR_V3SUBGRAPHSCHEDULE_H_
#define VERILATOR_V3SUBGRAPHSCHEDULE_H_

#include "V3SubgraphTemplates.h"

class V3SubgraphSchedule final {
public:
    static V3SubgraphTemplates::Schedule
    build(const V3SubgraphTemplates::Module& module) VL_MT_DISABLED;
};

#endif  // Guard
