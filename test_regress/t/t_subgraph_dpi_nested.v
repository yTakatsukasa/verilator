// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
  input logic clk,
  output logic [14:0] out_a,
  output logic [14:0] out_b
);

  subgraph_dpi_parent i_dpi_a (.clk(clk), .out(out_a));
  subgraph_dpi_parent i_dpi_b (.clk(clk), .out(out_b));

endmodule

module subgraph_dpi_parent (
  input logic clk,
  output logic [14:0] out
); /*verilator subgraph_boundary*/

  logic [479:0] parent_q;
  logic [479:0] parent_q_next;
  logic [14:0] child_out;

  export "DPI-C" function dpi_parent_transform;
  function void dpi_parent_transform(
    input logic [479:0] value,
    output logic [479:0] result
  );
    result = {value[1:0], value[479:2]};
  endfunction

  always_ff @(posedge clk) begin
    dpi_parent_transform(parent_q, parent_q_next);
    parent_q <= parent_q_next;
  end

  subgraph_dpi_leaf i_leaf (.clk(clk), .out(child_out));

  assign out = parent_q[14:0] ^ child_out;

endmodule

module subgraph_dpi_leaf (
  input logic clk,
  output logic [14:0] out
);

  logic [479:0] q;
  logic [479:0] q_next;

  export "DPI-C" function dpi_transform;
  function void dpi_transform(
    input logic [479:0] value,
    output logic [479:0] result
  );
    result = {value[0], value[479:1]};
  endfunction

  always_ff @(posedge clk) begin
    dpi_transform(q, q_next);
    q <= q_next;
  end

  assign out = q[14:0];

endmodule
