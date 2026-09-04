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

test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI calls\s+(\d+)", 18)
# Caller-context reuse is only enabled after V3Order proves that the call targets are
# self-contained and the ordered bodies are equivalent. Wide state is passed explicitly.
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper arguments\s+(\d+)", 9)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper artifacts\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper call artifacts\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper call reuses\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper composite artifacts\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper composite reuses\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper context artifacts\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper context reuses\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper reuses\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper skipped calls\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper skipped composite\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache order calls avoided\s+(\d+)",
               5)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache order calls executed\s+(\d+)",
               7)

test.passes()
