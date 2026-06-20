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

module t (
  input logic clk
);

  int cyc;
  logic [7:0] src0;
  logic [7:0] src1;
  logic [7:0] src2;
  logic [7:0] din0;
  logic [7:0] din1;
  logic [7:0] din2;
  logic [7:0] din3;
  logic [7:0] din4;
  logic [7:0] din5;
  logic [7:0] obs;
  logic [7:0] pos_q0;
  logic [7:0] pos_q1;
  logic [7:0] neg_q0;
  logic [7:0] neg_q1;

  always_comb begin
    din0 = src0 ^ {src1[3:0], src2[7:4]};
    din1 = src1 + src2;
    din2 = src2 ^ 8'h5a;
    din3 = src0 + src2;
    din4 = src1 ^ 8'ha5;
    din5 = src0 ^ src1 ^ src2;
  end

  sg_node i_pos (
    .clk(clk),
    .a_i(din0),
    .b_i(din1),
    .c_i(din2),
    .q_o(pos_q0)
  );

  sg_node i_pos2 (
    .clk(clk),
    .a_i(din3),
    .b_i(din4),
    .c_i(din5),
    .q_o(pos_q1)
  );

  sg_node_neg i_neg (
    .clk(clk),
    .a_i(din3),
    .b_i(din4),
    .c_i(din5),
    .q_o(neg_q0)
  );

  sg_node_neg i_neg2 (
    .clk(clk),
    .a_i(din0),
    .b_i(din1),
    .c_i(din2),
    .q_o(neg_q1)
  );

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    src0 <= src0 + 8'h11;
    src1 <= {src1[6:0], src1[7] ^ src1[5]};
    src2 <= src2 ^ src0 ^ 8'h3c;
    obs <= obs ^ pos_q0 ^ pos_q1 ^ neg_q0 ^ neg_q1;

    if (cyc == 0) begin
      src0 <= 8'h21;
      src1 <= 8'h43;
      src2 <= 8'h65;
      obs <= 8'h00;
    end

    if (cyc == 20) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_node (
  input  logic       clk,
  input  logic [7:0] a_i,
  input  logic [7:0] b_i,
  input  logic [7:0] c_i,
  output logic [7:0] q_o
); `SUBGRAPH_BOUNDARY

  logic [7:0] mix0;
  logic [7:0] mix1;
  logic [7:0] q;

  always_comb begin
    mix0 = (a_i ^ b_i) + c_i;
    mix1 = {b_i[3:0], a_i[7:4]} ^ c_i;
  end

  assign q_o = q;

  always_ff @(posedge clk) begin
    q <= mix0 ^ mix1;
  end

endmodule

module sg_node_neg (
  input  logic       clk,
  input  logic [7:0] a_i,
  input  logic [7:0] b_i,
  input  logic [7:0] c_i,
  output logic [7:0] q_o
); `SUBGRAPH_BOUNDARY

  logic [7:0] mix0;
  logic [7:0] mix1;
  logic [7:0] q;

  always_comb begin
    mix0 = (a_i + b_i) ^ c_i;
    mix1 = {c_i[3:0], b_i[7:4]} + a_i;
  end

  assign q_o = q;

  always_ff @(negedge clk) begin
    q <= mix0 + mix1;
  end

endmodule
