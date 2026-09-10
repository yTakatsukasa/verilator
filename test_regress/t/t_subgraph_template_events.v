// DESCRIPTION: Verilator: Shared template events and asynchronous reset
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

`timescale 1ns/1ns

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d: got=%0x exp=%0x\n", `__FILE__, `__LINE__, (gotv), (expv)); `stop; end while (0);
// verilog_format: on

module t;
  bit clk = 0;
  always #5 clk = ~clk;
  logic rst_n = 1;
  logic [6:0] data = 7'h35;
  int cyc = 0;
  for (genvar i = 0; i < 2; ++i) begin : g
    wire clk_b = i == 0 ? clk : ~clk;
    wire [6:0] result;
    wire [6:0] expected = q ^ b;
    logic [6:0] q = 7'h1b;
    logic [6:0] b = 7'h35;
    logic [6:0] sampled = 0;
    logic [6:0] ref_sampled = 0;
    sg_template_events dut (clk, clk_b, rst_n, data, result);
    always @(posedge clk or negedge rst_n) begin
      if (!rst_n) q <= 7'h1b;
      else if (data[0]) q <= q + b + data;
    end
    always @(negedge clk_b) b <= b ^ q;
    always @(posedge clk) begin
      sampled <= result;
      ref_sampled <= expected;
    end
    always @(clk or rst_n) begin
      #1;
      `checkh(result, expected)
      `checkh(sampled, ref_sampled)
    end
  end
  always @(negedge clk) begin
    #2;
    data = {data[5:0], data[6] ^ data[5]};
    rst_n = (cyc % 7) != 0;
    cyc = cyc + 1;
    if (cyc == 127) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule

module sg_template_events (
  input logic clk_a,
  input logic clk_b,
  input logic rst_n,
  input logic [6:0] data,
  output wire [6:0] result
);
  /*verilator subgraph_boundary*/
  logic [6:0] q = 7'h1b;
  logic [6:0] b = 7'h35;
  always @(posedge clk_a or negedge rst_n) begin
    if (!rst_n) q <= 7'h1b;
    else if (data[0]) q <= q + b + data;
  end
  always @(negedge clk_b) b <= b ^ q;
  assign result = q ^ b;
endmodule
