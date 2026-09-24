// DESCRIPTION: Verilator: Runtime driver for subgraph scale measurements
//
// This file ONLY is placed under the Creative Commons Public Domain.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: CC0-1.0

#include "Vscale.h"
#include "Vscale__Syms.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    const unsigned long cycles = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 1000000;
    VerilatedContext context;
    context.commandArgs(argc, argv);
    Vscale top{&context};
    top.clk = 0;
    top.eval();
    unsigned long long checksum = 0;
    for (unsigned long i = 0; i < cycles; ++i) {
        top.clk = 0;
        top.eval();
        top.clk = 1;
        top.eval();
        checksum += top.checksum;
    }
    std::printf("state_bytes=%zu cycles=%lu checksum=%llu\n", sizeof(Vscale__Syms), cycles,
                checksum);
    top.final();
    return 0;
}
