// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed into the Public Domain, for any use,
// without warranty, 2020 by Yutetsu TAKATSUKASA


module t (/*AUTOARG*/
   // Inputs
   clk
   );
   input clk;

   wire [7:0] out0;
   wire [7:0] out1;
   wire [7:0] out2;
   /* verilator lint_off UNOPTFLAT */
   wire [7:0] out3;
   /* verilator lint_on UNOPTFLAT */

   sub0 i_sub0(.clk(clk), .in(out3), .out(out0));
   sub1 i_sub1(.clk(clk), .in(out0), .out(out1));
   sub2 i_sub2(.clk(clk), .in(out1), .out(out2));
   sub3 i_sub3(.clk(clk), .in(out2), .out(out3));

   always_comb
      if (out3 == 15) begin
         $write("*-* All Finished *-*\n");
         $finish;
      end

   always_ff @(negedge clk) $display("out0:%d %d %d %d", out0, out1, out2, out3);

endmodule

module sub0(
   input wire clk,
   input wire [7:0] in,
   output wire [7:0] out); /* verilator hier_block */

   logic [7:0] ff;

   always_ff @(posedge clk) ff <= in;
   assign out = ff;
endmodule

module sub1(
   input wire clk,
   input wire [7:0] in,
   output wire [7:0] out); /* verilator hier_block */

   logic [7:0] ff;

   always_ff @(posedge clk) ff <= in + 1;
   assign out = ff;
endmodule

module sub2(
   input wire clk,
   input wire [7:0] in,
   output wire [7:0] out); /* verilator hier_block */

   logic [7:0] ff;

   always_ff @(posedge clk) ff <= in + 2;

   sub3 i_sub3(.clk(clk), .in(ff), .out(out));
endmodule

module sub3(
   input wire clk,
   input wire [7:0] in,
   output wire [7:0] out); /* verilator hier_block */

   logic [7:0] ff;
   always_ff @(posedge clk) ff <= in + 3;
   assign out = ff;
endmodule
