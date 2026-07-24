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
  logic aux_clk = 1'b0;
  logic [7:0] array_out;
  logic [7:0] scalar_out;

  sg_recipe_dtype i_sg (clk, aux_clk, scalar_out, array_out);

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    aux_clk <= ~aux_clk;
    if (cyc > 2) begin
      `checkh(scalar_out, 8'h35);
      `checkh(array_out, 8'ha7);
    end
    if (cyc == 20) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_recipe_dtype (
  input logic clk_scalar,
  input logic clk_array,
  output logic [7:0] scalar_out,
  output logic [7:0] array_out
); `SUBGRAPH_BOUNDARY

  typedef logic [7:0] data_t;
  typedef data_t array_t [256];

  data_t scalar_q = 8'h12;
  data_t scalar_d = 8'h35;
  array_t array_q = '{default: 8'h5a};
  array_t array_d = '{default: 8'ha7};

  assign scalar_out = scalar_q;
  assign array_out = array_q[0];

  always_ff @(posedge clk_scalar) scalar_q <= scalar_d;
  always_ff @(posedge clk_array) array_q <= array_d;

endmodule
