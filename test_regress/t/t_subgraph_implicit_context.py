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

test.compile(verilator_flags2=["--subgraph-schedule", "--stats"])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe replays\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe shared hits\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache shared skip constants\s+(\d+)",
               0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache shared skip var map\s+(\d+)", 0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, shared helper contract argument vars\s+(\d+)", 8)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, shared helper contract implicit context vars\s+(\d+)",
               0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper call args\s+(\d+)", 16)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper formal args after\s+(\d+)",
               8)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, shared helper implicit context vars\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper instance local args\s+(\d+)",
               4)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, triggered artifact unshareable\s+(\d+)",
               2)

test.passes()
