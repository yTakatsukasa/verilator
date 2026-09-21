// DESCRIPTION: Verilator: Subgraph NBA capture, evaluate, and publish ordering
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

module t (
  input  logic       clk,
  input  logic       reset,
  input  logic [6:0] data,
  output logic [6:0] serial0,
  output logic [6:0] serial1,
  output logic [6:0] ring_a,
  output logic [6:0] ring_b,
  output logic [6:0] parent_q = 7'd9,
  output logic [6:0] combo
);

  logic [6:0] serial0_in;
  logic [6:0] serial1_in;
  logic [6:0] ring_a_in;
  logic [6:0] ring_b_in;

  assign serial0_in = data + 7'd3;
  assign serial1_in = serial0 ^ 7'h2a;
  assign ring_a_in = ring_b + parent_q;
  assign ring_b_in = ring_a ^ data;
  assign combo = (serial1 + ring_b) ^ data;

  always_ff @(posedge clk) begin
    if (reset) parent_q <= 7'd14;
    else parent_q <= ring_a + data;
  end

  sg_phase_ff #(.INIT(7'd1), .RESET_VALUE(7'd10)) i_serial0 (
    .clk(clk),
    .reset(reset),
    .d(serial0_in),
    .q(serial0)
  );
  sg_phase_ff #(.INIT(7'd2), .RESET_VALUE(7'd11)) i_serial1 (
    .clk(clk),
    .reset(reset),
    .d(serial1_in),
    .q(serial1)
  );
  sg_phase_ff #(.INIT(7'd4), .RESET_VALUE(7'd12)) i_ring_a (
    .clk(clk),
    .reset(reset),
    .d(ring_a_in),
    .q(ring_a)
  );
  sg_phase_ff #(.INIT(7'd5), .RESET_VALUE(7'd13)) i_ring_b (
    .clk(clk),
    .reset(reset),
    .d(ring_b_in),
    .q(ring_b)
  );

endmodule

module sg_phase_ff #(
  parameter logic [6:0] INIT = 7'd0,
  parameter logic [6:0] RESET_VALUE = 7'd0
) (
  input  logic       clk,
  input  logic       reset,
  input  logic [6:0] d,
  output logic [6:0] q
);
  /*verilator subgraph_boundary*/

  logic [6:0] state = INIT;

  always_ff @(posedge clk) begin
    if (reset) state <= RESET_VALUE;
    else state <= d;
  end

  assign q = state;

endmodule
