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

test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, artifact reuse shared skip triggered\s+([1-9]\d*)")
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, artifact reuse shared calls\s+(?:[5-9]|\d{2,})")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse scope clone calls\s+0")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse scope clones\s+0")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse shared clone avoids\s+\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse shared call permille\s+\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, bundle builds\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, bundle materialized\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, bundle materialized plans\s+([1-9]\d*)")
test.file_grep(
    test.stats,
    r"Scheduling, Subgraph nba, bundle materialized plans per bundle permille\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, bundle plans\s+([1-9]\d*)")
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, bundle plans per build permille\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, schedule plans\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, ordered function clones\s+0")
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, triggered artifact no nonlocal writes\s+([1-9]\d*)")
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, triggered artifact input tail writes\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, triggered artifact shareable\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, triggered ref state\s+([1-9]\d*)")

test.passes()
