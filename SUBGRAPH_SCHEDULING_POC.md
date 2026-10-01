<!-- DESCRIPTION: Verilator: Experimental subgraph scheduling proposal
     SPDX-FileCopyrightText: 2026-2026 Wilson Snyder
     SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0 -->

# Experimental subgraph scheduling

This proof of concept explores reducing Verilation work for repeated module
instances. It preserves a selected module as a scheduling boundary, orders its
logic locally, and reuses eligible evaluation bodies across instances of the
same elaborated module. Each instance retains its own simulation state.

The intended benefit is less repeated procedure expansion and scheduling, with
less generated C++ for the child bodies. Per-instance state, port connections,
input acquisition, and calls remain necessary. This is an experimental,
opt-in facility, not a general replacement for the flat scheduler.

## Selecting a boundary

The implementation series introduces `--subgraph-schedule`, disabled by
default, and either of these module selectors:

```systemverilog
module child (...);
  /*verilator subgraph_boundary*/
  ...
endmodule
```

```text
`verilator_config
subgraph -module "child"
```

Selection preserves the module through inlining. Local scheduling and early
procedure sharing have separate eligibility checks. A selected instance that
cannot be scheduled locally remains in the ordinary parent scheduler;
`SUBGRAPHFALLBACK` identifies the first failed condition and its RTL location.

## Scheduling and state ownership

The implementation uses the existing AST, NBA lowering, region partitioning,
and Order machinery. It does not introduce a second RTL representation or a
separate simulation model.

At a clock event, input acquisition and next-state evaluation must precede
publication of new child outputs. Evaluation and commit/publication therefore
remain separate operations in the parent dependency graph. In particular,
two children connected by a registered feedback path must both observe the
old state on that edge, regardless of their call order.

Local combinational logic is also evaluated when inputs change. Output
combinational logic and publication run during initial settling and after
state commits, as required by the existing evaluation regions.

For sharing, one representative scope owns the procedure bodies. Receiver
scopes retain their own state and invoke the shared functions with their own
receiver pointers. Boundary contracts describe the effects visible to the
parent without exposing every internal state dependency. Optimizations must
preserve this receiver-relative interpretation until Descope converts it to
generated C++ calls.

## Scope of the proof of concept

Local scheduling starts with a single rising-edge clock and compatible child
logic. Multiple clocks, generated-clock dependencies, zero-delay scheduling,
unsupported timing controls, and combinational cycles can require fallback.
Eligibility is checked on the transformed AST, so the exact accepted shapes
are defined by the implementation and accompanying tests.

Early sharing has stricter requirements than local scheduling. It requires
compatible instances of the same elaborated specialization, supported local
procedures and input connections, and single-threaded compilation. Failure
to share does not itself imply failure to schedule locally.

The series preserves the source branch's accepted behavior, including its
later extensions for combinational writes, input aliases, and output
provenance. Tests cover both enabled and disabled operation, phase ordering,
fallback diagnostics, state independence, and generated-function sharing.

## Measurements and questions for discussion

The author reports benefits on a real design. Quantitative claims need the
design characteristics, Verilator revision, command-line options, compiler,
machine, and repeated measurements before they can be generalized. Useful
metrics are Verilation time, peak memory, generated C++ size, C++ compilation
time, and simulation throughput, with matching simulation results.

The older scale experiment in `exp_8` predates several eligibility extensions
and showed increased wrapper overhead. It is historical evidence, not a
measurement of this reorganized implementation.

Feedback is particularly useful on the boundary contract, preservation of
evaluation-region semantics, placement of eligibility checks, interaction
with existing optimizations, and the balance between shared bodies and
per-instance boundary work. Parallel local scheduling and broader language
support are outside this restructuring exercise.
