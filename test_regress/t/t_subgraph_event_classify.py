#!/usr/bin/env python3
# DESCRIPTION: Verilator: Classify unsupported subgraph event expressions
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")

test.compile(
    verilator_flags2=["--no-skip-identical", "--subgraph-schedule", "--stats", "-Wno-fatal"],
    expect_filename=test.golden_filename)
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast candidates\s+(\d+)", 3)
test.file_grep(test.stats,
               r"Scheduling, Subgraph V3Ast schedule rejection, event condition\s+(\d+)", 1)
test.file_grep(test.stats,
               r"Scheduling, Subgraph V3Ast schedule rejection, event edge type BOTH\s+(\d+)", 1)
test.file_grep(
    test.stats,
    r"Scheduling, Subgraph V3Ast schedule rejection instances, event condition\s+(\d+)", 2)
test.file_grep(
    test.stats,
    r"Scheduling, Subgraph V3Ast schedule rejection instances, event edge type BOTH\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules activated\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instances activated\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast local triggers\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast shared bodies\s+(\d+)", 5)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast entry calls\s+(\d+)", 10)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules fallback\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instances fallback\s+(\d+)", 4)

test.passes()
