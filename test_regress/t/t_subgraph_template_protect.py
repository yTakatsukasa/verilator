#!/usr/bin/env python3
# DESCRIPTION: Verilator: Protect identifiers in independent subgraph templates
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json
import vltest_bootstrap

test.scenarios("vlt")
test.top_filename = "t/t_subgraph_template_inputs.v"

test.compile(verilator_flags2=[
    "--subgraph-schedule", "--protect-ids", "--stats", "--binary", "--dump-tree-json"
])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph templates captured\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph template instance memberships\s+(\d+)", 4)
template_file = test.obj_dir + "/" + test.vm_prefix + "__subgraph_templates.json"
test.file_grep_not(template_file, r'"(?:sg_template_inputs|clk_a|clk_b|shadow)"')
test.files_identical(template_file,
                     test.obj_dir + "/" + test.vm_prefix + "__subgraph_templates_post_sched.json")
with open(template_file, encoding="utf8") as handle:
    json.load(handle)

test.passes()
