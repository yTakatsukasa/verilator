#!/usr/bin/env python3
# DESCRIPTION: Verilator: Canonical subgraph module-local call sharing
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
call_bodies = 0
for filename in cpp_files:
    with open(filename, "r", encoding="utf8") as file_handle:
        contents = file_handle.read()
        canonical_bodies += len(re.findall(
            r"(?m)^void .*___nba_subgraph_(?:pre|post)_\d+_sequent[^\n]*\{$",
            contents))
        call_bodies += len(re.findall(r"(?m)^void .*::__VnoInFunc_mix_[^\n]*\{$", contents))
if canonical_bodies != 2:
    test.error("Expected 2 canonical C++ process bodies, got %d" % canonical_bodies)
if call_bodies != 1:
    test.error("Expected 1 canonical module-local call body, got %d" % call_bodies)

test.file_grep(test.stats, r"Scheduling, Subgraph NBA contracts\s+(\d+)", 6)
test.file_grep(test.stats,
               r"Scheduling, Subgraph NBA contract instance bindings\s+(\d+)", 6)
test.file_grep(test.stats,
               r"Scheduling, Subgraph NBA contract recipe fallbacks\s+(\d+)", 0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph NBA contract uses not expanded\s+(\d+)", 14)
test.file_grep(test.stats,
               r"Scheduling, Subgraph NBA contract metadata bytes avoided\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph canonical context artifacts\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical context reuses\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical order calls avoided\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper skipped calls\s+(\d+)", 0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared order cache order calls executed\s+(\d+)", 2)

test.passes()
