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
  logic [14:0] y0;
  logic [14:0] y1;

  sg_array_driver i_sg0 (clk, drive, y0);
  sg_array_driver i_sg1 (clk, drive ^ 15'h1234, y1);
  sg_array_driver_ref i_ref0 (clk, drive, ref0);
  sg_array_driver_ref i_ref1 (clk, drive ^ 15'h1234, ref1);

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
    end
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_array_driver (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
); /*verilator subgraph_boundary*/

  logic [6:0] part [3:1];
  logic [14:0] mixed;
  logic [14:0] q = 15'h1357;

  for (genvar gi = 1; gi <= 3; ++gi) begin : gen_part
    assign part[gi] = {2'(gi), drive[5 * (gi - 1) +: 5]};
  end

  always_comb mixed = {part[3][4:0], part[2][4:0], part[1][4:0]};
  always_ff @(posedge clk) q <= mixed ^ {q[8:0], q[14:9]};
  always_comb y = q;

endmodule

module sg_array_driver_ref (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
);

  logic [6:0] part [3:1];
  logic [14:0] mixed;
  logic [14:0] q = 15'h1357;

  for (genvar gi = 1; gi <= 3; ++gi) begin : gen_part
    assign part[gi] = {2'(gi), drive[5 * (gi - 1) +: 5]};
  end

  always_comb mixed = {part[3][4:0], part[2][4:0], part[1][4:0]};
  always_ff @(posedge clk) q <= mixed ^ {q[8:0], q[14:9]};
  always_comb y = q;

endmodule
