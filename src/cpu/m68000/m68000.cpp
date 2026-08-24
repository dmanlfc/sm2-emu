//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
//
// See m68000.h. The core itself is Musashi; this file is only the glue between
// Musashi's global memory callbacks and a cpu::Bus.

#include "cpu/m68000/m68000.h"

#include "core/log.h"

#include <cassert>
#include <cstdio>

extern "C" {
#include <m68k.h>
}

namespace {

/// The bus Musashi's callbacks forward to.
///
/// Musashi's memory interface is a set of free functions, so the binding has to
/// live at file scope, above both the class and the extern "C" callbacks that
/// share it. Guarded by the single-instance assertion in the constructor.
sm2::cpu::Bus* g_bus = nullptr;

}  // namespace

namespace sm2::cpu::m68000 {

M68000::M68000(Bus& bus)
{
    // Musashi keeps its state in one global structure, so a second instance
    // would silently share registers with the first. Loud in every build, not
    // just the ones with assertions on, because the symptom otherwise is a sound
    // CPU that appears to execute someone else's program.
    if (g_bus != nullptr) {
        SM2_ERROR("m68000: a second M68000 was constructed; Musashi is a "
                  "single-instance core and the two will share state");
    }
    assert(g_bus == nullptr && "only one M68000 may exist; Musashi is a single-instance core");
    g_bus = &bus;

    // Builds the opcode jump table. Musashi guards the expensive part with its
    // own static, so calling it per instance is cheap and keeps the ordering
    // requirement (init before set_cpu_type before pulse_reset) local.
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
}

M68000::~M68000()
{
    g_bus = nullptr;
}

void M68000::reset()
{
    m_irq_mask     = 0;
    m_total_cycles = 0;
    m68k_pulse_reset();
    apply_irq();
}

s32 M68000::run(s32 cycles)
{
    if (cycles <= 0) {
        return 0;
    }

    // Musashi drops the interrupt level when it takes the exception, so a level
    // that is still being held has to be put back. See set_irq_line.
    apply_irq();

    const s32 used = m68k_execute(cycles);
    m_total_cycles += static_cast<u64>(used < 0 ? 0 : used);
    return used;
}

void M68000::set_irq_line(int level, bool asserted)
{
    if (level < kIrqMin || level > kIrqMax) {
        SM2_WARN("m68000: interrupt level %d out of range", level);
        return;
    }

    const u8 bit = static_cast<u8>(1u << level);
    if (asserted) {
        m_irq_mask |= bit;
    } else {
        m_irq_mask = static_cast<u8>(m_irq_mask & ~bit);
    }
    apply_irq();
}

void M68000::clear_irq_lines()
{
    m_irq_mask = 0;
    apply_irq();
}

void M68000::set_irq_level(int level)
{
    m68k_set_irq(static_cast<unsigned int>(level));
}

void M68000::apply_irq() const
{
    // The 68000's three IPL pins encode the highest pending level, so a lower
    // level asserted at the same time is simply invisible until the higher one
    // is released.
    unsigned int level = 0;
    for (int candidate = kIrqMax; candidate >= kIrqMin; --candidate) {
        if (m_irq_mask & (1u << candidate)) {
            level = static_cast<unsigned int>(candidate);
            break;
        }
    }
    m68k_set_irq(level);
}

u32 M68000::pc() const
{
    return m68k_get_reg(nullptr, M68K_REG_PC);
}

u32 M68000::sr() const
{
    return m68k_get_reg(nullptr, M68K_REG_SR);
}

u32 M68000::sp() const
{
    return m68k_get_reg(nullptr, M68K_REG_SP);
}

u32 M68000::data_reg(int index) const
{
    return m68k_get_reg(nullptr, static_cast<m68k_register_t>(M68K_REG_D0 + (index & 7)));
}

u32 M68000::address_reg(int index) const
{
    return m68k_get_reg(nullptr, static_cast<m68k_register_t>(M68K_REG_A0 + (index & 7)));
}

std::string M68000::state_string() const
{
    char buffer[256];
    std::snprintf(buffer, sizeof buffer,
                  "PC=%06X SR=%04X SP=%06X "
                  "D=%08X %08X %08X %08X %08X %08X %08X %08X",
                  pc(), sr(), sp(),
                  data_reg(0), data_reg(1), data_reg(2), data_reg(3),
                  data_reg(4), data_reg(5), data_reg(6), data_reg(7));
    return buffer;
}

}  // namespace sm2::cpu::m68000

// ---------------------------------------------------------------------------
// Musashi's memory interface
// ---------------------------------------------------------------------------
// M68K_SEPARATE_READS is off, so instruction and PC-relative fetches come
// through these same six functions.
//
// Reads before reset() (or with no bus, which cannot happen in practice) return
// 0 rather than dereferencing null, because Musashi calls the read callbacks
// from m68k_pulse_reset to fetch the reset vector.

extern "C" {

unsigned int m68k_read_memory_8(unsigned int address)
{
    return g_bus ? g_bus->read8(address) : 0;
}

unsigned int m68k_read_memory_16(unsigned int address)
{
    return g_bus ? g_bus->read16(address) : 0;
}

unsigned int m68k_read_memory_32(unsigned int address)
{
    return g_bus ? g_bus->read32(address) : 0;
}

void m68k_write_memory_8(unsigned int address, unsigned int value)
{
    if (g_bus) {
        g_bus->write8(address, static_cast<sm2::u8>(value));
    }
}

void m68k_write_memory_16(unsigned int address, unsigned int value)
{
    if (g_bus) {
        g_bus->write16(address, static_cast<sm2::u16>(value));
    }
}

void m68k_write_memory_32(unsigned int address, unsigned int value)
{
    if (g_bus) {
        g_bus->write32(address, static_cast<sm2::u32>(value));
    }
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Instruction trace for debugging
// ---------------------------------------------------------------------------

namespace {
int g_trace_remaining = 0;
FILE* g_trace_file = nullptr;

void trace_hook(unsigned int pc)
{
    if (g_trace_remaining > 0) {
        if (g_trace_file) {
            fprintf(g_trace_file, "%06X\n", pc);
        }
        --g_trace_remaining;
        if (g_trace_remaining == 0 && g_trace_file) {
            fclose(g_trace_file);
            g_trace_file = nullptr;
            fprintf(stderr, "sm2-emu: trace complete\n");
        }
    }
}
}  // namespace

namespace sm2::cpu::m68000 {

void M68000::start_trace(int count, const char* path)
{
    g_trace_file = fopen(path, "w");
    g_trace_remaining = count;
    m68k_set_instr_hook_callback(trace_hook);
}

}  // namespace sm2::cpu::m68000
