// DESCRIPTION: Verilator: Verilog Test module for SystemVerilog
//
// This file ONLY is placed under the Creative Commons Public Domain
// SPDX-FileCopyrightText: 2026 Antmicro
// SPDX-License-Identifier: CC0-1.0

module test1 (
    input logic b
);
  logic do_something;
  assign do_something = b;
endmodule

module test2 (
    input bit b[2]
);
  bit do_something[2];
  assign do_something = b;
endmodule

typedef struct {
  logic [6:0] head;
  logic [6:0] tail [2];
} aggregate_t;

module t (
    input logic a[2],  // unpacked array
    input logic [5:0] cyc
);

  logic b[2];

  assign b = a;

  test1 i_test1 (.b);
  test2 i_test2 (.b);

  // Inout connections require constant indices, including unpacked array selections.
  tri1 [1:0] pin_index;
  assign pin_index = cyc[0] ? cyc[2:1] : 2'bz;
  wire pin_pad [3:1];
  io indexed_io (.A(cyc[3]), .OE(cyc[4]), .Z(pin_pad[1 + int'(pin_index) % 3]));

  wire [6:0] packed_pad;
  io indexed_bit_io (.A(cyc[3]), .OE(cyc[4]), .Z(packed_pad[cyc[2:0]]));
  vector_io indexed_part_io (.pad(packed_pad[cyc[2:0] +: 3]));

  wire [2:0] vector_pad [3:1];
  vector_io indexed_vector_io (.pad(vector_pad[1 + int'(pin_index) % 3]));

  // Runtime system functions are not constant expressions, even without variable references.
  io time_bit_io (.A(cyc[3]), .OE(cyc[4]), .Z(packed_pad[3'($time % 7)]));
  io time_array_io (.A(cyc[3]), .OE(cyc[4]), .Z(pin_pad[1 + int'($time % 3)]));

  // Output net connections obey the constant-select rules for continuous assignments.
  scalar_output indexed_bit_output (.pad(packed_pad[cyc[2:0]]));
  scalar_output indexed_array_output (.pad(pin_pad[1 + int'(cyc[2:0]) % 3]));
  scalar_output time_bit_output (.pad(packed_pad[3'($time % 7)]));
  scalar_output time_array_output (.pad(pin_pad[1 + int'($time % 3)]));

  logic [6:0] variable_pad;
  scalar_output indexed_variable_bit_output (.pad(variable_pad[cyc[2:0]]));

  // Both sides of a concatenation must satisfy the destination selection rules.
  vector_io indexed_concat_lhs (.pad({packed_pad[cyc[2:0]], packed_pad[1:0]}));
  vector_io indexed_concat_rhs (.pad({packed_pad[1:0], packed_pad[cyc[2:0]]}));

  // Constant slices must not hide dynamic selections of the containing net array.
  wire [6:0] slice_pad [3:1][4:1];
  array_output indexed_slice_output (.pad(slice_pad[1 + int'(cyc[1:0]) % 3][3:2]));
  array_io indexed_slice_io (.pad(slice_pad[1 + int'(cyc[1:0]) % 3][3:2]));

  // Unpacked members can hide the same dynamic selections, including below a slice.
  typedef struct {
    logic data;
    logic [6:0] values [4:1];
  } entry_t;
  wire entry_t struct_pad [3:1];
  scalar_output indexed_member_output (.pad(struct_pad[1 + int'(cyc[1:0]) % 3].data));
  io indexed_member_io (.A(cyc[3]), .OE(cyc[4]), .Z(struct_pad[1 + int'(cyc[1:0]) % 3].data));
  array_output indexed_member_slice_output (.pad(struct_pad[1 + int'(cyc[1:0]) % 3].values[3:2]));

  // Aggregate constructors must not hide nonconstant output destination selections.
  wire [14:0] pattern_bits;
  array_output pattern_output (.pad('{pattern_bits[6:0], pattern_bits[cyc[3:0] +: 7]}));
  array_output concat_output (.pad({pattern_bits[6:0], pattern_bits[cyc[3:0] +: 7]}));
  array_output net_pattern_output (.pad('{slice_pad[1][1], slice_pad[1 + int'(cyc[1:0]) % 3][1]}));
  struct_output nested_pattern_output (
      .pad('{pattern_bits[6:0], '{slice_pad[1][1], slice_pad[1 + int'(cyc[1:0]) % 3][1]}})
  );

  // Inout aggregate connections have the same restrictions, before tristate lowering.
  array_io pattern_io (.pad('{pattern_bits[6:0], pattern_bits[cyc[3:0] +: 7]}));
  array_io concat_io (.pad({pattern_bits[6:0], pattern_bits[cyc[3:0] +: 7]}));
  array_io net_pattern_io (.pad('{slice_pad[1][1], slice_pad[1 + int'(cyc[1:0]) % 3][1]}));
  struct_io nested_pattern_io (
      .pad('{pattern_bits[6:0], '{slice_pad[1][1], slice_pad[1 + int'(cyc[1:0]) % 3][1]}})
  );

endmodule

module scalar_output (
    output wire pad
);
  pullup (pad);
endmodule

module io (
    input A,
    input OE,
    inout Z
);
  assign Z = OE ? A : 1'bz;
endmodule

module vector_io (
    inout wire [2:0] pad
);
endmodule

module array_output (
    output wire [6:0] pad [1:2]
);
  assign pad[1] = 7'h55;
  assign pad[2] = 7'h2a;
endmodule

module struct_output (
    output wire aggregate_t pad
);
  assign pad = '{7'h55, '{7'h2a, 7'h33}};
endmodule

module array_io (
    inout wire [6:0] pad [1:2]
);
endmodule

module struct_io (
    inout wire aggregate_t pad
);
endmodule
