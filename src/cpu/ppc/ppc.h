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
#pragma once

#include "core/types.h"
#include "cpu/page_map.h"
#include "cpu/ppc/ppc_ir.h"
#include "cpu/ppc/ppc_state.h"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// PowerPC 603e / 603ev core for Sega Model 3, written from the PowerPC
// manuals.
//
// Blocks of IR (ppc_ir.h) are interpreted or run natively via a BlockBackend.
// One cycle per instruction; TB/DEC tick every timer_ratio() = 4*cpu_hz/bus_hz
// cycles. set_irq() is a level, taken at an instruction boundary while
// MSR[EE]. Address translation is identity (Model 3 only programs identity
// BATs); misaligned accesses never fault. Fetch from unmapped memory faults.

namespace sm2 {
class Archive;
}
namespace sm2::hw {
struct CpuStatus;
}

namespace sm2::cpu::ppc {

enum class Model : u8 {
    Ppc603e,   ///< MPC603e, PVR 0x00060103 (step 1.5).
    Ppc603ev,  ///< MPC603ev ("603r", PVR 0x00071202), for steps 1.0 and 2.x.
};

enum class ExecMode : u8 {
    Interpreter,  ///< IR interpreter only.
    Jit,          ///< Native blocks from the attached BlockBackend where it made them.
};

struct PpcConfig {
    Model model  = Model::Ppc603ev;
    u32   cpu_hz = 66'000'000;  ///< rom::Model3Spec::cpu_hz().
    u32   bus_hz = 66'000'000;  ///< 66 MHz on every step; sets the timebase rate.
    u32   pvr    = 0;           ///< Non-zero overrides the model's PVR.
};

/// Slow path for unmapped pages; values are guest (big-endian) values.
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
    Ppc(const Ppc&)            = delete;
    Ppc& operator=(const Ppc&) = delete;

    void init(const PpcConfig& config, PageMap& pages, PpcBus& bus);

    void reset();

    /// Runs exactly `cycles` instructions unless end_timeslice() intervenes.
    s64 run(s64 cycles);

    void end_timeslice();

    void set_irq(bool asserted);

    /// Jit attaches the host's backend if none is set.
    void set_exec_mode(ExecMode mode);
    [[nodiscard]] ExecMode exec_mode() const { return m_mode; }

    /// Exact inside bus callbacks.
    [[nodiscard]] u64 total_cycles() const;

    [[nodiscard]] hw::CpuStatus status() const;

    void serialize(Archive& ar);

    /// After DMA into code or a ROM bank switch. Safe inside bus callbacks.
    void invalidate(u32 address, u32 size);

    void flush_blocks();

    /// Flushes existing blocks.
    void set_backend(BlockBackend* backend);
    [[nodiscard]] BlockBackend* backend() const { return m_backend; }

    [[nodiscard]] PpcState& state() { return m_st; }
    [[nodiscard]] const PpcState& state() const { return m_st; }
    [[nodiscard]] u32 pc() const { return m_st.pc; }
    [[nodiscard]] u32 timer_ratio() const { return m_st.timer_ratio; }
    [[nodiscard]] u64 timebase() const;
    [[nodiscard]] u32 decrementer() const;
    [[nodiscard]] bool faulted() const { return m_faulted; }
    [[nodiscard]] const std::string& fault_message() const { return m_fault_message; }
    [[nodiscard]] std::string state_string() const;

    [[nodiscard]] u32 read_spr(u32 spr);
    void write_spr(u32 spr, u32 value);
    void write_msr(u32 value);

    /// Called before every instruction; makes run() single-step.
    using TraceHook = void (*)(void* user, Ppc& cpu, u32 pc, u32 insn);
    void set_trace_hook(TraceHook hook, void* user)
    {
        m_trace = hook;
        m_trace_user = user;
    }

    struct Stats {
        u64 blocks_built       = 0;
        u64 blocks_invalidated = 0;
        u64 block_runs         = 0;
        u64 helper_calls       = 0;
    };
    [[nodiscard]] const Stats& stats() const { return m_stats; }

    IrBlock* block_at(u32 pc);

    // Runtime entry points behind ppc_rt_*.
    u8  slow_read8(u32 ea);
    u16 slow_read16(u32 ea);
    u32 slow_read32(u32 ea);
    u64 slow_read64(u32 ea);
    void slow_write8(u32 ea, u8 v);
    void slow_write16(u32 ea, u16 v);
    void slow_write32(u32 ea, u32 v);
    void slow_write64(u32 ea, u64 v);
    u32 helper(u32 insn, u32 insn_index);

private:
    friend struct Interp;

