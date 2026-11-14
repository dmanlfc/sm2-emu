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

// Run loop, block cache, timers, exceptions, slow memory paths.

#include "cpu/ppc/ppc.h"

#include "core/archive.h"
#include "core/log.h"
#include "cpu/ppc/ppc_internal.h"
#include "cpu/ppc/ppc_jit_arm64.h"
#include "cpu/ppc/ppc_jit_x64.h"
#include "hw/machine.h"

#include <algorithm>
#include <cstdio>

namespace sm2::cpu::ppc {

namespace {

constexpr u64 kDecPeriod = 1ull << 32;  // ticks between DEC passing 0 -> -1 twice

u32 model_pvr(Model model)
{
    switch (model) {
        case Model::Ppc603e:  return 0x00060103;
        case Model::Ppc603ev: return 0x00071202;
    }
    return 0x00071202;
}

/// HID1[PLL_CFG] from the 603e / 603r PLL tables (1:1 on a 603r is unlisted: 0xf).
u32 model_hid1(Model model, u32 cpu_hz, u32 bus_hz)
{
    const u32 half_ratio = bus_hz != 0 ? static_cast<u32>((2ull * cpu_hz + bus_hz / 2) / bus_hz) : 2;
    u32 pll = 0xf;
    if (model == Model::Ppc603e) {
        switch (half_ratio) {
            case 2: pll = 0x1; break;  // 1:1   (listed only for 33-50 MHz; closest)
            case 3: pll = 0xc; break;  // 1.5:1
            case 4: pll = 0x4; break;  // 2:1
            default: break;
        }
    } else {
        switch (half_ratio) {
            case 4: pll = 0x5; break;  // 2:1
            case 5: pll = 0x6; break;  // 2.5:1
            case 6: pll = 0x8; break;  // 3:1
            default: break;
        }
    }
    return pll << 28;
}

}  // namespace

Ppc::Ppc()
{
    m_st.timer_ratio = 4;
}

Ppc::~Ppc()
{
    flush_blocks();
    free_dead();
}

void Ppc::init(const PpcConfig& config, PageMap& pages, PpcBus& bus)
{
    flush_blocks();
    m_config = config;
    m_pages  = &pages;
    m_bus    = &bus;
    const u64 ratio = config.bus_hz != 0 ? (4ull * config.cpu_hz) / config.bus_hz : 4;
    m_st.timer_ratio = static_cast<u32>(std::max<u64>(1, ratio));
    reset();
}

void Ppc::reset()
{
    flush_blocks();
    free_dead();

    const u32 ratio = m_st.timer_ratio;
    m_st = PpcState{};
    m_st.timer_ratio = ratio;
    m_st.read_table  = m_pages != nullptr ? m_pages->read_table() : nullptr;
    m_st.write_table = m_pages != nullptr ? m_pages->write_table() : nullptr;
    m_st.cpu = this;
    m_st.pvr  = m_config.pvr != 0 ? m_config.pvr : model_pvr(m_config.model);
    m_st.hid0 = 1;  // deliberately not the 603e's documented reset value of 0
    m_st.hid1 = model_hid1(m_config.model, m_config.cpu_hz, m_config.bus_hz);
    m_st.msr  = kMsrIP;
    m_st.pc   = 0xfff00100;

    m_cycles       = 0;
    m_st.block_cycles = 0;
    m_st.tb_offset    = 0;
    m_dec_base     = 0xffffffffu;
    update_dec_deadline();
    m_pending  = m_irq_line ? kPendIrq : 0u;
    m_faulted  = false;
    m_fault_message.clear();
    m_instructions = 0;
}

// ---------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------

s64 Ppc::run(s64 cycles)
{
    if (m_faulted || cycles <= 0 || m_pages == nullptr) {
        return 0;
    }
    const u64 start = m_cycles;
    m_run_target = m_cycles + static_cast<u64>(cycles);
    m_in_run = true;
    m_st.exit_flags = 0;

    while (m_cycles < m_run_target) {
        if (m_cycles >= m_dec_deadline) {
            m_pending |= kPendDec;
            m_dec_base += kDecPeriod;
            update_dec_deadline();
        }
        if (m_pending != 0 && (m_st.msr & kMsrEE) != 0) {
            check_interrupts();
        }

        const u64 limit  = std::min(m_run_target, m_dec_deadline);
        u32       budget = static_cast<u32>(std::min<u64>(limit - m_cycles, 0x7fffffff));
        if (m_trace != nullptr) {
            budget = 1;
        }

        IrBlock* const b = lookup(m_st.pc, block_key());
        if (b == nullptr) {
            break;
        }
        if (m_trace != nullptr) {
            m_trace(m_trace_user, *this, b->pc, b->raw[0]);
        }

        m_st.block_cycles = m_cycles;
        m_st.block_pc     = b->pc;
        m_in_block     = true;
        u32 n;
        if (b->native != nullptr && budget >= b->num_insns && m_mode != ExecMode::Interpreter) {
            m_st.budget = budget;
            n = b->native(&m_st);
        } else {
            n = interpret(*b, budget);
        }
        m_in_block = false;
        m_cycles += n;
        m_instructions += n;
        ++m_stats.block_runs;

        if (m_st.exit_flags != 0) {
            const bool end = (m_st.exit_flags & kExitEndSlice) != 0;
            m_st.exit_flags = 0;
            if (end || m_faulted) {
                break;
            }
        }
    }

    // A DEC deadline exactly at the slice end is taken now, not in the next slice.
    if (!m_faulted && m_cycles >= m_dec_deadline) {
        m_pending |= kPendDec;
        m_dec_base += kDecPeriod;
        update_dec_deadline();
        check_interrupts();
    }

    m_in_run = false;
    free_dead();
    return static_cast<s64>(m_cycles - start);
}

void Ppc::end_timeslice()
{
    m_st.exit_flags |= kExitBreak | kExitEndSlice;
}

void Ppc::set_irq(bool asserted)
{
    m_irq_line = asserted;
    if (!asserted) {
        m_pending &= ~kPendIrq;
        return;
    }
    m_pending |= kPendIrq;
    if (m_in_run) {
        m_st.exit_flags |= kExitBreak;  // take it at the next instruction boundary
    } else {
        check_interrupts();
    }
}

void Ppc::check_interrupts()
{
    if ((m_st.msr & kMsrEE) == 0 || m_faulted) {
        return;
    }
    if (m_pending & kPendIrq) {
        take_exception(0x500, m_st.pc, 0);
    } else if (m_pending & kPendDec) {
        m_pending &= ~kPendDec;
        take_exception(0x900, m_st.pc, 0);
    }
}

u64 Ppc::total_cycles() const
{
    return m_in_block ? m_st.block_cycles + m_st.cur_insn : m_cycles;
}

void Ppc::fault(std::string message)
{
    if (!m_faulted) {
        SM2_ERROR("PowerPC: %s (pc %08x). Halted until reset.", message.c_str(), m_st.pc);
    }
    m_faulted = true;
    m_fault_message = std::move(message);
    m_st.exit_flags |= kExitBreak | kExitEndSlice;
}

// ---------------------------------------------------------------------------
// Exceptions and MSR
// ---------------------------------------------------------------------------

void Ppc::take_exception(u32 vector, u32 srr0, u32 srr1_or)
{
    m_st.srr0 = srr0;
    m_st.srr1 = (m_st.msr & 0xff73) | srr1_or;
    u32 msr = m_st.msr & ~(kMsrPOW | kMsrEE | kMsrPR | kMsrFP | kMsrFE0 | kMsrSE | kMsrBE |
                           kMsrFE1 | kMsrIR | kMsrDR | kMsrRI);
    msr = (msr & kMsrILE) ? (msr | kMsrLE) : (msr & ~kMsrLE);
    set_msr(msr);
    m_st.pc = ((msr & kMsrIP) ? 0xfff00000u : 0u) | vector;
}

bool Ppc::set_msr(u32 value)
{
    if (value & (kMsrILE | kMsrLE)) {
        m_st.msr = value;
        fault("little-endian mode is not supported");
        return false;
    }
    m_st.msr = value;
    return true;
}

void Ppc::write_msr(u32 value)
{
    if (set_msr(value) && !m_in_run) {
        check_interrupts();
    }
}

// ---------------------------------------------------------------------------
// Timebase and decrementer
// ---------------------------------------------------------------------------

u64 Ppc::timebase() const
{
    return read_tb(total_cycles());
}

u32 Ppc::decrementer() const
{
    return read_dec(total_cycles());
}

void Ppc::write_tbl(u64 cycles, u32 v)
{
    const u64 tb = (read_tb(cycles) & ~0xffffffffull) | v;
    m_st.tb_offset  = tb - ticks_at(cycles);
}

void Ppc::write_tbu(u64 cycles, u32 v)
{
    const u64 tb = (read_tb(cycles) & 0xffffffffull) | (u64{v} << 32);
    m_st.tb_offset  = tb - ticks_at(cycles);
}

void Ppc::write_dec(u64 cycles, u32 v)
{
    // The exception fires when DEC's MSB goes 0 -> 1, including by a write.
    if ((v & 0x80000000u) && !(read_dec(cycles) & 0x80000000u)) {
        m_pending |= kPendDec;
        if (m_in_run) {
            m_st.exit_flags |= kExitBreak;
        }
    }
    m_dec_base = ticks_at(cycles) + v;
    update_dec_deadline();
}

void Ppc::update_dec_deadline()
{
    m_dec_deadline = (m_dec_base + 1) * m_st.timer_ratio;
}

// ---------------------------------------------------------------------------
// Block cache
// ---------------------------------------------------------------------------

IrBlock* Ppc::lookup(u32 pc, u32 key)
{
    const u32 idx = (pc >> 2) & ((1u << kFastBits) - 1);
    IrBlock* b = m_fast[idx];
    if (b != nullptr && b->pc == pc && b->key == key) [[likely]] {
        return b;
    }
    const auto it = m_blocks.find((u64{key} << 32) | pc);
    b = it != m_blocks.end() ? it->second.get() : build(pc, key);
    if (b != nullptr) {
        m_fast[idx] = b;
    }
    return b;
}

IrBlock* Ppc::block_at(u32 pc)
{
    return lookup(pc, block_key());
}

IrBlock* Ppc::build(u32 pc, u32 key)
{
    if (pc & 3) {
        fault("misaligned instruction address");
        return nullptr;
    }
    auto block = std::make_unique<IrBlock>();
    PageMap* const pages = m_pages;
    const bool ok = decode_block(*block, pc, key, [pages](u32 addr, u32* word) {
        const u8* p = pages->read_ptr(addr);
        if (p == nullptr) {
            return false;
        }
        u32 w;
        std::memcpy(&w, p, 4);
        *word = byteswap_any(w);
        return true;
    });
    if (!ok) {
        char msg[64];
        std::snprintf(msg, sizeof msg, "instruction fetch from unmapped address %08x", pc);
        fault(msg);
        return nullptr;
    }

    // RAM pages holding code send stores to the slow path.
    const u32 first = pc >> PageMap::kPageBits;
    const u32 last  = (block->end_pc() - 1) >> PageMap::kPageBits;
    for (u32 page = first;; ++page) {
        const u32 addr = page << PageMap::kPageBits;
        if (m_pages->ram_write_ptr(addr) != nullptr) {
            m_pages->set_code_page(addr, true);
            m_page_blocks[page].push_back(block.get());
        }
        if (page == last) {
            break;
        }
    }

    if (m_backend != nullptr) {
        m_backend->compile(*block);
        if (block->native == nullptr && m_backend->full()) {
            const u32 flags = m_st.exit_flags;
            flush_blocks();
            m_st.exit_flags = flags;
            m_backend->reset_code();
            m_backend->compile(*block);
        }
    }
    ++m_stats.blocks_built;
    IrBlock* const raw = block.get();
    m_blocks.emplace((u64{key} << 32) | pc, std::move(block));
    return raw;
}

void Ppc::kill_block(IrBlock* b)
{
    if (!b->valid) {
        return;
    }
    b->valid = false;
    ++m_stats.blocks_invalidated;
    if (m_backend != nullptr) {
        m_backend->invalidated(*b);
    }

    const u32 idx = (b->pc >> 2) & ((1u << kFastBits) - 1);
    if (m_fast[idx] == b) {
        m_fast[idx] = nullptr;
    }
    const u32 first = b->pc >> PageMap::kPageBits;
    const u32 last  = (b->end_pc() - 1) >> PageMap::kPageBits;
    for (u32 page = first;; ++page) {
        const auto it = m_page_blocks.find(page);
        if (it != m_page_blocks.end()) {
            auto& v = it->second;
            v.erase(std::remove(v.begin(), v.end(), b), v.end());
            if (v.empty()) {
                m_page_blocks.erase(it);
                m_pages->set_code_page(page << PageMap::kPageBits, false);
            }
        }
        if (page == last) {
            break;
        }
    }
    const auto it = m_blocks.find((u64{b->key} << 32) | b->pc);
    if (it != m_blocks.end() && it->second.get() == b) {
        m_dead.push_back(std::move(it->second));
        m_blocks.erase(it);
    }
}

void Ppc::free_dead()
{
    if (m_in_run) {
        return;
    }
    for (auto& b : m_dead) {
        if (m_backend != nullptr) {
            m_backend->release(*b);
        }
    }
    m_dead.clear();
}

void Ppc::invalidate(u32 address, u32 size)
{
    if (size == 0) {
        return;
    }
    const u64 end = u64{address} + size;
    const u32 first = address >> PageMap::kPageBits;
    const u32 last  = static_cast<u32>((end - 1) >> PageMap::kPageBits);
    std::vector<IrBlock*> victims;
    for (u32 page = first;; ++page) {
        const auto it = m_page_blocks.find(page);
        if (it != m_page_blocks.end()) {
            for (IrBlock* b : it->second) {
                if (u64{b->pc} < end && b->end_pc() > address) {
                    victims.push_back(b);
                }
            }
        }
        if (page == last) {
            break;
        }
    }
    // ROM blocks are not in m_page_blocks.
    for (auto& [k, b] : m_blocks) {
        (void)k;
        if (m_pages->ram_write_ptr(b->pc) == nullptr && u64{b->pc} < end && b->end_pc() > address) {
            victims.push_back(b.get());
        }
    }
    for (IrBlock* b : victims) {
        kill_block(b);
    }
    if (m_in_run) {
        m_st.exit_flags |= kExitBreak;
    }
    free_dead();
}

void Ppc::flush_blocks()
{
    std::vector<IrBlock*> all;
    all.reserve(m_blocks.size());
    for (auto& [k, b] : m_blocks) {
        (void)k;
        all.push_back(b.get());
    }
    for (IrBlock* b : all) {
        kill_block(b);
    }
    m_fast.fill(nullptr);
    if (m_in_run) {
        m_st.exit_flags |= kExitBreak;
    }
}

void Ppc::set_backend(BlockBackend* backend)
{
    flush_blocks();
    free_dead();
    m_backend = backend;
}

void Ppc::set_exec_mode(ExecMode mode)
{
    m_mode = mode;
    if (mode != ExecMode::Interpreter && m_backend == nullptr) {
        if (!m_own_backend) {
            m_own_backend = create_jit_arm64();
            if (!m_own_backend) m_own_backend = create_jit_x64();
        }
        if (m_own_backend) {
            set_backend(m_own_backend.get());
        }
    }
}

// ---------------------------------------------------------------------------
// Slow memory paths
// ---------------------------------------------------------------------------

namespace {

template <class T>
[[nodiscard]] bool crosses_page(u32 ea)
{
    return (ea & PageMap::kPageMask) > PageMap::kPageSize - sizeof(T);
}

}  // namespace

u8 Ppc::slow_read8(u32 ea)
{
    if (const u8* p = m_pages->read_ptr(ea)) {
        return *p;
    }
    return m_bus->read8(ea);
}

template <class T>
static T read_split(Ppc& cpu, u32 ea)
{
    T v = 0;
    for (u32 i = 0; i < sizeof(T); ++i) {
        v = static_cast<T>((v << 8) | cpu.slow_read8(ea + i));
    }
    return v;
}

u16 Ppc::slow_read16(u32 ea)
{
    if (crosses_page<u16>(ea)) {
        return read_split<u16>(*this, ea);
    }
    if (const u8* p = m_pages->read_ptr(ea)) {
        u16 v;
        std::memcpy(&v, p, 2);
        return byteswap_any(v);
    }
    return m_bus->read16(ea);
}

u32 Ppc::slow_read32(u32 ea)
{
    if (crosses_page<u32>(ea)) {
        return read_split<u32>(*this, ea);
    }
    if (const u8* p = m_pages->read_ptr(ea)) {
        u32 v;
        std::memcpy(&v, p, 4);
        return byteswap_any(v);
    }
    return m_bus->read32(ea);
}

u64 Ppc::slow_read64(u32 ea)
{
    if (crosses_page<u64>(ea)) {
        return read_split<u64>(*this, ea);
    }
    if (const u8* p = m_pages->read_ptr(ea)) {
        u64 v;
        std::memcpy(&v, p, 8);
        return byteswap_any(v);
    }
    return m_bus->read64(ea);
}

template <class T>
void Ppc::slow_write_impl(u32 ea, T v)
{
    if (crosses_page<T>(ea)) {
        for (u32 i = 0; i < sizeof(T); ++i) {
            slow_write8(ea + i, static_cast<u8>(v >> (8 * (sizeof(T) - 1 - i))));
        }
        return;
    }
    if (u8* p = m_pages->write_ptr(ea)) {
        const T s = byteswap_any(v);
        std::memcpy(p, &s, sizeof(T));
        return;
    }
    if (u8* p = m_pages->ram_write_ptr(ea)) {
        const T s = byteswap_any(v);
        std::memcpy(p, &s, sizeof(T));
        const auto it = m_page_blocks.find(ea >> PageMap::kPageBits);
        if (it != m_page_blocks.end()) {
            std::vector<IrBlock*> victims;
            for (IrBlock* b : it->second) {
                if (b->pc < ea + sizeof(T) && b->end_pc() > ea) {
                    victims.push_back(b);
                }
            }
            for (IrBlock* b : victims) {
                kill_block(b);
            }
            if (!victims.empty() && m_in_run) {
                m_st.exit_flags |= kExitBreak;  // the running block may be one of them
            }
        }
        return;
    }
    if constexpr (sizeof(T) == 1) {
        m_bus->write8(ea, v);
    } else if constexpr (sizeof(T) == 2) {
        m_bus->write16(ea, v);
    } else if constexpr (sizeof(T) == 4) {
        m_bus->write32(ea, v);
    } else {
        m_bus->write64(ea, v);
    }
}

void Ppc::slow_write8(u32 ea, u8 v) { slow_write_impl<u8>(ea, v); }
void Ppc::slow_write16(u32 ea, u16 v) { slow_write_impl<u16>(ea, v); }
void Ppc::slow_write32(u32 ea, u32 v) { slow_write_impl<u32>(ea, v); }
void Ppc::slow_write64(u32 ea, u64 v) { slow_write_impl<u64>(ea, v); }

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

std::string Ppc::state_string() const
{
    char buf[512];
    int n = std::snprintf(buf, sizeof buf, "pc=%08x msr=%08x lr=%08x ctr=%08x cr=%08x xer=%08x",
                          m_st.pc, m_st.msr, m_st.slot[kSlotLR], m_st.slot[kSlotCTR], m_st.get_cr(),
                          m_st.get_xer());
    for (unsigned i = 0; i < 32 && n > 0 && n < static_cast<int>(sizeof buf); ++i) {
        n += std::snprintf(buf + n, sizeof buf - static_cast<usize>(n), " r%u=%08x", i, m_st.slot[i]);
    }
    return buf;
}

hw::CpuStatus Ppc::status() const
{
    hw::CpuStatus s;
    s.state_string  = state_string();
    s.fault_message = m_fault_message;
    s.instructions  = m_instructions;
    s.halted        = false;
    s.faulted       = m_faulted;
    return s;
}

void Ppc::serialize(Archive& ar)
{
    u32 version = 1;
    ar.raw(version);
    if (ar.loading() && version != 1) {
        ar.mark_failed();
        return;
    }
    ar.bytes(m_st.slot, 34);  // r0..r31, LR, CTR (not the zero slot or temporaries)
    ar.bytes(m_st.cr, 8);
    ar.raw(m_st.xer_so);
    ar.raw(m_st.xer_ov);
    ar.raw(m_st.xer_ca);
    ar.raw(m_st.xer_bc);
    ar.raw(m_st.fpscr);
    ar.bytes(m_st.fpr, 32);
    ar.raw(m_st.pc);
    ar.raw(m_st.msr);
    ar.raw(m_st.srr0);
    ar.raw(m_st.srr1);
    ar.bytes(m_st.sprg, 4);
    ar.raw(m_st.dsisr);
    ar.raw(m_st.dar);
    ar.raw(m_st.sdr1);
    ar.raw(m_st.ear);
    ar.raw(m_st.hid0);
    ar.raw(m_st.hid1);
    ar.raw(m_st.hid2);
    ar.raw(m_st.iabr);
    ar.raw(m_st.dabr);
    ar.raw(m_st.pvr);
    ar.bytes(m_st.sr, 16);
    ar.bytes(m_st.ibat, 8);
    ar.bytes(m_st.dbat, 8);
    ar.raw(m_st.dmiss);
    ar.raw(m_st.dcmp);
    ar.raw(m_st.hash1);
    ar.raw(m_st.hash2);
    ar.raw(m_st.imiss);
    ar.raw(m_st.icmp);
    ar.raw(m_st.rpa);
    ar.raw(m_st.reserved);
    ar.raw(m_st.reserved_addr);

    ar.raw(m_cycles);
    ar.raw(m_st.tb_offset);
    ar.raw(m_dec_base);
    ar.raw(m_pending);
    ar.raw(m_irq_line);
    ar.raw(m_instructions);
    ar.raw(m_faulted);

    if (ar.loading()) {
        m_st.slot[kSlotZero] = 0;
        m_st.block_cycles = m_cycles;
        update_dec_deadline();
        m_fault_message = m_faulted ? "faulted before the state was saved" : "";
        flush_blocks();
        free_dead();
    }
}

}  // namespace sm2::cpu::ppc

