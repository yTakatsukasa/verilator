<!-- DESCRIPTION: Verilator: Subgraph scheduling experiment 3 measurements
     SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
     SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0 -->

# Subgraph scheduling experiment 3 results

Measured on 2026-09-24 with the `exp_8` experiment 2 implementation. The same
single-FF, 7-bit child circuit was instantiated 1, 2, 8, 32, 128, and 512
times. Each child received `cycles + i` on the same clock. The top combined
every child output into an ordered 32-bit hash, keeping the state observable.
Flat and shared runs produced identical hashes after two million cycles at
every scale.

The command below reproduces the measurements. Generated C++ compilation is
excluded from Verilation time and simulation is measured separately.
`--no-skip-identical` forces a complete conversion on each of the five
Verilation repetitions; the table reports their median. Simulation time is
the median of three runs. Peak RSS comes from `wait4` for each process. C++
bytes sum emitted `.cpp` files, excluding generated headers and the makefile.
The
[raw CSV](nodist/subgraph_scale_results.csv),
[measurement script](nodist/subgraph_scale_bench.py), and
[simulation driver](nodist/subgraph_scale_main.cpp) are included.

```sh
python3 nodist/subgraph_scale_bench.py --counts 1 2 8 32 128 512 --repeats 5 --sim-repeats 3 --cycles 2000000 --graphs --out /tmp/subgraph-exp3-final
```

| Instances | Verilation ms flat / shared | Peak RSS MiB flat / shared | C++ KiB flat / shared | Simulation s flat / shared |
|---:|---:|---:|---:|---:|
| 1 | 46.9 / 47.4 | 20.7 / 20.6 | 23.3 / 27.3 | 0.101 / 0.116 |
| 2 | 46.9 / 47.3 | 20.4 / 20.9 | 24.0 / 34.0 | 0.103 / 0.133 |
| 8 | 48.3 / 49.6 | 20.7 / 20.8 | 29.0 / 56.0 | 0.103 / 0.171 |
| 32 | 48.8 / 51.1 | 21.0 / 21.4 | 50.7 / 134.8 | 0.122 / 0.296 |
| 128 | 56.0 / 66.6 | 22.0 / 23.1 | 83.8 / 423.2 | 0.383 / 0.976 |
| 512 | 93.0 / 165.3 | 27.0 / 30.8 | 263.3 / 1614.1 | 1.192 / 3.714 |

The shared path kept two child procedures and 11 child procedure AST nodes at
Scope for every instance count. It emitted two shared evaluation functions,
totaling 1192 C++ bytes, for every count of at least two. Skipped Order calls
equaled `2 * (instances - 1)`. In contrast, each receiver retained five
boundary VarScopes. The parent NBA graph had 267 flat versus 558 shared
vertices at 32 instances, and 4623 versus 8720 at 512. At 512 instances,
there were 512 NBA shadow pairs, 512 publications, and 1024 child NBA actives.
The 1026 receiver wrapper functions alone occupied 818391 C++ bytes at 512
instances, while the shared evaluation body stayed at 1192 bytes. Total
generated C++ was about 6.1 times larger than flat, and simulation was
about 3.1 times slower. Flat inlines the child, so its `Scope, Subgraph boundary procedures` counter is zero; that counter alone does not mean flat
eliminated the child logic.

Early subgraph separation succeeded for one instance but fell back for every
instance when sharing was active. There were 512 fallbacks at 512 instances.
The reason reported with `--debugi-V3SchedSubgraph 4` was `child combinational logic`: the early path cannot yet isolate the child's combinational output
handling. The late Order evaluation body is shared, but per-instance input
capture, output publication, and wrapper functions remain in the parent
schedule. Total AST nodes after Scope were 23643 flat versus 25196 shared at
512 instances. This extra boundary work and metadata likely contribute to the
higher peak RSS.

**Conclusion:** Functional sharing held through 512 instances of this narrow
single-FF circuit. The original goal of reducing Verilation time, peak memory,
and generated C++ size was not met. A next implementation needs to isolate
child combinational handling earlier and reduce the parent graph and
per-instance wrappers. These measurements do not establish a performance
trend for general large designs.
