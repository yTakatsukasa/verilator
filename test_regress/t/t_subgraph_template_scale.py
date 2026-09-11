#!/usr/bin/env python3
# DESCRIPTION: Verilator: Shared V3Ast subgraph bodies do not scale with instance count
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import re
import shutil
import vltest_bootstrap

test.scenarios("vlt")
test.top_filename = "t/t_subgraph_template_inputs.v"

baseline_bodies = None
for count in (4, 12):
    test.compile(verilator_flags2=[
        "--subgraph-schedule", "--stats", "--binary", "-GN=" + str(count)
    ])
    test.execute()
    stats = test.obj_dir + "/scale_" + str(count) + ".stats"
    shutil.copyfile(test.stats, stats)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast instance memberships\s+(\d+)", count)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast schedule builds\s+(\d+)", 1)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast schedules activated\s+(\d+)", 1)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast shared bodies\s+(\d+)", 9)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast entry calls\s+(\d+)", 9 * count)
    bodies = []
    for filename in test.glob_some(test.obj_dir + "/" + test.vm_prefix + "*.cpp"):
        with open(filename, encoding="utf8") as handle:
            bodies.extend(re.findall(
                r"^(?:VL_ATTR_COLD )?void [^\n]*__VsubgraphV3Ast\d+__\d+"
                r"\([^\n]*\) \{\n.*?^\}", handle.read(), re.M | re.S))
    bodies.sort()
    if len(bodies) != 9:
        test.error("Expected exactly nine shared V3Ast function definitions")
    if baseline_bodies is not None and bodies != baseline_bodies:
        test.error("Instance count changed the shared V3Ast function bodies")
    baseline_bodies = bodies

test.passes()
