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
  logic aux_clk_a = 1'b0;
  logic aux_clk_b = 1'b0;
  logic [14:0] ref_a;
  logic [14:0] ref_b;
  logic [14:0] ref_c;
  logic [3:0] step_a;
  logic [3:0] step_b;
  logic [3:0] step_c;
  logic [14:0] y_a;
  logic [14:0] y_b;
  logic [14:0] y_c;

  assign step_a = cyc[3:0] ^ 4'd1;
  assign step_b = cyc[3:0] ^ 4'd5;
  assign step_c = cyc[3:0] ^ 4'd9;

  sg_implicit_context i_sg_a (clk, step_a, y_a);
  sg_implicit_context i_sg_b (aux_clk_a, step_b, y_b);
  sg_implicit_context i_sg_c (aux_clk_b, step_c, y_c);
  sg_implicit_context_ref i_ref_a (clk, step_a, ref_a);
  sg_implicit_context_ref i_ref_b (aux_clk_a, step_b, ref_b);
  sg_implicit_context_ref i_ref_c (aux_clk_b, step_c, ref_c);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    aux_clk_a <= ~aux_clk_a;
    if (cyc[1:0] == 2'b01) aux_clk_b <= ~aux_clk_b;
    if (cyc > 3) begin
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

module sg_implicit_context (
  input logic clk,
  input logic [3:0] step,
  output logic [14:0] y
); `SUBGRAPH_BOUNDARY

  logic [14:0] q = 15'h1234;

  assign y = q;

  always_ff @(posedge clk) q <= {q[13:0], q[14] ^ q[12]} + {11'b0, step};

endmodule

module sg_implicit_context_ref (
  input logic clk,
  input logic [3:0] step,
  output logic [14:0] y
);

  logic [14:0] q = 15'h1234;

  assign y = q;

  always_ff @(posedge clk) q <= {q[13:0], q[14] ^ q[12]} + {11'b0, step};

endmodule
