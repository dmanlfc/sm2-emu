// SPDX-License-Identifier: GPL-3.0-or-later
//
// Epson RTC-72421 real-time clock, as wired on the Model 3 CPU board.
//
// Derived from Supermodel's Src/Model3/RTC72421.cpp and RTC72421.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c) 2011
// Bart Trzynadlowski, Nik Henson.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// Time is virtual (a base Unix time plus emulated cycles) and is saved in
// states and NVRAM; the host clock is never read. The UTC calendar uses
// civil-from-days rather than the C library.
#pragma once

#include "core/types.h"

namespace sm2 {
class Archive;
}

namespace sm2::hw::model3 {

class Rtc72421 {
public:
    /// 2000-01-01 00:00:00 UTC, used until the owner sets a time.
    static constexpr s64 kDefaultTime = 946684800;

    /// Set the current time (Unix seconds). Resets the sub-second phase.
    void set_time(s64 unix_seconds);
    [[nodiscard]] s64 time() const { return m_seconds; }

    /// Advance by `cycles` of a clock running at `hz`.
    void advance(u64 cycles, u32 hz);

    [[nodiscard]] u8 read_register(unsigned reg) const;
    void write_register(unsigned reg, u8 data);

    void reset() {}

    void serialize(Archive& ar);

private:
    s64 m_seconds = kDefaultTime;
    u64 m_cycle_phase = 0;
};

}  // namespace sm2::hw::model3
