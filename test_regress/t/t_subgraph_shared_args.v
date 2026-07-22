// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

`ifdef USE_VLT
`define SUBGRAPH_BOUNDARY
`else
`define SUBGRAPH_BOUNDARY /*verilator subgraph_boundary*/
`endif

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0x exp=%0x (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`"); `stop; end while (0);
// verilog_format: on

module t (
  input logic clk
);

  int cyc;
  logic [14:0] y_a;
  logic [14:0] y_b;

  sg_many_args i_a (clk, y_a);
  sg_many_args i_b (clk, y_b);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    if (cyc > 2) `checkh(y_a, y_b);
    if (cyc == 40) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_many_args (
  input logic clk,
  output logic [14:0] y
); `SUBGRAPH_BOUNDARY

  logic [14:0] q0 = 15'h0123;
  logic [14:0] q1 = 15'h1234;
  logic [14:0] q2 = 15'h2345;
  logic [14:0] q3 = 15'h3456;
  logic [14:0] q4 = 15'h4567;
  logic [14:0] q5 = 15'h5678;
  logic [14:0] q6 = 15'h6789;
  logic [14:0] q7 = 15'h789a;

  assign y = q0 ^ q1 ^ q2 ^ q3 ^ q4 ^ q5 ^ q6 ^ q7;

  always_ff @(posedge clk) begin
    q0 <= {q0[13:0], q0[14]} ^ 15'h1021;
    q1 <= {q1[12:0], q1[14:13]} ^ 15'h2043;
    q2 <= {q2[11:0], q2[14:12]} ^ 15'h4087;
    q3 <= {q3[10:0], q3[14:11]} ^ 15'h0109;
    q4 <= {q4[9:0], q4[14:10]} ^ 15'h0211;
    q5 <= {q5[8:0], q5[14:9]} ^ 15'h0421;
    q6 <= {q6[7:0], q6[14:8]} ^ 15'h0841;
    q7 <= {q7[6:0], q7[14:7]} ^ 15'h1081;
  end

endmodule
