// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

module t (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
);

  logic [7:0] feedthrough_out;
  logic [7:0] grandchild_out;
  logic [7:0] nested_out;

  bad_feedthrough i_feedthrough (.clk(clk), .in(in), .out(feedthrough_out));
  bad_nested_grandparent i_grandchild (.clk(clk), .in(in), .out(grandchild_out));
  bad_nested_parent i_nested (.clk(clk), .in(in), .out(nested_out));

  assign out = feedthrough_out ^ grandchild_out ^ nested_out;

endmodule

module bad_feedthrough (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
); /*verilator subgraph_boundary*/

  logic [7:0] q;

  always_ff @(posedge clk) q <= in;
  assign out = q ^ in;

endmodule

module bad_nested_grandparent (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
); /*verilator subgraph_boundary*/

  bad_nested_middle i_middle (.clk(clk), .in(in), .out(out));

endmodule

module bad_nested_middle (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
);

  bad_nested_grandchild i_grandchild (.clk(clk), .in(in), .out(out));

endmodule

module bad_nested_grandchild (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
); /*verilator subgraph_boundary*/

  always_ff @(posedge clk) out <= in;

endmodule

module bad_nested_parent (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
); /*verilator subgraph_boundary*/

  bad_nested_child i_child (.clk(clk), .in(in), .out(out));

endmodule

module bad_nested_child (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
); /*verilator subgraph_boundary*/

  always_ff @(posedge clk) out <= in;

endmodule
