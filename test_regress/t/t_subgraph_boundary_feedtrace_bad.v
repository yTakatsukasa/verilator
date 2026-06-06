// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Yutetsu TAKATSUKASA
// SPDX-License-Identifier: Unlicense

module t (
  input  logic clk,
  input  logic in,
  output logic out
);

  logic concat_out;
  logic child_direct_out;
  logic func_arg_out;
  logic func_cond_out;
  logic func_out;
  logic func_named_arg_out;
  logic grandchild_direct_out;

  bad_concat_feedthrough i_bad_concat (.clk(clk), .in(in), .out(concat_out));
  bad_child_direct_feedthrough i_bad_child_direct (.clk(clk), .in(in), .out(child_direct_out));
  bad_func_arg_feedthrough i_bad_func_arg (.clk(clk), .in(in), .out(func_arg_out));
  bad_func_cond_feedthrough i_bad_func_cond (.clk(clk), .in(in), .out(func_cond_out));
  bad_func_feedthrough i_bad_func (.clk(clk), .in(in), .out(func_out));
  bad_func_named_arg_feedthrough i_bad_func_named_arg (
    .clk(clk),
    .in(in),
    .out(func_named_arg_out)
  );
  bad_grandchild_direct_feedthrough i_bad_grandchild_direct (
    .clk(clk),
    .in(in),
    .out(grandchild_direct_out)
  );

  assign out = concat_out ^ child_direct_out ^ func_arg_out ^ func_cond_out ^ func_out
             ^ func_named_arg_out ^ grandchild_direct_out;

endmodule

module bad_concat_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic unused;

  bad_concat_child i_child (.clk(clk), .in(in), .out({unused, out}));

endmodule

module bad_concat_child (
  input  logic clk,
  input  logic in,
  output logic [1:0] out
);

  logic q;

  always_ff @(posedge clk) q <= in;
  assign out = {q, in};

endmodule

module bad_func_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic q;

  function automatic logic tap();
    tap = in;
  endfunction

  always_ff @(posedge clk) q <= in;
  assign out = q ^ tap();

endmodule

module bad_func_arg_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic q;

  function automatic logic tap(input logic value);
    tap = value;
  endfunction

  always_ff @(posedge clk) q <= in;
  assign out = q ^ tap(in);

endmodule

module bad_func_cond_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic q;

  function automatic logic gate();
    gate = in;
  endfunction

  always_ff @(posedge clk) q <= in;
  assign out = gate() ? q : 1'b0;

endmodule

module bad_func_named_arg_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  logic q;

  function automatic logic tap(input logic value);
    tap = value;
  endfunction

  always_ff @(posedge clk) q <= in;
  assign out = q ^ tap(.value(in));

endmodule

module bad_child_direct_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  bad_child_direct_leaf i_child (.clk(clk), .in(in), .out(out));

endmodule

module bad_child_direct_leaf (
  input  logic clk,
  input  logic in,
  output logic out
);

  logic q;

  always_ff @(posedge clk) q <= in;
  assign out = q | in;

endmodule

module bad_grandchild_direct_feedthrough (
  input  logic clk,
  input  logic in,
  output logic out
); /*verilator subgraph_boundary*/

  bad_grandchild_direct_middle i_middle (.clk(clk), .in(in), .out(out));

endmodule

module bad_grandchild_direct_middle (
  input  logic clk,
  input  logic in,
  output logic out
);

  bad_grandchild_direct_leaf i_leaf (.clk(clk), .in(in), .out(out));

endmodule

module bad_grandchild_direct_leaf (
  input  logic clk,
  input  logic in,
  output logic out
);

  logic q;

  always_ff @(posedge clk) q <= in;
  assign out = q & in;

endmodule
