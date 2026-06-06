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

  logic named_out;
  logic positional_out;

  bad_task_named_feedthrough i_bad_named (.clk(clk), .in(in), .out(named_out));
  bad_task_positional_feedthrough i_bad_positional (.clk(clk), .in(in), .out(positional_out));

  assign out = named_out ^ positional_out;

endmodule

module bad_task_positional_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic comb;
  logic q;

  task automatic drive(output logic value);
    value = in;
  endtask

  always_ff @(posedge clk) q <= in;
  always_comb begin
    comb = 1'b0;
    drive(comb);
    out = q ^ comb;
  end

endmodule

module bad_task_named_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic comb;
  logic q;

  task automatic drive(output logic value);
    value = in;
  endtask

  always_ff @(posedge clk) q <= in;
  always_comb begin
    comb = 1'b0;
    drive(.value(comb));
    out = q ^ comb;
  end

endmodule
