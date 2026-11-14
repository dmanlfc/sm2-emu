// SPDX-License-Identifier: GPL-3.0-or-later
//
// Epson RTC-72421 real-time clock.
//
// Derived from Supermodel's Src/Model3/RTC72421.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c) 2011
// Bart Trzynadlowski, Nik Henson.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.

#include "hw/model3/rtc72421.h"

#include "core/archive.h"

namespace sm2::hw::model3 {
namespace {

struct Calendar {
    int year;   ///< Years since 1900, as struct tm.
    int month;  ///< 0-11.
    int mday;   ///< 1-31.
    int hour, minute, second;
    int wday;   ///< 0 = Sunday.
};

Calendar to_calendar(s64 t)
{
    s64 days = t / 86400;
    s64 secs = t % 86400;
    if (secs < 0) {
        secs += 86400;
        --days;
    }
    Calendar c{};
    c.hour   = static_cast<int>(secs / 3600);
    c.minute = static_cast<int>((secs / 60) % 60);
    c.second = static_cast<int>(secs % 60);
    // 1970-01-01 was a Thursday.
    c.wday = static_cast<int>(((days % 7) + 11) % 7);

    // Howard Hinnant's civil_from_days.
    const s64 z   = days + 719468;
    const s64 era = (z >= 0 ? z : z - 146096) / 146097;
    const s64 doe = z - era * 146097;
    const s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const s64 mp  = (5 * doy + 2) / 153;
    const s64 d   = doy - (153 * mp + 2) / 5 + 1;
    const s64 m   = mp < 10 ? mp + 3 : mp - 9;
    const s64 y   = yoe + era * 400 + (m <= 2 ? 1 : 0);
    c.year  = static_cast<int>(y - 1900);
    c.month = static_cast<int>(m - 1);
    c.mday  = static_cast<int>(d);
    return c;
}

}  // namespace

void Rtc72421::set_time(s64 unix_seconds)
{
    m_seconds     = unix_seconds;
    m_cycle_phase = 0;
}

void Rtc72421::advance(u64 cycles, u32 hz)
{
    if (hz == 0) return;
    m_cycle_phase += cycles;
    m_seconds += static_cast<s64>(m_cycle_phase / hz);
    m_cycle_phase %= hz;
}

u8 Rtc72421::read_register(unsigned reg) const
{
    const Calendar t = to_calendar(m_seconds);
    switch (reg & 0xF) {
        case 0:  return static_cast<u8>((t.second % 10) & 0xF);
        case 1:  return static_cast<u8>((t.second / 10) & 0x7);
        case 2:  return static_cast<u8>((t.minute % 10) & 0xF);
        case 3:  return static_cast<u8>((t.minute / 10) & 0x7);
        case 4:  return static_cast<u8>((t.hour % 10) & 0xF);
        case 5:  return static_cast<u8>((t.hour / 10) & 0x7);
        case 6:  return static_cast<u8>((t.mday % 10) & 0xF);
        case 7:  return static_cast<u8>((t.mday / 10) & 0x3);
        case 8:  return static_cast<u8>(((t.month + 1) % 10) & 0xF);
        case 9:  return static_cast<u8>(((t.month + 1) / 10) & 0x1);
        case 10: return static_cast<u8>((t.year % 10) & 0xF);
        case 11: return static_cast<u8>(((t.year % 100) / 10) & 0xF);
        case 12: return static_cast<u8>(t.wday & 0x7);
        default: break;
    }
    return 0;
}

void Rtc72421::write_register(unsigned reg, u8 data)
{
    // TODO: register writes (setting the clock) are not emulated.
    (void)reg;
    (void)data;
}

void Rtc72421::serialize(Archive& ar)
{
    ar.raw(m_seconds);
    ar.raw(m_cycle_phase);
}

}  // namespace sm2::hw::model3
