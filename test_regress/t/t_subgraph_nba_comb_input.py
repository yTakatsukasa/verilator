#!/usr/bin/env python3
# DESCRIPTION: Verilator: Subgraph combinational next-state input ordering
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")

test.compile(verilator_flags2=["--dump-tree-json", "--stats", "--subgraph-schedule"])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedule builds\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules activated\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instance memberships\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast local triggers\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast phase entries\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast shared bodies\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast entry calls\s+(\d+)", 6)

sched_tree = test.glob_one(test.obj_dir + "/*_sched.tree.json")
test.file_grep(
    sched_tree,
    r'"type":"SUBGRAPHINSTANCE".*"phase":"pre".*"logicalUses":4.*"materializedUses":3',
)
test.file_grep(
    sched_tree,
    r'"type":"SUBGRAPHINSTANCE".*"phase":"post".*"logicalUses":4.*"materializedUses":3',
)
test.file_grep(
    sched_tree,
    r'"type":"SUBGRAPHINSTANCE".*"phase":"refresh".*"logicalUses":4.*"materializedUses":4',
)

test.passes()
