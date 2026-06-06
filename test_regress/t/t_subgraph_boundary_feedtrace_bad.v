// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

module t (
  input  logic clk,
  input  logic in,
  output logic out
);

  logic concat_out;
  logic func_cond_out;
  logic func_out;

  bad_concat_feedthrough i_bad_concat (.clk(clk), .in(in), .out(concat_out));
  bad_func_cond_feedthrough i_bad_func_cond (.clk(clk), .in(in), .out(func_cond_out));
  bad_func_feedthrough i_bad_func (.clk(clk), .in(in), .out(func_out));

  assign out = concat_out ^ func_cond_out ^ func_out;

endmodule

module bad_concat_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic unused;

  bad_concat_child i_child (.clk(clk), .in(in), .out({unused, out}));

endmodule

module bad_concat_child (
  input  logic clk,
  input  logic in,
  output logic [1:0] out
);

  logic q;

  always_ff @(posedge clk) q <= in;
  assign out = {q, in};

endmodule

module bad_func_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic q;

  function automatic logic tap();
    tap = in;
  endfunction

  always_ff @(posedge clk) q <= in;
  assign out = q ^ tap();

endmodule

module bad_func_cond_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic q;

  function automatic logic gate();
    gate = in;
  endfunction

  always_ff @(posedge clk) q <= in;
  assign out = gate() ? q : 1'b0;

endmodule
