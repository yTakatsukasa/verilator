// DESCRIPTION: Verilator: Share subgraph schedules across exact parent domains
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
  logic pos_clk;
  logic neg_clk;
  logic [14:0] salt_pos = 15'h013;
  logic [14:0] salt_neg = 15'h127;
  logic [14:0] y_pos;
  logic [14:0] y_neg;
  logic [14:0] ref_pos;
  logic [14:0] ref_neg;

  assign pos_clk = clk;
  assign neg_clk = ~clk;

  sg_cross_domain i_sg_pos (pos_clk, salt_pos, y_pos);
  sg_cross_domain i_sg_neg (neg_clk, salt_neg, y_neg);
  sg_cross_domain_ref i_ref_pos (pos_clk, salt_pos, ref_pos);
  sg_cross_domain_ref i_ref_neg (neg_clk, salt_neg, ref_neg);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    salt_pos <= salt_pos + 15'h011;
    salt_neg <= salt_neg + 15'h023;
    if (cyc > 3) begin
      `checkh(y_pos, ref_pos)
      `checkh(y_neg, ref_neg)
    end
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_cross_domain (
  input  logic        clk,
  input  logic [14:0] salt,
  output logic [14:0] y
); `SUBGRAPH_BOUNDARY

  logic [14:0] q0 = 15'h001;
  logic [14:0] q1 = 15'h123;
  logic [14:0] q2 = 15'h345;

  always_ff @(posedge clk) begin
    q0 <= q0 + q2 + salt;
    q1 <= q1 + q0 + 15'h037;
    q2 <= q2 + q1 + 15'h061;
  end

  always_comb y = q0 ^ q1 ^ q2;

endmodule

module sg_cross_domain_ref (
  input  logic        clk,
  input  logic [14:0] salt,
  output logic [14:0] y
);

  logic [14:0] q0 = 15'h001;
  logic [14:0] q1 = 15'h123;
  logic [14:0] q2 = 15'h345;

  always_ff @(posedge clk) begin
    q0 <= q0 + q2 + salt;
    q1 <= q1 + q0 + 15'h037;
    q2 <= q2 + q1 + 15'h061;
  end

  always_comb y = q0 ^ q1 ^ q2;

endmodule
