// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0x exp=%0x (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`"); `stop; end while (0)
// verilog_format: on

module t (
  input logic clk
);

  int cyc;
  logic [14:0] drive;
  logic [14:0] y0;
  logic [14:0] y1;

  sg_call_classify i_sg0 (clk, drive, y0);
  sg_call_classify i_sg1 (clk, drive ^ 15'h1234, y1);

  initial begin
    cyc = 0;
    drive = 15'h2345;
  end

  always @(posedge clk) begin
    cyc <= cyc + 1;
    drive <= {drive[12:0], drive[14:13]} ^ 15'h1021;
    if (cyc == 40) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_call_classify (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
); /*verilator subgraph_boundary*/

  logic [14:0] q;

  function automatic logic [14:0] pure_auto(input logic [14:0] value);
    pure_auto = {value[9:0], value[14:10]} ^ 15'h1234;
  endfunction

  function logic [14:0] pure_static(input logic [14:0] value);
    pure_static = {value[5:0], value[14:6]} ^ 15'h2345;
  endfunction

  task automatic update(output logic [14:0] result, input logic [14:0] value);
    result = value ^ 15'h3456;
  endtask

  always @(posedge clk) begin
    logic [14:0] next_q;
    next_q = pure_auto(drive);
    next_q ^= pure_static(drive);
    update(next_q, next_q ^ drive);
    q <= next_q;
  end

  always_comb y = q;

endmodule
