// DESCRIPTION: Verilator: Verilog Test module with a cutable subgraph scheduling cycle
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
  input logic clk,
  output logic sink
);

  int cyc;
  logic [14:0] out0;
  logic [14:0] ref0;

  sg_circular i_sg0 (clk, out0, out0);
  sg_circular_ref i_ref0 (clk, ref0, ref0);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    if (cyc > 2) `checkh(out0, ref0);
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

  assign sink = ^{out0, ref0};

endmodule

module sg_circular #(
  parameter logic [44:0] RESET_VALUE = 45'h1234_5678_0abc
) (
  input logic clk,
  input logic [14:0] in,
  output logic [14:0] out
); `SUBGRAPH_BOUNDARY

  logic [44:0] child_out;
  logic [44:0] next_q;
  logic [44:0] q = RESET_VALUE;

  sg_circular_leaf i_leaf (q, child_out);

  always_comb next_q = ({q[28:0], q[44:29]} ^ {30'b0, in}) | {next_q[43:0], 1'b0};
  always_ff @(posedge clk) q <= next_q;
  assign out = child_out[14:0] ^ child_out[44:30];

endmodule

module sg_circular_ref #(
  parameter logic [44:0] RESET_VALUE = 45'h1234_5678_0abc
) (
  input logic clk,
  input logic [14:0] in,
  output logic [14:0] out
);

  logic [44:0] child_out;
  logic [44:0] next_q;
  logic [44:0] q = RESET_VALUE;

  sg_circular_leaf i_leaf (q, child_out);

  always_comb next_q = ({q[28:0], q[44:29]} ^ {30'b0, in}) | {next_q[43:0], 1'b0};
  always_ff @(posedge clk) q <= next_q;
  assign out = child_out[14:0] ^ child_out[44:30];

endmodule

module sg_circular_leaf (
  input logic [44:0] in,
  output logic [44:0] out
);

  assign out = {in[12:0], in[44:13]} ^ 45'h0555_1234_0abc;

endmodule
