// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

`ifdef USE_VLT
`define SUBGRAPH_BOUNDARY
`else
`define SUBGRAPH_BOUNDARY /*verilator subgraph_boundary*/
`endif

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0x exp=%0x (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`"); `stop; end while (0);
// verilog_format: on

module t (
  input logic clk
);

  int cyc;
  logic aux_clk = 1'b0;
  logic aux_clk_2 = 1'b0;
  logic [14:0] ref_a;
  logic [14:0] ref_b;
  logic [14:0] ref_c;
  logic [14:0] y_a;
  logic [14:0] y_b;
  logic [14:0] y_c;

  sg_dual_recipe i_sg (clk, aux_clk, aux_clk_2, y_a, y_b, y_c);
  sg_dual_recipe_ref i_ref (clk, aux_clk, aux_clk_2, ref_a, ref_b, ref_c);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    aux_clk <= ~aux_clk;
    aux_clk_2 <= ~aux_clk_2;
    if (cyc > 2) begin
      `checkh(y_a, ref_a);
      `checkh(y_b, ref_b);
      `checkh(y_c, ref_c);
    end
    if (cyc == 40) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_dual_recipe (
  input logic clk_a,
  input logic clk_b,
  input logic clk_c,
  output logic [14:0] y_a,
  output logic [14:0] y_b,
  output logic [14:0] y_c
); `SUBGRAPH_BOUNDARY

  logic [14:0] q_a = 15'h1234;
  logic [14:0] q_b = 15'h5678;
  logic [14:0] q_c = 15'h6abc;

  assign y_a = q_a;
  assign y_b = q_b;
  assign y_c = q_c;

  always_ff @(posedge clk_a) q_a <= {q_a[13:0], q_a[14]} + 15'h1021;
  always_ff @(posedge clk_b) q_b <= {q_b[13:0], q_b[14]} + 15'h2043;
  always_ff @(posedge clk_c) q_c <= {q_c[13:0], q_c[14]} + 15'h1021;

endmodule

module sg_dual_recipe_ref (
  input logic clk_a,
  input logic clk_b,
  input logic clk_c,
  output logic [14:0] y_a,
  output logic [14:0] y_b,
  output logic [14:0] y_c
);

  logic [14:0] q_a = 15'h1234;
  logic [14:0] q_b = 15'h5678;
  logic [14:0] q_c = 15'h6abc;

  assign y_a = q_a;
  assign y_b = q_b;
  assign y_c = q_c;

  always_ff @(posedge clk_a) q_a <= {q_a[13:0], q_a[14]} + 15'h1021;
  always_ff @(posedge clk_b) q_b <= {q_b[13:0], q_b[14]} + 15'h2043;
  always_ff @(posedge clk_c) q_c <= {q_c[13:0], q_c[14]} + 15'h1021;

endmodule
