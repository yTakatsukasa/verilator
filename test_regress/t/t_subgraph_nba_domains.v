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

  sg_nba_domains i_sg0 (clk, clk, drive, y0);
  sg_nba_domains i_sg1 (clk, clk, drive ^ 15'h1234, y1);
  sg_nba_domains_ref i_ref0 (clk, clk, drive, ref0);
  sg_nba_domains_ref i_ref1 (clk, clk, drive ^ 15'h1234, ref1);

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
    if (cyc == 20) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_nba_domains (
  input logic clk_a,
  input logic clk_b,
  input logic [14:0] drive,
  output logic [14:0] y
); /*verilator subgraph_boundary*/

  typedef struct packed {
    logic [6:0] high;
    logic [7:0] low;
  } state_t;

  state_t state = 15'h1357;

  always_ff @(posedge clk_a) state.high <= drive[14:8];
  always_ff @(posedge clk_b) state.low <= drive[7:0];
  always_comb y = {state.low, state.high};

endmodule

module sg_nba_domains_ref (
  input logic clk_a,
  input logic clk_b,
  input logic [14:0] drive,
  output logic [14:0] y
);

  typedef struct packed {
    logic [6:0] high;
    logic [7:0] low;
  } state_t;

  state_t state = 15'h1357;

  always_ff @(posedge clk_a) state.high <= drive[14:8];
  always_ff @(posedge clk_b) state.low <= drive[7:0];
  always_comb y = {state.low, state.high};

endmodule
