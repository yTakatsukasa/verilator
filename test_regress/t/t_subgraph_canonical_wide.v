// DESCRIPTION: Verilator: Canonical subgraph wide external ABI sharing
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
  logic [64:0] drive0 = 65'h1_0000_0000_0000_0013;
  logic [64:0] drive1 = 65'h0_0000_0000_0000_0127;
  logic [64:0] drive2 = 65'h1_0000_0000_0000_031d;
  logic [64:0] y0;
  logic [64:0] y1;
  logic [64:0] y2;
  logic [64:0] ref0;
  logic [64:0] ref1;
  logic [64:0] ref2;

  sg_canonical_wide i_sg0 (clk, drive0, y0);
  sg_canonical_wide i_sg1 (clk, drive1, y1);
  sg_canonical_wide i_sg2 (clk, drive2, y2);
  sg_canonical_wide_ref i_ref0 (clk, drive0, ref0);
  sg_canonical_wide_ref i_ref1 (clk, drive1, ref1);
  sg_canonical_wide_ref i_ref2 (clk, drive2, ref2);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    drive0 <= {drive0[53:0], drive0[64:54]} ^ 65'h0_0000_0000_0000_0011;
    drive1 <= {drive1[46:0], drive1[64:47]} ^ 65'h1_0000_0000_0000_0023;
    drive2 <= {drive2[38:0], drive2[64:39]} ^ 65'h0_0000_0000_0000_0037;
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

module sg_canonical_wide (
  input logic clk,
  input logic [64:0] drive,
  output logic [64:0] y
); `SUBGRAPH_BOUNDARY

  logic [64:0] q = 65'h1_1234_5678_9abc_def0;

  always_ff @(posedge clk) q <= {q[57:0], q[64:58]} ^ drive;
  always_comb y = q ^ {q[48:0], q[64:49]};

endmodule

module sg_canonical_wide_ref (
  input logic clk,
  input logic [64:0] drive,
  output logic [64:0] y
);

  logic [64:0] q = 65'h1_1234_5678_9abc_def0;

  always_ff @(posedge clk) q <= {q[57:0], q[64:58]} ^ drive;
  always_comb y = q ^ {q[48:0], q[64:49]};

endmodule
