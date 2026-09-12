// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0x exp=%0x (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`"); `stop; end while (0)
// verilog_format: on

package sg_task_pkg;
  task automatic transform(input logic [14:0] value, inout logic [14:0] state,
                           output logic [14:0] result, ref logic [14:0] target);
    state = {state[8:0], state[14:9]} + value;
    target = {target[12:0], target[14:13]} ^ state;
    result = target + value;
  endtask
endpackage

module t (
  input logic clk
);

  int cyc;
  logic [14:0] drive;
  logic [14:0] ref0;
  logic [14:0] ref1;
  logic [14:0] ref2;
  logic [14:0] y0;
  logic [14:0] y1;
  logic [14:0] y2;

  sg_task i_sg0 (clk, drive, y0);
  sg_task i_sg1 (clk, drive ^ 15'h1234, y1);
  sg_task i_sg2 (clk, {drive[6:0], drive[14:7]}, y2);
  sg_task_ref i_ref0 (clk, drive, ref0);
  sg_task_ref i_ref1 (clk, drive ^ 15'h1234, ref1);
  sg_task_ref i_ref2 (clk, {drive[6:0], drive[14:7]}, ref2);

  initial begin
    cyc = 0;
    drive = 15'h2345;
  end

  always @(posedge clk) begin
    cyc <= cyc + 1;
    drive <= {drive[12:0], drive[14:13]} ^ 15'h1021;
    if (cyc > 2) begin
      `checkh(y0, ref0);
      `checkh(y1, ref1);
      `checkh(y2, ref2);
    end
    if (cyc == 40) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_task (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
); /*verilator subgraph_boundary*/

  import sg_task_pkg::*;

  logic [14:0] accum;
  logic [14:0] count;
  logic [14:0] out;
  logic [14:0] q;
  logic [14:0] sampled;

  task update(input logic [14:0] value, inout logic [14:0] state,
    output logic [14:0] result, ref logic [14:0] target);
    logic [14:0] scratch;
    accum = {accum[10:0], accum[14:11]} + value;
    scratch = scratch + (state ^ 15'h0123);
    transform(value, scratch, result, target);
    result = result ^ accum;
    state = scratch ^ result;
  endtask

  initial begin
    accum = 15'h0234;
    count = 15'h0456;
    sampled = 15'h3456;
    q = 15'h1234;
    update(15'h0123, q, out, count);
    sampled = out ^ q;
  end
  always @(posedge clk) begin
    q = drive ^ 15'h2345;
    update(drive, q, out, count);
    sampled <= out ^ q;
  end
  always_comb y = sampled;

endmodule

module sg_task_ref (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
);

  import sg_task_pkg::*;

  logic [14:0] accum;
  logic [14:0] count;
  logic [14:0] out;
  logic [14:0] q;
  logic [14:0] sampled;

  task update(input logic [14:0] value, inout logic [14:0] state,
    output logic [14:0] result, ref logic [14:0] target);
    logic [14:0] scratch;
    accum = {accum[10:0], accum[14:11]} + value;
    scratch = scratch + (state ^ 15'h0123);
    transform(value, scratch, result, target);
    result = result ^ accum;
    state = scratch ^ result;
  endtask

  initial begin
    accum = 15'h0234;
    count = 15'h0456;
    sampled = 15'h3456;
    q = 15'h1234;
    update(15'h0123, q, out, count);
    sampled = out ^ q;
  end
  always @(posedge clk) begin
    q = drive ^ 15'h2345;
    update(drive, q, out, count);
    sampled <= out ^ q;
  end
  always_comb y = sampled;

endmodule
