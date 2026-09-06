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

test.file_grep(test.stats, r"Scheduling, Subgraph NBA contracts\s+(\d+)", 19)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA coarse nodes\s+(\d+)", 19)
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI analyses\s+(\d+)", 19)
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI hidden uses\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical context artifacts\s+(\d+)", 8)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical context reuses\s+(\d+)", 11)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical order calls avoided\s+(\d+)", 11)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence classes\s+(\d+)", 8)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence binding rejects\s+(\d+)",
               0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph schedule equivalence fallback order calls\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence instances\s+(\d+)", 19)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence max class size\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence potential reuses\s+(\d+)",
               11)
test.file_grep(test.stats,
               r"Scheduling, Subgraph schedule equivalence representative order calls\s+(\d+)",
               8)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence reuses\s+(\d+)", 11)
test.file_grep(test.stats,
               r"Scheduling, Subgraph canonical contract local trigger uses\s+(\d+)", 7)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper artifacts\s+(\d+)", 8)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper body mismatches\s+(\d+)", 0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared helper isolation global trigger refs\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper reuses\s+(\d+)", 11)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared exact parent domain reused body wrappers\s+(\d+)",
               11)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared exact parent domain wrapper bindings\s+(\d+)", 19)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper skipped oversized\s+(\d+)", 0)
# Trigger guards stay in per-instance wrappers, not in shared process helpers.
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper skipped triggered\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared local trigger IDs\s+(\d+)", 8)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared local trigger instance bindings\s+(\d+)", 19)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared order cache hash candidates\s+(\d+)", 11)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared order cache hash collisions\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache hash lookups\s+(\d+)", 19)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared order cache hash max candidates\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache hash misses\s+(\d+)", 8)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache logic matches\s+(\d+)", 11)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache logic mismatches\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache lookups\s+(\d+)", 11)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache order calls avoided\s+(\d+)",
               11)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache order calls executed\s+(\d+)",
               8)

test.passes()
