#!/usr/bin/env python3
# DESCRIPTION: Verilator: Execute shared independent schedules with instance-local state
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json
import re
import shutil
import vltest_bootstrap

test.scenarios("vlt")

baseline = None
baseline_bodies = None
for count in (4, 12):
    test.compile(verilator_flags2=[
        "--subgraph-schedule", "--stats", "--binary", "--dump-tree-json", "-GN=" + str(count)
    ])
    test.execute()
    stats = test.obj_dir + "/lower_" + str(count) + ".stats"
    shutil.copyfile(test.stats, stats)
    test.file_grep(stats, r"Scheduling, Subgraph template schedule builds\s+(\d+)", 1)
    test.file_grep(stats, r"Scheduling, Subgraph template schedules activated\s+(\d+)", 1)
    test.file_grep(stats, r"Scheduling, Subgraph template shared bodies materialized\s+(\d+)", 6)
    test.file_grep(stats, r"Scheduling, Subgraph template entry calls materialized\s+(\d+)",
                   6 * count)
    with open(test.obj_dir + "/" + test.vm_prefix + "__subgraph_templates.json",
              encoding="utf8") as handle:
        templates = json.load(handle)["templates"]
    if baseline is not None and templates != baseline:
        test.error("Instance count must not change the shared schedule")
    baseline = templates
    bodies = []
    for filename in test.glob_some(test.obj_dir + "/" + test.vm_prefix + "*.cpp"):
        with open(filename, encoding="utf8") as handle:
            bodies.extend(
                re.findall(
                    r"^(?:VL_ATTR_COLD )?void [^\n]*__VsubgraphTemplate\d+__\d+"
                    r"\([^\n]*\) \{\n.*?^\}", handle.read(), re.M | re.S))
    bodies.sort()
    if len(bodies) != 6:
        test.error("Expected exactly six shared C++ function definitions")
    if baseline_bodies is not None and bodies != baseline_bodies:
        test.error("Instance count must not change shared C++ bodies")
    baseline_bodies = bodies

test.passes()
