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

common_flags = [
    "-Wno-ALWCOMBORDER",
    "-Wno-UNOPTFLAT",
]

test.compile(verilator_flags2=["--stats", "--subgraph-schedule"] + common_flags)
test.file_grep(
    test.stats,
    r"Scheduling, Subgraph order graph stl, contract uses soft unavailable\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph input refresh candidate targets\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph input refresh calls\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph input refresh inputs\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph input refresh instances\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph input refresh selected targets\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph input refresh scopes\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph input refresh unmatched instances\s+(\d+)", 0)
test.execute()

test.passes()
