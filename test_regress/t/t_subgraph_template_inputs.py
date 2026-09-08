#!/usr/bin/env python3
# DESCRIPTION: Verilator: Subgraph template instance connection semantics
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")

# Sharing counts are deliberately separate from this semantic baseline. Enable
# template-count assertions when the independent template path is introduced.
test.compile(verilator_flags2=["--subgraph-schedule", "--stats", "--binary"])
test.execute()

test.passes()
