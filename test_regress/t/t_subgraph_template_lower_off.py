#!/usr/bin/env python3
# DESCRIPTION: Verilator: Compare shared template test with the baseline execution path
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")
test.top_filename = "t/t_subgraph_template_lower.v"
test.compile(verilator_flags2=["--binary", "--stats"])
test.execute()
# Sum statistics omit zero entries. No template output may be reported when disabled.
test.file_grep_not(test.stats, r"Output, C\+\+ template .*\s+[1-9]\d*\s*$")
test.file_grep(test.stats, r"Output, C\+\+ other function bytes\s+[1-9]\d*")
test.passes()
