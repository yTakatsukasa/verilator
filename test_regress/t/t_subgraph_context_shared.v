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
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d:  got=%0x exp=%0x (%s !== %s)\n", `__FILE__,`__LINE__, (gotv), (expv), `"gotv`", `"expv`" ); `stop; end while (0);
// verilog_format: on

module t (
  input logic clk
);

  int cyc = 0;
  logic [14:0] y0;
  logic [14:0] y1;
  logic [14:0] y2;
  logic [14:0] ref0;
  logic [14:0] ref1;
  logic [14:0] ref2;
  logic [14:0] salt0 = 15'h123;
  logic [14:0] salt1 = 15'h456;
  logic [14:0] salt2 = 15'h789;

  sg_context_shared i_sg0 (clk, salt0, y0);
  sg_context_shared i_sg1 (clk, salt1, y1);
  sg_context_shared i_sg2 (clk, salt2, y2);
  sg_context_ref i_ref0 (clk, salt0, ref0);
  sg_context_ref i_ref1 (clk, salt1, ref1);
  sg_context_ref i_ref2 (clk, salt2, ref2);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    salt0 <= salt0 + 1'b1;
    salt1 <= salt1 + 1'b1;
    salt2 <= salt2 + 1'b1;
    if (cyc > 2) begin
      `checkh(y0, ref0)
      `checkh(y1, ref1)
      `checkh(y2, ref2)
    end
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_context_shared (
  input logic clk,
  input logic [14:0] salt,
  output logic [14:0] y
); `SUBGRAPH_BOUNDARY

  logic [94:0] q = 95'h1234_5678_9abc_def0_1234_567;
  sg_context_mid i_mid ();

  function automatic logic [94:0] scramble(input logic [94:0] value);
    // verilator no_inline_task
    scramble = {value[61:0], value[94:62]} ^ {80'b0, value[44:30]};
  endfunction

  always_ff @(posedge clk) begin
    q <= scramble(q) ^ i_mid.i_leaf.aux ^ {80'b0, salt}
         ^ 95'h0123_4567_89ab_cdef_0123_456;
  end
  assign y = q[14:0] ^ q[54:40] ^ q[94:80];

endmodule

module sg_context_ref (
  input logic clk,
  input logic [14:0] salt,
  output logic [14:0] y
);

  logic [94:0] q = 95'h1234_5678_9abc_def0_1234_567;
  sg_context_mid i_mid ();

  function automatic logic [94:0] scramble(input logic [94:0] value);
    // verilator no_inline_task
    scramble = {value[61:0], value[94:62]} ^ {80'b0, value[44:30]};
  endfunction

  always_ff @(posedge clk) begin
    q <= scramble(q) ^ i_mid.i_leaf.aux ^ {80'b0, salt}
         ^ 95'h0123_4567_89ab_cdef_0123_456;
  end
  assign y = q[14:0] ^ q[54:40] ^ q[94:80];

endmodule

module sg_context_mid; /*verilator no_inline_module*/

  sg_context_leaf i_leaf ();

endmodule

module sg_context_leaf; /*verilator no_inline_module*/

  logic [94:0] aux /*verilator public_flat*/ = 95'h7654_3210_fedc_ba98_7654_321;

endmodule
