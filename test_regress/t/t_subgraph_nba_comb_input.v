// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0x exp=%0x (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`"); `stop; end while (0)
// verilog_format: on

module t (
  input logic clk
);

  int cyc;
  logic rst_n;
  logic [7:0] in_q;
  logic [7:0] y_ref;
  logic [7:0] y_sub;

  sg_nba_comb_input i_sub (.*,.y_o(y_sub));
  sg_nba_comb_input_ref i_ref (.*,.y_o(y_ref));

  initial begin
    cyc = 0;
    rst_n = 1'b0;
    in_q = 8'h31;
  end

  always @(posedge clk) begin
    cyc <= cyc + 1;
    rst_n <= cyc >= 2;
    in_q <= {in_q[5:0], in_q[7:6]} ^ 8'h5b;

    if (cyc > 4) `checkh(y_sub, y_ref);
    if (cyc == 40) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_nba_comb_input (
  input logic clk,
  input logic rst_n,
  input logic [7:0] in_q,
  output logic [7:0] y_o
); /*verilator subgraph_boundary*/

  logic [7:0] next_w;
  logic [7:0] state_q;

  always_comb next_w = {state_q[4:0], state_q[7:5]} + in_q;
  always_ff @(posedge clk) begin
    if (!rst_n) state_q <= 8'ha7;
    else state_q <= next_w;
  end
  assign y_o = state_q;

endmodule

module sg_nba_comb_input_ref (
  input logic clk,
  input logic rst_n,
  input logic [7:0] in_q,
  output logic [7:0] y_o
);

  logic [7:0] next_w;
  logic [7:0] state_q;

  always_comb next_w = {state_q[4:0], state_q[7:5]} + in_q;
  always_ff @(posedge clk) begin
    if (!rst_n) state_q <= 8'ha7;
    else state_q <= next_w;
  end
  assign y_o = state_q;

endmodule
