// PLACEHOLDER: replaced by the PowerPC core's real header on merge
//
//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
//
// Declarations only: the PowerPC core's public surface, so the Model 3 machine
// compiles before the core lands.
// There is deliberately no .cpp: nothing may link against these until the real
// header and its implementation replace this file.
#pragma once

#include "core/types.h"
#include "hw/machine.h"

namespace sm2 {
class Archive;
}

namespace sm2::cpu {
class PageMap;
}

namespace sm2::cpu::ppc {

enum class Model : u8 {
    Ppc603e,
    Ppc603ev,
};

enum class ExecMode : u8 {
    Interpreter,
    Jit,
};

struct PpcConfig {
    Model model  = Model::Ppc603e;
    u32   cpu_hz = 66'000'000;  ///< rom::Model3Spec::cpu_hz()
    u32   bus_hz = 66'000'000;  ///< 66 MHz on every step; sets the timebase rate
};

/// The slow path: every access a PageMap does not back. Values are big-endian,
/// i.e. the numeric value the PowerPC loads or stores.
class PpcBus {
public:
    virtual ~PpcBus() = default;

    virtual u8  read8(u32 address)  = 0;
    virtual u16 read16(u32 address) = 0;
    virtual u32 read32(u32 address) = 0;
    virtual u64 read64(u32 address) = 0;

    virtual void write8(u32 address, u8 value)   = 0;
    virtual void write16(u32 address, u16 value) = 0;
    virtual void write32(u32 address, u32 value) = 0;
    virtual void write64(u32 address, u64 value) = 0;
};

class Ppc {
public:
    Ppc();
    ~Ppc();

    void init(const PpcConfig& config, PageMap& pages, PpcBus& bus);
    void reset();
    s64  run(s64 cycles);
    void end_timeslice();
    void set_irq(bool asserted);
    void set_exec_mode(ExecMode mode);
    [[nodiscard]] u64           total_cycles() const;
    [[nodiscard]] hw::CpuStatus status() const;
    void serialize(Archive& ar);
    void invalidate(u32 address, u32 size);
};

}  // namespace sm2::cpu::ppc
