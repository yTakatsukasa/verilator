// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
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

  logic [31:0] master_lfsr_q;
  logic [31:0] master_mix_q;

  logic dev0_rd;
  logic dev0_wr;
  logic dev1_rd;
  logic dev1_wr;
  logic [31:0] dev0_wdata;
  logic [31:0] dev1_wdata;

  logic dev0_irq_ref;
  logic dev0_irq_sub;
  logic dev1_irq_ref;
  logic dev1_irq_sub;
  logic dev0_rvalid_ref;
  logic dev0_rvalid_sub;
  logic dev1_rvalid_ref;
  logic dev1_rvalid_sub;
  logic [31:0] dev0_rdata_ref;
  logic [31:0] dev0_rdata_sub;
  logic [31:0] dev1_rdata_ref;
  logic [31:0] dev1_rdata_sub;

  logic [31:0] cpu_acc_ref;
  logic [31:0] cpu_acc_sub;
  logic [31:0] cpu_hist_ref;
  logic [31:0] cpu_hist_sub;
  logic [15:0] irq_hist_ref;
  logic [15:0] irq_hist_sub;

  logic [31:0] unused_ref;
  logic [31:0] unused_sub;

  function automatic logic [31:0] lfsr32(input logic [31:0] s);
    lfsr32 = {s[30:0], s[31] ^ s[21] ^ s[1] ^ s[0]};
  endfunction

  function automatic logic [31:0] rol32(input logic [31:0] s, input int unsigned amount);
    logic [63:0] dbl;
    dbl = {s, s};
    return dbl[amount +: 32];
  endfunction

  always_comb begin
    dev0_wr = (cyc >= 2) && (cyc < 50) && (((cyc + 1) % 4) == 0);
    dev0_rd = (cyc >= 10) && (cyc < 58) && (((cyc + 2) % 5) == 0);
    dev1_wr = (cyc >= 4) && (cyc < 56) && (((cyc + 3) % 3) == 0);
    dev1_rd = 1'b0;  // Device 1 is never read by the program

    dev0_wdata = rol32(master_lfsr_q, int'(cyc[2:0])) ^ master_mix_q ^ 32'h1357_9bdf;
    dev1_wdata = rol32(master_mix_q, (cyc + 3) % 11)
                 ^ {master_lfsr_q[15:0], master_lfsr_q[31:16]}
                 ^ 32'h2468_ace1;

    // Keep a reference to a never-consumed data path so the RTL structure matches the
    // "software does not read this device" scenario rather than optimizing it away entirely.
    unused_ref = dev1_rvalid_ref ? (dev1_rdata_ref ^ 32'h55aa_3cc3) : 32'h0;
    unused_sub = dev1_rvalid_sub ? (dev1_rdata_sub ^ 32'h55aa_3cc3) : 32'h0;
  end

  sg_prog_dev #(
    .P_ID(8'h31)
  ) i_dev0_sub (
    .clk(clk),
    .rst_n(rst_n),
    .rd_i(dev0_rd),
    .wr_i(dev0_wr),
    .wdata_i(dev0_wdata),
    .irq_o(dev0_irq_sub),
    .rvalid_o(dev0_rvalid_sub),
    .rdata_o(dev0_rdata_sub),
    .trace_o()
  );

  sg_prog_dev #(
    .P_ID(8'h93)
  ) i_dev1_sub (
    .clk(clk),
    .rst_n(rst_n),
    .rd_i(dev1_rd),
    .wr_i(dev1_wr),
    .wdata_i(dev1_wdata),
    .irq_o(dev1_irq_sub),
    .rvalid_o(dev1_rvalid_sub),
    .rdata_o(dev1_rdata_sub),
    .trace_o()
  );

  sg_prog_dev_ref #(
    .P_ID(8'h31)
  ) i_dev0_ref (
    .clk(clk),
    .rst_n(rst_n),
    .rd_i(dev0_rd),
    .wr_i(dev0_wr),
    .wdata_i(dev0_wdata),
    .irq_o(dev0_irq_ref),
    .rvalid_o(dev0_rvalid_ref),
    .rdata_o(dev0_rdata_ref),
    .trace_o()
  );

  sg_prog_dev_ref #(
    .P_ID(8'h93)
  ) i_dev1_ref (
    .clk(clk),
    .rst_n(rst_n),
    .rd_i(dev1_rd),
    .wr_i(dev1_wr),
    .wdata_i(dev1_wdata),
    .irq_o(dev1_irq_ref),
    .rvalid_o(dev1_rvalid_ref),
    .rdata_o(dev1_rdata_ref),
    .trace_o()
  );

  initial begin
    cyc = 0;
    rst_n = 1'b0;
    master_lfsr_q = 32'h1bad_f00d;
    master_mix_q = 32'hc001_d00d;
    cpu_acc_ref = '0;
    cpu_acc_sub = '0;
    cpu_hist_ref = '0;
    cpu_hist_sub = '0;
    irq_hist_ref = '0;
    irq_hist_sub = '0;
  end

  always @(posedge clk) begin
    cyc <= cyc + 1;
    rst_n <= (cyc >= 2) && !((cyc >= 41) && (cyc < 44));
    master_lfsr_q <= lfsr32(master_lfsr_q ^ master_mix_q ^ 32'h9e37_79b9);
    master_mix_q <= rol32(master_mix_q + master_lfsr_q + 32'h7f4a_7c15, 5);

    if (dev0_rd && dev0_rvalid_ref) cpu_acc_ref <= cpu_acc_ref + dev0_rdata_ref;
    if (dev0_rd && dev0_rvalid_sub) cpu_acc_sub <= cpu_acc_sub + dev0_rdata_sub;

    cpu_hist_ref <= {cpu_hist_ref[28:0], dev0_irq_ref, dev1_irq_ref, dev0_rvalid_ref};
    cpu_hist_sub <= {cpu_hist_sub[28:0], dev0_irq_sub, dev1_irq_sub, dev0_rvalid_sub};

    irq_hist_ref <= {irq_hist_ref[13:0], dev0_irq_ref ^ dev1_irq_ref, dev0_rvalid_ref ^ dev1_rvalid_ref};
    irq_hist_sub <= {irq_hist_sub[13:0], dev0_irq_sub ^ dev1_irq_sub, dev0_rvalid_sub ^ dev1_rvalid_sub};

    if (cyc > 3) begin
      `checkh(dev0_irq_sub, dev0_irq_ref);
      `checkh(dev0_rvalid_sub, dev0_rvalid_ref);
      `checkh(dev0_rdata_sub, dev0_rdata_ref);
      `checkh(dev1_irq_sub, dev1_irq_ref);
      `checkh(dev1_rvalid_sub, dev1_rvalid_ref);
      `checkh(dev1_rdata_sub, dev1_rdata_ref);
      `checkh(cpu_acc_sub, cpu_acc_ref);
      `checkh(cpu_hist_sub, cpu_hist_ref);
      `checkh(irq_hist_sub, irq_hist_ref);
      `checkh(unused_sub, unused_ref);
    end

    if (cyc == 96) begin
      $write("*-* All Finished *-*\n");
      $finish;
    end
  end

endmodule

module sg_prog_dev #(
  parameter logic [7:0] P_ID = 8'h00
) (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        rd_i,
  input  logic        wr_i,
  input  logic [31:0] wdata_i,
  output logic        irq_o,
  output logic        rvalid_o,
  output logic [31:0] rdata_o,
  output logic [31:0] trace_o
); `SUBGRAPH_BOUNDARY

  logic [7:0] ctr_q;
  logic [7:0] key_q;
  logic [15:0] acc_q;
  logic pending_q;
  logic [31:0] wdata_q;
  logic [31:0] mix_w;

  function automatic logic [31:0] mix32(
    input logic [15:0] acc,
    input logic [7:0] ctr,
    input logic [7:0] key,
    input logic [31:0] wdata
  );
    logic [31:0] t0;
    logic [31:0] t1;
    t0 = {acc, ctr, key} ^ {wdata[15:0], wdata[31:16]} ^ {4{P_ID}};
    t1 = {t0[23:0], t0[31:24]} ^ (t0 >> 3) ^ 32'h04c1_1db7;
    return t1;
  endfunction

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      ctr_q <= P_ID;
      key_q <= P_ID ^ 8'h5a;
      acc_q <= {P_ID, P_ID ^ 8'hc3};
      pending_q <= 1'b0;
      wdata_q <= 32'h0;
    end else begin
      if (wr_i) begin
        ctr_q <= ctr_q + wdata_i[7:0] + 8'(P_ID[3:0]);
        key_q <= {key_q[6:0], key_q[7] ^ key_q[5] ^ key_q[4] ^ key_q[0]} ^ wdata_i[15:8];
        acc_q <= acc_q + wdata_i[31:16] + {8'h0, ctr_q};
        pending_q <= 1'b1;
        wdata_q <= wdata_i;
      end
      if (pending_q) begin
        acc_q <= acc_q ^ {ctr_q, key_q} ^ {8'h0, P_ID};
      end
      if (rd_i) pending_q <= 1'b0;
    end
  end

  assign mix_w = mix32(acc_q, ctr_q, key_q, wdata_q);
  assign rvalid_o = pending_q;
  assign irq_o = pending_q ^ mix_w[0] ^ acc_q[3];
  assign rdata_o = mix_w ^ {8'h0, acc_q, ctr_q};
  assign trace_o = {mix_w[15:0], key_q, ctr_q};

endmodule

module sg_prog_dev_ref #(
  parameter logic [7:0] P_ID = 8'h00
) (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        rd_i,
  input  logic        wr_i,
  input  logic [31:0] wdata_i,
  output logic        irq_o,
  output logic        rvalid_o,
  output logic [31:0] rdata_o,
  output logic [31:0] trace_o
);

  logic [7:0] ctr_q;
  logic [7:0] key_q;
  logic [15:0] acc_q;
  logic pending_q;
  logic [31:0] wdata_q;
  logic [31:0] mix_w;

  function automatic logic [31:0] mix32(
    input logic [15:0] acc,
    input logic [7:0] ctr,
    input logic [7:0] key,
    input logic [31:0] wdata
  );
    logic [31:0] t0;
    logic [31:0] t1;
    t0 = {acc, ctr, key} ^ {wdata[15:0], wdata[31:16]} ^ {4{P_ID}};
    t1 = {t0[23:0], t0[31:24]} ^ (t0 >> 3) ^ 32'h04c1_1db7;
    return t1;
  endfunction

  always_ff @(posedge clk) begin
    if (!rst_n) begin
      ctr_q <= P_ID;
      key_q <= P_ID ^ 8'h5a;
      acc_q <= {P_ID, P_ID ^ 8'hc3};
      pending_q <= 1'b0;
      wdata_q <= 32'h0;
    end else begin
      if (wr_i) begin
        ctr_q <= ctr_q + wdata_i[7:0] + 8'(P_ID[3:0]);
        key_q <= {key_q[6:0], key_q[7] ^ key_q[5] ^ key_q[4] ^ key_q[0]} ^ wdata_i[15:8];
        acc_q <= acc_q + wdata_i[31:16] + {8'h0, ctr_q};
        pending_q <= 1'b1;
        wdata_q <= wdata_i;
      end
      if (pending_q) begin
        acc_q <= acc_q ^ {ctr_q, key_q} ^ {8'h0, P_ID};
      end
      if (rd_i) pending_q <= 1'b0;
    end
  end

  assign mix_w = mix32(acc_q, ctr_q, key_q, wdata_q);
  assign rvalid_o = pending_q;
  assign irq_o = pending_q ^ mix_w[0] ^ acc_q[3];
  assign rdata_o = mix_w ^ {8'h0, acc_q, ctr_q};
  assign trace_o = {mix_w[15:0], key_q, ctr_q};

endmodule
