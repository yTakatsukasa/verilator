// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

module t (
  input logic clk
);

  logic [7:0] out;
  logic [7:0] tap;

  bad_hier i_bad (.clk(clk), .out(out));

  always_ff @(posedge clk) tap <= i_bad.q;

endmodule

module bad_hier (
  input logic clk,
  output logic [7:0] out
); /*verilator subgraph_boundary*/

  logic [7:0] q;

  always_ff @(posedge clk) q <= q + 8'd1;
  assign out = q;

endmodule
