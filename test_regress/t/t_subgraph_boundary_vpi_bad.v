// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

module t (
  input logic clk,
  input logic in,
  output logic out
);

  bad_vpi i_bad (.clk(clk), .in(in), .out(out));

endmodule

module bad_vpi (
  input logic clk,
  input logic in,
  output logic out
); /*verilator subgraph_boundary*/

  always_ff @(posedge clk) out <= in;

endmodule
