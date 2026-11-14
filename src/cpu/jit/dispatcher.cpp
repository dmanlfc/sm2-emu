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
#include "cpu/jit/dispatcher.h"

#include "core/log.h"

#include <algorithm>

namespace sm2::cpu::jit {

bool Dispatcher::generate(CodeBuffer& code, const BlockCache& cache, const DispatcherConfig& cfg, HostArch arch)
{
    m_cfg = cfg;
    if (cfg.lookup == nullptr) return false;
    const bool ok = arch == HostArch::Arm64 ? generate_arm64(code, cache) : generate_x64(code, cache);
    if (!ok) SM2_ERROR("jit: could not generate the dispatcher");
    return ok;
}

// -- arm64 ----------------------------------------------------------------------------

bool Dispatcher::generate_arm64(CodeBuffer& code, const BlockCache& cache)
{
    using namespace a64;
    CodeBuffer::WriteScope scope(code);
    Emitter                a(code.tail());

    // Every callee-saved register: blocks may use any of them for guest state.
    const Frame frame(kCalleeSavedGprs, kCalleeSavedFprs, 0);
    const Label dispatch   = a.new_label();
    const Label miss       = a.new_label();
    const Label exit       = a.new_label();
    const Label lit_table  = a.new_label();
    const Label lit_user   = a.new_label();
    const Label lit_lookup = a.new_label();

    frame.prologue(a);
    a.mov(kContextReg, X0);
    a.mov(kBudgetReg, X1);
    for (const auto& p : m_cfg.pinned) a.ldr(X(p.reg), mem(kContextReg, p.offset));

    a.bind(dispatch);
    a.cmp(kBudgetReg, 0U);
    a.b(Cond::LE, exit);
    a.ldr(W1, mem(kContextReg, m_cfg.pc_offset));
    if (m_cfg.mode_offset >= 0) {
        a.ldr(W2, mem(kContextReg, m_cfg.mode_offset));
    } else {
        a.mov(W2, WZR);
    }
    const u32 shift = cache.fast_shift();
    const u32 width = std::min(cache.fast_bits(), 32 - shift);
    a.ldr(X17, lit_table);
    a.ubfx(W16, W1, shift, width);
    a.add(X17, X17, X16, Shift::LSL, 4);
    a.ldp(X3, X4, mem(X17));
    a.orr(X5, X1, X2, Shift::LSL, 32);  // the key; W1 was zero-extended
    a.cmp(X3, X5);
    a.b(Cond::NE, miss);
    a.br(X4);

    a.bind(miss);  // W1 = pc, W2 = mode, as lookup() wants them
    a.ldr(X0, lit_user);
    a.ldr(X16, lit_lookup);
    a.blr(X16);
    a.cbz(X0, exit);
    a.br(X0);

    a.bind(exit);
    a.mov(X0, kBudgetReg);
    frame.epilogue(a);

    while ((a.size() & 7) != 0) a.nop();
    a.bind(lit_table);
    a.emit64(reinterpret_cast<uintptr_t>(cache.fast_table()));
    a.bind(lit_user);
    a.emit64(reinterpret_cast<uintptr_t>(m_cfg.user));
    a.bind(lit_lookup);
    a.emit64(reinterpret_cast<uintptr_t>(m_cfg.lookup));

    if (!a.ok()) return false;
    u8* const base = code.commit(a.size());
    if (base == nullptr) return false;
    m_enter    = reinterpret_cast<EnterFn>(base);
    m_dispatch = a.label_address(dispatch);
    m_exit     = a.label_address(exit);
    m_size     = a.size();
    return true;
}

namespace a64 {

void emit_charge(Emitter& a, u32 cycles)
{
    if (Emitter::addsub_imm_encodable(cycles)) {
        a.subs(kBudgetReg, kBudgetReg, cycles);
    } else {
        a.mov(X16, u64{cycles});
        a.subs(kBudgetReg, kBudgetReg, X16);
    }
}

BlockExit emit_linked_exit(Emitter& a, const Dispatcher& d, u32 cycles, u32 target_pc, u32 target_mode)
{
    emit_charge(a, cycles);
    const Label fallback = a.new_label();
    a.b(Cond::LE, fallback);
    const usize site = a.size();
    a.b(fallback);  // the link site
    a.bind(fallback);
    const usize fb = a.size();
    a.mov(W16, u64{target_pc});
    a.str(W16, mem(kContextReg, d.config().pc_offset));
    a.jump(d.dispatch());
    return BlockExit{a.exec_at(site), a.exec_at(fb), target_pc, target_mode};
}

void emit_indirect_exit(Emitter& a, const Dispatcher& d, u32 cycles, Reg pc)
{
    a.str(pc.w(), mem(kContextReg, d.config().pc_offset));
    emit_charge(a, cycles);
    a.jump(d.dispatch());
}

void emit_exit_now(Emitter& a, const Dispatcher& d, u32 pc)
{
    a.mov(W16, u64{pc});
    a.str(W16, mem(kContextReg, d.config().pc_offset));
    a.jump(d.exit());
}

}  // namespace a64

// -- x86-64 ---------------------------------------------------------------------------

bool Dispatcher::generate_x64(CodeBuffer& code, const BlockCache& cache)
{
    using namespace x64;
    CodeBuffer::WriteScope scope(code);
    Emitter                a(code.tail());

    const AbiInfo& abi = abi_info(kHostAbi);
    const Frame    frame(kHostAbi, abi.callee_saved_gprs, abi.callee_saved_xmms, 0);
    const Label    dispatch = a.new_label();
    const Label    miss     = a.new_label();
    const Label    exit     = a.new_label();

    frame.prologue(a);
    a.mov(kContextReg, abi.args[0]);
    a.mov(kBudgetReg, abi.args[1]);
    for (const auto& p : m_cfg.pinned) a.mov(Gp{p.reg, 8}, ptr(kContextReg, static_cast<s32>(p.offset)));

    // pc and mode go straight into lookup()'s argument registers.
    const Gp pc   = abi.args[1].r32();
    const Gp mode = abi.args[2].r32();

    a.bind(dispatch);
    a.test(kBudgetReg, kBudgetReg);
    a.j(Cond::LE, exit);
    a.mov(pc, ptr(kContextReg, static_cast<s32>(m_cfg.pc_offset)));
    if (m_cfg.mode_offset >= 0) {
        a.mov(mode, ptr(kContextReg, m_cfg.mode_offset));
    } else {
        a.mov(mode, u64{0});
    }
    const u32 shift = cache.fast_shift();
    const u32 width = std::min(cache.fast_bits(), 31U);
    a.mov(EAX, pc);
    if (shift != 0) a.shr(EAX, static_cast<u8>(shift));
    a.and_(EAX, static_cast<s32>((1U << width) - 1));
    a.shl(RAX, 4);
    a.mov_address(R10, cache.fast_table());
    a.add(R10, RAX);
    a.mov(R11, mode.r64());
    a.shl(R11, 32);
    a.or_(R11, pc.r64());
    a.cmp(R11, ptr(R10));
    a.j(Cond::NE, miss);
    a.jmp(ptr(R10, 8));

    a.bind(miss);
    a.mov(abi.args[0], static_cast<u64>(reinterpret_cast<uintptr_t>(m_cfg.user)));
    a.call(reinterpret_cast<const void*>(m_cfg.lookup));
    a.test(RAX, RAX);
    a.j(Cond::E, exit);
    a.jmp(RAX);

    a.bind(exit);
    a.mov(RAX, kBudgetReg);
    frame.epilogue(a);

    if (!a.ok()) return false;
    u8* const base = code.commit(a.size());
    if (base == nullptr) return false;
    m_enter    = reinterpret_cast<EnterFn>(base);
    m_dispatch = a.label_address(dispatch);
    m_exit     = a.label_address(exit);
    m_size     = a.size();
    return true;
}

namespace x64 {

void emit_charge(Emitter& a, u32 cycles)
{
    a.sub(kBudgetReg, static_cast<s32>(cycles));
}

BlockExit emit_linked_exit(Emitter& a, const Dispatcher& d, u32 cycles, u32 target_pc, u32 target_mode)
{
    emit_charge(a, cycles);
    const Label fallback = a.new_label();
    a.j_short(Cond::LE, fallback);
    const usize site = a.size();
    a.jmp(fallback);  // unbound, so always the 5-byte rel32 form: the link site
    a.bind(fallback);
    const usize fb = a.size();
    a.mov(dword_ptr(kContextReg, static_cast<s32>(d.config().pc_offset)), static_cast<s32>(target_pc));
    a.jump(d.dispatch());
    return BlockExit{a.exec_at(site), a.exec_at(fb), target_pc, target_mode};
}

void emit_indirect_exit(Emitter& a, const Dispatcher& d, u32 cycles, Gp pc)
{
    a.mov(ptr(kContextReg, static_cast<s32>(d.config().pc_offset)), pc.r32());
    emit_charge(a, cycles);
    a.jump(d.dispatch());
}

void emit_exit_now(Emitter& a, const Dispatcher& d, u32 pc)
{
    a.mov(dword_ptr(kContextReg, static_cast<s32>(d.config().pc_offset)), static_cast<s32>(pc));
    a.jump(d.exit());
}

}  // namespace x64

}  // namespace sm2::cpu::jit
