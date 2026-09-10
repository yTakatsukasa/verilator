#!/usr/bin/env python3
# DESCRIPTION: Verilator: Subgraph template SystemVerilog type semantics
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")

test.compile(verilator_flags2=["--subgraph-schedule", "--stats", "--binary"])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast templates\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instance memberships\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast port bindings\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules built\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast local triggers\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast phase entries\s+(\d+)", 4)

test.passes()
