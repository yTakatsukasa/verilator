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

test.file_grep(test.stats, r"Scheduling, Subgraph stl, artifact reuses\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph stl, artifacts\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph stl, groups\s+(\d+)", 5)
test.file_grep(test.stats, r"Scheduling, Subgraph stl, internal order aggregate calls\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph stl, order cache entries\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph stl, order cache misses\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph stl, parent consumed contract writes\s+(\d+)",
               5)
test.file_grep(test.stats, r"Scheduling, Subgraph stl, parent consumed subgraph vars\s+(\d+)", 5)
test.file_grep(test.stats, r"Scheduling, Subgraph stl, shared helper parameterizations\s+(\d+)",
               2)
test.file_grep(test.stats, r"Scheduling, Subgraph stl, shared helper stl argument skips\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph stl, tail wrappers\s+(\d+)", 5)

test.passes()
