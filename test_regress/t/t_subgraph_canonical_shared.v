// DESCRIPTION: Verilator: Subgraph canonical schedule and aggregate helper sharing
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
  logic [14:0] salt0 = 15'h12;
  logic [14:0] salt1 = 15'h34;
  logic [14:0] salt2 = 15'h56;
  logic [14:0] y0;
  logic [14:0] y1;
  logic [14:0] y2;
  logic [14:0] y3;
  logic [14:0] ref0;
  logic [14:0] ref1;
  logic [14:0] ref2;
  logic [14:0] ref3;

  sg_canonical i_sg0 (clk, salt0, y0);
  sg_canonical i_sg1 (clk, salt1, y1);
  sg_canonical i_sg2 (clk, salt2, y2);
  sg_canonical i_sg3 (clk, salt0 ^ 15'h17, y3);
  sg_canonical_ref i_ref0 (clk, salt0, ref0);
  sg_canonical_ref i_ref1 (clk, salt1, ref1);
  sg_canonical_ref i_ref2 (clk, salt2, ref2);
  sg_canonical_ref i_ref3 (clk, salt0 ^ 15'h17, ref3);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    salt0 <= salt0 + 15'h11;
    salt1 <= salt1 + 15'h23;
    salt2 <= salt2 + 15'h37;
    if (cyc > 2) begin
      `checkh(y0, ref0)
      `checkh(y1, ref1)
      `checkh(y2, ref2)
      `checkh(y3, ref3)
    end
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_canonical (
  input logic clk,
  input logic [14:0] salt,
  output logic [14:0] y
); `SUBGRAPH_BOUNDARY

  logic [14:0] mem [0:3] = '{15'h10, 15'h21, 15'h32, 15'h43};
  logic [1:0] ptr = 0;

  always_ff @(posedge clk) begin
    mem[ptr] <= mem[ptr ^ 2'b01] + salt;
    ptr <= ptr + 1'b1;
  end
  assign y = mem[0] ^ mem[1] ^ mem[2] ^ mem[3];

endmodule

module sg_canonical_ref (
  input logic clk,
  input logic [14:0] salt,
  output logic [14:0] y
);

  logic [14:0] mem [0:3] = '{15'h10, 15'h21, 15'h32, 15'h43};
  logic [1:0] ptr = 0;

  always_ff @(posedge clk) begin
    mem[ptr] <= mem[ptr ^ 2'b01] + salt;
    ptr <= ptr + 1'b1;
  end
  assign y = mem[0] ^ mem[1] ^ mem[2] ^ mem[3];

endmodule
