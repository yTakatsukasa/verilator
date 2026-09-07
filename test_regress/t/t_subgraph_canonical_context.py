#!/usr/bin/env python3
# DESCRIPTION: Verilator: Canonical subgraph schedule uses the calling instance context
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")

test.compile(verilator_flags2=["--subgraph-schedule", "--stats"])

cpp_files = [
    filename for filename in test.glob_some(test.obj_dir + "/" + test.vm_prefix + "*.cpp")
    if not re.search(r"__(ALL|main)\.cpp$", filename)
]
canonical_bodies = 0
for filename in cpp_files:
    with open(filename, "r", encoding="utf8") as file_handle:
        canonical_bodies += len(
            re.findall(r"(?m)^void .*___nba_subgraph_(?:pre|post)_\d+_sequent[^\n]*\{$",
                       file_handle.read()))
if canonical_bodies != 2:
    test.error("Expected 2 canonical C++ process bodies, got %d" % canonical_bodies)

test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph canonical context artifacts\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical C\+\+ bodies\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical context reuses\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical order calls avoided\s+(\d+)", 4)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared exact parent domain reused body wrappers\s+(\d+)", 4)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared exact parent domain wrapper bindings\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence classes\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence binding rejects\s+(\d+)", 0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph schedule equivalence fallback order calls\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence instances\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence max class size\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence potential reuses\s+(\d+)",
               4)
test.file_grep(test.stats,
               r"Scheduling, Subgraph schedule equivalence representative order calls\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence reuses\s+(\d+)", 4)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared logic instance binding checks\s+(\d+)", 4)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared logic instance binding matches\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph shared logic signature builds\s+(\d+)", 2)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared logic signature builds avoided\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph shared logic template analyses\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache order calls executed\s+(\d+)",
               2)

test.passes()
