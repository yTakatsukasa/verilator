// DESCRIPTION: Verilator: Verilog Test module
//
// This file ONLY is placed into the Public Domain, for any use,
// without warranty, 2009 by Iztok Jeras.
// SPDX-License-Identifier: CC0-1.0

module t (/*AUTOARG*/
   // Inputs
   clk
   );

   input clk;

   localparam NO = 7;  // number of access events

   // packed structures
   struct packed {
      logic       e0;
      logic [1:0] e1;
      logic [3:0] e2;
      logic [7:0] e3;
   } struct_dsc;  // descending range structure
   /* verilator lint_off ASCRANGE */
   struct packed {
      logic       e0;
      logic [0:1] e1;
      logic [0:3] e2;
      logic [0:7] e3;
   } struct_asc;  // ascending range structure
   /* verilator lint_on ASCRANGE */

   localparam WS = 15;  // $bits(struct_dsc)

   integer cnt = 0;

   // event counter
   always @ (posedge clk)
   begin
      cnt <= cnt + 1;
   end

   // finish report
   always @ (posedge clk)
   if ((cnt[30:2]==(NO-1)) && (cnt[1:0]==2'd3)) begin
      $write("*-* All Finished *-*\n");
      $finish;
   end

   // descending range
   always @ (posedge clk)
   if (cnt[1:0]==2'd0) begin
      // initialize to defaults (all bits 1'b0)
      if      (cnt[30:2]==0)  struct_dsc <= '0;
      else if (cnt[30:2]==1)  struct_dsc <= '0;
      else if (cnt[30:2]==2)  struct_dsc <= '0;
      else if (cnt[30:2]==3)  struct_dsc <= '0;
      else if (cnt[30:2]==4)  struct_dsc <= '0;
      else if (cnt[30:2]==5)  struct_dsc <= '0;
      else if (cnt[30:2]==6)  struct_dsc <= '0;
   end else if (cnt[1:0]==2'd1) begin
      // write data into whole or part of the array using literals
      if      (cnt[30:2]==0)  begin end
      else if (cnt[30:2]==1)  struct_dsc <= '{0 ,1 , 2, 3};
      else if (cnt[30:2]==2)  struct_dsc <= '{e0:1, e1:2, e2:3, e3:4};
      else if (cnt[30:2]==3)  struct_dsc <= '{e3:6, e2:4, e1:2, e0:0};
      // verilator lint_off WIDTH
      else if (cnt[30:2]==4)  struct_dsc <= '{default:13};
      else if (cnt[30:2]==5)  struct_dsc <= '{e2:8'haa, default:1};
      else if (cnt[30:2]==6)  struct_dsc <= '{cnt+0 ,cnt+1 , cnt+2, cnt+3};
      // verilator lint_on WIDTH
   end else if (cnt[1:0]==2'd2) begin
      // chack array agains expected value
      if      (cnt[30:2]==0)  begin if (struct_dsc !== 15'b0_00_0000_00000000) begin $display("%b", struct_dsc); $stop(); end end
      else if (cnt[30:2]==1)  begin if (struct_dsc !== 15'b0_01_0010_00000011) begin $display("%b", struct_dsc); $stop(); end end
      else if (cnt[30:2]==2)  begin if (struct_dsc !== 15'b1_10_0011_00000100) begin $display("%b", struct_dsc); $stop(); end end
      else if (cnt[30:2]==3)  begin if (struct_dsc !== 15'b0_10_0100_00000110) begin $display("%b", struct_dsc); $stop(); end end
      else if (cnt[30:2]==4)  begin if (struct_dsc !== 15'b1_01_1101_00001101) begin $display("%b", struct_dsc); $stop(); end end
      else if (cnt[30:2]==5)  begin if (struct_dsc !== 15'b1_01_1010_00000001) begin $display("%b", struct_dsc); $stop(); end end
      else if (cnt[30:2]==6)  begin if (struct_dsc !== 15'b1_10_1011_00011100) begin $display("%b", struct_dsc); $stop(); end end
   end

   // ascending range
   always @ (posedge clk)
   if (cnt[1:0]==2'd0) begin
      // initialize to defaults (all bits 1'b0)
      if      (cnt[30:2]==0)  struct_asc <= '0;
      else if (cnt[30:2]==1)  struct_asc <= '0;
      else if (cnt[30:2]==2)  struct_asc <= '0;
      else if (cnt[30:2]==3)  struct_asc <= '0;
      else if (cnt[30:2]==4)  struct_asc <= '0;
      else if (cnt[30:2]==5)  struct_asc <= '0;
      else if (cnt[30:2]==6)  struct_asc <= '0;
   end else if (cnt[1:0]==2'd1) begin
      // write data into whole or part of the array using literals
      if      (cnt[30:2]==0)  begin end
      else if (cnt[30:2]==1)  struct_asc <= '{0 ,1 , 2, 3};
      else if (cnt[30:2]==2)  struct_asc <= '{e0:1, e1:2, e2:3, e3:4};
      else if (cnt[30:2]==3)  struct_asc <= '{e3:6, e2:4, e1:2, e0:0};
      // verilator lint_off WIDTH
      else if (cnt[30:2]==4)  struct_asc <= '{default:13};
      else if (cnt[30:2]==5)  struct_asc <= '{e2:8'haa, default:1};
      else if (cnt[30:2]==6)  struct_asc <= '{cnt+0 ,cnt+1 , cnt+2, cnt+3};
      // verilator lint_on WIDTH
   end else if (cnt[1:0]==2'd2) begin
      // chack array agains expected value
      if      (cnt[30:2]==0)  begin if (struct_asc !== 15'b0_00_0000_00000000) begin $display("%b", struct_asc); $stop(); end end
      else if (cnt[30:2]==1)  begin if (struct_asc !== 15'b0_01_0010_00000011) begin $display("%b", struct_asc); $stop(); end end
      else if (cnt[30:2]==2)  begin if (struct_asc !== 15'b1_10_0011_00000100) begin $display("%b", struct_asc); $stop(); end end
      else if (cnt[30:2]==3)  begin if (struct_asc !== 15'b0_10_0100_00000110) begin $display("%b", struct_asc); $stop(); end end
      else if (cnt[30:2]==4)  begin if (struct_asc !== 15'b1_01_1101_00001101) begin $display("%b", struct_asc); $stop(); end end
      else if (cnt[30:2]==5)  begin if (struct_asc !== 15'b1_01_1010_00000001) begin $display("%b", struct_asc); $stop(); end end
      else if (cnt[30:2]==6)  begin if (struct_asc !== 15'b1_10_1011_00011100) begin $display("%b", struct_asc); $stop(); end end
   end

endmodule
