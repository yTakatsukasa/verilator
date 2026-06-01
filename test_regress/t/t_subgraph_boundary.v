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

module t (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
);

  sub i_sub0 (.clk(clk), .in(in), .out());
  sub i_sub1 (.clk(clk), .in(in + 8'd1), .out(out));

endmodule

module sub (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
); `SUBGRAPH_BOUNDARY

  logic [7:0] q;

  always_ff @(posedge clk) q <= in;
  assign out = q;

endmodule
