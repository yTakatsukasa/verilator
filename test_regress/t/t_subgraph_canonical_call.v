// DESCRIPTION: Verilator: Canonical subgraph module-local call sharing
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

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
  logic [14:0] drive0 = 15'h013;
  logic [14:0] drive1 = 15'h127;
  logic [14:0] drive2 = 15'h31d;
  logic [14:0] y0;
  logic [14:0] y1;
  logic [14:0] y2;
  logic [14:0] ref0;
  logic [14:0] ref1;
  logic [14:0] ref2;

  sg_canonical_call i_sg0 (clk, drive0, y0);
  sg_canonical_call i_sg1 (clk, drive1, y1);
  sg_canonical_call i_sg2 (clk, drive2, y2);
  sg_canonical_call_ref i_ref0 (clk, drive0, ref0);
  sg_canonical_call_ref i_ref1 (clk, drive1, ref1);
  sg_canonical_call_ref i_ref2 (clk, drive2, ref2);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    drive0 <= {drive0[10:0], drive0[14:11]} ^ 15'h011;
    drive1 <= {drive1[8:0], drive1[14:9]} ^ 15'h023;
    drive2 <= {drive2[6:0], drive2[14:7]} ^ 15'h037;
    if (cyc > 3) begin
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

module sg_canonical_call (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
); `SUBGRAPH_BOUNDARY

  logic [14:0] q = 15'h155;

  function automatic logic [14:0] mix(input logic [14:0] lhs, input logic [14:0] rhs);
    /*verilator no_inline_task*/
    mix = {lhs[8:0], lhs[14:9]} + rhs + 15'h12d;
  endfunction

  always_ff @(posedge clk) q <= mix(q, drive);
  always_comb y = q ^ {q[11:0], q[14:12]};

endmodule

module sg_canonical_call_ref (
  input logic clk,
  input logic [14:0] drive,
  output logic [14:0] y
);

  logic [14:0] q = 15'h155;

  function automatic logic [14:0] mix(input logic [14:0] lhs, input logic [14:0] rhs);
    /*verilator no_inline_task*/
    mix = {lhs[8:0], lhs[14:9]} + rhs + 15'h12d;
  endfunction

  always_ff @(posedge clk) q <= mix(q, drive);
  always_comb y = q ^ {q[11:0], q[14:12]};

endmodule
