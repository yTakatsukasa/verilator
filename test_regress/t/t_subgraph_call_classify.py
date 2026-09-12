#!/usr/bin/env python3
# DESCRIPTION: Verilator: Classify calls in shared subgraph candidates
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

test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast candidates\s+(\d+)", 1)
test.file_grep(test.stats,
               r"Scheduling, Subgraph V3Ast schedule rejection, task call\s+(\d+)", 1)
test.file_grep(test.stats,
               r"Scheduling, Subgraph V3Ast schedule rejection instances, task call\s+(\d+)", 2)

for category in (
        "local automatic pure function",
        "local automatic pure task",
        "local static pure function",
):
    test.file_grep(test.stats,
                   r"Scheduling, Subgraph V3Ast call sites, " + category + r"\s+(\d+)", 1)
    test.file_grep(test.stats,
                   r"Scheduling, Subgraph V3Ast call templates, " + category + r"\s+(\d+)", 1)
    test.file_grep(test.stats,
                   r"Scheduling, Subgraph V3Ast call instances, " + category + r"\s+(\d+)", 2)

test.passes()
