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
  logic [14:0] consumed_b;
  logic [14:0] consumed_ref;
  logic [14:0] consumed_ref_b;
  logic [14:0] direct;
  logic [14:0] direct_b;
  logic [14:0] direct_ref;
  logic [14:0] direct_ref_b;
  logic [14:0] parent_comb;
  logic [14:0] parent_comb_b;
  logic [14:0] parent_comb_ref;
  logic [14:0] parent_comb_ref_b;
  logic [14:0] sampled;
  logic [14:0] sampled_b;
  logic [14:0] sampled_ref;
  logic [14:0] sampled_ref_b;
  logic [14:0] summary_only;
  logic [14:0] summary_only_b;
  logic [14:0] summary_only_ref;
  logic [14:0] summary_only_ref_b;

  sg_contract_output i_sg (clk, direct, summary_only);
  sg_contract_output i_sg_b (clk, direct_b, summary_only_b);
  sg_contract_output_ref i_ref (clk, direct_ref, summary_only_ref);
  sg_contract_output_ref i_ref_b (clk, direct_ref_b, summary_only_ref_b);
  sg_contract_empty i_sg_empty (clk);

  always_comb parent_comb = {direct[5:0], direct[14:6]} ^ 15'h421;
  always_comb parent_comb_b = {direct_b[5:0], direct_b[14:6]} ^ 15'h124;
  always_comb parent_comb_ref = {direct_ref[5:0], direct_ref[14:6]} ^ 15'h421;
  always_comb parent_comb_ref_b = {direct_ref_b[5:0], direct_ref_b[14:6]} ^ 15'h124;

  always_ff @(posedge clk) begin
    consumed <= parent_comb + 15'h123;
    consumed_b <= parent_comb_b + 15'h321;
    consumed_ref <= parent_comb_ref + 15'h123;
    consumed_ref_b <= parent_comb_ref_b + 15'h321;
    sampled <= direct;
    sampled_b <= direct_b;
    sampled_ref <= direct_ref;
    sampled_ref_b <= direct_ref_b;
  end

  always_ff @(negedge clk) begin
    cyc <= cyc + 1;
    if (cyc > 2) begin
      `checkh(consumed, consumed_ref);
      `checkh(consumed_b, consumed_ref_b);
      `checkh(direct, direct_ref);
      `checkh(direct_b, direct_ref_b);
      `checkh(parent_comb, parent_comb_ref);
      `checkh(parent_comb_b, parent_comb_ref_b);
      `checkh(sampled, sampled_ref);
      `checkh(sampled_b, sampled_ref_b);
      `checkh(summary_only, summary_only_ref);
      `checkh(summary_only_b, summary_only_ref_b);
    end
    if (cyc == 30) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_contract_empty (
  input logic clk
); `SUBGRAPH_BOUNDARY

  logic [14:0] state /*verilator public_flat_rd*/ = 15'h3210;

  always_ff @(posedge clk) begin
    state <= {state[11:0], state[14:12]} ^ 15'h1423;
  end

endmodule

module sg_contract_output #(
  parameter logic [14:0] RESET_VALUE = 15'h1234
) (
  input logic clk,
  output logic [14:0] direct = RESET_VALUE,
  output logic [14:0] summary_only = RESET_VALUE ^ 15'h1555
); `SUBGRAPH_BOUNDARY

  always_ff @(posedge clk) begin
    direct <= {direct[12:0], direct[14:13]} ^ 15'h2105;
  end

endmodule

module sg_contract_output_ref #(
  parameter logic [14:0] RESET_VALUE = 15'h1234
) (
  input logic clk,
  output logic [14:0] direct = RESET_VALUE,
  output logic [14:0] summary_only = RESET_VALUE ^ 15'h1555
);

  always_ff @(posedge clk) begin
    direct <= {direct[12:0], direct[14:13]} ^ 15'h2105;
  end

endmodule
