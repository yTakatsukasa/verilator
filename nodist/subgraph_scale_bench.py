#!/usr/bin/env python3
# DESCRIPTION: Verilator: Compare flat and shared subgraph scale
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import argparse
import csv
import os
import re
import statistics
import subprocess
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'test_regress/t/t_subgraph_shared_scale.v'
DRIVER = ROOT / 'nodist/subgraph_scale_main.cpp'
VERILATOR = ROOT / 'bin/verilator'


def timed(command, directory, name):
    metrics = directory / (name + '.time')
    log = directory / (name + '.log')
    start = time.perf_counter()
    with log.open('w', encoding='utf8') as stream:
        proc = subprocess.Popen(command, cwd=ROOT,
                                env={**os.environ, 'VERILATOR_ROOT': str(ROOT)},
                                stdout=stream, stderr=subprocess.STDOUT)
        _, status, usage = os.wait4(proc.pid, 0)
        proc.returncode = os.waitstatus_to_exitcode(status)
    wall = time.perf_counter() - start
    rss = usage.ru_maxrss
    metrics.write_text(f'{wall} {rss}\n', encoding='utf8')
    if proc.returncode:
        raise RuntimeError(f'{name} failed; see {directory / (name + ".log")}')
    return wall, rss, log.read_text(encoding='utf8')


def stat(contents, name):
    match = re.search(r'^\s*' + re.escape(name) + r'\s+([\d.]+)\s*$',
                      contents, re.MULTILINE)
    return float(match.group(1)) if match else 0


def stage_time(contents, stage):
    match = re.search(r'^\s*Stage, Elapsed time \(sec\), \d+_' + re.escape(stage)
                      + r'\s+([\d.]+)\s*$', contents, re.MULTILINE)
    return float(match.group(1)) if match else 0


def graph_size(directory, command):
    graph_dir = directory / 'graphs'
    graph_dir.mkdir(exist_ok=True)
    graph_command = command.copy()
    graph_command[graph_command.index('--Mdir') + 1] = str(graph_dir)
    graph_command.insert(-1, '--dumpi-graph')
    graph_command.insert(-1, '6')
    timed(graph_command, graph_dir, 'inspect')
    graph_files = list(graph_dir.glob('*nba_orderg_pre.dot'))
    if len(graph_files) != 1:
        raise RuntimeError(f'expected one parent NBA graph in {graph_dir}')
    graph = graph_files[0].read_text(encoding='utf8')
    vertices = len(re.findall(r'^\s*n\d+\s+\[', graph, re.MULTILINE))
    edges = len(re.findall(r'^\s*n\d+ -> n\d+', graph, re.MULTILINE))
    return vertices, edges


