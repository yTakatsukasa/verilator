#!/usr/bin/env python3
# DESCRIPTION: Verilator: Independent scheduling work is per specialization
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
test.top_filename = "t/t_subgraph_template_inputs.v"

baseline = None
baseline_bodies = None
for count in (4, 12):
    test.compile(verilator_flags2=[
        "--subgraph-schedule", "--stats", "--binary", "--dump-tree-json", f"-GN={count}"
    ])
    test.execute()
    # The harness caches file contents by path, so retain each compilation's statistics.
    stats = f"{test.obj_dir}/scale_{count}.stats"
    shutil.copyfile(test.stats, stats)
    test.file_grep(stats, r"Scheduling, Subgraph template instance memberships\s+(\d+)", count)
    test.file_grep(stats, r"Scheduling, Subgraph template schedule builds\s+(\d+)", 1)
    test.file_grep(stats, r"Scheduling, Subgraph template schedules built\s+(\d+)", 1)
    test.file_grep(stats, r"Scheduling, Subgraph template schedules activated\s+(\d+)", 1)
    test.file_grep(stats, r"Scheduling, Subgraph template shared bodies materialized\s+(\d+)", 9)
    test.file_grep(stats, r"Scheduling, Subgraph template entry calls materialized\s+(\d+)",
                   count * 9)
    test.file_grep(stats, r"Scheduling, Subgraph template schedules rejected\s+(\d+)", 0)
    test.file_grep(stats, r"Scheduling, Subgraph template local triggers\s+(\d+)", 2)
    test.file_grep(stats, r"Scheduling, Subgraph template NBA shadow slots\s+(\d+)", 3)
    test.file_grep(stats, r"Scheduling, Subgraph template ABI storage slots\s+(\d+)", 13)
    test.file_grep(stats, r"Scheduling, Subgraph template ABI entry points\s+(\d+)", 9)
    test.file_grep(stats, r"Scheduling, Subgraph template ABI instance bindings\s+(\d+)", count)
    test.file_grep(stats, r"Scheduling, Subgraph template local trigger bindings\s+(\d+)",
                   count * 2)
    with open(test.obj_dir + "/" + test.vm_prefix + "__subgraph_templates.json",
              encoding="utf8") as handle:
        templates = json.load(handle)["templates"]
    if baseline is not None and templates != baseline:
        test.error("Increasing instance count must not change the template or its schedule")
    baseline = templates
    bodies = []
    for filename in test.glob_some(test.obj_dir + "/" + test.vm_prefix + "*.cpp"):
        with open(filename, encoding="utf8") as handle:
            bodies.extend(
                re.findall(
                    r"^(?:VL_ATTR_COLD )?void [^\n]*__VsubgraphTemplate\d+__\d+"
                    r"\([^\n]*\) \{\n.*?^\}", handle.read(), re.M | re.S))
    bodies.sort()
    if len(bodies) != 9:
        test.error("Expected exactly nine shared C++ function definitions")
    if baseline_bodies is not None and bodies != baseline_bodies:
        test.error("Instance count must not change shared multi-clock C++ bodies")
    baseline_bodies = bodies
    test.file_grep(stats, r"Output, C\+\+ template body functions\s+(\d+)", 9)
    test.file_grep(stats, r"Output, C\+\+ template body bytes\s+(\d+)",
                   sum(len(body.encode("utf8")) + 1 for body in bodies))

test.passes()
