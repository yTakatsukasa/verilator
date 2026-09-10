#!/usr/bin/env python3
# DESCRIPTION: Verilator: Preserve cross-domain state dependencies without schedule reuse
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")
test.top_filename = "t/t_subgraph_template_inputs.v"

test.compile(verilator_flags2=[
    "--subgraph-schedule", "--debug-subgraph-fresh-order", "--stats", "--binary"
])
test.execute()

test.file_grep(test.stats, r"Scheduling, Subgraph NBA cross domain internal variables\s+(\d+)", 6)

test.passes()
