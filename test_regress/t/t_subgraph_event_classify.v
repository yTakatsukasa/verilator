// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0x exp=%0x (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`"); `stop; end while (0);
// verilog_format: on

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
  logic [14:0] changed0;
  logic [14:0] changed1;
  logic [14:0] changed_ref0;
  logic [14:0] changed_ref1;
  logic [14:0] internal0;
  logic [14:0] internal1;
  logic [14:0] internal_ref0;
  logic [14:0] internal_ref1;

  sg_event_edge i_edge0 (clk, drive, edge0);
  sg_event_edge i_edge1 (clk, drive ^ 15'h1234, edge1);
  sg_event_condition i_condition0 (clk, enable, drive, condition0);
  sg_event_condition i_condition1 (clk, enable, drive ^ 15'h2345, condition1);
  sg_event_changed i_changed0 (clk, enable, drive, changed0);
  sg_event_changed i_changed1 (clk, enable, drive ^ 15'h3456, changed1);
  sg_event_changed_ref i_changed_ref0 (clk, enable, drive, changed_ref0);
  sg_event_changed_ref i_changed_ref1 (clk, enable, drive ^ 15'h3456, changed_ref1);
  sg_internal_trigger i_internal0 (clk, drive, internal0);
  sg_internal_trigger i_internal1 (clk, drive ^ 15'h4567, internal1);
  sg_internal_trigger_ref i_internal_ref0 (clk, drive, internal_ref0);
  sg_internal_trigger_ref i_internal_ref1 (clk, drive ^ 15'h4567, internal_ref1);

  initial begin
    cyc = 0;
    enable = 1'b0;
    drive = 15'h3456;
  end

  always @(posedge clk) begin
    cyc <= cyc + 1;
    enable <= ~enable;
    drive <= {drive[12:0], drive[14:13]} ^ 15'h1021;
    if (cyc > 2) begin
      `checkh(changed0, changed_ref0);
      `checkh(changed1, changed_ref1);
      `checkh(internal0, internal_ref0);
      `checkh(internal1, internal_ref1);
    end
    if (cyc == 20) begin
      $write("*-* All Finished *-* %x %x %x %x\n", edge0, edge1, condition0, condition1);
      $finish;
    end
  end

endmodule

module sg_event_changed (
  input logic clk,
  input logic enable,
  input logic [14:0] drive,
  output logic [14:0] y
); /*verilator subgraph_boundary*/

  logic [14:0] mixed;
  logic [14:0] q = 15'h1835;

  always @(enable or drive) begin
    if (enable) mixed = {drive[6:0], drive[14:7]} ^ 15'h1729;
    else mixed = {drive[10:0], drive[14:11]} + 15'h2851;
  end
  always_ff @(posedge clk) q <= mixed;
  always_comb y = q;

endmodule

module sg_event_changed_ref (
  input logic clk,
  input logic enable,
  input logic [14:0] drive,
  output logic [14:0] y
);

  logic [14:0] mixed;
  logic [14:0] q = 15'h1835;

  always @(enable or drive) begin
    if (enable) mixed = {drive[6:0], drive[14:7]} ^ 15'h1729;
    else mixed = {drive[10:0], drive[14:11]} + 15'h2851;
  end
  always_ff @(posedge clk) q <= mixed;
  always_comb y = q;

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

module sg_internal_trigger (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
); /*verilator subgraph_boundary*/

  logic clk_alias;
  logic [14:0] q = 15'h2946;

  assign clk_alias = clk;
  always_ff @(posedge clk_alias) q <= {q[9:0], q[14:10]} ^ drive;
  always_comb y = q;

endmodule

module sg_internal_trigger_ref (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
);

  logic clk_alias;
  logic [14:0] q = 15'h2946;

  assign clk_alias = clk;
  always_ff @(posedge clk_alias) q <= {q[9:0], q[14:10]} ^ drive;
  always_comb y = q;

endmodule
