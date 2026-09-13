// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
  input logic clk
);

  int cyc;
  logic enable;
  logic [14:0] drive;
  logic [14:0] edge0;
  logic [14:0] edge1;
  logic [14:0] condition0;
  logic [14:0] condition1;

  sg_event_edge i_edge0 (clk, drive, edge0);
  sg_event_edge i_edge1 (clk, drive ^ 15'h1234, edge1);
  sg_event_condition i_condition0 (clk, enable, drive, condition0);
  sg_event_condition i_condition1 (clk, enable, drive ^ 15'h2345, condition1);

  initial begin
    cyc = 0;
    enable = 1'b0;
    drive = 15'h3456;
  end

  always @(posedge clk) begin
    cyc <= cyc + 1;
    enable <= ~enable;
    drive <= {drive[12:0], drive[14:13]} ^ 15'h1021;
    if (cyc == 20) begin
      $write("*-* All Finished *-* %x %x %x %x\n", edge0, edge1, condition0, condition1);
      $finish;
    end
  end

endmodule

module sg_event_edge (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
); /*verilator subgraph_boundary*/

  logic [14:0] q = 15'h0123;

  always @(edge clk) q <= {q[10:0], q[14:11]} ^ drive;
  always_comb y = q;

endmodule

module sg_event_condition (
  input logic clk,
  input logic enable,
  input logic [14:0] drive,
  output logic [14:0] y
); /*verilator subgraph_boundary*/

  logic [14:0] q = 15'h2345;

  always @(posedge clk iff enable) q <= {q[8:0], q[14:9]} + drive;
  always_comb y = q;

endmodule
