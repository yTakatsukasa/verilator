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

test.scenarios('vlt')

out_filename = test.obj_dir + "/V" + test.name + "_010_linkdotparam.tree.json"
cpp_filename = test.obj_dir + "/V" + test.name + "___024root__0.cpp"
sub_cpp_filename = test.obj_dir + "/V" + test.name + "_sub__0.cpp"

test.compile(verilator_flags2=["--subgraph-schedule --dump-tree-json --no-json-edit-nums"])
test.execute()

test.file_grep(out_filename, r'"name":"sub".*"subgraphBoundary":true')
test.file_grep(cpp_filename, r"Vt_subgraph_boundary_sub___nba_sequent__TOP__t__DOT__i_sub0__")
test.file_grep(cpp_filename, r"t__DOT__i_ref0__DOT__q")
test.file_grep(sub_cpp_filename, r"___nba_sequent__TOP__t__DOT__i_sub0__")

with open(out_filename, 'r', encoding="utf8") as fh:
    json.load(fh)

test.passes()
