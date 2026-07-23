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

test.compile(verilator_flags2=["--output-split-cfuncs", "10", "--subgraph-schedule", "--stats"])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper formal args after\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper formal args before\s+(\d+)",
               392)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper constant args\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper call args\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper call args max\s+(\d+)", 0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, shared helper implicit context vars\s+(\d+)", 32)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper formal args max\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper instance local args\s+(\d+)",
               0)
test.file_grep(test.stats, r"Output, C\+\+ max file bytes\s+([1-9]\d*)")
test.file_grep(test.stats, r"Output, C\+\+ max function bytes\s+([1-9]\d*)")

test.passes()
