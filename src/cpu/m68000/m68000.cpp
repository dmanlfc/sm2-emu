// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
//
// See m68000.h. The core itself is Musashi; this file is only the glue between
// Musashi's global memory callbacks and a cpu::Bus.

#include "cpu/m68000/m68000.h"

#include "core/archive.h"
#include "core/log.h"

#include <cassert>
#include <algorithm>
#include <cstdio>

extern "C" {
#include <m68k.h>
// Defined in m68kcpu.c but missing from m68k.h.
void m68k_set_cmpild_instr_callback(void (*callback)(unsigned int, int));
void m68k_set_rte_instr_callback(void (*callback)(void));
}

namespace sm2::cpu::m68000 {
class M68000;
}

namespace {

/// Musashi's global memory binding and current-context tracking. Only one
/// instance's context is live at a time; make_current swaps it and g_bus in.
sm2::cpu::Bus* g_bus = nullptr;
const sm2::cpu::m68000::M68000* g_current = nullptr;
bool g_musashi_inited = false;

/// Point the live context's cycle tables and callbacks at this process's copies.
void rebind_host_pointers();

}  // namespace

namespace sm2::cpu::m68000 {

M68000::M68000(Bus& bus) : m_bus(&bus)
{
    if (!g_musashi_inited) {
        m68k_init();
        m68k_set_cpu_type(M68K_CPU_TYPE_68000);
        g_musashi_inited = true;
    }

    // Seed from Musashi's initialised state, not zeros: a zero context wipes the
    // cpu type and callbacks and the next pulse_reset segfaults.
    m_context.assign(m68k_context_size(), 0);
    std::vector<u8> saved(m68k_context_size(), 0);
    m68k_get_context(saved.data());
    std::copy(saved.begin(), saved.end(), m_context.begin());
    m68k_set_context(saved.data());
}

M68000::~M68000()
{
    if (g_current == this) {
        g_current = nullptr;
        g_bus     = nullptr;
    }
}

void M68000::make_current() const
{
    if (g_current == this) {
        return;
    }
    if (g_current != nullptr) {
        m68k_get_context(g_current->m_context.data());
    }
    m68k_set_context(m_context.data());
    g_current = this;
    g_bus     = m_bus;
}

void M68000::reset()
{
    make_current();
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

    make_current();

    // Musashi drops the interrupt level when it takes the exception, so a level
    // that is still being held has to be put back. See set_irq_line.
    apply_irq();

    m_running = true;
    m_stalled = 0;
    // Separate statement: stall() updates m_stalled during the call.
    const s32 executed = m68k_execute(cycles);
    const s32 used     = executed + m_stalled;
    m_running = false;
    m_total_cycles += static_cast<u64>(used < 0 ? 0 : used);
    return used;
}

void M68000::stall(s32 cycles)
{
    if (!m_running) {
        return;
    }
    // Shrinking the timeslice ends it that much sooner without counting towards
    // what Musashi reports as used, so run() adds the stalls back itself.
    m68k_modify_timeslice(-cycles);
    m_stalled += cycles;
}

void M68000::set_irq_line(int level, bool asserted)
{
    make_current();
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
    make_current();
    m_irq_mask = 0;
    apply_irq();
}

void M68000::set_irq_level(int level)
{
    make_current();
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
    make_current();
    return m68k_get_reg(nullptr, M68K_REG_PC);
}

u32 M68000::sr() const
{
    make_current();
    return m68k_get_reg(nullptr, M68K_REG_SR);
}

u32 M68000::sp() const
{
    make_current();
    return m68k_get_reg(nullptr, M68K_REG_SP);
}

u32 M68000::data_reg(int index) const
{
    make_current();
    return m68k_get_reg(nullptr, static_cast<m68k_register_t>(M68K_REG_D0 + (index & 7)));
}

u32 M68000::address_reg(int index) const
{
    make_current();
    return m68k_get_reg(nullptr, static_cast<m68k_register_t>(M68K_REG_A0 + (index & 7)));
}

void M68000::serialize(Archive& ar)
{
    if (ar.saving()) {
        make_current();
        m68k_get_context(m_context.data());
        // m68ki_cpu_core ends with 13 host pointers (cycle tables, callbacks),
        // rebound on load; zero them so a state does not depend on the process.
        std::vector<u8> blob = m_context;
        constexpr usize kPointerTail = 13 * sizeof(void*);
        if (blob.size() >= kPointerTail) {
            std::fill(blob.end() - static_cast<std::ptrdiff_t>(kPointerTail), blob.end(), u8{0});
        }
        ar.vector_pod(blob);
    } else {
        ar.vector_pod(m_context);
    }
    ar.raw(m_irq_mask);
    ar.raw(m_total_cycles);
    if (ar.loading() && !ar.failed()) {
        g_current = nullptr;  // force make_current to load the restored blob
        make_current();
        rebind_host_pointers();
        m68k_get_context(m_context.data());
    }
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
// M68K_SEPARATE_READS is off, so instruction fetches come through these too.
// Reads with no bus return 0: m68k_pulse_reset fetches the reset vector.

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

void trace_hook(unsigned int pc);

void rebind_host_pointers()
{
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_set_int_ack_callback(nullptr);
    m68k_set_bkpt_ack_callback(nullptr);
    m68k_set_reset_instr_callback(nullptr);
    m68k_set_cmpild_instr_callback(nullptr);
    m68k_set_rte_instr_callback(nullptr);
    m68k_set_tas_instr_callback(nullptr);
    m68k_set_illg_instr_callback(nullptr);
    m68k_set_trap_instr_callback(nullptr);
    m68k_set_pc_changed_callback(nullptr);
    m68k_set_fc_callback(nullptr);
    m68k_set_instr_hook_callback(g_trace_remaining > 0 ? trace_hook : nullptr);
}

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
            fprintf(stderr, "keisho: trace complete\n");
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
