// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
  input logic clk
);

  logic [14:0] out;

  bad_dpi_parent i_bad (.clk(clk), .out(out));

endmodule

module bad_dpi_parent (
  input logic clk,
  output logic [14:0] out
); /*verilator subgraph_boundary*/

  bad_dpi_leaf i_leaf (.clk(clk), .out(out));

endmodule

module bad_dpi_leaf (
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
