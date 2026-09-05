#!/usr/bin/env python3
# DESCRIPTION: Verilator: Canonical subgraph refresh schedule sharing
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

cpp_files = [
    filename for filename in test.glob_some(test.obj_dir + "/" + test.vm_prefix + "*.cpp")
    if not re.search(r"__(ALL|main)\.cpp$", filename)
]
canonical_bodies = 0
for filename in cpp_files:
    with open(filename, "r", encoding="utf8") as file_handle:
        canonical_bodies += len(re.findall(
            r"(?m)^void .*___nba_subgraph_(?:pre|post|refresh)_\d+_sequent[^\n]*\{$",
            file_handle.read()))
if canonical_bodies != 3:
    test.error("Expected 3 canonical C++ process bodies, got %d" % canonical_bodies)

test.file_grep(test.stats, r"Scheduling, Subgraph NBA contracts\s+(\d+)", 9)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA refresh helpers\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical context artifacts\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical context reuses\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical order calls avoided\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper arguments\s+(\d+)", 1)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared order cache order calls executed\s+(\d+)", 3)

test.passes()
