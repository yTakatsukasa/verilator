// DESCRIPTION: Verilator: Local function calls in next-state logic of shared subgraphs
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d: got=%0x exp=%0x (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`"  ); `stop; end while (0)
// verilog_format: on

module t (
  input logic clk
);
  int unsigned cycles = 0;
  wire [6:0] q0;
  wire [6:0] q1;
  wire [6:0] y0;
  wire [6:0] y1;
  logic [6:0] expected0 = 7'd1;
  logic [6:0] expected1 = 7'd1;

  sg_function_comb i0 (.clk(clk), .d(q1), .q(q0), .y(y0));
  sg_function_comb i1 (.clk(clk), .d(q0 + 7'd3), .q(q1), .y(y1));

  always @(posedge clk) begin
    `checkh(q0, expected0);
    `checkh(q1, expected1);
    `checkh(y0, expected0 + 7'd1);
    `checkh(y1, expected1 + 7'd1);
    if (cycles == 10) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
    expected0 <= expected0 + expected1 + 7'd1;
    expected1 <= expected1 + expected0 + 7'd4;
    cycles <= cycles + 1;
  end
endmodule

module sg_function_comb (
  input logic clk,
  input logic [6:0] d,
  output logic [6:0] q = 7'd1,
  output wire [6:0] y
);
  /*verilator subgraph_boundary*/
  logic [6:0] next_q;
  logic [6:0] view_q;

  function automatic logic [6:0] advance(input logic [6:0] old_q, input logic [6:0] value);
    // verilator no_inline_task
    return old_q + value + 7'd1;
  endfunction

  always_comb next_q = advance(q, d);
  always_comb view_q = advance(q, 7'd0);
  always_ff @(posedge clk) q <= next_q;
  assign y = view_q;
endmodule