    enum : u32 {
        kPendIrq = 1,
        kPendDec = 2,
    };

    // run loop pieces
    IrBlock* lookup(u32 pc, u32 key);
    IrBlock* build(u32 pc, u32 key);
    void     kill_block(IrBlock* b);
    template <class T> void slow_write_impl(u32 ea, T v);
    void     free_dead();
    u32      interpret(IrBlock& b, u32 max_insns);
    void     check_interrupts();
    void     fault(std::string message);

    // exceptions and supervisor state
    void take_exception(u32 vector, u32 srr0, u32 srr1_or);
    bool set_msr(u32 value);  ///< false if the mode is unsupported (faults).
    [[nodiscard]] u32 block_key() const { return m_st.fpscr & kKeyRnMask; }

    // timers
    [[nodiscard]] u64 now_cycles(u32 insn_index) const { return m_st.block_cycles + insn_index; }
    [[nodiscard]] u64 ticks_at(u64 cycles) const { return cycles / m_st.timer_ratio; }
    [[nodiscard]] u64 read_tb(u64 cycles) const { return m_st.tb_offset + ticks_at(cycles); }
    void write_tbl(u64 cycles, u32 v);
    void write_tbu(u64 cycles, u32 v);
    [[nodiscard]] u32 read_dec(u64 cycles) const
    {
        return static_cast<u32>(m_dec_base - ticks_at(cycles));
    }
    void write_dec(u64 cycles, u32 v);
    void update_dec_deadline();

    // helper interpreter pieces (ppc_helper.cpp)
    u32  mfspr(u32 spr, u32 insn_index);
    void mtspr(u32 spr, u32 value, u32 insn_index);
    void mtfpscr(u32 value, u32 mask);
    void unknown_insn(u32 insn);

    // fast memory, for the helper and interpreter
    template <class T> T load(u32 ea);
    template <class T> void store(u32 ea, T v);

    PpcState m_st{};

    PpcConfig m_config{};
    PageMap*  m_pages = nullptr;
    PpcBus*   m_bus   = nullptr;
    ExecMode  m_mode  = ExecMode::Interpreter;
    BlockBackend* m_backend = nullptr;
    std::unique_ptr<BlockBackend> m_own_backend;

    // timing
    u64 m_cycles       = 0;   ///< Completed cycles, at block granularity.
    u64 m_run_target   = 0;
    u64 m_dec_base     = 0;
    u64 m_dec_deadline = 0;   ///< Cycle at which DEC next reaches 0xffffffff.
    u32 m_pending      = 0;   ///< kPend* bits.
    bool m_irq_line    = false;
    bool m_in_run      = false;
    bool m_in_block    = false;

    // status
    bool        m_faulted = false;
    std::string m_fault_message;
    u64         m_instructions = 0;

    // block cache
    static constexpr u32 kFastBits = 16;
    std::array<IrBlock*, 1u << kFastBits> m_fast{};
    std::unordered_map<u64, std::unique_ptr<IrBlock>> m_blocks;
    std::unordered_map<u32, std::vector<IrBlock*>>   m_page_blocks;
    std::vector<std::unique_ptr<IrBlock>>            m_dead;

    TraceHook m_trace = nullptr;
    void*     m_trace_user = nullptr;

    Stats m_stats;
};

}  // namespace sm2::cpu::ppc

// C entry points for generated code (see ppc_ir.h).
extern "C" {
sm2::u8  ppc_rt_read8(sm2::cpu::ppc::PpcState* st, sm2::u32 ea);
sm2::u16 ppc_rt_read16(sm2::cpu::ppc::PpcState* st, sm2::u32 ea);
sm2::u32 ppc_rt_read32(sm2::cpu::ppc::PpcState* st, sm2::u32 ea);
sm2::u64 ppc_rt_read64(sm2::cpu::ppc::PpcState* st, sm2::u32 ea);
void     ppc_rt_write8(sm2::cpu::ppc::PpcState* st, sm2::u32 ea, sm2::u8 v);
void     ppc_rt_write16(sm2::cpu::ppc::PpcState* st, sm2::u32 ea, sm2::u16 v);
void     ppc_rt_write32(sm2::cpu::ppc::PpcState* st, sm2::u32 ea, sm2::u32 v);
void     ppc_rt_write64(sm2::cpu::ppc::PpcState* st, sm2::u32 ea, sm2::u64 v);
sm2::u32 ppc_rt_helper(sm2::cpu::ppc::PpcState* st, sm2::u32 insn, sm2::u32 insn_index);
}
