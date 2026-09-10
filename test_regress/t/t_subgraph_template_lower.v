// DESCRIPTION: Verilator: Execute shared independent schedules with instance-local state
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

`timescale 1ns/1ns

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d: got=%0x exp=%0x\n", `__FILE__, `__LINE__, (gotv), (expv)); `stop; end while (0);
// verilog_format: on

module t #(parameter N = 4);
  bit clk = 0;
  always #5 clk = ~clk;
  logic [14:0] drive = 15'h135;
  wire [14:0] result [N];
  wire [14:0] expected [N];
  int cyc = 0;
  for (genvar i = 0; i < N; ++i) begin : g
    wire local_clk = (i % 2 == 0) ? clk : ~clk;
    wire [14:0] data;
    if (i == 0) assign data = 15'h35;
    else if (i == 2) assign data = result[0];
    else assign data = drive ^ 15'(i);
    wire [14:0] ref_data;
    if (i == 2) assign ref_data = expected[0];
    else assign ref_data = data;
    wire enable = drive[2];
    sg_template_lower dut (local_clk, enable, data, result[i]);
    logic [14:0] q = 1;
    logic [14:0] shadow = 3;
    wire [14:0] mixed = ref_data ^ 15'h127;
    always @(posedge local_clk) begin
      if (enable) q <= q + mixed;
      shadow <= q;
    end
    assign expected[i] = q ^ shadow;
    logic [14:0] sampled = 0;
    logic [14:0] ref_sampled = 0;
    always @(posedge local_clk) begin
      sampled <= result[i];
      ref_sampled <= expected[i];
    end
    always @(clk) begin
      #1;
      `checkh(result[i], expected[i])
      `checkh(sampled, ref_sampled)
    end
  end
  always @(negedge clk) begin
    #2;
    drive = {drive[13:0], drive[14] ^ drive[12]};
    cyc = cyc + 1;
    if (cyc == 127) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule

module sg_template_lower (
  input logic clk,
  input logic enable,
  input logic [14:0] data,
  output wire [14:0] result
);
  /*verilator subgraph_boundary*/
  logic [14:0] q = 1;
  logic [14:0] shadow = 3;
  wire [14:0] mixed = data ^ 15'h127;
  always @(posedge clk) begin
    if (enable) q <= q + mixed;
    shadow <= q;
  end
  assign result = q ^ shadow;
endmodule
