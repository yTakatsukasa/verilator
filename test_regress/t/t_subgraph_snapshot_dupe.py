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

root_h = test.obj_dir + "/" + test.vm_prefix + "___024root.h"

test.compile(verilator_flags2=["--dump-tree-json", "--stats", "--subgraph-schedule"])
test.execute()

test.file_grep(root_h, r"__VsubgraphSnapshot__TOP__t__DOT__i_pos__d\d+")
test.file_grep(root_h, r"__VsubgraphSnapshot__TOP__t__DOT__i_pos2__d\d+")
test.file_grep(root_h, r"__VsubgraphSnapshot__TOP__t__DOT__i_neg__d\d+")
test.file_grep(root_h, r"__VsubgraphSnapshot__TOP__t__DOT__i_neg2__d\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph NBA snapshot instances\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA snapshot sources\s+(\d+)", 12)

sched_tree = test.glob_one(test.obj_dir + "/*_sched.tree.json")
test.file_grep(
    sched_tree,
    r'"type":"SUBGRAPHINSTANCE".*"boundary":"TOP.__PVT__t__DOT__i_pos".*"phase":"snapshot"',
)
test.file_grep(
    sched_tree,
    r'"type":"SUBGRAPHINSTANCE".*"boundary":"TOP.__PVT__t__DOT__i_pos2".*"phase":"snapshot"',
)
test.file_grep(
    sched_tree,
    r'"type":"SUBGRAPHINSTANCE".*"boundary":"TOP.__PVT__t__DOT__i_neg".*"phase":"snapshot"',
)
test.file_grep(
    sched_tree,
    r'"type":"SUBGRAPHINSTANCE".*"boundary":"TOP.__PVT__t__DOT__i_neg2".*"phase":"snapshot"',
)

test.passes()
