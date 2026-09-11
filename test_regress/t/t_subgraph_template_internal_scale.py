#!/usr/bin/env python3
# DESCRIPTION: Verilator: Internal subgraph state does not expand the parent scheduling contract
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


def read_stat(filename, name):
    with open(filename, encoding="utf8") as handle:
        match = re.search(r"^\s*" + re.escape(name) + r"\s+(\d+)\s*$", handle.read(), re.M)
    if match is None:
        test.error("Missing statistic: " + name)
    return int(match.group(1))


baseline_parent = None
for label, define, context_variables in (("small", [], 20), ("large", ["+define+INTERNAL_LARGE"],
                                                             80)):
    test.compile(verilator_flags2=["--subgraph-schedule", "--stats", "--binary"] + define)
    test.execute()
    stats = test.obj_dir + "/" + label + ".stats"
    shutil.copyfile(test.stats, stats)

    test.file_grep(stats, r"Scheduling, Subgraph V3Ast schedules activated\s+(\d+)", 1)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast instances activated\s+(\d+)", 4)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast instances fallback\s+(\d+)", 0)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast shared bodies\s+(\d+)", 3)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast entry calls\s+(\d+)", 12)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast shared body arguments\s+(\d+)", 3)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast shared body max arguments\s+(\d+)", 2)
    test.file_grep(stats, r"Scheduling, Subgraph V3Ast shared body context variables\s+(\d+)",
                   context_variables)

    parent = tuple(
        read_stat(stats, name) for name in (
            "Scheduling, Subgraph NBA contract boundary uses",
            "Scheduling, Subgraph NBA contract external uses",
            "Scheduling, Subgraph NBA contract internal uses",
            "Scheduling, Subgraph NBA coarse nodes",
            "Scheduling, Subgraph NBA logical uses",
            "Scheduling, Subgraph order graph contract nodes",
            "Scheduling, Subgraph order graph contract uses",
        ))
    if baseline_parent is not None and parent != baseline_parent:
        test.error("Internal state changed the parent scheduling contract: %s != %s" %
                   (parent, baseline_parent))
    baseline_parent = parent

test.passes()