def measure(count, shared, args):
    label = f'{"shared" if shared else "flat"}_{count}'
    directory = args.out / label
    directory.mkdir(parents=True, exist_ok=True)
    cmd = [str(VERILATOR), '--cc', '--exe', str(DRIVER), '--top-module', 't',
           '--prefix', 'Vscale', '--Mdir', str(directory), '--stats', '--no-skip-identical',
           '-GN=' + str(count), '-GBENCH=1', str(SOURCE)]
    if shared:
        cmd.insert(-1, '--subgraph-schedule')
    times = []
    rss_values = []
    for repeat in range(args.repeats):
        wall, rss, _ = timed(cmd, directory, f'verilate_{repeat}')
        times.append(wall)
        rss_values.append(rss)
    stats = (directory / 'Vscale__stats.txt').read_text(encoding='utf8')
    cpp_files = [file for file in directory.glob('*.cpp') if '__ALL' not in file.name]
    cpp_bytes = sum(file.stat().st_size for file in cpp_files)
    child_cpp_bytes = sum(file.stat().st_size for file in cpp_files
                          if file.name.startswith('Vscale_sg_shared_scale'))
    shared_bodies = []
    receiver_wrappers = []
    for file in cpp_files:
        contents = file.read_text(encoding='utf8')
        shared_bodies.extend(re.findall(
            r'^void Vscale_sg_shared_scale___eval_body__nba_subgraph_[^{]+\{.*?^\}',
            contents, re.MULTILINE | re.DOTALL))
        receiver_wrappers.extend(re.findall(
            r'^void Vscale_sg_shared_scale___(?:ico|nba)_sequent__TOP[^{]+\{.*?^\}',
            contents, re.MULTILINE | re.DOTALL))
    shared_body_bytes = sum(len(body.encode('utf8')) for body in shared_bodies)
    wrapper_bytes = sum(len(body.encode('utf8')) for body in receiver_wrappers)
    build = subprocess.run(['make', '-C', str(directory), '-f', 'Vscale.mk', '-j4'],
                           cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, check=False)
    (directory / 'build.log').write_text(build.stdout, encoding='utf8')
    if build.returncode:
        raise RuntimeError(f'build failed; see {directory / "build.log"}')
    sim_times = []
    sim_rss = []
    state_bytes = None
    checksum = None
    for repeat in range(args.sim_repeats):
        wall, rss, output = timed([str(directory / 'Vscale'), str(args.cycles)],
                                  directory, f'sim_{repeat}')
        sim_times.append(wall)
        sim_rss.append(rss)
        match = re.search(r'state_bytes=(\d+)', output)
        if not match:
            raise RuntimeError(f'missing state size in {label}')
        state_bytes = int(match.group(1))
        match = re.search(r'checksum=(\d+)', output)
        if not match:
            raise RuntimeError(f'missing checksum in {label}')
        current_checksum = int(match.group(1))
        if checksum is not None and checksum != current_checksum:
            raise RuntimeError(f'inconsistent simulation output in {label}')
        checksum = current_checksum
    vertices, edges = graph_size(directory, cmd) if args.graphs else (None, None)
    return {
        'mode': 'shared' if shared else 'flat',
        'instances': count,
        'verilation_wall_s_median': statistics.median(times),
        'verilation_rss_kb_median': statistics.median(rss_values),
        'sim_wall_s_median': statistics.median(sim_times),
        'sim_rss_kb_median': statistics.median(sim_rss),
        'state_bytes': state_bytes,
        'sim_checksum': checksum,
        'cpp_bytes': cpp_bytes,
        'child_cpp_bytes': child_cpp_bytes,
        'shared_eval_functions': len(shared_bodies),
        'shared_eval_cpp_bytes': shared_body_bytes,
        'receiver_wrapper_functions': len(receiver_wrappers),
        'receiver_wrapper_cpp_bytes': wrapper_bytes,
        'other_cpp_bytes': cpp_bytes - shared_body_bytes,
        'scope_shared_procedures': int(stat(stats, 'Scope, Subgraph shared procedures')),
        'scope_receiver_varscopes': int(stat(stats, 'Scope, Subgraph receiver VarScopes')),
        'scope_ast_nodes': int(stat(stats, 'Scope, AST nodes')),
        'scope_boundary_scopes': int(stat(stats, 'Scope, Subgraph boundary scopes')),
        'scope_boundary_varscopes': int(stat(stats, 'Scope, Subgraph boundary VarScopes')),
        'scope_boundary_procedures': int(stat(stats, 'Scope, Subgraph boundary procedures')),
        'scope_boundary_procedure_nodes': int(
            stat(stats, 'Scope, Subgraph boundary procedure AST nodes')),
        'sched_shared_order_skips': int(stat(stats, 'Scheduling, Subgraph shared Order skips')),
        'sched_nba_groups': int(stat(stats, 'Scheduling, Subgraph NBA groups')),
        'sched_early_groups': int(stat(stats, 'Scheduling, Subgraph early groups')),
        'sched_early_fallbacks': int(stat(stats, 'Scheduling, Subgraph early fallbacks')),
        'sched_nba_internal_actives': int(
            stat(stats, 'Scheduling, Subgraph NBA internal actives')),
        'boundary_nba_shadow_pairs': int(stat(stats, 'Subgraph boundary, NBA shadow pairs')),
        'boundary_nba_publications': int(stat(stats, 'Subgraph boundary, NBA publications')),
        'scope_time_s': stage_time(stats, 'scope'),
        'delayed_time_s': stage_time(stats, 'delayed'),
        'active_top_time_s': stage_time(stats, 'activetop'),
        'sched_order_time_s': stage_time(stats, 'sched-create-nba'),
        'parent_nba_vertices': vertices,
        'parent_nba_edges': edges,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=Path('/tmp/subgraph-exp3'))
    parser.add_argument('--counts', type=int, nargs='+', default=[1, 2, 8, 32])
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--sim-repeats', type=int, default=3)
    parser.add_argument('--cycles', type=int, default=1000000)
    parser.add_argument('--graphs', action='store_true')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    for count in args.counts:
        for shared in (False, True):
            row = measure(count, shared, args)
            if shared and row['sim_checksum'] != rows[-1]['sim_checksum']:
                raise RuntimeError(f'flat/shared simulation mismatch at {count} instances')
            rows.append(row)
            print(row, flush=True)
    with (args.out / 'results.csv').open('w', newline='', encoding='utf8') as stream:
        stream.write('# SPDX-FileCopyrightText: 2026-2026 Wilson Snyder\n')
        stream.write('# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0\n')
        writer = csv.DictWriter(stream, fieldnames=rows[0].keys(), lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)


if __name__ == '__main__':
    main()
