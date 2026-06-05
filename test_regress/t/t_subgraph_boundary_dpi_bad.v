// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

import "DPI-C" function int dpi_add1(input int value);

module t (
  input logic clk
);

  logic [31:0] out;

  bad_dpi i_bad (.clk(clk), .out(out));

endmodule

module bad_dpi (
  input logic clk,
  output logic [31:0] out
); /*verilator subgraph_boundary*/

  logic [31:0] q;

  always_ff @(posedge clk) q <= dpi_add1(q);
  assign out = q;

endmodule
