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

  logic [44:0] q_a = 45'h1234_5678_6abc;
  logic [44:0] q_b = 45'h5678_6abc_1234;
  logic [44:0] q_c = 45'h6abc_1234_5678;

  assign y_a = q_a[14:0];
  assign y_b = q_b[14:0];
  assign y_c = q_c[14:0];

  always_ff @(posedge clk_a) q_a <= {q_a[43:0], q_a[44]} ^ {30'b0, q_a[0 +: 15]};
  always_ff @(posedge clk_b) q_b <= {q_b[43:0], q_b[44]} ^ {30'b0, q_b[15 +: 15]};
  always_ff @(posedge clk_c) q_c <= {q_c[43:0], q_c[44]} ^ {30'b0, q_c[30 +: 15]};

endmodule

module sg_dual_recipe_ref (
  input logic clk_a,
  input logic clk_b,
  input logic clk_c,
  output logic [14:0] y_a,
  output logic [14:0] y_b,
  output logic [14:0] y_c
);

  logic [44:0] q_a = 45'h1234_5678_6abc;
  logic [44:0] q_b = 45'h5678_6abc_1234;
  logic [44:0] q_c = 45'h6abc_1234_5678;

  assign y_a = q_a[14:0];
  assign y_b = q_b[14:0];
  assign y_c = q_c[14:0];

  always_ff @(posedge clk_a) q_a <= {q_a[43:0], q_a[44]} ^ {30'b0, q_a[0 +: 15]};
  always_ff @(posedge clk_b) q_b <= {q_b[43:0], q_b[44]} ^ {30'b0, q_b[15 +: 15]};
  always_ff @(posedge clk_c) q_c <= {q_c[43:0], q_c[44]} ^ {30'b0, q_c[30 +: 15]};

endmodule
