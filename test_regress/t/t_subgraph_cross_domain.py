#!/usr/bin/env python3
# DESCRIPTION: Verilator: Share subgraph schedules across exact parent domains
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

test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence classes\s+(\d+)", 2)
test.file_grep(test.stats,
               r"Scheduling, Subgraph schedule equivalence cross domain members\s+(\d+)", 2)
test.file_grep(test.stats,
               r"Scheduling, Subgraph schedule equivalence cross domain reuses\s+(\d+)", 2)
test.file_grep(test.stats,
               r"Scheduling, Subgraph schedule equivalence representative order calls\s+(\d+)",
               2)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence reuses\s+(\d+)", 2)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared exact parent domain wrapper bindings\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper artifacts\s+(\d+)", 2)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared order cache order calls avoided\s+(\d+)", 2)

test.passes()
