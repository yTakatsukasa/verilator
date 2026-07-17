// DESCRIPTION: Verilator: C++ test driver for DPI import in a subgraph
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

#include "svdpi.h"

extern "C" void dpi_transform(const svLogicVecVal* value, svLogicVecVal* result) {
    for (int i = 0; i < 15; ++i) result[i] = value[i];
}
