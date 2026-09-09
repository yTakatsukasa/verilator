#!/usr/bin/env python3
# DESCRIPTION: Verilator: Independent scheduling work is per specialization
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json
import shutil
import vltest_bootstrap

test.scenarios("vlt")
test.top_filename = "t/t_subgraph_template_inputs.v"

baseline = None
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
    test.file_grep(stats, r"Scheduling, Subgraph template schedules rejected\s+(\d+)", 0)
    test.file_grep(stats, r"Scheduling, Subgraph template local triggers\s+(\d+)", 2)
    test.file_grep(stats, r"Scheduling, Subgraph template NBA shadow slots\s+(\d+)", 3)
    with open(test.obj_dir + "/" + test.vm_prefix + "__subgraph_templates.json",
              encoding="utf8") as handle:
        templates = json.load(handle)["templates"]
    if baseline is not None and templates != baseline:
        test.error("Increasing instance count must not change the template or its schedule")
    baseline = templates

test.passes()
