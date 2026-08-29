#!/usr/bin/env python3
# DESCRIPTION: Verilator: Subgraph combinational next-state input ordering
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")

test.compile(verilator_flags2=["--stats", "--subgraph-schedule"])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph NBA contract boundary uses\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA contract external uses\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA contract internal uses\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA contracts\s+(\d+)", 2)

test.passes()
