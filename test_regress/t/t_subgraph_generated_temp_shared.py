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

test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse shared calls\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper call args\s+(\d+)", 12)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper call args max\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper external args\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper formal args max\s+(\d+)", 2)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, triggered artifact writes local temp\s+([1-9]\d*)")
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, triggered artifact writes vlem temp\s+([1-9]\d*)")

test.passes()
