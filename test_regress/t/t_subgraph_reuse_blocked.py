#!/usr/bin/env python3
# DESCRIPTION: Verilator: Report why canonical subgraph reuse is blocked
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
               r"Scheduling, Subgraph schedule equivalence unavailable artifact classes\s+(\d+)",
               3)
test.file_grep(test.stats,
               r"Scheduling, Subgraph schedule equivalence unavailable artifacts\s+(\d+)", 6)
test.file_grep(test.stats,
               r"Scheduling, Subgraph schedule reuse blocked classes, unsafe helper\s+(\d+)", 3)
test.file_grep(test.stats,
               r"Scheduling, Subgraph schedule reuse blocked instances, unsafe helper\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules activated\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instances activated\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules fallback\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instances fallback\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast phase entries\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast shared bodies\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast entry calls\s+(\d+)", 12)

test.passes()
