// DESCRIPTION: Verilator: Verilog Test module with a cutable subgraph scheduling cycle
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
  input logic clk,
  output logic sink
);

  int cyc;
  logic [6:0] aux0;
  logic [6:0] aux_ref0;
  logic [14:0] out0;
  logic [14:0] ref0;

  sg_circular i_sg0 (clk, out0, out0, aux0);
  sg_circular_ref i_ref0 (clk, ref0, ref0, aux_ref0);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    if (cyc > 2) begin
      `checkh(aux0, aux_ref0);
      `checkh(out0, ref0);
    end
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

  assign sink = ^{aux0, aux_ref0, out0, ref0};

endmodule

module sg_circular #(
  parameter logic [44:0] RESET_VALUE = 45'h1234_5678_0abc
) (
  input logic clk,
  input logic [14:0] in,
  output logic [14:0] out,
  output logic [6:0] aux_out
); `SUBGRAPH_BOUNDARY

  logic [6:0] aux_q = 7'h35;
  logic [44:0] child_out;
  logic [44:0] next_q;
  logic [44:0] q = RESET_VALUE;

  sg_circular_leaf i_leaf (q, child_out);

  always @(aux_q or clk) aux_out = {aux_q[3:0], aux_q[6:4]} ^ 7'h19;
  always_comb next_q = ({q[28:0], q[44:29]} ^ {30'b0, in}) | {next_q[43:0], 1'b0};
  always_ff @(posedge clk) begin
    aux_q <= {aux_q[4:0], aux_q[6:5]} ^ 7'h2d;
    q <= next_q;
  end
  assign out = child_out[14:0] ^ child_out[44:30];

endmodule

module sg_circular_ref #(
  parameter logic [44:0] RESET_VALUE = 45'h1234_5678_0abc
) (
  input logic clk,
  input logic [14:0] in,
  output logic [14:0] out,
  output logic [6:0] aux_out
);

  logic [6:0] aux_q = 7'h35;
  logic [44:0] child_out;
  logic [44:0] next_q;
  logic [44:0] q = RESET_VALUE;

  sg_circular_leaf i_leaf (q, child_out);

  always @(aux_q or clk) aux_out = {aux_q[3:0], aux_q[6:4]} ^ 7'h19;
  always_comb next_q = ({q[28:0], q[44:29]} ^ {30'b0, in}) | {next_q[43:0], 1'b0};
  always_ff @(posedge clk) begin
    aux_q <= {aux_q[4:0], aux_q[6:5]} ^ 7'h2d;
    q <= next_q;
  end
  assign out = child_out[14:0] ^ child_out[44:30];

endmodule

module sg_circular_leaf (
  input logic [44:0] in,
  output logic [44:0] out
);

  assign out = {in[12:0], in[44:13]} ^ 45'h0555_1234_0abc;

endmodule
