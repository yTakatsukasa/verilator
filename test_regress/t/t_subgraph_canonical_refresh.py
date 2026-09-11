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
shared_bodies = 0
for filename in cpp_files:
    with open(filename, "r", encoding="utf8") as file_handle:
        shared_bodies += len(
            re.findall(r"(?m)^(?:VL_ATTR_COLD )?void .*__VsubgraphV3Ast\d+__\d+[^\n]*\{$",
                       file_handle.read()))
if shared_bodies != 4:
    test.error("Expected 4 emitted shared V3Ast bodies, got %d" % shared_bodies)

test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedule builds\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules activated\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast local triggers\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast phase entries\s+(\d+)", 5)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast shared bodies\s+(\d+)", 5)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast entry calls\s+(\d+)", 15)

test.passes()
