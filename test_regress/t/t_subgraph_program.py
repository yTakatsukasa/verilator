#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json
import vltest_bootstrap

test.scenarios("vlt")

tree_filename = test.obj_dir + "/V" + test.name + "_010_linkdotparam.tree.json"
cpp_filename = test.obj_dir + "/V" + test.name + "___024root__0.cpp"
sub_cpp_filename = test.obj_dir + "/V" + test.name + "_sg_prog_dev__Pz1__0.cpp"

test.compile(verilator_flags2=["--stats --subgraph-schedule --dump-tree-json --no-json-edit-nums"])
test.execute()

test.file_grep(tree_filename, r'"name":"sg_prog_dev".*"subgraphBoundary":true')
test.file_grep(cpp_filename,
               r"Vt_subgraph_program_sg_prog_dev__Pz\d+___nba_sequent__TOP__t__DOT__i_dev0_sub__")
test.file_grep(sub_cpp_filename, r"___nba_sequent__TOP__t__DOT__i_dev0_sub__")
test.file_grep(test.stats, r"Scheduling, Subgraph NBA coarse nodes\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA groups\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph NBA refresh helpers\s+(\d+)", 2)
test.file_grep(test.stats,
               r"Scheduling, Subgraph order graph contract cuttable uses\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph order graph contract nodes\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph order graph contract uses\s+(\d+)", 65)

with open(tree_filename, "r", encoding="utf8") as fh:
    json.load(fh)

test.passes()
