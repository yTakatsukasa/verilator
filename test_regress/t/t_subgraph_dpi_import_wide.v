// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
  input logic clk,
  input logic [479:0] in,
  output logic [14:0] out_a,
  output logic [14:0] out_b
);

  subgraph_dpi_import_parent i_dpi_a (.clk(clk), .in(in), .out(out_a));
  subgraph_dpi_import_parent i_dpi_b (.clk(clk), .in(in), .out(out_b));

endmodule

module subgraph_dpi_import_parent (
  input logic clk,
  input logic [479:0] in,
  output logic [14:0] out
); /*verilator subgraph_boundary*/

  subgraph_dpi_import_leaf i_leaf (.clk(clk), .in(in), .out(out));

endmodule

module subgraph_dpi_import_leaf (
  input logic clk,
  input logic [479:0] in,
  output logic [14:0] out
);

  import "DPI-C" function void dpi_transform(
    input logic [479:0] value,
    output logic [479:0] result
  );

  logic [479:0] q;
  logic [479:0] q_next;

  always_ff @(posedge clk) begin
    dpi_transform(q ^ in, q_next);
    q <= q_next;
  end

  assign out = q[14:0];

endmodule
