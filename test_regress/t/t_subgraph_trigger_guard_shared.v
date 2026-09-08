// DESCRIPTION: Verilator: Share subgraph helpers with lifted trigger guards
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
  logic neg_clk;
  logic slow_clk = 0;
  logic [14:0] drive = 15'h013;
  logic [14:0] y0;
  logic [14:0] y1;
  logic [14:0] y2;
  logic [14:0] ref0;
  logic [14:0] ref1;
  logic [14:0] ref2;

  assign neg_clk = ~clk;

  sg_trigger_guard i_sg0 (clk, drive, y0);
  sg_trigger_guard i_sg1 (neg_clk, drive + 15'h011, y1);
  sg_trigger_guard i_sg2 (slow_clk, drive + 15'h023, y2);
  sg_trigger_guard_ref i_ref0 (clk, drive, ref0);
  sg_trigger_guard_ref i_ref1 (neg_clk, drive + 15'h011, ref1);
  sg_trigger_guard_ref i_ref2 (slow_clk, drive + 15'h023, ref2);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    slow_clk <= ~slow_clk;
    drive <= {drive[13:0], drive[14] ^ drive[12]};
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

module sg_trigger_guard (
  input  logic        clk,
  input  logic [14:0] drive,
  output logic [14:0] y
); `SUBGRAPH_BOUNDARY

  logic [14:0] q0 = 15'h001;
  logic [14:0] q1 = 15'h123;
  logic [14:0] q2 = 15'h345;

  always_ff @(posedge clk) begin
    q0 <= q0 + q2 + drive;
    q1 <= q1 + q0 + 15'h037;
    q2 <= q2 + q1 + 15'h061;
  end

  always_comb y = q0 ^ q1 ^ q2;

endmodule

module sg_trigger_guard_ref (
  input  logic        clk,
  input  logic [14:0] drive,
  output logic [14:0] y
);

  logic [14:0] q0 = 15'h001;
  logic [14:0] q1 = 15'h123;
  logic [14:0] q2 = 15'h345;

  always_ff @(posedge clk) begin
    q0 <= q0 + q2 + drive;
    q1 <= q1 + q0 + 15'h037;
    q2 <= q2 + q1 + 15'h061;
  end

  always_comb y = q0 ^ q1 ^ q2;

endmodule
