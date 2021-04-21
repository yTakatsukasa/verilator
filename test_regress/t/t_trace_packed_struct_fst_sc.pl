#!/usr/bin/env perl
if (!$::Driver) { use FindBin; exec("$FindBin::Bin/bootstrap.pl", @ARGV, $0); die; }
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# Copyright 2003 by Wilson Snyder. This program is free software; you
# can redistribute it and/or modify it under the terms of either the GNU
# Lesser General Public License Version 3 or the Perl Artistic License
# Version 2.0.
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

scenarios(simulator => 1);

if (!$Self->have_sc) {
    skip("No SystemC installed");
}
else {
    top_filename("t/t_trace_packed_struct.v");

    compile(
        v_flags2 => ["--sc --trace-fst"]
        );

    execute(
        check_finished => 1,
        );

    fst_identical($Self->trace_filename, $Self->{golden_filename});
}

ok(1);
1;