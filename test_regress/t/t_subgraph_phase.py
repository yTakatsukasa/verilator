#!/usr/bin/env python3
# DESCRIPTION: Verilator: Subgraph NBA capture, evaluate, and publish ordering
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import re

import vltest_bootstrap

test.scenarios('vlt')
test.pli_filename = "t/t_subgraph_phase.cpp"

test.compile(
    make_top_shell=False,
    make_main=False,
    verilator_flags2=[
        "--cc",
        "--exe",
        test.pli_filename,
        "--subgraph-schedule",
        "--stats",
        "--dumpi-graph 6",
    ])
test.execute()

test.file_grep(test.stats, r'Scheduling, Subgraph NBA groups\s+(\d+)', 6)
test.file_grep(test.stats, r'Scheduling, Subgraph NBA internal actives\s+(\d+)', 12)
test.file_grep(test.stats, r'Scheduling, Subgraph early candidates\s+(\d+)', 6)
test.file_grep(test.stats, r'Scheduling, Subgraph early groups\s+(\d+)', 5)
test.file_grep(test.stats, r'Scheduling, Subgraph early fallbacks\s+(\d+)', 1)
test.file_grep(test.stats, r'Scheduling, Subgraph early clocked actives\s+(\d+)', 10)

sched_graphs = test.glob_some(test.obj_dir + "/*_sched.dot")
if len(sched_graphs) != 6:
    test.error("Expected five child scheduler graphs and one parent graph, got "
               + str(len(sched_graphs)))

boundary_cases = {
    'i_direct': ('__Vdly__q', 'q'),
    'i_serial0': ('__Vdly__state', '__PVT__state'),
    'i_serial1': ('__Vdly__state', '__PVT__state'),
    'i_ring_a': ('__Vdly__state', '__PVT__state'),
    'i_ring_b': ('__Vdly__state', '__PVT__state'),
}
parent_sched = []
for filename in sched_graphs:
    with open(filename, 'r', encoding='utf8') as fh:
        contents = fh.read()
    if "__Vdly__parent_q" in contents:
        parent_sched.append(contents)
if len(parent_sched) != 1:
    test.error("Expected one parent scheduler graph, got " + str(len(parent_sched)))
else:
    parent_sched = parent_sched[0]
    partition_nodes = dict(re.findall(r'^\s*n(\d+)\s+\[fontsize=8 label="([^"]*)"',
                                   parent_sched, re.MULTILINE))
    partition_edges = set(re.findall(r'\bn(\d+) -> n(\d+)', parent_sched))
    clocks = [node for node, label in partition_nodes.items() if label == 'posedge clk']
    if len(clocks) != 1:
        test.error("Expected one parent clock event")
    for instance, variables in boundary_cases.items():
        boundary = [node for node, label in partition_nodes.items()
                    if label.startswith(r'SUBGRAPH\n') and label.endswith(instance)]
        if len(boundary) != 1:
            test.error("Expected one parent partition boundary for " + instance)
            continue
        for variable in variables:
            value = [node for node, label in partition_nodes.items()
                     if label.endswith(instance + '->' + variable)]
            if len(value) != 1 or (boundary[0], value[0]) not in partition_edges:
                test.error("Missing parent boundary write for " + instance + " " + variable)
            if variable.startswith('__Vdly__') and len(value) == 1:
                writers = [source for source, target in partition_edges if target == value[0]]
                if writers != boundary:
                    test.error("Eligible child procedure leaked into parent scheduler: " + instance)
        if len(clocks) == 1 and (clocks[0], boundary[0]) not in partition_edges:
            test.error("Missing parent boundary clock for " + instance)
    if "i_fallback->__Vdly__state" not in parent_sched:
        test.error("Ineligible child procedure did not remain on the fallback path")

child_graphs = test.glob_some(test.obj_dir + "/*nba_subgraph_pre_*_orderg_pre.dot")
parent_graphs = test.glob_some(test.obj_dir + "/*nba_orderg_pre.dot")
if len(child_graphs) != 6:
    test.error("Expected six child Order graphs, got " + str(len(child_graphs)))
if len(parent_graphs) != 1:
    test.error("Expected one parent NBA Order graph, got " + str(len(parent_graphs)))
test.file_grep_any(child_graphs, r'__Vdly__state')
test.file_grep_any(child_graphs, r'__Vdly__q')
test.file_grep_count(parent_graphs[0], r'shape=doubleoctagon', 6)

with open(parent_graphs[0], 'r', encoding='utf8') as fh:
    graph = fh.read()

nodes = dict(re.findall(r'^\s*n(\d+)\s+\[.*label="(.*?)", color=', graph, re.MULTILINE))
edges = set(re.findall(r'^\s*n(\d+) -> n(\d+)', graph, re.MULTILINE))


def find_node(instance, variable, marker):
    matches = []
    for node, label in nodes.items():
        if instance + "->" + variable not in label:
            continue
        prefix = label.split(r'\n', 1)[0]
        if marker is None:
            if all(word not in prefix for word in ('PRE', 'POST', 'PORD')):
                matches.append(node)
        elif marker in prefix:
            matches.append(node)
    if len(matches) != 1:
        test.error("Expected one {} {} node, got {}".format(instance, variable, matches))
    return matches[0]


for instance, (delayed_name, state_name) in boundary_cases.items():
    delayed_pord = find_node(instance, delayed_name, 'PORD')
    delayed_value = find_node(instance, delayed_name, None)
    state_post = find_node(instance, state_name, 'POST')
    state_value = find_node(instance, state_name, None)

    evaluate = [target for source, target in edges
                if source == delayed_pord and 'ACTIVE' in nodes.get(target, '')]
    publish = [target for source, target in edges
               if source == state_post and 'ALWAYSPOST' in nodes.get(target, '')]
    if len(evaluate) != 1 or len(publish) != 1:
        test.error("Missing coarse evaluate/publish nodes for " + instance)
    required_edges = {
        (evaluate[0], delayed_value),
        (delayed_value, publish[0]),
        (publish[0], state_value),
    }
    missing = required_edges - edges
    if missing:
        test.error("Missing capture/evaluate/publish dependency for {}: {}".format(
            instance, sorted(missing)))

test.passes()
