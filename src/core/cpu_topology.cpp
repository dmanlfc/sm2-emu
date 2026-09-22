//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//

#include "core/cpu_topology.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(__linux__)
#include <sched.h>
#endif

namespace sm2::core {

CpuTopology CpuTopology::detect()
{
    CpuTopology topology;
#if defined(__linux__)
    struct Core {
        int cpu;
        long capacity;
    };
    std::vector<Core> cores;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/devices/system/cpu", ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() < 4 || name.compare(0, 3, "cpu") != 0
            || !std::all_of(name.begin() + 3, name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        std::ifstream file(entry.path() / "cpu_capacity");
        long          capacity = 0;
        if (!(file >> capacity) || capacity <= 0) {
            continue;
        }
        cores.push_back({std::stoi(name.substr(3)), capacity});
    }
    if (cores.empty()) {
        return topology;
    }

    std::sort(cores.begin(), cores.end(), [](const Core& a, const Core& b) { return a.cpu < b.cpu; });
    const long fastest = std::max_element(cores.begin(), cores.end(), [](const Core& a, const Core& b) {
                             return a.capacity < b.capacity;
                         })->capacity;
    for (const Core& core : cores) {
        (core.capacity * 10 >= fastest * 6 ? topology.fast : topology.slow).push_back(core.cpu);
    }
#endif
    return topology;
}

void pin_current_thread(const std::vector<int>& cpus)
{
#if defined(__linux__)
    if (cpus.empty()) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (const int cpu : cpus) {
        if (cpu >= 0 && cpu < CPU_SETSIZE) {
            CPU_SET(cpu, &set);
        }
    }
    (void)sched_setaffinity(0, sizeof(set), &set);
#else
    (void)cpus;
#endif
}

}  // namespace sm2::core
