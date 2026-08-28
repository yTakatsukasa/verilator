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
  logic [14:0] noninline_ref0;
  logic [14:0] noninline_ref1;
  logic [14:0] noninline_y0;
  logic [14:0] noninline_y1;
  logic [14:0] ref0;
  logic [14:0] ref1;
  logic [14:0] ref2;
  logic [14:0] y0;
  logic [14:0] y1;
  logic [14:0] y2;

  sg_stl_noninline i_noninline0 (clk, noninline_y0);
  sg_stl_noninline i_noninline1 (clk, noninline_y1);
  sg_stl_noninline_ref i_noninline_ref0 (clk, noninline_ref0);
  sg_stl_noninline_ref i_noninline_ref1 (clk, noninline_ref1);
  sg_stl_shared i_sg0 (clk, y0);
  sg_stl_shared i_sg1 (clk, y1);
  sg_stl_shared i_sg2 (clk, y2);
  sg_stl_shared_ref i_ref0 (clk, ref0);
  sg_stl_shared_ref i_ref1 (clk, ref1);
  sg_stl_shared_ref i_ref2 (clk, ref2);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    if (cyc > 2) begin
      `checkh(noninline_y0, noninline_ref0);
      `checkh(noninline_y1, noninline_ref1);
      `checkh(y0, ref0);
      `checkh(y1, ref1);
      `checkh(y2, ref2);
    end
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_stl_noninline (
  input logic clk,
  output logic [14:0] y
); `SUBGRAPH_BOUNDARY

  logic [44:0] child_out;
  logic [44:0] q = 45'h2345_6789_0abc;

  sg_stl_noninline_leaf i_leaf (q, child_out);

  always_ff @(posedge clk) q <= {q[42:0], q[44:43]} ^ 45'h1234_5678_9abc;
  assign y = child_out[14:0] ^ child_out[44:30];

endmodule

module sg_stl_noninline_ref (
  input logic clk,
  output logic [14:0] y
);

  logic [44:0] child_out;
  logic [44:0] q = 45'h2345_6789_0abc;

  sg_stl_noninline_leaf i_leaf (q, child_out);

  always_ff @(posedge clk) q <= {q[42:0], q[44:43]} ^ 45'h1234_5678_9abc;
  assign y = child_out[14:0] ^ child_out[44:30];

endmodule

module sg_stl_noninline_leaf (
  input logic [44:0] in,
  output logic [44:0] out
); /*verilator no_inline_module*/

  assign out = {in[30:0], in[44:31]} ^ 45'h1111_5678_0abc;

endmodule

module sg_stl_shared (
  input logic clk,
  output logic [14:0] y
); `SUBGRAPH_BOUNDARY

  logic [44:0] child_out;
  logic [44:0] q = 45'h1234_5678_6abc;

  sg_stl_shared_leaf i_leaf (q, child_out);

  always_ff @(posedge clk) q <= {q[43:0], q[44]} ^ {30'b0, q[29 +: 15]};
  assign y = child_out[14:0] ^ child_out[44:30];

endmodule

module sg_stl_shared_ref (
  input logic clk,
  output logic [14:0] y
);

  logic [44:0] child_out;
  logic [44:0] q = 45'h1234_5678_6abc;

  sg_stl_shared_leaf i_leaf (q, child_out);

  always_ff @(posedge clk) q <= {q[43:0], q[44]} ^ {30'b0, q[29 +: 15]};
  assign y = child_out[14:0] ^ child_out[44:30];

endmodule

module sg_stl_shared_leaf (
  input logic [44:0] in,
  output logic [44:0] out
);

  assign out = {in[12:0], in[44:13]} ^ 45'h0555_1234_0abc;

endmodule
