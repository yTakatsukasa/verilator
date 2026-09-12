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

test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedule builds\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules built\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules activated\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules rejected\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instances activated\s+(\d+)", 9)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules fallback\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instances fallback\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instance memberships\s+(\d+)", 9)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast local triggers\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast phase entries\s+(\d+)", 10)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast shared bodies\s+(\d+)", 10)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast entry calls\s+(\d+)", 30)

test.passes()
