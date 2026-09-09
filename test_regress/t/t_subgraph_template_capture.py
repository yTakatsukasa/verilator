#!/usr/bin/env python3
# DESCRIPTION: Verilator: Capture specialized templates and reject incomplete bodies
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json
import vltest_bootstrap

test.scenarios("vlt")

test.compile(verilator_flags2=["--subgraph-schedule", "--stats", "--binary", "--dump-tree-json"])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph template candidates\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph templates captured\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph templates rejected\s+(\d+)", 2)
test.file_grep(test.stats, r"Scheduling, Subgraph template rejection, dtype\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph template rejection, statement CASE\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph template instance memberships\s+(\d+)", 3)

template_file = test.obj_dir + "/" + test.vm_prefix + "__subgraph_templates.json"
test.files_identical(template_file,
                     test.obj_dir + "/" + test.vm_prefix + "__subgraph_templates_post_sched.json")
with open(template_file, encoding="utf8") as handle:
    checkpoint = json.load(handle)
widths = sorted(
    next(slot["dtype"]["width"] for slot in module["slots"] if slot["name"] == "data")
    for module in checkpoint["templates"])
if widths != [7, 15]:
    test.error("Parameter specializations must have distinct typed templates")
members = sorted(
    sum(instance["template"] == index for instance in checkpoint["instances"])
    for index in range(1,
                       len(checkpoint["templates"]) + 1))
if members != [1, 2]:
    test.error("Expected one template per specialization, not per instance")

test.passes()
