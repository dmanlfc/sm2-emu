// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/mame/sega/model2.cpp (BSD-3-Clause, copyright-holders
// R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels).

#include "hw/copro_tgp.h"

#include "core/log.h"

#include <algorithm>
#include <cstring>

namespace sm2::hw {
namespace {

/// io space addresses of the mathematical units, in words.
constexpr u16 kSinCosFirst = 0x0020;
constexpr u16 kSinCosLast  = 0x0023;
constexpr u16 kAtanFirst   = 0x0024;
constexpr u16 kAtanLast    = 0x0027;
constexpr u16 kInverseFirst = 0x0028;
constexpr u16 kInverseLast  = 0x0029;
constexpr u16 kInverseSqrtFirst = 0x002a;
constexpr u16 kInverseSqrtLast  = 0x002b;

/// Base index of each table within the CPU board's table ROM, in words.
constexpr u32 kSinCosTable      = 0x0000;
constexpr u32 kAtanTable        = 0x4000;
constexpr u32 kInverseTable     = 0x8000;
constexpr u32 kInverseSqrtTable = 0xc000;

/// Words of display list buffer the coprocessor can reach.
constexpr u32 kBufferWords = 0x8000;

}  // namespace

CoproTgp::CoproTgp()
    : m_cpu(*this)
    , m_program(kProgramWords, 0)
    , m_data_low(0x100, 0)
    , m_data_high(0x200, 0)
{
    m_fifo_in.configure(kFifoDepth);
    m_fifo_out.configure(kFifoDepth);

    // Flow control. The coprocessor consumes the input FIFO and produces the
    // output one, so it is the destination of one and the source of the other.
    // The host is the opposite in both cases, and the host's side is wired up by
    // the machine because only it can halt the host CPU.
    m_fifo_in.set_on_empty_retry([this] { m_cpu.stall(); });
    m_fifo_in.set_on_empty_halt([this] { m_cpu.set_halted(true); });
    m_fifo_in.set_on_unempty([this] { m_cpu.set_halted(false); });

    m_fifo_out.set_on_full([this] { m_cpu.set_halted(true); });
    m_fifo_out.set_on_unfull([this] { m_cpu.set_halted(false); });
}

void CoproTgp::attach(std::span<const u32> tables, std::span<const u32> data_rom,
                      std::span<u32> buffer_ram)
{
    m_tables     = tables;
    m_data_rom   = data_rom;
    m_buffer_ram = buffer_ram;

    if (m_tables.empty()) {
        SM2_WARN("copro: no mathematical table ROM; every trigonometric result "
                 "will be zero");
    } else if (m_tables.size() < 0x10000) {
        SM2_WARN("copro: the table ROM holds %zu of the expected 65536 words; "
                 "results above that will be zero",
                 m_tables.size());
    }
}

void CoproTgp::reset()
{
    m_cpu.reset();
    // Held until the host uploads microcode and releases it. Without this the
    // coprocessor would run whatever the program RAM happens to contain.
    m_cpu.set_halted(true);

    m_fifo_in.clear();
    m_fifo_out.clear();

    std::fill(m_data_low.begin(), m_data_low.end(), 0u);
    std::fill(m_data_high.begin(), m_data_high.end(), 0u);

    m_control      = 0;
    m_upload_count = 0;
    m_bank         = 0;

    m_sincos_base       = 0;
    m_inverse_base      = 0;
    m_inverse_sqrt_base = 0;
    std::fill(std::begin(m_atan_base), std::end(m_atan_base), 0u);

    m_activity = Activity{};
}

s32 CoproTgp::run(s32 cycles)
{
    return m_cpu.run(cycles);
}

// ---------------------------------------------------------------------------
// Host side
// ---------------------------------------------------------------------------

void CoproTgp::control_write(u32 value)
{
    // Only a change of the top bit means anything; the rest of the register is
    // storage.
    if (((value ^ m_control) & 0x80000000u) != 0) {
        if ((value & 0x80000000u) != 0) {
            SM2_DEBUG("copro: microcode upload started");
            m_upload_count = 0;
            m_cpu.set_halted(true);
        } else {
            SM2_DEBUG("copro: booting, %u word(s) uploaded", m_upload_count);
            m_cpu.reset();
            m_cpu.set_halted(false);
        }
    }
    m_control = value;
}

void CoproTgp::host_fifo_write(u32 value)
{
    if ((m_control & 0x80000000u) != 0) {
        if (m_upload_count < kProgramWords) {
            m_program[m_upload_count] = value;
        }
        ++m_upload_count;
        return;
    }
    m_fifo_in.push(value);
}

u32 CoproTgp::host_fifo_read()
{
    return m_fifo_out.pop();
}

void CoproTgp::function_port_write(u32 byte_offset, u32 value)
{
    // The port's own address carries the function number, which the host folds
    // into the command word. Sixteen bytes per function, six bits of function
    // number, matching the geometrizer's identical function-port convention
    // (kGeoPort in model2.cpp uses the same offset >> 4 with a 6-bit mask). Bits
    // 22:20 of the value are discarded because that is where the function number
    // lands.
    const u32 function = (byte_offset >> 4) & 0x3f;
    const u32 command  = (value & 0x800fffffu) | (function << 23);
    m_fifo_in.push(command);
}

// ---------------------------------------------------------------------------
// Program and data spaces
// ---------------------------------------------------------------------------

u32 CoproTgp::fetch(u16 address)
{
    return m_program[address & (kProgramWords - 1)];
}

u32 CoproTgp::read_program(u16 address)
{
    return m_program[address & (kProgramWords - 1)];
}

u32 CoproTgp::read_data(u16 address)
{
    // Only the two banks are decoded. Everything else, including the gap at
    // 0x100-0x1ff, reads as zero.
    if (address < 0x100) {
        return m_data_low[address];
    }
    if (address >= 0x200 && address < 0x400) {
        return m_data_high[address - 0x200];
    }
    return 0;
}

void CoproTgp::write_data(u16 address, u32 value)
{
    if (address < 0x100) {
        m_data_low[address] = value;
        return;
    }
    if (address >= 0x200 && address < 0x400) {
        m_data_high[address - 0x200] = value;
    }
}

// ---------------------------------------------------------------------------
// io space: the mathematical units, or a window onto external memory
// ---------------------------------------------------------------------------

u32 CoproTgp::read_io(u16 address)
{
    // The bank register switches the whole io space between external memory and
    // the mathematical units. That looks odd, because it means the tables are
    // unreachable while the window is open, but it is what the hardware does: the
    // window covers the entire space, so the program has to close it before doing
    // any trigonometry.
    if (external_window_enabled()) {
        return external_read(address);
    }

    if (address >= kSinCosFirst && address <= kSinCosLast) {
        return sincos_read(address - kSinCosFirst);
    }
    if (address >= kAtanFirst && address <= kAtanLast) {
        return atan_read();
    }
    if (address >= kInverseFirst && address <= kInverseLast) {
        return inverse_read(address - kInverseFirst);
    }
    if (address >= kInverseSqrtFirst && address <= kInverseSqrtLast) {
        return inverse_sqrt_read(address - kInverseSqrtFirst);
    }
    return 0;
}

void CoproTgp::write_io(u16 address, u32 value)
{
    if (external_window_enabled()) {
        external_write(address, value);
        return;
    }

    if (address >= kSinCosFirst && address <= kSinCosLast) {
        m_sincos_base = value;
        return;
    }
    if (address >= kAtanFirst && address <= kAtanLast) {
        m_atan_base[address - kAtanFirst] = value;
        // The unit drives a general-purpose input with the comparison of its two
        // arguments' magnitudes, which is how the program learns which octant it
        // is in without a branch on the result.
        m_cpu.set_gpio(0, (m_atan_base[0] & 0x7fffffffu) <= (m_atan_base[1] & 0x7fffffffu));
        return;
    }
    if (address >= kInverseFirst && address <= kInverseLast) {
        m_inverse_base = value;
        return;
    }
    if (address >= kInverseSqrtFirst && address <= kInverseSqrtLast) {
        m_inverse_sqrt_base = value;
    }
}

u32 CoproTgp::external_read(u16 offset) const
{
    const u32 address = (m_bank & 0xff0000u) | offset;

    if ((address & 0x800000u) != 0) {
        ++m_activity.data_rom_reads;
        if (m_data_rom.empty()) {
            return 0;
        }
        return m_data_rom[address & (m_data_rom.size() - 1)];
    }
    if ((address & 0x400000u) != 0) {
        ++m_activity.buffer_reads;
        const u32 index = address & (kBufferWords - 1);
        return index < m_buffer_ram.size() ? m_buffer_ram[index] : 0u;
    }
    return 0;
}

void CoproTgp::external_write(u16 offset, u32 value)
{
    const u32 address = (m_bank & 0xff0000u) | offset;

    // Only the display list buffer is writable; the data ROM half is not.
    if ((address & 0x400000u) != 0) {
        ++m_activity.buffer_writes;
        const u32 index = address & (kBufferWords - 1);
        if (index < m_buffer_ram.size()) {
            m_buffer_ram[index] = value;
        }
    }
}

// ---------------------------------------------------------------------------
// The mathematical units
// ---------------------------------------------------------------------------

u32 CoproTgp::sincos_read(u16 offset) const
{
    // One table of a quarter turn, reflected and negated to cover the circle. The
    // port offset adds a quarter turn, so reading offset 0 and offset 1 gives sine
    // and cosine of the same angle.
    const u32 angle = m_sincos_base + offset * 0x4000u;
    u32       index = angle & 0x3fff;
    if ((angle & 0x4000u) != 0) {
        index = std::min<u32>(0x4000u - index, 0x3fff);
    }
    u32 result = table(kSinCosTable | index);
    if ((angle & 0x8000u) != 0) {
        result ^= 0x80000000u;
    }
    return result;
}

u32 CoproTgp::atan_read() const
{
    // The program writes the two operands and a normalised ratio, and this folds
    // the table's first-octant answer out to the full circle using the signs and
    // the magnitude comparison.
    const u8   ie = static_cast<u8>(0x88 - (m_atan_base[3] >> 23));
    const bool s0 = (m_atan_base[0] & 0x80000000u) != 0;
    const bool s1 = (m_atan_base[1] & 0x80000000u) != 0;
    const bool s2 = (m_atan_base[0] & 0x7fffffffu) <= (m_atan_base[1] & 0x7fffffffu);

    const u32 im    = m_atan_base[3] & 0x7fffff;
    u32       index = ie <= 0x17 ? (im | 0x800000u) >> ie : 0u;
    if (index == 0x4000) {
        index = 0x3fff;
    }

    u32 result = table(kAtanTable | index);
    if (s0 ^ s1 ^ s2) {
        result >>= 16;
    }
    if (s2) {
        result += 0x4000;
    }
    if ((s0 && !s2) || (s1 && s2)) {
        result += 0x8000;
    }
    return result & 0xffff;
}

u32 CoproTgp::inverse_read(u16 offset) const
{
    // The table holds reciprocals of mantissas; the exponent is corrected here.
    const u32 index  = ((m_inverse_base >> 9) & 0x3ffe) | (offset & 1);
    u32       result = table(kInverseTable | index);

    const u8 base_exponent = static_cast<u8>((m_inverse_base >> 23) & 0xff);
    const u8 exponent = static_cast<u8>((result >> 23) + (0x7f - base_exponent));
    result = (result & 0x007fffffu) | (static_cast<u32>(exponent) << 23);

    if ((m_inverse_base & 0x80000000u) != 0 && offset != 0) {
        result |= 0x80000000u;
    }
    return result;
}

u32 CoproTgp::inverse_sqrt_read(u16 offset) const
{
    // The exclusive-or with 0x2000 is a table layout detail, not a sign trick: the
    // two halves of the mantissa range are stored the other way round.
    const u32 index  = 0x2000u ^ (((m_inverse_sqrt_base >> 10) & 0x3ffe) | (offset & 1));
    u32       result = table(kInverseSqrtTable | index);

    const u8 base_exponent = static_cast<u8>((m_inverse_sqrt_base >> 24) & 0x7f);
    const u8 exponent = static_cast<u8>((result >> 23) + (0x3f - base_exponent));
    result = (result & 0x807fffffu) | (static_cast<u32>(exponent) << 23);

    if ((offset & 1) == 0) {
        result &= 0x7fffffffu;
    }
    return result;
}

// ---------------------------------------------------------------------------
// rf space: the host FIFOs and the bank register
// ---------------------------------------------------------------------------

u32 CoproTgp::read_rf(u8 address)
{
    // Four bits of address on this part, so anything wider aliases down. The core
    // passes through what the instruction encoded, exactly as MAME's address space
    // would before truncating.
    switch (address & 0xf) {
        case 1:
            ++m_activity.commands_received;
            return m_fifo_in.pop();
        default:
            return 0;
    }
}

void CoproTgp::write_rf(u8 address, u32 value)
{
    switch (address & 0xf) {
        case 0:
            // Status or indicator lamps. Written by the coprocessor programs and
            // read by nothing.
            break;
        case 2:
            ++m_activity.results_sent;
            m_fifo_out.push(value);
            break;
        case 3:
            m_bank = value;
            break;
        default:
            break;
    }
}

}  // namespace sm2::hw
