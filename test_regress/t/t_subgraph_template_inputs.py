#!/usr/bin/env python3
# DESCRIPTION: Verilator: Subgraph template instance connection semantics
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import json
import vltest_bootstrap

test.scenarios("vlt")

# Sharing counts are deliberately separate from this semantic baseline. Enable
# template-count assertions when the independent template path is introduced.
test.compile(verilator_flags2=["--subgraph-schedule", "--stats", "--binary", "--dump-tree-json"])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph NBA cross domain internal variables\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph template candidates\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph templates captured\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph templates rejected\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph template instance memberships\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph template instance bindings\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph template instance bindings rejected\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph template port bindings\s+(\d+)", 28)
test.file_grep(test.stats, r"Scheduling, Subgraph template open ports\s+(\d+)", 1)

template_file = test.obj_dir + "/" + test.vm_prefix + "__subgraph_templates.json"
test.files_identical(template_file,
                     test.obj_dir + "/" + test.vm_prefix + "__subgraph_templates_post_sched.json")
with open(template_file, encoding="utf8") as handle:
    checkpoint = json.load(handle)
module = checkpoint["templates"][0]
slots = {slot["name"]: index for index, slot in enumerate(module["slots"], 1)}
if set(slots) != {
        "clk_a", "clk_b", "reset", "enable", "data", "result", "extra", "a", "b", "shadow"
}:
    test.error("Template lost a port or internal state slot")
if any(instance["template"] != 1 for instance in checkpoint["instances"]):
    test.error("Different parent connections must use the same template")
constant_inputs = 0
open_outputs = 0
for instance in checkpoint["instances"]:
    if instance["bindingRejection"]:
        test.error("Scalar port binding unexpectedly rejected")
    bindings = {conn["formal"]: conn["actual"] for conn in instance["connections"]}
    if len(bindings) != 7 or set(bindings) != {
            slots[name]
            for name in ("clk_a", "clk_b", "reset", "enable", "data", "result", "extra")
    }:
        test.error("Formal storage slots must remain distinct across aliased inputs")
    actual = instance["nodes"][bindings[slots["data"]] - 1]
    if actual["kind"] == "CONST" and actual["value"] == "15'h127":
        constant_inputs += 1
    elif actual["kind"] != "VARREF":
        test.error("Unexpected data connection")
    if bindings[slots["extra"]] == 0:
        open_outputs += 1
if constant_inputs != 1 or open_outputs != 1:
    test.error("Instance binding lost constant input or disconnected output")
if not any(node["kind"] == "VARREF" and node["slot"] == slots["data"] and node["value"] == "RD"
           for node in module["nodes"]):
    test.error("Template specialized away the data input")
if any(node["kind"] == "CONST" and node["value"] == "15'h127" for node in module["nodes"]):
    test.error("Parent constant leaked into the template")
for node in module["nodes"]:
    if node["kind"] == "SENITEM" and node["value"] == "POS":
        trigger = module["nodes"][node["operands"][0] - 1]
        if trigger["kind"] != "VARREF" or trigger["slot"] not in (slots["clk_a"], slots["clk_b"]):
            test.error("Template trigger must reference a local clock slot")

test.passes()
