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

typedef struct packed {
  logic [7:0] hi;
  logic [7:0] lo;
} packed_pair_t;

module t (
  input logic clk
);

  int cyc;
  logic rst_n;
  logic [7:0] data;
  logic [15:0] y0;
  logic [15:0] y1;
  logic [15:0] y2;
  logic [15:0] ref0;
  logic [15:0] ref1;
  logic [15:0] ref2;

  sg_generated_temp i_sg0 (clk, rst_n, data, y0);
  sg_generated_temp i_sg1 (clk, rst_n, data, y1);
  sg_generated_temp i_sg2 (clk, rst_n, data, y2);
  sg_generated_temp_ref i_ref0 (clk, rst_n, data, ref0);
  sg_generated_temp_ref i_ref1 (clk, rst_n, data, ref1);
  sg_generated_temp_ref i_ref2 (clk, rst_n, data, ref2);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    rst_n <= cyc > 1;
    data <= {data[6:0], data[7] ^ data[5] ^ data[4] ^ data[3]};
    if (cyc == 0) begin
      rst_n <= 1'b0;
      data <= 8'h5a;
    end
    else if (cyc > 4) begin
      `checkh(y0, ref0);
      `checkh(y1, ref1);
      `checkh(y2, ref2);
    end
    if (cyc == 40) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_generated_temp (
  input  logic        clk,
  input  logic        rst_n,
  input  logic [7:0]  data_i,
  output logic [15:0] y_o
); `SUBGRAPH_BOUNDARY

  logic [7:0] leaf_q;
  logic [15:0] q;

  function automatic packed_pair_t mixed(input logic [7:0] x, input logic [7:0] y);
    packed_pair_t pair;
    pair.hi = x ^ y;
    pair.lo = x + y;
    return pair;
  endfunction

  sg_generated_temp_leaf i_leaf (clk, rst_n, data_i, leaf_q);
  assign y_o = q;

  always_ff @(posedge clk) begin
    if (rst_n) q <= {mixed(leaf_q, data_i).lo, mixed(leaf_q, data_i).hi};
    else q <= 16'h1234;
  end

endmodule

module sg_generated_temp_ref (
  input  logic        clk,
  input  logic        rst_n,
  input  logic [7:0]  data_i,
  output logic [15:0] y_o
);

  logic [7:0] leaf_q;
  logic [15:0] q;

  function automatic packed_pair_t mixed(input logic [7:0] x, input logic [7:0] y);
    packed_pair_t pair;
    pair.hi = x ^ y;
    pair.lo = x + y;
    return pair;
  endfunction

  sg_generated_temp_leaf i_leaf (clk, rst_n, data_i, leaf_q);
  assign y_o = q;

  always_ff @(posedge clk) begin
    if (rst_n) q <= {mixed(leaf_q, data_i).lo, mixed(leaf_q, data_i).hi};
    else q <= 16'h1234;
  end

endmodule

module sg_generated_temp_leaf (
  input  logic       clk,
  input  logic       rst_n,
  input  logic [7:0] data_i,
  output logic [7:0] q_o
);

  always_ff @(posedge clk) begin
    if (rst_n) q_o <= {q_o[5:0], q_o[7:6]} ^ data_i;
    else q_o <= 8'ha5;
  end

endmodule
