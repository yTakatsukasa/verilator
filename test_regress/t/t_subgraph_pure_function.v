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
  logic [14:0] ref0;
  logic [14:0] ref1;
  logic [14:0] ref2;
  logic [14:0] y0;
  logic [14:0] y1;
  logic [14:0] y2;

  sg_pure_function i_sg0 (clk, drive, y0);
  sg_pure_function i_sg1 (clk, drive ^ 15'h1234, y1);
  sg_pure_function i_sg2 (clk, {drive[6:0], drive[14:7]}, y2);
  sg_pure_function_ref i_ref0 (clk, drive, ref0);
  sg_pure_function_ref i_ref1 (clk, drive ^ 15'h1234, ref1);
  sg_pure_function_ref i_ref2 (clk, {drive[6:0], drive[14:7]}, ref2);

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

module sg_pure_function (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
); /*verilator subgraph_boundary*/

  logic [14:0] q;

  function automatic logic [14:0] rotate(input logic [14:0] value);
    logic [14:0] temporary;
    temporary = {value[8:0], value[14:9]};
    rotate = temporary;
  endfunction

  function automatic logic [14:0] mix(
    input logic [14:0] lhs,
    input logic [14:0] rhs
  );
    if (lhs[0]) mix = lhs + rhs;
    else mix = lhs ^ rhs;
  endfunction

  always @(posedge clk) q <= mix(drive, rotate(drive ^ q));
  always_comb y = q;

endmodule

module sg_pure_function_ref (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
);

  logic [14:0] q;

  function automatic logic [14:0] rotate(input logic [14:0] value);
    logic [14:0] temporary;
    temporary = {value[8:0], value[14:9]};
    rotate = temporary;
  endfunction

  function automatic logic [14:0] mix(
    input logic [14:0] lhs,
    input logic [14:0] rhs
  );
    if (lhs[0]) mix = lhs + rhs;
    else mix = lhs ^ rhs;
  endfunction

  always @(posedge clk) q <= mix(drive, rotate(drive ^ q));
  always_comb y = q;

endmodule
