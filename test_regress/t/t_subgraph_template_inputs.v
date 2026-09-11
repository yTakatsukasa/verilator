// DESCRIPTION: Verilator: Subgraph template instance connection semantics
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

`timescale 1ns/1ns

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d: %m at %0t: got=%0x exp=%0x (%s !== %s)\n", `__FILE__, `__LINE__, $time, (gotv), (expv), `"gotv`", `"expv`" ); `stop; end while (0);
// verilog_format: on

module t #(parameter N = 4);
  logic clk = 0;
  always #5 clk = ~clk;
  int cyc = 0;
  logic [14:0] drive = 15'h013;
  wire reset = cyc < 3 || (cyc >= 47 && cyc < 51);
  wire enable = (cyc % 5) != 0;
  wire sparse_clk = clk && (cyc % 3 == 0);
  wire [14:0] result [N];
  wire [14:0] expected [N];

  // All four cells have the same specialization, but different parent wiring.
  for (genvar i = 0; i < N; ++i) begin : g
    wire clock_b = i == 0 ? clk : i == 1 ? ~clk : sparse_clk;
    wire [14:0] data = i == 0 ? 15'h127 : i == 3 ? result[2] : drive;
    wire [14:0] ref_data = i == 0 ? 15'h127 : i == 3 ? expected[2] : drive;
    wire [14:0] extra;
    logic [14:0] a = 15'h001;
    logic [14:0] b = 15'h123;
    logic [14:0] shadow = 15'h345;
    logic [14:0] parent_sample = 0;
    logic [14:0] ref_sample = 0;

    if (i == 2) begin : unused_output
      sg_template_inputs dut (.clk_a(clk), .clk_b(clock_b), .reset(reset),
                              .enable(enable), .data(data), .result(result[i]), .extra());
    end else begin : used_output
      sg_template_inputs dut (clk, clock_b, reset, enable, data, result[i], extra);
      always @(negedge clk) begin
        #1;
        `checkh(extra, shadow)
      end
    end

    // Independent, non-boundary reference state. The two clocked processes
    // must both see old state when their clocks fire together.
    always @(posedge clk) begin
      if (reset) begin
        a <= 15'h001;
        shadow <= 15'h345;
      end else if (enable) begin
        a <= a + b + ref_data;
        shadow <= a ^ ref_data;
      end
      parent_sample <= result[i] + drive;
      ref_sample <= expected[i] + drive;
    end
    always @(posedge clock_b) begin
      if (reset) b <= 15'h123;
      else if (enable) b <= b + shadow + 15'h037;
    end
    assign expected[i] = a ^ b;

    always @(negedge clk) begin
      #1;
      `checkh(result[i], expected[i])
      `checkh(parent_sample, ref_sample)
    end
  end

  always @(negedge clk) begin
    cyc <= cyc + 1;
    drive <= {drive[13:0], drive[14] ^ drive[12]};
    if (cyc == 127) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule

module sg_template_inputs (
  input logic clk_a,
  input logic clk_b,
  input logic reset,
  input logic enable,
  input logic [14:0] data,
  output wire [14:0] result,
  output wire [14:0] extra
);
  /*verilator subgraph_boundary*/
  logic [14:0] a = 15'h001;
  logic [14:0] b = 15'h123;
  logic [14:0] shadow = 15'h345;

  always @(posedge clk_a) begin
    if (reset) begin
      a <= 15'h001;
      shadow <= 15'h345;
    end else if (enable) begin
      a <= a + b + data;
      shadow <= a ^ data;
    end
  end
  always @(posedge clk_b) begin
    if (reset) b <= 15'h123;
    else if (enable) b <= b + shadow + 15'h037;
  end
  assign result = a ^ b;
  assign extra = shadow;
endmodule
