// DESCRIPTION: Verilator: Subgraph caller-context relative path sharing
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

`ifdef USE_VLT
`define SUBGRAPH_BOUNDARY
`else
`define SUBGRAPH_BOUNDARY /*verilator subgraph_boundary*/
`endif

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0x exp=%0x (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`" ); `stop; end while (0);
// verilog_format: on

module t (
  input logic clk
);

  int cyc = 0;
  logic slow_clk = 0;
  logic [14:0] y_fast;
  logic [14:0] y_slow;
  logic [14:0] ref_fast;
  logic [14:0] ref_slow;

  sg_context_path i_sg (clk, slow_clk, y_fast, y_slow);
  sg_context_path_ref i_ref (clk, slow_clk, ref_fast, ref_slow);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    slow_clk <= ~slow_clk;
    if (cyc > 3) begin
      `checkh(y_fast, ref_fast)
      `checkh(y_slow, ref_slow)
    end
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_context_path (
  input  logic        fast_clk,
  input  logic        slow_clk,
  output logic [14:0] y_fast,
  output logic [14:0] y_slow
); `SUBGRAPH_BOUNDARY

  // Same-shaped processes at distinct relative paths must not exchange state.
  sg_context_path_leaf i_fast (fast_clk, 15'h013, y_fast);
  sg_context_path_leaf i_slow (slow_clk, 15'h127, y_slow);

endmodule

module sg_context_path_ref (
  input  logic        fast_clk,
  input  logic        slow_clk,
  output logic [14:0] y_fast,
  output logic [14:0] y_slow
);

  sg_context_path_leaf i_fast (fast_clk, 15'h013, y_fast);
  sg_context_path_leaf i_slow (slow_clk, 15'h127, y_slow);

endmodule

module sg_context_path_leaf (
  input  logic        clk,
  input  logic [14:0] step,
  output logic [14:0] y
);

  // Keep enough state to exercise caller-context helper sharing.
  logic [14:0] q0 = 15'h001;
  logic [14:0] q1 = 15'h012;
  logic [14:0] q2 = 15'h123;
  logic [14:0] q3 = 15'h234;
  logic [14:0] q4 = 15'h345;
  logic [14:0] q5 = 15'h456;
  logic [14:0] q6 = 15'h567;
  logic [14:0] q7 = 15'h678;
  logic [14:0] q8 = 15'h789;

  always_ff @(posedge clk) begin
    q0 <= q0 + q8 + step;
    q1 <= q1 + q0 + 15'h011;
    q2 <= q2 + q1 + 15'h023;
    q3 <= q3 + q2 + 15'h037;
    q4 <= q4 + q3 + 15'h04d;
    q5 <= q5 + q4 + 15'h061;
    q6 <= q6 + q5 + 15'h077;
    q7 <= q7 + q6 + 15'h08b;
    q8 <= q8 + q7 + 15'h0a1;
  end

  always_comb y = q0 ^ q1 ^ q2 ^ q3 ^ q4 ^ q5 ^ q6 ^ q7 ^ q8;

endmodule
