// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

module t (
  input  logic clk,
  input  logic in,
  output logic out
);

  bad_concat_feedthrough i_bad (.clk(clk), .in(in), .out(out));

endmodule

module bad_concat_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic unused;

  bad_concat_child i_child (.clk(clk), .in(in), .out({unused, out}));

endmodule

module bad_concat_child (
  input  logic clk,
  input  logic in,
  output logic [1:0] out
);

  logic q;

  always_ff @(posedge clk) q <= in;
  assign out = {q, in};

endmodule
