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
  logic [7:0] in;
  logic [31:0] out0;
  logic [31:0] out1;
  logic [31:0] ref0;
  logic [31:0] ref1;

  sub i_sub0 (.clk(clk), .in(in), .out_child_comb(out0[31:24]), .out_child_direct(out0[23:16]), .out_reg_child_comb(out0[15:8]), .out_reg_comb(out0[7:0]));
  sub i_sub1 (.clk(clk), .in(in + 8'd1), .out_child_comb(out1[31:24]), .out_child_direct(out1[23:16]), .out_reg_child_comb(out1[15:8]), .out_reg_comb(out1[7:0]));
  sub_ref i_ref0 (.clk(clk), .in(in), .out_child_comb(ref0[31:24]), .out_child_direct(ref0[23:16]), .out_reg_child_comb(ref0[15:8]), .out_reg_comb(ref0[7:0]));
  sub_ref i_ref1 (.clk(clk), .in(in + 8'd1), .out_child_comb(ref1[31:24]), .out_child_direct(ref1[23:16]), .out_reg_child_comb(ref1[15:8]), .out_reg_comb(ref1[7:0]));

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    in <= {in[6:0], in[7] ^ in[5] ^ in[4] ^ in[3]};

    if (cyc == 0) begin
      in <= 8'h5a;
    end
    else begin
      `checkh(out0, ref0);
      `checkh(out1, ref1);
    end

    if (cyc == 32) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sub (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out_child_comb,
  output logic [7:0] out_child_direct,
  output logic [7:0] out_reg_child_comb,
  output logic [7:0] out_reg_comb
); `SUBGRAPH_BOUNDARY

  logic [7:0] child_comb_out;
  logic [7:0] q;
  logic [7:0] q_child_in;
  logic [7:0] reg_child_out;
  logic [7:0] leaf_out;

  sub_flop i_child_comb (.clk(clk), .in(in ^ 8'h3c), .out(child_comb_out));
  sub_flop i_child_direct (.clk(clk), .in(in + 8'd7), .out(out_child_direct));
  sub_leaf i_reg_child (.in(q_child_in), .out(reg_child_out));
  sub_leaf i_leaf (.in(in), .out(leaf_out));

  always_ff @(posedge clk) q <= leaf_out;
  always_ff @(posedge clk) q_child_in <= in - 8'd9;
  assign out_child_comb = {child_comb_out[0], child_comb_out[7:1]} ^ 8'h69;
  assign out_reg_child_comb = reg_child_out ^ 8'h42;
  assign out_reg_comb = {q[6:0], q[7]} ^ 8'h11;

endmodule

module sub_ref (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out_child_comb,
  output logic [7:0] out_child_direct,
  output logic [7:0] out_reg_child_comb,
  output logic [7:0] out_reg_comb
);

  logic [7:0] child_comb_out;
  logic [7:0] q;
  logic [7:0] q_child_in;
  logic [7:0] reg_child_out;
  logic [7:0] leaf_out;

  sub_flop i_child_comb (.clk(clk), .in(in ^ 8'h3c), .out(child_comb_out));
  sub_flop i_child_direct (.clk(clk), .in(in + 8'd7), .out(out_child_direct));
  sub_leaf i_reg_child (.in(q_child_in), .out(reg_child_out));
  sub_leaf i_leaf (.in(in), .out(leaf_out));

  always_ff @(posedge clk) q <= leaf_out;
  always_ff @(posedge clk) q_child_in <= in - 8'd9;
  assign out_child_comb = {child_comb_out[0], child_comb_out[7:1]} ^ 8'h69;
  assign out_reg_child_comb = reg_child_out ^ 8'h42;
  assign out_reg_comb = {q[6:0], q[7]} ^ 8'h11;

endmodule

module sub_flop (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
);

  always_ff @(posedge clk) out <= in;

endmodule

module sub_leaf (
  input logic [7:0] in,
  output logic [7:0] out
);

  assign out = {in[3:0], in[7:4]} ^ 8'ha5;

endmodule
