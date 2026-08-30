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

test.compile(verilator_flags2=["--dump-tree-json", "--stats", "--subgraph-schedule"])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph NBA coarse nodes\s+(\d+)", 5)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA contract boundary uses\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA contract external uses\s+(\d+)", 8)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA contract internal uses\s+(\d+)", 11)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA contracts\s+(\d+)", 5)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA groups\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA logical uses\s+(\d+)", 20)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA refresh helpers\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA snapshot instances\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA snapshot sources\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph order graph contract nodes\s+(\d+)", 7)
test.file_grep(test.stats, r"Scheduling, Subgraph order graph contract uses\s+(\d+)", 25)
test.file_grep(test.stats,
               r"Scheduling, Subgraph order graph contract cuttable uses\s+(\d+)", 3)

sched_tree = test.glob_one(test.obj_dir + "/*_sched.tree.json")
test.file_grep(
    sched_tree,
    r'"type":"SUBGRAPHINSTANCE".*"phase":"pre".*"logicalUses":4.*"materializedUses":5',
)
test.file_grep(
    sched_tree,
    r'"type":"SUBGRAPHINSTANCE".*"phase":"post".*"logicalUses":4.*"materializedUses":3',
)
test.file_grep(
    sched_tree,
    r'"type":"SUBGRAPHINSTANCE".*"phase":"refresh".*"logicalUses":4.*"materializedUses":3',
)

test.passes()
