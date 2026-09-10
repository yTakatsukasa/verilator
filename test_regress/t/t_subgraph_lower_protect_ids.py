#!/usr/bin/env python3
# DESCRIPTION: Verilator: Protect identifiers in shared template code
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")
test.top_filename = "t/t_subgraph_template_lower.v"
test.compile(verilator_flags2=["--subgraph-schedule", "--protect-ids", "--stats", "--binary"])
test.execute()
test.file_grep(test.stats, r"Scheduling, Subgraph template schedules activated\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph template shared bodies materialized\s+(\d+)", 6)
for filename in test.glob_some(test.obj_dir + "/" + test.vm_prefix + "*.cpp"):
    test.file_grep_not(filename, r"sg_template_lower|__PVT__shadow|__PVT__mixed")
test.passes()
