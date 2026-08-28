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

test.compile(verilator_flags2=[
    "--debug-subgraph-no-artifact-scope-clone",
    "--debug-subgraph-no-artifact-shared",
    "--debug-subgraph-no-order-cache-clone",
    "--stats",
    "--subgraph-schedule",
])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse scope clones\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, artifact reuse shared calls\s+(\d+)", 0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, diagnostic artifact scope clone groups\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, diagnostic artifact shared groups\s+(\d+)",
               6)
test.file_grep(test.stats,
               r"Scheduling, Subgraph nba, diagnostic order cache clone groups\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, internal order aggregate calls\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe clones\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph nba, order cache recipe replays\s+(\d+)", 0)

test.passes()
