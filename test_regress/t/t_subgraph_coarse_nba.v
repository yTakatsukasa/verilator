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
  logic rst_n;
  logic [14:0] drive;
  logic [14:0] ref0;
  logic [14:0] ref1;
  logic [14:0] ref2;
  logic [14:0] y0;
  logic [14:0] y1;
  logic [14:0] y2;

  sg_coarse_nba i_sg0 (clk, rst_n, drive, y0);
  sg_coarse_nba i_sg1 (clk, rst_n, drive ^ 15'h1234, y1);
  sg_coarse_nba i_sg2 (clk, rst_n, {drive[6:0], drive[14:7]}, y2);
  sg_coarse_nba_ref i_ref0 (clk, rst_n, drive, ref0);
  sg_coarse_nba_ref i_ref1 (clk, rst_n, drive ^ 15'h1234, ref1);
  sg_coarse_nba_ref i_ref2 (clk, rst_n, {drive[6:0], drive[14:7]}, ref2);

  initial begin
    cyc = 0;
    rst_n = 1'b0;
    drive = 15'h2345;
  end

  always @(posedge clk) begin
    cyc <= cyc + 1;
    rst_n <= cyc >= 2;
    drive <= {drive[12:0], drive[14:13]} ^ 15'h1021;
    if (cyc > 4) begin
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

module sg_coarse_nba (
  input logic clk,
  input logic rst_n,
  input logic [14:0] drive,
  output logic [14:0] y
); /*verilator subgraph_boundary*/

  logic [14:0] a_q;
  logic [14:0] b_q;
  logic [14:0] blocking_tmp;
  logic [14:0] split_q;

  always @(posedge clk) begin
    if (!rst_n) begin
      blocking_tmp = 15'h1357;
      a_q <= 15'h0123;
    end
    else begin
      blocking_tmp = {a_q[10:0], a_q[14:11]} + drive;
      a_q <= blocking_tmp ^ 15'h2341;
    end
  end

  always @(posedge clk) begin
    if (!rst_n) b_q <= 15'h4567;
    else b_q <= {b_q[8:0], b_q[14:9]} ^ a_q ^ drive;
  end

  always @(posedge clk) begin
    if (!rst_n) split_q[6:0] <= 7'h35;
    else split_q[6:0] <= split_q[13:7] ^ drive[6:0];
  end

  always @(posedge clk) begin
    if (!rst_n) split_q[14:7] <= 8'h92;
    else split_q[14:7] <= split_q[7:0] + drive[14:7];
  end

  always_comb y = a_q ^ {b_q[3:0], b_q[14:4]} ^ split_q;

endmodule

module sg_coarse_nba_ref (
  input logic clk,
  input logic rst_n,
  input logic [14:0] drive,
  output logic [14:0] y
);

  logic [14:0] a_q;
  logic [14:0] b_q;
  logic [14:0] blocking_tmp;
  logic [14:0] split_q;

  always @(posedge clk) begin
    if (!rst_n) begin
      blocking_tmp = 15'h1357;
      a_q <= 15'h0123;
    end
    else begin
      blocking_tmp = {a_q[10:0], a_q[14:11]} + drive;
      a_q <= blocking_tmp ^ 15'h2341;
    end
  end

  always @(posedge clk) begin
    if (!rst_n) b_q <= 15'h4567;
    else b_q <= {b_q[8:0], b_q[14:9]} ^ a_q ^ drive;
  end

  always @(posedge clk) begin
    if (!rst_n) split_q[6:0] <= 7'h35;
    else split_q[6:0] <= split_q[13:7] ^ drive[6:0];
  end

  always @(posedge clk) begin
    if (!rst_n) split_q[14:7] <= 8'h92;
    else split_q[14:7] <= split_q[7:0] + drive[14:7];
  end

  always_comb y = a_q ^ {b_q[3:0], b_q[14:4]} ^ split_q;

endmodule
