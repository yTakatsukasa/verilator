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

pos_h = test.obj_dir + "/" + test.vm_prefix + "_sg_node.h"
neg_h = test.obj_dir + "/" + test.vm_prefix + "_sg_node_neg.h"

test.compile(verilator_flags2=["--dump-tree-json", "--stats", "--subgraph-schedule"])
test.execute()

test.file_grep(pos_h, r"__VsubgraphPending\d+")
test.file_grep(neg_h, r"__VsubgraphPending\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedule builds\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast schedules activated\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast instance memberships\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast local triggers\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast phase entries\s+(\d+)", 8)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast shared bodies\s+(\d+)", 8)
test.file_grep(test.stats, r"Scheduling, Subgraph V3Ast entry calls\s+(\d+)", 16)

sched_tree = test.glob_one(test.obj_dir + "/*_sched.tree.json")
for instance in ("i_pos", "i_pos2", "i_neg", "i_neg2"):
    for phase in ("pre", "post", "refresh"):
        test.file_grep(
            sched_tree,
            r'"type":"SUBGRAPHINSTANCE".*"boundary":"TOP.__PVT__t__DOT__'
            + instance + r'".*"phase":"' + phase + r'"',
        )

test.passes()
