#!/usr/bin/env python3
# DESCRIPTION: Verilator: Verilog Test driver/expect definition
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of either the GNU Lesser General Public License Version 3
# or the Perl Artistic License Version 2.0.
# SPDX-FileCopyrightText: 2026 Wilson Snyder
# SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0

import vltest_bootstrap

test.scenarios("vlt")

test.compile(verilator_flags2=["--subgraph-schedule", "--stats"])


def subgraph_function_sizes(filename):
    with open(filename, "r", encoding="utf8") as file_handle:
        source = file_handle.read()
    sizes = []
    for match in re.finditer(r"(?m)^void [^\n]*__nba_subgraph_[^\n]*\{$", source):
        depth = 0
        for offset in range(source.find("{", match.start(), match.end()), len(source)):
            if source[offset] == "{":
                depth += 1
            elif source[offset] == "}":
                depth -= 1
                if depth == 0:
                    sizes.append(len(source[match.start():offset + 1].encode("utf8")))
                    break
    return sizes


cpp_files = [
    filename for filename in test.glob_some(test.obj_dir + "/" + test.vm_prefix + "*.cpp")
    if not re.search(r"__(ALL|main)\.cpp$", filename)
]
cpp_sizes = [os.path.getsize(filename) for filename in cpp_files]
function_sizes = [size for filename in cpp_files for size in subgraph_function_sizes(filename)]
if sum(cpp_sizes) > 85000:
    test.error("Generated C++ exceeds 85000 bytes: %d" % sum(cpp_sizes))
if max(cpp_sizes) > 26000:
    test.error("Largest generated C++ file exceeds 26000 bytes: %d" % max(cpp_sizes))
if not function_sizes:
    test.error("No generated subgraph helper function found")
elif max(function_sizes) > 1600:
    test.error("Largest generated subgraph helper exceeds 1600 bytes: %d" % max(function_sizes))

test.execute()

test.file_grep(test.stats, r"Scheduling, NBA elapsed time \(sec\), measured total\s+\d+\.\d+")
test.file_grep(test.stats,
               r"Scheduling, Subgraph NBA elapsed time \(sec\), order calls\s+\d+\.\d+")
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI analyses\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI DPI calls\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI eligible helpers\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI external vars\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI hidden uses\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI module-phase candidates\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared ABI state vars\s+([1-9]\d*)")
test.file_grep(test.stats, r"Scheduling, Subgraph canonical context artifacts\s+(\d+)", 6)
test.file_grep(test.stats,
               r"Scheduling, Subgraph canonical contract direct external uses\s+(\d+)", 4)
test.file_grep(test.stats,
               r"Scheduling, Subgraph canonical contract direct global trigger uses\s+(\d+)",
               4)
test.file_grep(test.stats,
               r"Scheduling, Subgraph canonical contract direct other uses\s+(\d+)", 0)
test.file_grep(test.stats,
               r"Scheduling, Subgraph canonical contract local trigger uses\s+(\d+)", 4)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical context reuses\s+(\d+)", 9)
test.file_grep(test.stats, r"Scheduling, Subgraph canonical order calls avoided\s+(\d+)", 9)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence classes\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence instances\s+(\d+)", 15)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence max class size\s+(\d+)", 3)
test.file_grep(test.stats, r"Scheduling, Subgraph schedule equivalence potential reuses\s+(\d+)",
               9)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper arguments\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper artifacts\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper body mismatches\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper isolation checks\s+(\d+)", 6)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared helper isolation global trigger refs\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper isolation refs\s+([1-9]\d*)")
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared helper isolation unbound refs\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper parameterizations\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper reuses\s+(\d+)", 9)
test.file_grep(test.stats, r"Scheduling, Subgraph shared helper skipped DPI calls\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared local trigger IDs\s+(\d+)", 6)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared local trigger instance bindings\s+(\d+)", 15)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache hash candidates\s+(\d+)", 9)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache hash collisions\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache hash lookups\s+(\d+)", 15)
test.file_grep(test.stats,
               r"Scheduling, Subgraph shared order cache hash max candidates\s+(\d+)", 1)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache hash misses\s+(\d+)", 6)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache logic matches\s+(\d+)", 9)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache logic mismatches\s+(\d+)", 0)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache lookups\s+(\d+)", 9)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache miss ABI\s+(\d+)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache miss domain\s+(\d+)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache miss dtype/access\s+(\d+)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache miss phase\s+(\d+)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache miss relative path\s+(\d+)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache miss topology\s+(\d+)")
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache order calls executed\s+(\d+)",
               6)
test.file_grep(test.stats, r"Scheduling, Subgraph shared order cache order calls avoided\s+(\d+)",
               9)

test.passes()
