#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")

common_flags = [
    "-Wno-ALWCOMBORDER",
    "-Wno-UNOPTFLAT",
]

test.compile(verilator_flags2=["--stats", "--subgraph-schedule"] + common_flags)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA coarse nodes\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA groups\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA refresh helpers\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph order graph contract cuttable uses\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph order graph contract nodes\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph order graph contract uses\s+(\d+)", 15)
test.execute()

test.passes()
