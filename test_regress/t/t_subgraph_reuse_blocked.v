// DESCRIPTION: Verilator: Report why canonical subgraph reuse is blocked
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
  input logic clk
);

  int cyc = 0;
  logic [7:0] ext0 = 8'h01;
  logic [7:0] ext1 = 8'h02;
  logic [7:0] ext2 = 8'h04;
  logic [7:0] ext3 = 8'h08;
  logic [7:0] ext4 = 8'h10;
  logic [7:0] ext5 = 8'h20;
  logic [7:0] ext6 = 8'h40;
  logic [7:0] ext7 = 8'h80;
  logic [7:0] ext8 = 8'h5a;
  logic [7:0] y0;
  logic [7:0] y1;
  logic [7:0] y2;

  sg_reuse_blocked i_sg0 (clk, ext0, ext1, ext2, ext3, ext4, ext5, ext6, ext7, ext8, y0);
  sg_reuse_blocked i_sg1 (clk, ext0, ext1, ext2, ext3, ext4, ext5, ext6, ext7, ext8, y1);
  sg_reuse_blocked i_sg2 (clk, ext0, ext1, ext2, ext3, ext4, ext5, ext6, ext7, ext8, y2);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    ext0 <= ext0 + 8'h01;
    ext1 <= ext1 + 8'h03;
    ext2 <= ext2 + 8'h05;
    ext3 <= ext3 + 8'h07;
    ext4 <= ext4 + 8'h0b;
    ext5 <= ext5 + 8'h0d;
    ext6 <= ext6 + 8'h11;
    ext7 <= ext7 + 8'h13;
    ext8 <= ext8 + 8'h17;
    if (cyc > 2 && (y0 !== y1 || y0 !== y2)) $stop;
    if (cyc == 10) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_reuse_blocked (
  input logic clk,
  input logic [7:0] in0,
  input logic [7:0] in1,
  input logic [7:0] in2,
  input logic [7:0] in3,
  input logic [7:0] in4,
  input logic [7:0] in5,
  input logic [7:0] in6,
  input logic [7:0] in7,
  input logic [7:0] in8,
  output logic [7:0] y
); /*verilator subgraph_boundary*/

  logic [7:0] q = 8'h35;

  always_ff @(posedge clk) begin
    q <= q ^ in0 ^ in1 ^ in2 ^ in3 ^ in4 ^ in5 ^ in6 ^ in7 ^ in8;
  end
  always_comb y = q;

endmodule
