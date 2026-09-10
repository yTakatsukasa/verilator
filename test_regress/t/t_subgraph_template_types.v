// DESCRIPTION: Verilator: Subgraph template SystemVerilog type semantics
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

`timescale 1ns/1ns

// verilog_format: off
`define stop $stop
`define checkh(gotv,expv) do if ((gotv) !== (expv)) begin $write("%%Error: %s:%0d: %m at %0t: got=%0x exp=%0x (%s !== %s)\n", `__FILE__, `__LINE__, $time, (gotv), (expv), `"gotv`", `"expv`" ); `stop; end while (0);
// verilog_format: on

package sg_template_types_pkg;
  typedef enum logic [2:0] {
    IDLE = 3'd0,
    LOAD = 3'd2,
    RUN = 3'd5,
    DONE = 3'd7
  } state_t;
  typedef state_t state_alias_t;
  typedef logic signed [6:0] signed_word_t;
  typedef struct packed {
    state_alias_t state;
    signed_word_t delta;
    logic [4:0] flags;
  } packed_state_t;
  typedef packed_state_t packed_alias_t;
  typedef struct {
    logic [6:0] data;
    state_alias_t state;
  } unpacked_state_t;
endpackage

module t;
  import sg_template_types_pkg::*;

  logic clk = 0;
  logic reset = 1;
  packed_alias_t in_p;
  unpacked_state_t in_u;
  packed_alias_t got_p;
  packed_alias_t expected_p;
  unpacked_state_t got_u;
  unpacked_state_t expected_u;
  int cyc = 0;

  always #5 clk = ~clk;

  sg_template_types dut (.*,
                         .out_p(got_p),
                         .out_u(got_u));
  sg_template_types_ref ref_dut (.*,
                                 .out_p(expected_p),
                                 .out_u(expected_u));

  always @(negedge clk) begin
    `checkh(got_p.state, expected_p.state)
    `checkh(got_p.delta, expected_p.delta)
    `checkh(got_p.flags, expected_p.flags)
    `checkh(got_u.data, expected_u.data)
    `checkh(got_u.state, expected_u.state)
    cyc <= cyc + 1;
    reset <= cyc == 0 || cyc == 37;
    in_p.state <= state_alias_t'(cyc[2:0] ^ 3'b101);
    in_p.delta <= signed_word_t'({cyc[5:0], 1'b1});
    in_p.flags <= cyc[4:0] ^ 5'h13;
    in_u.data <= cyc[6:0] ^ 7'h35;
    in_u.state <= state_alias_t'({cyc[1:0], cyc[4]});
    if (cyc == 63) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end
endmodule

module sg_template_types (
  input logic clk,
  input logic reset,
  input sg_template_types_pkg::packed_alias_t in_p,
  input sg_template_types_pkg::unpacked_state_t in_u,
  output sg_template_types_pkg::packed_alias_t out_p,
  output sg_template_types_pkg::unpacked_state_t out_u
);
  import sg_template_types_pkg::*;
  /*verilator subgraph_boundary*/
  packed_alias_t q;
  unpacked_state_t uq;

  always @(posedge clk or posedge reset) begin
    if (reset) begin
      q <= '{state: IDLE, delta: signed_word_t'(7'sd3), flags: 5'h12};
      uq <= '{data: 7'h21, state: LOAD};
    end else begin
      q.state <= in_p.state;
      q.delta <= q.delta + in_p.delta;
      q.flags <= q.flags ^ in_p.flags;
      uq.data <= uq.data + in_u.data + q.delta;
      uq.state <= q.state;
    end
  end

  assign out_p = q;
  assign out_u = uq;
endmodule

module sg_template_types_ref (
  input logic clk,
  input logic reset,
  input sg_template_types_pkg::packed_alias_t in_p,
  input sg_template_types_pkg::unpacked_state_t in_u,
  output sg_template_types_pkg::packed_alias_t out_p,
  output sg_template_types_pkg::unpacked_state_t out_u
);
  import sg_template_types_pkg::*;
  packed_alias_t q;
  unpacked_state_t uq;

  always @(posedge clk or posedge reset) begin
    if (reset) begin
      q <= '{state: IDLE, delta: signed_word_t'(7'sd3), flags: 5'h12};
      uq <= '{data: 7'h21, state: LOAD};
    end else begin
      q.state <= in_p.state;
      q.delta <= q.delta + in_p.delta;
      q.flags <= q.flags ^ in_p.flags;
      uq.data <= uq.data + in_u.data + q.delta;
      uq.state <= q.state;
    end
  end

  assign out_p = q;
  assign out_u = uq;
endmodule
