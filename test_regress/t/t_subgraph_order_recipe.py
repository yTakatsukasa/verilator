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

test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse scope clones\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse shared calls\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate calls\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate constants\s+(\d+)",
               4)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate groups\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate nodes\s+(\d+)", 34)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate refs\s+(\d+)", 12)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache direct index fallbacks\s+(\d+)",
               0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache direct index hits\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache direct index lookups\s+(\d+)",
               2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe clones\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe constant remaps\s+(\d+)",
               0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe hits\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe replays\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe shared hits\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache shared skip var map\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache shared skip arguments\s+(\d+)",
               0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, order cache shared skip call function\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache shared skip constants\s+(\d+)",
               0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache shared skip other\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, ordered function clones\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper constant args\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper remap variant builds\s+(\d+)",
               0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, shared helper remap variant candidate vars\s+(\d+)", 0)
test.file_grep(
    test.stats,
    r"Scheduling, Subgraph nba, shared helper remap variant candidate vars max\s+(\d+)", 0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, shared helper remap variant constant remaps\s+(\d+)", 0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, shared helper remap variant oversize skips\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, shared helper remap variant vars\s+(\d+)",
               0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, triggered artifact unshareable\s+(\d+)",
               2)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, time build contract sec\s+\d+\.\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, time clone ordered funcs sec\s+\d+\.\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, time discard logic sec\s+\d+\.\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, time make artifacts sec\s+\d+\.\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, time mark hidden uses sec\s+\d+\.\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, time parameterize helpers sec\s+\d+\.\d+")
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, time parameterize remap variants sec\s+\d+\.\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, time populate helper args sec\s+\d+\.\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, time recipe replay sec\s+\d+\.\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, time split ordered funcs sec\s+\d+\.\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph nba, time triggered analysis sec\s+\d+\.\d+")
test.file_grep(test.stats,
               r"Scheduling, Subgraph order graph nba, delayed shadow index vars\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph order graph nba, delayed shadow lookups\s+(\d+)",
               0)

test.passes()
