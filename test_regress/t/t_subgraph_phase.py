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

test.file_grep(test.stats, r'Scheduling, Subgraph NBA groups\s+(\d+)', 4)
test.file_grep(test.stats, r'Scheduling, Subgraph NBA internal actives\s+(\d+)', 8)

child_graphs = test.glob_some(test.obj_dir + "/*nba_subgraph_pre_*_orderg_pre.dot")
parent_graphs = test.glob_some(test.obj_dir + "/*nba_orderg_pre.dot")
if len(child_graphs) != 4:
    test.error("Expected four child Order graphs, got " + str(len(child_graphs)))
if len(parent_graphs) != 1:
    test.error("Expected one parent NBA Order graph, got " + str(len(parent_graphs)))
test.file_grep_any(child_graphs, r'__Vdly__state')
test.file_grep_count(parent_graphs[0], r'shape=doubleoctagon', 4)

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


for instance in ('i_serial0', 'i_serial1', 'i_ring_a', 'i_ring_b'):
    delayed_pord = find_node(instance, '__Vdly__state', 'PORD')
    delayed_value = find_node(instance, '__Vdly__state', None)
    state_post = find_node(instance, '__PVT__state', 'POST')
    state_value = find_node(instance, '__PVT__state', None)

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
