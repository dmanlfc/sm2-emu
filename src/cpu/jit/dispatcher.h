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

#include "cpu/jit/arm64_emitter.h"
#include "cpu/jit/block_cache.h"
#include "cpu/jit/x64_emitter.h"

#include "core/types.h"

#include <vector>

// The host loop that runs blocks (see block_cache.h):
//   enter:    save callee-saved regs, ctx -> X19/RBX, cycles -> X20/R14,
//             load pinned registers
//   dispatch: budget <= 0 -> exit; probe fast table for (ctx->pc, ctx->mode);
//             miss -> lookup(user, pc, mode), nullptr -> exit
//   exit:     restore, return the remaining budget
// Generate it before CodeBuffer::set_persistent_mark().

namespace sm2::cpu::jit {

/// Find or compile the block for (pc, mode); nullptr stops run().
using LookupFn = const void* (*)(void* user, u32 pc, u32 mode);

struct DispatcherConfig {
    u32      pc_offset   = 0;
    s32      mode_offset = -1;  ///< -1: the mode key is always 0.
    LookupFn lookup      = nullptr;
    void*    user        = nullptr;

    /// u64 [context + offset] -> host register `reg` on entry
    /// (arm64 X21-X28; x86-64 R12, R13, R15, plus RSI/RDI on Win64).
    struct Pinned {
        u8  reg;
        u32 offset;
    };
    std::vector<Pinned> pinned;
};

class Dispatcher {
public:
    using EnterFn = s64 (*)(void* ctx, s64 cycles);

    /// Emit the dispatcher into `code` (call before code.set_persistent_mark()).
    /// `arch` other than the host's is for inspection only.
    [[nodiscard]] bool generate(CodeBuffer& code, const BlockCache& cache, const DispatcherConfig& cfg,
                                HostArch arch = kHostArch);

    /// Run blocks until the budget is spent or lookup returns nullptr.
    /// Returns the remaining budget.
    s64 run(void* ctx, s64 cycles) const { return m_enter(ctx, cycles); }

    [[nodiscard]] const u8*               dispatch() const { return m_dispatch; }
    [[nodiscard]] const u8*               exit() const { return m_exit; }
    [[nodiscard]] EnterFn                 enter() const { return m_enter; }
    [[nodiscard]] const DispatcherConfig& config() const { return m_cfg; }
    [[nodiscard]] usize                   size() const { return m_size; }

private:
    bool generate_arm64(CodeBuffer& code, const BlockCache& cache);
    bool generate_x64(CodeBuffer& code, const BlockCache& cache);

    DispatcherConfig m_cfg;
    EnterFn          m_enter    = nullptr;
    const u8*        m_dispatch = nullptr;
    const u8*        m_exit     = nullptr;
    usize            m_size     = 0;
};

// -- Exit sequences for block code -------------------------------------------------

namespace a64 {

inline constexpr Reg kContextReg = X19;
inline constexpr Reg kBudgetReg  = X20;

/// Charge `cycles`, then a linkable jump to `target_pc`. Pass the result to
/// BlockCache::insert() after commit. Clobbers W16 and flags.
[[nodiscard]] BlockExit emit_linked_exit(Emitter& a, const Dispatcher& d, u32 cycles, u32 target_pc,
                                         u32 target_mode = 0);
/// Charge `cycles`, store `pc` (a W register) as the next PC and go back to
/// the dispatcher (for indirect branches).
void emit_indirect_exit(Emitter& a, const Dispatcher& d, u32 cycles, Reg pc);
/// Store `pc` and leave run() at once, whatever the budget.
void emit_exit_now(Emitter& a, const Dispatcher& d, u32 pc);
/// `subs budget, budget, #cycles` for any cycle count (uses X16 if large).
void emit_charge(Emitter& a, u32 cycles);

}  // namespace a64

namespace x64 {

inline constexpr Gp kContextReg = RBX;
inline constexpr Gp kBudgetReg  = R14;

[[nodiscard]] BlockExit emit_linked_exit(Emitter& a, const Dispatcher& d, u32 cycles, u32 target_pc,
                                         u32 target_mode = 0);
/// `pc` is a 32-bit register.
void emit_indirect_exit(Emitter& a, const Dispatcher& d, u32 cycles, Gp pc);
void emit_exit_now(Emitter& a, const Dispatcher& d, u32 pc);
void emit_charge(Emitter& a, u32 cycles);

}  // namespace x64

}  // namespace sm2::cpu::jit
