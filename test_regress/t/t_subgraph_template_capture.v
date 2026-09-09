// DESCRIPTION: Verilator: Capture specialized templates and reject incomplete bodies
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
  logic clk = 0;
  always #5 clk = ~clk;
  logic [14:0] data = 15'h135;
  wire [6:0] y0;
  wire [6:0] y1;
  wire [14:0] y2;
  wire [6:0] ya;
  wire [6:0] yc;
  logic [6:0] r0 = 0;
  logic [6:0] r1 = 0;
  logic [14:0] r2 = 0;
  logic [6:0] ra = 0;
  logic [6:0] rc = 0;
  int cyc = 0;

  sg_capture #(.W(7)) i0 (clk, data[6:0], y0);
  sg_capture #(.W(7)) i1 (clk, 7'h35, y1);
  sg_capture #(.W(15)) i2 (clk, data, y2);
  sg_capture_array ia (clk, data[6:0], ya);
  sg_capture_case ic (clk, data[6:0], yc);

  always @(posedge clk) begin
    r0 <= r0 + data[6:0];
    r1 <= r1 + 7'h35;
    r2 <= r2 + data;
    ra <= ra ^ data[6:0];
    rc <= data[0] ? rc + 7'h13 : rc ^ 7'h35;
  end
  always @(negedge clk) begin
    #1;
    `checkh(y0, r0)
    `checkh(y1, r1)
    `checkh(y2, r2)
    `checkh(ya, ra)
    `checkh(yc, rc)
    data = {data[13:0], data[14] ^ data[12]};
    cyc = cyc + 1;
    if (cyc == 60) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule

module sg_capture #(parameter W = 7) (
  input logic clk,
  input logic [W-1:0] data,
  output wire [W-1:0] y
);
  /*verilator subgraph_boundary*/
  logic [W-1:0] q = 0;
  always @(posedge clk) q <= q + data;
  assign y = q;
endmodule

module sg_capture_array (
  input logic clk,
  input logic [6:0] data,
  output wire [6:0] y
);
  /*verilator subgraph_boundary*/
  logic [6:0] q [2];
  initial begin
    q[0] = 0;
    q[1] = 0;
  end
  always @(posedge clk) begin
    q[0] <= q[0] ^ data;
    q[1] <= q[0];
  end
  assign y = q[0];
endmodule

module sg_capture_case (
  input logic clk,
  input logic [6:0] data,
  output wire [6:0] y
);
  /*verilator subgraph_boundary*/
  logic [6:0] q = 0;
  always @(posedge clk) begin
    case (data[0])
      1'b0: q <= q ^ 7'h35;
      default: q <= q + 7'h13;
    endcase
  end
  assign y = q;
endmodule
