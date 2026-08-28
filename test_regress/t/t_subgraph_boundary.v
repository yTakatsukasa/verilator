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
  logic [3:0] hi;
  logic [3:0] lo;
} packed_pair_t;

package subgraph_param_pkg;
  parameter logic [7:0] PKG_BIAS = 8'ha7;
  localparam int unsigned PKG_TOP = 5;
  localparam int unsigned PKG_MID = 3;
  localparam int unsigned PKG_LOW = PKG_TOP - PKG_MID + 1;
endpackage

module t (
  input logic clk
);

  int cyc;
  logic [7:0] in;
  logic [191:0] out0;
  logic [191:0] out1;
  logic [191:0] ref0;
  logic [191:0] ref1;
  logic [7:0] out_pkg_param_const0;
  logic [7:0] out_pkg_param_const1;
  logic [7:0] out_pkg_param_partsel0;
  logic [7:0] out_pkg_param_partsel1;
  logic [7:0] ref_pkg_param_const0;
  logic [7:0] ref_pkg_param_const1;
  logic [7:0] ref_pkg_param_partsel0;
  logic [7:0] ref_pkg_param_partsel1;

  sub i_sub0 (.clk(clk), .in(in), .out_child_comb(out0[95:88]), .out_child_direct(out0[87:80]), .out_func_default(out0[79:72]), .out_func_local_init(out0[71:64]), .out_func_return(out0[63:56]), .out_gen_always_comb(out0[55:48]), .out_gen_assign(out0[47:40]), .out_gen_child(out0[39:32]), .out_gen_concat(out0[111:104]), .out_gen_localparam_const(out0[159:152]), .out_gen_localparam_partsel(out0[167:160]), .out_gen_packed_func(out0[103:96]), .out_gen_param_const(out0[143:136]), .out_gen_param_partsel(out0[151:144]), .out_gen_partsel(out0[119:112]), .out_gen_pkg_param_const(out_pkg_param_const0), .out_gen_pkg_param_partsel(out_pkg_param_partsel0), .out_gen_struct(out0[127:120]), .out_gen_sysconst(out0[175:168]), .out_gen_sysrepl(out0[183:176]), .out_gen_sysrepl_outbits(out0[191:184]), .out_gen_unpacked(out0[135:128]), .out_genvar_value(out0[31:24]), .out_reg_child_comb(out0[23:16]), .out_reg_child_func(out0[15:8]), .out_reg_comb(out0[7:0]));
  sub i_sub1 (.clk(clk), .in(in + 8'd1), .out_child_comb(out1[95:88]), .out_child_direct(out1[87:80]), .out_func_default(out1[79:72]), .out_func_local_init(out1[71:64]), .out_func_return(out1[63:56]), .out_gen_always_comb(out1[55:48]), .out_gen_assign(out1[47:40]), .out_gen_child(out1[39:32]), .out_gen_concat(out1[111:104]), .out_gen_localparam_const(out1[159:152]), .out_gen_localparam_partsel(out1[167:160]), .out_gen_packed_func(out1[103:96]), .out_gen_param_const(out1[143:136]), .out_gen_param_partsel(out1[151:144]), .out_gen_partsel(out1[119:112]), .out_gen_pkg_param_const(out_pkg_param_const1), .out_gen_pkg_param_partsel(out_pkg_param_partsel1), .out_gen_struct(out1[127:120]), .out_gen_sysconst(out1[175:168]), .out_gen_sysrepl(out1[183:176]), .out_gen_sysrepl_outbits(out1[191:184]), .out_gen_unpacked(out1[135:128]), .out_genvar_value(out1[31:24]), .out_reg_child_comb(out1[23:16]), .out_reg_child_func(out1[15:8]), .out_reg_comb(out1[7:0]));
  sub_ref i_ref0 (.clk(clk), .in(in), .out_child_comb(ref0[95:88]), .out_child_direct(ref0[87:80]), .out_func_default(ref0[79:72]), .out_func_local_init(ref0[71:64]), .out_func_return(ref0[63:56]), .out_gen_always_comb(ref0[55:48]), .out_gen_assign(ref0[47:40]), .out_gen_child(ref0[39:32]), .out_gen_concat(ref0[111:104]), .out_gen_localparam_const(ref0[159:152]), .out_gen_localparam_partsel(ref0[167:160]), .out_gen_packed_func(ref0[103:96]), .out_gen_param_const(ref0[143:136]), .out_gen_param_partsel(ref0[151:144]), .out_gen_partsel(ref0[119:112]), .out_gen_pkg_param_const(ref_pkg_param_const0), .out_gen_pkg_param_partsel(ref_pkg_param_partsel0), .out_gen_struct(ref0[127:120]), .out_gen_sysconst(ref0[175:168]), .out_gen_sysrepl(ref0[183:176]), .out_gen_sysrepl_outbits(ref0[191:184]), .out_gen_unpacked(ref0[135:128]), .out_genvar_value(ref0[31:24]), .out_reg_child_comb(ref0[23:16]), .out_reg_child_func(ref0[15:8]), .out_reg_comb(ref0[7:0]));
  sub_ref i_ref1 (.clk(clk), .in(in + 8'd1), .out_child_comb(ref1[95:88]), .out_child_direct(ref1[87:80]), .out_func_default(ref1[79:72]), .out_func_local_init(ref1[71:64]), .out_func_return(ref1[63:56]), .out_gen_always_comb(ref1[55:48]), .out_gen_assign(ref1[47:40]), .out_gen_child(ref1[39:32]), .out_gen_concat(ref1[111:104]), .out_gen_localparam_const(ref1[159:152]), .out_gen_localparam_partsel(ref1[167:160]), .out_gen_packed_func(ref1[103:96]), .out_gen_param_const(ref1[143:136]), .out_gen_param_partsel(ref1[151:144]), .out_gen_partsel(ref1[119:112]), .out_gen_pkg_param_const(ref_pkg_param_const1), .out_gen_pkg_param_partsel(ref_pkg_param_partsel1), .out_gen_struct(ref1[127:120]), .out_gen_sysconst(ref1[175:168]), .out_gen_sysrepl(ref1[183:176]), .out_gen_sysrepl_outbits(ref1[191:184]), .out_gen_unpacked(ref1[135:128]), .out_genvar_value(ref1[31:24]), .out_reg_child_comb(ref1[23:16]), .out_reg_child_func(ref1[15:8]), .out_reg_comb(ref1[7:0]));

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    in <= {in[6:0], in[7] ^ in[5] ^ in[4] ^ in[3]};

    if (cyc == 0) begin
      in <= 8'h5a;
    end
    else begin
      `checkh(out0, ref0);
      `checkh(out1, ref1);
      `checkh(out_pkg_param_const0, ref_pkg_param_const0);
      `checkh(out_pkg_param_const1, ref_pkg_param_const1);
      `checkh(out_pkg_param_partsel0, ref_pkg_param_partsel0);
      `checkh(out_pkg_param_partsel1, ref_pkg_param_partsel1);
    end

    if (cyc == 32) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sub #(
  parameter logic [7:0] PARAM_BIAS = 8'h3c,
  parameter int unsigned PARAM_MID = 4,
  parameter int unsigned PARAM_TOP = 6
) (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out_child_comb,
  output logic [7:0] out_child_direct,
  output logic [7:0] out_func_default,
  output logic [7:0] out_func_local_init,
  output logic [7:0] out_func_return,
  output logic [7:0] out_gen_always_comb,
  output logic [7:0] out_gen_assign,
  output logic [7:0] out_gen_child,
  output logic [7:0] out_gen_concat,
  output logic [7:0] out_gen_localparam_const,
  output logic [7:0] out_gen_localparam_partsel,
  output logic [7:0] out_gen_packed_func,
  output logic [7:0] out_gen_param_const,
  output logic [7:0] out_gen_param_partsel,
  output logic [7:0] out_gen_partsel,
  output logic [7:0] out_gen_pkg_param_const,
  output logic [7:0] out_gen_pkg_param_partsel,
  output logic [7:0] out_gen_struct,
  output logic [7:0] out_gen_sysconst,
  output logic [7:0] out_gen_sysrepl,
  output logic [7:0] out_gen_sysrepl_outbits,
  output logic [7:0] out_gen_unpacked,
  output logic [7:0] out_genvar_value,
  output logic [7:0] out_reg_child_comb,
  output logic [7:0] out_reg_child_func,
  output logic [7:0] out_reg_comb
); `SUBGRAPH_BOUNDARY
  import subgraph_param_pkg::*;

  logic [7:0] child_comb_out;
  logic [7:0] gen_child_out [2];
  logic [1:0] gen_unpacked [4];
  packed_pair_t gen_struct;
  logic [7:0] q;
  logic [7:0] q_child_in;
  logic [7:0] reg_child_out;
  logic [7:0] reg_child_func_out;
  logic [7:0] leaf_out;
  localparam int unsigned PARAM_LOW = PARAM_TOP - PARAM_MID + 1;

  sub_flop i_child_comb (.clk(clk), .in(in ^ 8'h3c), .out(child_comb_out));
  sub_flop i_child_direct (.clk(clk), .in(in + 8'd7), .out(out_child_direct));
  sub_leaf i_reg_child (.in(q_child_in), .out(reg_child_out));
  sub_leaf_func i_reg_child_func (.in(q_child_in), .out(reg_child_func_out));
  sub_leaf i_leaf (.in(in), .out(leaf_out));

  for (genvar gi = 0; gi < 2; ++gi) begin : gen_child
    localparam logic [7:0] LOC = gi * 8'h33;
    sub_leaf_func i_leaf_gen (.in(q_child_in ^ LOC), .out(gen_child_out[gi]));
  end

  for (genvar gi = 0; gi < 8; ++gi) begin : gen_assign
    assign out_gen_assign[gi] = q[gi] ^ q[(gi + 3) % 8];
  end

  for (genvar gi = 0; gi < 8; ++gi) begin : gen_assign_genvar_value
    assign out_genvar_value[gi] = q[(gi + 1) % 8] ^ ((gi + 1) < 5);
  end

  for (genvar gi = 0; gi < 8; ++gi) begin : gen_always
    always_comb out_gen_always_comb[gi] = q[gi] ^ q[(gi + 5) % 8] ^ 1'b1;
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_concat_assign
    assign {out_gen_concat[(2 * gi) + 1], out_gen_concat[2 * gi]} = {2{q[(gi + 2) % 8]}};
  end

  for (genvar gi = 0; gi < 8; ++gi) begin : gen_localparam_const_assign
    localparam int unsigned IDX = (gi + 2) % 8;
    localparam logic MIX = ((gi * 3) & 1) != 0;
    assign out_gen_localparam_const[gi] = q[IDX] ^ MIX;
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_localparam_partsel_assign
    localparam int unsigned TOP = 7 - (2 * gi);
    localparam int unsigned W = 2;
    assign out_gen_localparam_partsel[2 * gi +: 2] = q[TOP -: W] ^ {2{TOP[0]}};
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_packed_func_assign
    assign out_gen_packed_func[2 * gi +: 2] = packmix(q ^ (gi * 8'h11), gi).lo[1:0];
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_partsel_assign
    assign out_gen_partsel[2 * gi +: 2] = q[2 * gi +: 2] ^ {2{gi[0]}};
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_struct_assign
    assign gen_struct.hi[gi] = q[gi] ^ gi[0];
    assign gen_struct.lo[gi] = q[gi + 4] ^ ~gi[0];
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_unpacked_assign
    assign gen_unpacked[gi] = q[2 * gi +: 2] ^ gi[1:0];
  end

  function automatic logic [7:0] rotmix(input logic [7:0] value);
    return {value[2:0], value[7:3]} ^ 8'h96;
  endfunction

  function automatic logic [7:0] mix_local_init(input logic [7:0] value);
    logic [7:0] biased = value ^ 8'hc3;
    return {biased[0], biased[7:1]} + 8'h12;
  endfunction

  function automatic logic [7:0] add_bias(input logic [7:0] value, input logic [7:0] bias = 8'h2d);
    return value + bias;
  endfunction

  function automatic packed_pair_t packmix(input logic [7:0] value,
                                           input logic [7:0] bias = 8'h12);
    packed_pair_t pair;
    pair.hi = value[7:4] ^ bias[3:0];
    pair.lo = value[3:0] + bias[7:4];
    return pair;
  endfunction

  always_ff @(posedge clk) q <= leaf_out;
  always_ff @(posedge clk) q_child_in <= in - 8'd9;
  assign out_child_comb = {child_comb_out[0], child_comb_out[7:1]} ^ 8'h69;
  assign out_func_default = add_bias(q);
  assign out_func_local_init = mix_local_init(q);
  assign out_func_return = rotmix(q);
  assign out_gen_child = gen_child_out[0] ^ {gen_child_out[1][6:0], gen_child_out[1][7]};
  assign out_gen_param_const = q ^ PARAM_BIAS;
  assign out_gen_param_partsel = {q[PARAM_TOP -: PARAM_MID], q[PARAM_LOW +: (8 - PARAM_MID)]}
                                 ^ {PARAM_BIAS[PARAM_TOP -: PARAM_MID],
                                    PARAM_BIAS[0 +: (8 - PARAM_MID)]};
  assign out_gen_pkg_param_const = q ^ PKG_BIAS;
  assign out_gen_pkg_param_partsel = {q[PKG_TOP -: PKG_MID], q[PKG_LOW +: (8 - PKG_MID)]}
                                     ^ {PKG_BIAS[PKG_TOP -: PKG_MID],
                                        PKG_BIAS[0 +: (8 - PKG_MID)]};
  assign out_gen_struct = gen_struct;
  assign out_gen_sysconst = {q[$left(q) -: 4], q[$right(q) +: 3], $bits(q) == 8} ^ 8'h5c;
  assign out_gen_sysrepl = {$bits(q){q[0]}} ^ 8'h3a;
  assign out_gen_sysrepl_outbits = {$bits(out_gen_sysrepl_outbits){q[0]}} & q;
  assign out_gen_unpacked = {gen_unpacked[3], gen_unpacked[2], gen_unpacked[1], gen_unpacked[0]};
  assign out_reg_child_comb = reg_child_out ^ 8'h42;
  assign out_reg_child_func = reg_child_func_out ^ 8'h24;
  assign out_reg_comb = {q[6:0], q[7]} ^ 8'h11;

endmodule

module sub_ref #(
  parameter logic [7:0] PARAM_BIAS = 8'h3c,
  parameter int unsigned PARAM_MID = 4,
  parameter int unsigned PARAM_TOP = 6
) (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out_child_comb,
  output logic [7:0] out_child_direct,
  output logic [7:0] out_func_default,
  output logic [7:0] out_func_local_init,
  output logic [7:0] out_func_return,
  output logic [7:0] out_gen_always_comb,
  output logic [7:0] out_gen_assign,
  output logic [7:0] out_gen_child,
  output logic [7:0] out_gen_concat,
  output logic [7:0] out_gen_localparam_const,
  output logic [7:0] out_gen_localparam_partsel,
  output logic [7:0] out_gen_packed_func,
  output logic [7:0] out_gen_param_const,
  output logic [7:0] out_gen_param_partsel,
  output logic [7:0] out_gen_partsel,
  output logic [7:0] out_gen_pkg_param_const,
  output logic [7:0] out_gen_pkg_param_partsel,
  output logic [7:0] out_gen_struct,
  output logic [7:0] out_gen_sysconst,
  output logic [7:0] out_gen_sysrepl,
  output logic [7:0] out_gen_sysrepl_outbits,
  output logic [7:0] out_gen_unpacked,
  output logic [7:0] out_genvar_value,
  output logic [7:0] out_reg_child_comb,
  output logic [7:0] out_reg_child_func,
  output logic [7:0] out_reg_comb
);
  import subgraph_param_pkg::*;

  logic [7:0] child_comb_out;
  logic [7:0] gen_child_out [2];
  logic [1:0] gen_unpacked [4];
  packed_pair_t gen_struct;
  logic [7:0] q;
  logic [7:0] q_child_in;
  logic [7:0] reg_child_out;
  logic [7:0] reg_child_func_out;
  logic [7:0] leaf_out;
  localparam int unsigned PARAM_LOW = PARAM_TOP - PARAM_MID + 1;

  sub_flop i_child_comb (.clk(clk), .in(in ^ 8'h3c), .out(child_comb_out));
  sub_flop i_child_direct (.clk(clk), .in(in + 8'd7), .out(out_child_direct));
  sub_leaf i_reg_child (.in(q_child_in), .out(reg_child_out));
  sub_leaf_func i_reg_child_func (.in(q_child_in), .out(reg_child_func_out));
  sub_leaf i_leaf (.in(in), .out(leaf_out));

  for (genvar gi = 0; gi < 2; ++gi) begin : gen_child
    sub_leaf_func i_leaf_gen (.in(q_child_in ^ (gi * 8'h33)), .out(gen_child_out[gi]));
  end

  for (genvar gi = 0; gi < 8; ++gi) begin : gen_assign
    assign out_gen_assign[gi] = q[gi] ^ q[(gi + 3) % 8];
  end

  for (genvar gi = 0; gi < 8; ++gi) begin : gen_assign_genvar_value
    assign out_genvar_value[gi] = q[(gi + 1) % 8] ^ ((gi + 1) < 5);
  end

  for (genvar gi = 0; gi < 8; ++gi) begin : gen_always
    always_comb out_gen_always_comb[gi] = q[gi] ^ q[(gi + 5) % 8] ^ 1'b1;
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_concat_assign
    assign {out_gen_concat[(2 * gi) + 1], out_gen_concat[2 * gi]} = {2{q[(gi + 2) % 8]}};
  end

  for (genvar gi = 0; gi < 8; ++gi) begin : gen_localparam_const_assign
    localparam int unsigned IDX = (gi + 2) % 8;
    localparam logic MIX = ((gi * 3) & 1) != 0;
    assign out_gen_localparam_const[gi] = q[IDX] ^ MIX;
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_localparam_partsel_assign
    localparam int unsigned TOP = 7 - (2 * gi);
    localparam int unsigned W = 2;
    assign out_gen_localparam_partsel[2 * gi +: 2] = q[TOP -: W] ^ {2{TOP[0]}};
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_packed_func_assign
    assign out_gen_packed_func[2 * gi +: 2] = packmix(q ^ (gi * 8'h11), gi).lo[1:0];
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_partsel_assign
    assign out_gen_partsel[2 * gi +: 2] = q[2 * gi +: 2] ^ {2{gi[0]}};
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_struct_assign
    assign gen_struct.hi[gi] = q[gi] ^ gi[0];
    assign gen_struct.lo[gi] = q[gi + 4] ^ ~gi[0];
  end

  for (genvar gi = 0; gi < 4; ++gi) begin : gen_unpacked_assign
    assign gen_unpacked[gi] = q[2 * gi +: 2] ^ gi[1:0];
  end

  function automatic logic [7:0] rotmix(input logic [7:0] value);
    return {value[2:0], value[7:3]} ^ 8'h96;
  endfunction

  function automatic logic [7:0] mix_local_init(input logic [7:0] value);
    logic [7:0] biased = value ^ 8'hc3;
    return {biased[0], biased[7:1]} + 8'h12;
  endfunction

  function automatic logic [7:0] add_bias(input logic [7:0] value, input logic [7:0] bias = 8'h2d);
    return value + bias;
  endfunction

  function automatic packed_pair_t packmix(input logic [7:0] value,
                                           input logic [7:0] bias = 8'h12);
    packed_pair_t pair;
    pair.hi = value[7:4] ^ bias[3:0];
    pair.lo = value[3:0] + bias[7:4];
    return pair;
  endfunction

  always_ff @(posedge clk) q <= leaf_out;
  always_ff @(posedge clk) q_child_in <= in - 8'd9;
  assign out_child_comb = {child_comb_out[0], child_comb_out[7:1]} ^ 8'h69;
  assign out_func_default = add_bias(q);
  assign out_func_local_init = mix_local_init(q);
  assign out_func_return = rotmix(q);
  assign out_gen_child = gen_child_out[0] ^ {gen_child_out[1][6:0], gen_child_out[1][7]};
  assign out_gen_param_const = q ^ PARAM_BIAS;
  assign out_gen_param_partsel = {q[PARAM_TOP -: PARAM_MID], q[PARAM_LOW +: (8 - PARAM_MID)]}
                                 ^ {PARAM_BIAS[PARAM_TOP -: PARAM_MID],
                                    PARAM_BIAS[0 +: (8 - PARAM_MID)]};
  assign out_gen_pkg_param_const = q ^ PKG_BIAS;
  assign out_gen_pkg_param_partsel = {q[PKG_TOP -: PKG_MID], q[PKG_LOW +: (8 - PKG_MID)]}
                                     ^ {PKG_BIAS[PKG_TOP -: PKG_MID],
                                        PKG_BIAS[0 +: (8 - PKG_MID)]};
  assign out_gen_struct = gen_struct;
  assign out_gen_sysconst = {q[$left(q) -: 4], q[$right(q) +: 3], $bits(q) == 8} ^ 8'h5c;
  assign out_gen_sysrepl = {$bits(q){q[0]}} ^ 8'h3a;
  assign out_gen_sysrepl_outbits = {$bits(out_gen_sysrepl_outbits){q[0]}} & q;
  assign out_gen_unpacked = {gen_unpacked[3], gen_unpacked[2], gen_unpacked[1], gen_unpacked[0]};
  assign out_reg_child_comb = reg_child_out ^ 8'h42;
  assign out_reg_child_func = reg_child_func_out ^ 8'h24;
  assign out_reg_comb = {q[6:0], q[7]} ^ 8'h11;

endmodule

module sub_flop (
  input logic clk,
  input logic [7:0] in,
  output logic [7:0] out
);

  always_ff @(posedge clk) out <= in;

endmodule

module sub_leaf (
  input logic [7:0] in,
  output logic [7:0] out
);

  assign out = {in[3:0], in[7:4]} ^ 8'ha5;

endmodule

module sub_leaf_func (
  input logic [7:0] in,
  output logic [7:0] out
);

  function automatic logic [7:0] remix(input logic [7:0] value);
    logic [7:0] biased = value ^ 8'h3f;
    return {biased[1:0], biased[7:2]} - 8'h05;
  endfunction

  assign out = remix(in);

endmodule
