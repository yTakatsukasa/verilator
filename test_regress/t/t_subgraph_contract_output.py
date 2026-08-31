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

test.compile(verilator_flags2=["--stats", "--subgraph-schedule"])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph NBA coarse nodes\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA contract boundary uses\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA groups\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph order graph contract cuttable uses\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph order graph contract nodes\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph order graph contract uses\s+(\d+)", 18)

test.passes()
