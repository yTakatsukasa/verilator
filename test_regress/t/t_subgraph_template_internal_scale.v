// DESCRIPTION: Verilator: Keep internal subgraph state out of the parent scheduling contract
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t;
  logic clk = 0;
  logic reset = 1;
  logic din = 0;
  logic [7:0] y[4];

  always #1 clk = ~clk;
  always @(negedge clk) din <= ~din;

  for (genvar i = 0; i < 4; ++i) begin : g
    sg_template_internal_scale dut (clk, reset, din, y[i]);
  end

  initial begin
    repeat (2) @(negedge clk);
    reset = 0;
    repeat (20) begin
      @(negedge clk);
      if (y[0] !== y[1] || y[0] !== y[2] || y[0] !== y[3]) $stop;
    end
    $write("*-* All Finished *-*\n");
    $finish;
  end
endmodule

module sg_template_internal_scale (
  input  logic       clk,
  input  logic       reset,
  input  logic       din,
  output logic [7:0] y
); /*verilator subgraph_boundary*/

  logic [7:0] q0;
  logic [7:0] q1;
  logic [7:0] q2;
  logic [7:0] q3;
`ifdef INTERNAL_LARGE
  logic [7:0] q4;
  logic [7:0] q5;
  logic [7:0] q6;
  logic [7:0] q7;
  logic [7:0] q8;
  logic [7:0] q9;
  logic [7:0] q10;
  logic [7:0] q11;
  logic [7:0] q12;
  logic [7:0] q13;
  logic [7:0] q14;
  logic [7:0] q15;
`endif

  always_ff @(posedge clk) begin
    if (reset) begin
      q0 <= 8'h01;
      q1 <= 8'h12;
      q2 <= 8'h23;
      q3 <= 8'h34;
`ifdef INTERNAL_LARGE
      q4 <= 8'h45;
      q5 <= 8'h56;
      q6 <= 8'h67;
      q7 <= 8'h78;
      q8 <= 8'h89;
      q9 <= 8'h9a;
      q10 <= 8'hab;
      q11 <= 8'hbc;
      q12 <= 8'hcd;
      q13 <= 8'hde;
      q14 <= 8'hef;
      q15 <= 8'hf0;
`endif
    end else begin
      q0 <= q0 + {7'b0, din};
      q1 <= q1 + q0;
      q2 <= q2 + q1;
      q3 <= q3 + q2;
`ifdef INTERNAL_LARGE
      q4 <= q4 + q3;
      q5 <= q5 + q4;
      q6 <= q6 + q5;
      q7 <= q7 + q6;
      q8 <= q8 + q7;
      q9 <= q9 + q8;
      q10 <= q10 + q9;
      q11 <= q11 + q10;
      q12 <= q12 + q11;
      q13 <= q13 + q12;
      q14 <= q14 + q13;
      q15 <= q15 + q14;
`endif
    end
  end

`ifdef INTERNAL_LARGE
  always_comb y = q0 ^ q1 ^ q2 ^ q3 ^ q4 ^ q5 ^ q6 ^ q7
                  ^ q8 ^ q9 ^ q10 ^ q11 ^ q12 ^ q13 ^ q14 ^ q15;
`else
  always_comb y = q0 ^ q1 ^ q2 ^ q3;
`endif
endmodule
