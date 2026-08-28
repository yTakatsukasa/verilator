// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

module t (
  input logic clk,
  input logic in
);

  logic out_delay;
  logic out_event;

  bad_timing_delay i_delay (.clk(clk), .in(in), .out(out_delay));
  bad_timing_event i_event (.clk(clk), .in(in), .out(out_event));

endmodule

module bad_timing_delay (
  input logic clk,
  input logic in,
  output logic out
); /*verilator subgraph_boundary*/

  always @(posedge clk) out <= #1 in;

endmodule

module bad_timing_event (
  input logic clk,
  input logic in,
  output logic out
); /*verilator subgraph_boundary*/

  event e;
  logic dummy;
  logic q;

  always_ff @(posedge clk) q <= in;
  always @(posedge clk) -> e;
  initial begin
    @(e);
    dummy = 1'b1;
  end
  assign out = q;

endmodule
