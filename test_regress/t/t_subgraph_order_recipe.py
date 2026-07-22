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

test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse scope clones\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse shared calls\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate calls\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate constants\s+(\d+)",
               3)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate groups\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate nodes\s+(\d+)", 23)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate refs\s+(\d+)", 7)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe clones\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe constant remaps\s+(\d+)",
               0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe hits\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe shared hits\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache shared skip arguments\s+(\d+)",
               0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, order cache shared skip call function\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache shared skip constants\s+(\d+)",
               0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache shared skip other\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache shared skip var map\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, ordered function clones\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper constant args\s+(\d+)", 1)

test.passes()
