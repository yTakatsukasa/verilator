// DESCRIPTION: Verilator: Verilog Test module with parent-consumed subgraph outputs
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
  logic [14:0] consumed;
  logic [14:0] consumed_ref;
  logic [14:0] direct;
  logic [14:0] direct_ref;
  logic [14:0] parent_comb;
  logic [14:0] parent_comb_ref;
  logic [14:0] sampled;
  logic [14:0] sampled_ref;

  sg_contract_output i_sg (clk, direct);
  sg_contract_output_ref i_ref (clk, direct_ref);

  always_comb parent_comb = {direct[5:0], direct[14:6]} ^ 15'h421;
  always_comb parent_comb_ref = {direct_ref[5:0], direct_ref[14:6]} ^ 15'h421;

  always_ff @(posedge clk) begin
    consumed <= parent_comb + 15'h123;
    consumed_ref <= parent_comb_ref + 15'h123;
    sampled <= direct;
    sampled_ref <= direct_ref;
  end

  always_ff @(negedge clk) begin
    cyc <= cyc + 1;
    if (cyc > 2) begin
      `checkh(consumed, consumed_ref);
      `checkh(direct, direct_ref);
      `checkh(parent_comb, parent_comb_ref);
      `checkh(sampled, sampled_ref);
    end
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_contract_output (
  input logic clk,
  output logic [14:0] direct = 15'h1234
); `SUBGRAPH_BOUNDARY

  always_ff @(posedge clk) begin
    direct <= {direct[12:0], direct[14:13]} ^ 15'h2105;
  end

endmodule

module sg_contract_output_ref (
  input logic clk,
  output logic [14:0] direct = 15'h1234
);

  always_ff @(posedge clk) begin
    direct <= {direct[12:0], direct[14:13]} ^ 15'h2105;
  end

endmodule
