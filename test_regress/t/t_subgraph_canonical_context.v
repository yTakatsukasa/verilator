// DESCRIPTION: Verilator: Canonical subgraph schedule uses the calling instance context
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
  logic [14:0] salt0 = 15'h013;
  logic [14:0] salt1 = 15'h127;
  logic [14:0] salt2 = 15'h31d;
  logic [14:0] y0;
  logic [14:0] y1;
  logic [14:0] y2;
  logic [14:0] ref0;
  logic [14:0] ref1;
  logic [14:0] ref2;

  sg_canonical_context i_sg0 (clk, salt0, y0);
  sg_canonical_context i_sg1 (clk, salt1, y1);
  sg_canonical_context i_sg2 (clk, salt2, y2);
  sg_canonical_context_ref i_ref0 (clk, salt0, ref0);
  sg_canonical_context_ref i_ref1 (clk, salt1, ref1);
  sg_canonical_context_ref i_ref2 (clk, salt2, ref2);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    salt0 <= salt0 + 15'h011;
    salt1 <= salt1 + 15'h023;
    salt2 <= salt2 + 15'h037;
    if (cyc > 3) begin
      `checkh(y0, ref0)
      `checkh(y1, ref1)
      `checkh(y2, ref2)
    end
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_canonical_context (
  input  logic        clk,
  input  logic [14:0] salt,
  output logic [14:0] y
); `SUBGRAPH_BOUNDARY

  sg_canonical_context_leaf i_leaf (clk, salt, y);

endmodule

module sg_canonical_context_ref (
  input  logic        clk,
  input  logic [14:0] salt,
  output logic [14:0] y
);

  sg_canonical_context_leaf i_leaf (clk, salt, y);

endmodule

module sg_canonical_context_leaf (
  input  logic        clk,
  input  logic [14:0] salt,
  output logic [14:0] y
);

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
    q0 <= q0 + q8 + salt;
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
