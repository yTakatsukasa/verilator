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
shared_bodies = 0
for filename in cpp_files:
    with open(filename, "r", encoding="utf8") as file_handle:
        shared_bodies += len(
            re.findall(r"(?m)^void .*__VsubgraphV3Ast\d+__\d+[^\n]*\{$", file_handle.read()))
if shared_bodies != 3:
    test.error("Expected 3 shared V3Ast process bodies, got %d" % shared_bodies)

test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedule builds\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules activated\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instances activated\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instances fallback\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast phase entries\s+(\d+)", 12)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast shared bodies\s+(\d+)", 12)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast entry calls\s+(\d+)", 36)
test.file_grep(test.stats, r"Output, C\+\+ subgraph V3Ast shared body functions\s+(\d+)", 3)

test.passes()
