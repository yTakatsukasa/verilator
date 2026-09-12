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
baseline_body_bytes = None
baseline_call_sites = None
baseline_call_bytes = None
for count in (4, 12):
    test.compile(
        verilator_flags2=["--subgraph-schedule", "--stats", "--binary", "-GN=" + str(count)])
    test.execute()
    stats = test.obj_dir + "/scale_" + str(count) + ".stats"
    shutil.copyfile(test.stats, stats)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast instance memberships\s+(\d+)", count)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast schedule builds\s+(\d+)", 1)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast schedules activated\s+(\d+)", 1)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast instances activated\s+(\d+)", count)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast schedules fallback\s+(\d+)", 0)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast instances fallback\s+(\d+)", 0)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast shared bodies\s+(\d+)", 6)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast entry calls\s+(\d+)", 6 * count)
    bodies = []
    for filename in test.glob_some(test.obj_dir + "/" + test.vm_prefix + "*.cpp"):
        with open(filename, encoding="utf8") as handle:
            bodies.extend(
                re.findall(
                    r"^(?:VL_ATTR_COLD )?void [^\n]*__VsubgraphV3Ast\d+__\d+"
                    r"\([^\n]*\) \{\n.*?^\}", handle.read(), re.M | re.S))
    bodies.sort()
    if len(bodies) != 5:
        test.error("Expected exactly five shared V3Ast function definitions")
    if baseline_bodies is not None and bodies != baseline_bodies:
        test.error("Instance count changed the shared V3Ast function bodies")
    baseline_bodies = bodies
    body_bytes = sum(len(body.encode("utf8")) + 1 for body in bodies)
    test.file_grep(stats, r"Output, C\+\+ subgraph V3Ast shared body functions\s+(\d+)", 5)
    test.file_grep(stats, r"Output, C\+\+ subgraph V3Ast shared body bytes\s+(\d+)", body_bytes)
    # Later optimization keeps five shared definitions and six marked call expressions per
    # instance.
    call_sites = 6 * count
    test.file_grep(stats, r"Output, C\+\+ subgraph V3Ast call sites\s+(\d+)", call_sites)
    with open(stats, encoding="utf8") as handle:
        call_bytes_match = re.search(r"Output, C\+\+ subgraph V3Ast call expression bytes\s+(\d+)",
                                     handle.read())
    if call_bytes_match is None:
        test.error("Missing emitted V3Ast call byte count")
    call_bytes = int(call_bytes_match.group(1))
    if baseline_body_bytes is not None and body_bytes != baseline_body_bytes:
        test.error("Instance count changed shared V3Ast body bytes")
    if baseline_call_sites is not None and call_sites <= baseline_call_sites:
        test.error("Instance count did not increase emitted V3Ast call sites")
    if baseline_call_bytes is not None and call_bytes <= baseline_call_bytes:
        test.error("Instance count did not increase emitted V3Ast call bytes")
    baseline_body_bytes = body_bytes
    baseline_call_sites = call_sites
    baseline_call_bytes = call_bytes

test.passes()
