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
  logic rst_n;
  logic [7:0] a;
  logic [7:0] b;
  logic [15:0] y0;
  logic [15:0] y1;
  logic [15:0] y2;
  logic [15:0] y3;
  logic [15:0] y4;
  logic [15:0] y5;
  logic [15:0] ref0;
  logic [15:0] ref1;
  logic [15:0] ref2;
  logic [15:0] ref3;
  logic [15:0] ref4;
  logic [15:0] ref5;

  sg_node i_sg0 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a),
    .b_i(b),
    .y_o(y0)
  );

  sg_node i_sg1 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a + 8'h13),
    .b_i({b[3:0], b[7:4]}),
    .y_o(y1)
  );

  sg_node i_sg2 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a),
    .b_i(b),
    .y_o(y2)
  );

  sg_node_ref i_ref0 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a),
    .b_i(b),
    .y_o(ref0)
  );

  sg_node_ref i_ref1 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a + 8'h13),
    .b_i({b[3:0], b[7:4]}),
    .y_o(ref1)
  );

  sg_node_ref i_ref2 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a),
    .b_i(b),
    .y_o(ref2)
  );

  sg_node_blocking i_sg3 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a),
    .b_i(b),
    .y_o(y3)
  );

  sg_node_blocking i_sg4 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a),
    .b_i(b),
    .y_o(y4)
  );

  sg_node_blocking i_sg5 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a),
    .b_i(b),
    .y_o(y5)
  );

  sg_node_blocking_ref i_ref3 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a),
    .b_i(b),
    .y_o(ref3)
  );

  sg_node_blocking_ref i_ref4 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a),
    .b_i(b),
    .y_o(ref4)
  );

  sg_node_blocking_ref i_ref5 (
    .clk(clk),
    .rst_n(rst_n),
    .a_i(a),
    .b_i(b),
    .y_o(ref5)
  );

  always_ff @(posedge clk) begin
    cyc <= cyc + 1;
    rst_n <= cyc > 1;
    a <= {a[6:0], a[7] ^ a[5] ^ a[4] ^ a[3]};
    b <= b + 8'h29;

    if (cyc == 0) begin
      rst_n <= 1'b0;
      a <= 8'h5a;
      b <= 8'hc3;
    end
    else if (cyc > 3) begin
      `checkh(y0, ref0);
      `checkh(y1, ref1);
      `checkh(y2, ref2);
      `checkh(y3, ref3);
      `checkh(y4, ref4);
      `checkh(y5, ref5);
    end

    if (cyc == 40) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_node_blocking (
  input  logic       clk,
  input  logic       rst_n,
  input  logic [7:0] a_i,
  input  logic [7:0] b_i,
  output logic [15:0] y_o
); `SUBGRAPH_BOUNDARY

  logic [15:0] q;

  assign y_o = q;

  always_ff @(posedge clk) begin
    if (rst_n) begin
      q <= {q[6:0], q[15:7]} ^ 16'h3142;
    end
    else begin
      q <= 16'h8241;
    end
  end

endmodule

module sg_node_blocking_ref (
  input  logic       clk,
  input  logic       rst_n,
  input  logic [7:0] a_i,
  input  logic [7:0] b_i,
  output logic [15:0] y_o
);

  logic [15:0] q;

  assign y_o = q;

  always_ff @(posedge clk) begin
    if (rst_n) begin
      q <= {q[6:0], q[15:7]} ^ 16'h3142;
    end
    else begin
      q <= 16'h8241;
    end
  end

endmodule

module sg_node (
  input  logic       clk,
  input  logic       rst_n,
  input  logic [7:0] a_i,
  input  logic [7:0] b_i,
  output logic [15:0] y_o
); `SUBGRAPH_BOUNDARY

  logic [7:0] acc_q;
  logic [7:0] mix_q;
  logic [15:0] tag_q;

  assign y_o = tag_q ^ {acc_q, mix_q};

  always_ff @(posedge clk) begin
    if (rst_n) begin
      acc_q <= (acc_q + a_i) ^ {b_i[2:0], b_i[7:3]};
      mix_q <= (mix_q ^ b_i) + {a_i[4:0], a_i[7:5]};
      tag_q <= {tag_q[10:0], tag_q[15:11]} ^ {acc_q, mix_q} ^ {a_i, b_i};
    end
    else begin
      acc_q <= 8'h11;
      mix_q <= 8'h73;
      tag_q <= 16'h4255;
    end
  end

endmodule

module sg_node_ref (
  input  logic       clk,
  input  logic       rst_n,
  input  logic [7:0] a_i,
  input  logic [7:0] b_i,
  output logic [15:0] y_o
);

  logic [7:0] acc_q;
  logic [7:0] mix_q;
  logic [15:0] tag_q;

  assign y_o = tag_q ^ {acc_q, mix_q};

  always_ff @(posedge clk) begin
    if (rst_n) begin
      acc_q <= (acc_q + a_i) ^ {b_i[2:0], b_i[7:3]};
      mix_q <= (mix_q ^ b_i) + {a_i[4:0], a_i[7:5]};
      tag_q <= {tag_q[10:0], tag_q[15:11]} ^ {acc_q, mix_q} ^ {a_i, b_i};
    end
    else begin
      acc_q <= 8'h11;
      mix_q <= 8'h73;
      tag_q <= 16'h4255;
    end
  end

endmodule
