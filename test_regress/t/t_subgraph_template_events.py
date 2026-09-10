#!/usr/bin/env python3
# DESCRIPTION: Verilator: Shared template events and asynchronous reset
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")
test.compile(verilator_flags2=["--subgraph-schedule", "--stats", "--binary", "--dump-tree-json"])
test.execute()
test.file_grep(test.stats, r"Scheduling, Subgraph template schedules activated\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph template schedule builds\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph template local triggers\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph template shared bodies materialized\s+(\d+)", 7)
test.file_grep(test.stats, r"Scheduling, Subgraph template entry calls materialized\s+(\d+)", 14)
test.passes()
