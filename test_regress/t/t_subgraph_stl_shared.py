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

test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI analyses\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI DPI calls\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI eligible helpers\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI external vars\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI hidden uses\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI module-phase candidates\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI state vars\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper arguments\s+(\d+)", 12)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper artifacts\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper body mismatches\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper parameterizations\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper reuses\s+(\d+)", 9)

test.passes()
