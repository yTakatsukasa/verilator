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

test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI calls\s+(\d+)", 9)
# Caller-context reuse is unsafe until equivalence after V3Order can be proven. Keep these
# wide, hierarchical, and call-containing instances ordered independently.
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper context artifacts\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper context reuses\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper skipped calls\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper skipped composite\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache order calls avoided\s+(\d+)",
               0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache order calls executed\s+(\d+)",
               6)

test.passes()
