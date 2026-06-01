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
  logic [7:0] out0;
  logic [7:0] out1;
  logic [7:0] ref0;
  logic [7:0] ref1;

  sub i_sub0 (.clk(clk), .in(in), .out(out0));
  sub i_sub1 (.clk(clk), .in(in + 8'd1), .out(out1));
  sub_ref i_ref0 (.clk(clk), .in(in), .out(ref0));
  sub_ref i_ref1 (.clk(clk), .in(in + 8'd1), .out(ref1));

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
  output logic [7:0] out
); `SUBGRAPH_BOUNDARY

  logic [7:0] q;
  logic [7:0] leaf_out;

  sub_leaf i_leaf (.in(in), .out(leaf_out));

  always_ff @(posedge clk) q <= leaf_out;
  assign out = q;

endmodule

module sub_ref (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
);

  logic [7:0] q;
  logic [7:0] leaf_out;

  sub_leaf i_leaf (.in(in), .out(leaf_out));

  always_ff @(posedge clk) q <= leaf_out;
  assign out = q;

endmodule

module sub_leaf (
  input logic [7:0] in,
  output logic [7:0] out
);

  assign out = {in[3:0], in[7:4]} ^ 8'ha5;

endmodule
