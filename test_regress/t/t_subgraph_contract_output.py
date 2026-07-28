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

test.compile(verilator_flags2=["--stats", "--subgraph-schedule", "--trace"])
test.execute()

test.file_grep(
    test.stats,
    r"Scheduling, Subgraph order graph nba, contract uses force post\s+(\d+)", 4)
test.file_grep(
    test.stats,
    r"Scheduling, Subgraph nba, artifact reuse shared calls\s+([1-9]\d*)")

test.passes()