// ---------------------------------------------------------------------------
// C entry points for generated code
// ---------------------------------------------------------------------------

using sm2::cpu::ppc::Ppc;
using sm2::cpu::ppc::PpcState;

static Ppc& cpu_of(PpcState* st) { return *static_cast<Ppc*>(st->cpu); }

extern "C" {
sm2::u8  ppc_rt_read8(PpcState* st, sm2::u32 ea) { return cpu_of(st).slow_read8(ea); }
sm2::u16 ppc_rt_read16(PpcState* st, sm2::u32 ea) { return cpu_of(st).slow_read16(ea); }
sm2::u32 ppc_rt_read32(PpcState* st, sm2::u32 ea) { return cpu_of(st).slow_read32(ea); }
sm2::u64 ppc_rt_read64(PpcState* st, sm2::u32 ea) { return cpu_of(st).slow_read64(ea); }
void ppc_rt_write8(PpcState* st, sm2::u32 ea, sm2::u8 v) { cpu_of(st).slow_write8(ea, v); }
void ppc_rt_write16(PpcState* st, sm2::u32 ea, sm2::u16 v) { cpu_of(st).slow_write16(ea, v); }
void ppc_rt_write32(PpcState* st, sm2::u32 ea, sm2::u32 v) { cpu_of(st).slow_write32(ea, v); }
void ppc_rt_write64(PpcState* st, sm2::u32 ea, sm2::u64 v) { cpu_of(st).slow_write64(ea, v); }
sm2::u32 ppc_rt_helper(PpcState* st, sm2::u32 insn, sm2::u32 index) { return cpu_of(st).helper(insn, index); }
}
