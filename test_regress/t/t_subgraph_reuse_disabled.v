// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

`ifdef TEST_HARNESS_SCAN
module t (
  input logic clk
);
endmodule
`endif

`include "t_subgraph_order_recipe.v"
