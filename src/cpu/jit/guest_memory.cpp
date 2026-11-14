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
#include "cpu/jit/guest_memory.h"

#include "cpu/page_map.h"

#include <bit>

namespace sm2::cpu::jit {

namespace {

constexpr u32 kPageBits = PageMap::kPageBits;
constexpr u32 kPageMask = PageMap::kPageMask;
constexpr u32 kPageSize = PageMap::kPageSize;

u32 size_index(u32 size)
{
    switch (size) {
    case 1: return 0;
    case 2: return 1;
    case 4: return 2;
    default: return 3;
    }
}

}  // namespace

// == arm64 ==========================================================================

namespace a64 {

Label GuestMemory::lookup(u32 size, Reg addr, Reg table)
{
    Emitter&    a    = m_a;
    const Label slow = a.new_label();
    a.lsr(W16, addr.w(), kPageBits);
    a.ldr(X16, mem(table.as_x(), X16, Extend::UXTX, true));
    a.cbz(X16, slow);
    a.and_(W17, addr.w(), u64{kPageMask});
    if (size > 1) {
        a.cmp(W17, kPageSize - size);
        a.b(Cond::HI, slow);
    }
    return slow;
}

void GuestMemory::load(u32 size, Reg dst, Reg addr, bool sign_extend, u32 live_gprs, u32 live_fprs)
{
    Emitter&    a    = m_a;
    const Label slow = lookup(size, addr, m_cfg.read_table);
    const Mem   m    = mem(X16, X17);
    switch (size) {
    case 1:
        if (sign_extend) {
            a.ldrsb(dst, m);
        } else {
            a.ldrb(dst.w(), m);
        }
        break;
    case 2:
        a.ldrh(dst.w(), m);
        a.rev16(dst.w(), dst.w());
        if (sign_extend) a.sxth(dst, dst.w());
        break;
    case 4:
        a.ldr(dst.w(), m);
        a.rev(dst.w(), dst.w());
        if (sign_extend) {
            if (dst.x) a.sxtw(dst, dst.w());
        }
        break;
    default:
        a.ldr(dst.as_x(), m);
        a.rev(dst.as_x(), dst.as_x());
        break;
    }
    const Label resume = a.new_label();
    a.bind(resume);
    m_slow.push_back(Slow{slow, resume, false, false, sign_extend, size, dst, D0, addr, live_gprs, live_fprs});
}

void GuestMemory::store(u32 size, Reg value, Reg addr, u32 live_gprs, u32 live_fprs)
{
    Emitter&    a    = m_a;
    const Label slow = lookup(size, addr, m_cfg.write_table);
    switch (size) {
    case 1: a.strb(value.w(), mem(X16, X17)); break;
    case 2:
        a.add(X16, X16, X17);
        a.rev16(W17, value.w());
        a.strh(W17, mem(X16));
        break;
    case 4:
        a.add(X16, X16, X17);
        a.rev(W17, value.w());
        a.str(W17, mem(X16));
        break;
    default:
        a.add(X16, X16, X17);
        a.rev(X17, value.as_x());
        a.str(X17, mem(X16));
        break;
    }
    const Label resume = a.new_label();
    a.bind(resume);
    m_slow.push_back(Slow{slow, resume, true, false, false, size, value, D0, addr, live_gprs, live_fprs});
}

void GuestMemory::load_fp(VReg dst, Reg addr, u32 live_gprs, u32 live_fprs)
{
    Emitter&    a    = m_a;
    const u32   size = dst.size == VSize::D ? 8 : 4;
    const Label slow = lookup(size, addr, m_cfg.read_table);
    const Reg   t    = size == 8 ? X17 : W17;
    a.ldr(t, mem(X16, X17));
    a.rev(t, t);
    a.fmov(dst, t);
    const Label resume = a.new_label();
    a.bind(resume);
    m_slow.push_back(Slow{slow, resume, false, true, false, size, X0, dst, addr, live_gprs, live_fprs});
}

void GuestMemory::store_fp(VReg value, Reg addr, u32 live_gprs, u32 live_fprs)
{
    Emitter&    a    = m_a;
    const u32   size = value.size == VSize::D ? 8 : 4;
    const Label slow = lookup(size, addr, m_cfg.write_table);
    const Reg   t    = size == 8 ? X17 : W17;
    a.add(X16, X16, X17);
    a.fmov(t, value);
    a.rev(t, t);
    a.str(t, mem(X16));
    const Label resume = a.new_label();
    a.bind(resume);
    m_slow.push_back(Slow{slow, resume, true, true, false, size, X0, value, addr, live_gprs, live_fprs});
}

void GuestMemory::finish()
{
    Emitter& a = m_a;
    for (const Slow& s : m_slow) {
        a.bind(s.entry);

        u32 gprs = s.live_gprs & kCallerSavedGprs & ~((1U << 16) | (1U << 17));
        u32 fprs = s.live_fprs & kCallerSavedFprs;
        if (!s.store && !s.fp) gprs &= ~(1U << s.data.id);
        if (!s.store && s.fp) fprs &= ~(1U << s.vdata.id);
        push_regs(a, gprs, fprs);

        // Stage through X16/X17 so no argument register is overwritten
        // before it has been read.
        a.mov(W16, s.addr.w());
        if (s.store) {
            if (s.fp) {
                a.fmov(s.size == 8 ? X17 : W17, s.vdata);
            } else if (s.size == 8) {
                a.mov(X17, s.data.as_x());
            } else if (s.size == 4) {
                a.mov(W17, s.data.w());
            } else {
                a.ubfx(W17, s.data.w(), 0, 8 * s.size);  // zero-extended, as documented
            }
        }
        a.mov(W1, W16);
        if (s.store) a.mov(X2, X17);
        a.mov(X0, m_cfg.ctx.as_x());
        const u32 fn = size_index(s.size);
        if (s.store) {
            a.call(reinterpret_cast<const void*>(m_cfg.fns.write[fn]));
        } else {
            a.call(reinterpret_cast<const void*>(m_cfg.fns.read[fn]));
            if (s.fp) {
                a.fmov(s.vdata, s.size == 8 ? X0 : W0);
            } else if (s.size == 8) {
                a.mov(s.data.as_x(), X0);
            } else if (s.sign) {
                if (s.size == 1) a.sxtb(s.data, W0);
                if (s.size == 2) a.sxth(s.data, W0);
                if (s.size == 4) {
                    if (s.data.x) {
                        a.sxtw(s.data, W0);
                    } else {
                        a.mov(s.data, W0);
                    }
                }
            } else {
                a.mov(s.data.w(), W0);
            }
        }

        pop_regs(a, gprs, fprs);
        a.b(s.resume);
    }
    m_slow.clear();
}

}  // namespace a64

// == x86-64 =========================================================================

namespace x64 {

namespace {

u32 caller_saved_gprs(Abi abi)
{
    // RAX RCX RDX R8-R11, plus RSI RDI for SysV.
    return abi == Abi::Win64 ? 0x0F07U : 0x0FC7U;
}

u32 caller_saved_xmms(Abi abi)
{
    return abi == Abi::Win64 ? 0x003FU : 0xFFFFU;
}

}  // namespace

Label GuestMemory::lookup(u32 size, Gp addr, Gp table)
{
    Emitter&    a    = m_a;
    const Label slow = a.new_label();
    a.mov(R11D, addr.r32());
    a.shr(R11D, static_cast<u8>(kPageBits));
    a.mov(R11, ptr(table.r64(), R11, 8));
    a.test(R11, R11);
    a.j(Cond::Z, slow);
    a.mov(R10D, addr.r32());
    a.and_(R10D, static_cast<s32>(kPageMask));
    if (size > 1) {
        a.cmp(R10D, static_cast<s32>(kPageSize - size));
        a.j(Cond::A, slow);
    }
    return slow;
}

void GuestMemory::load(u32 size, Gp dst, Gp addr, bool sign_extend, u32 live_gprs, u32 live_xmms)
{
    Emitter&    a    = m_a;
    const Label slow = lookup(size, addr, m_cfg.read_table);
    const Mem   m    = ptr(R11, R10, 1);
    switch (size) {
    case 1:
        if (sign_extend) {
            a.movsx(dst, sized(m, 1));
        } else {
            a.movzx(dst.r32(), sized(m, 1));
        }
        break;
    case 2:
        a.movzx(dst.r32(), sized(m, 2));
        a.rol(dst.r16(), 8);
        if (sign_extend) a.movsx(dst, dst.r16());
        break;
    case 4:
        if (m_cfg.use_movbe) {
            a.movbe(dst.r32(), m);
        } else {
            a.mov(dst.r32(), m);
            a.bswap(dst.r32());
        }
        if (sign_extend && dst.size == 8) a.movsx(dst, dst.r32());
        break;
    default:
        if (m_cfg.use_movbe) {
            a.movbe(dst.r64(), m);
        } else {
            a.mov(dst.r64(), m);
            a.bswap(dst.r64());
        }
        break;
    }
    const Label resume = a.new_label();
    a.bind(resume);
    m_slow.push_back(Slow{slow, resume, false, false, sign_extend, size, dst, XMM0, addr, live_gprs, live_xmms});
}

void GuestMemory::store(u32 size, Gp value, Gp addr, u32 live_gprs, u32 live_xmms)
{
    Emitter&    a    = m_a;
    const Label slow = lookup(size, addr, m_cfg.write_table);
    const Mem   m    = ptr(R11, R10, 1);
    if (size == 1) {
        a.mov(m, value.r8());
    } else if (m_cfg.use_movbe) {
        a.movbe(m, Gp{value.id, static_cast<u8>(size)});
    } else {
        a.add(R11, R10);
        switch (size) {
        case 2:
            a.mov(R10D, value.r32());
            a.rol(R10W, 8);
            a.mov(ptr(R11), R10W);
            break;
        case 4:
            a.mov(R10D, value.r32());
            a.bswap(R10D);
            a.mov(ptr(R11), R10D);
            break;
        default:
            a.mov(R10, value.r64());
            a.bswap(R10);
            a.mov(ptr(R11), R10);
            break;
        }
    }
    const Label resume = a.new_label();
    a.bind(resume);
    m_slow.push_back(Slow{slow, resume, true, false, false, size, value, XMM0, addr, live_gprs, live_xmms});
}

void GuestMemory::load_fp(u32 size, Xmm dst, Gp addr, u32 live_gprs, u32 live_xmms)
{
    Emitter&    a    = m_a;
    const Label slow = lookup(size, addr, m_cfg.read_table);
    const Gp    t    = size == 8 ? R11 : R11D;
    if (m_cfg.use_movbe) {
        a.movbe(t, ptr(R11, R10, 1));
    } else {
        a.mov(t, ptr(R11, R10, 1));
        a.bswap(t);
    }
    a.movd(dst, t);
    const Label resume = a.new_label();
    a.bind(resume);
    m_slow.push_back(Slow{slow, resume, false, true, false, size, RAX, dst, addr, live_gprs, live_xmms});
}

void GuestMemory::store_fp(u32 size, Xmm value, Gp addr, u32 live_gprs, u32 live_xmms)
{
    Emitter&    a    = m_a;
    const Label slow = lookup(size, addr, m_cfg.write_table);
    const Gp    t    = size == 8 ? R10 : R10D;
    a.add(R11, R10);
    a.movd(t, value);
    a.bswap(t);
    a.mov(ptr(R11), t);
    const Label resume = a.new_label();
    a.bind(resume);
    m_slow.push_back(Slow{slow, resume, true, true, false, size, RAX, value, addr, live_gprs, live_xmms});
}

void GuestMemory::finish()
{
    Emitter&       a   = m_a;
    const AbiInfo& abi = abi_info(m_cfg.abi);
    for (const Slow& s : m_slow) {
        a.bind(s.entry);

        u32 gprs = s.live_gprs & caller_saved_gprs(m_cfg.abi) & ~((1U << 10) | (1U << 11));
        u32 xmms = s.live_xmms & caller_saved_xmms(m_cfg.abi);
        if (!s.store && !s.fp) gprs &= ~(1U << s.data.id);
        if (!s.store && s.fp) xmms &= ~(1U << s.xdata.id);

        const u32 pushes = static_cast<u32>(std::popcount(gprs));
        const u32 nx     = static_cast<u32>(std::popcount(xmms));
        u32       sub    = 16 * nx + abi.shadow_space;
        if (((8 * pushes + sub) & 15) != 0) sub += 8;  // RSP is 16-aligned in block code
        for (unsigned r = 0; r < 16; ++r) {
            if ((gprs & (1U << r)) != 0) a.push(Gp{static_cast<u8>(r), 8});
        }
        if (sub != 0) a.sub(RSP, static_cast<s32>(sub));
        u32 off = abi.shadow_space;
        for (unsigned r = 0; r < 16; ++r) {
            if ((xmms & (1U << r)) == 0) continue;
            a.movdqu(ptr(RSP, static_cast<s32>(off)), Xmm{static_cast<u8>(r)});
            off += 16;
        }

        // Stage through R10/R11, which are not argument registers.
        a.mov(R10D, s.addr.r32());
        if (s.store) {
            if (s.fp) {
                a.movd(s.size == 8 ? R11 : R11D, s.xdata);
            } else if (s.size == 8) {
                a.mov(R11, s.data.r64());
            } else if (s.size == 4) {
                a.mov(R11D, s.data.r32());
            } else {
                a.movzx(R11D, Gp{s.data.id, static_cast<u8>(s.size)});  // zero-extended
            }
        }
        a.mov(abi.args[1].r32(), R10D);
        if (s.store) a.mov(abi.args[2].r64(), R11);
        a.mov(abi.args[0].r64(), m_cfg.ctx.r64());
        const u32 fn = size_index(s.size);
        if (s.store) {
            a.call(reinterpret_cast<const void*>(m_cfg.fns.write[fn]));
        } else {
            a.call(reinterpret_cast<const void*>(m_cfg.fns.read[fn]));
            if (s.fp) {
                a.movd(s.xdata, s.size == 8 ? RAX : EAX);
            } else if (s.size == 8) {
                a.mov(s.data.r64(), RAX);
            } else if (s.sign) {
                const Gp src = s.size == 1 ? AL : s.size == 2 ? AX : EAX;
                if (s.size == 4 && s.data.size != 8) {
                    a.mov(s.data.r32(), EAX);
                } else {
                    a.movsx(s.data, src);
                }
            } else if (s.size == 1) {
                a.movzx(s.data.r32(), AL);
            } else if (s.size == 2) {
                a.movzx(s.data.r32(), AX);
            } else {
                a.mov(s.data.r32(), EAX);
            }
        }

        off = abi.shadow_space;
        for (unsigned r = 0; r < 16; ++r) {
            if ((xmms & (1U << r)) == 0) continue;
            a.movdqu(Xmm{static_cast<u8>(r)}, ptr(RSP, static_cast<s32>(off)));
            off += 16;
        }
        if (sub != 0) a.add(RSP, static_cast<s32>(sub));
        for (int r = 15; r >= 0; --r) {
            if ((gprs & (1U << r)) != 0) a.pop(Gp{static_cast<u8>(r), 8});
        }
        a.jmp(s.resume);
    }
    m_slow.clear();
}

}  // namespace x64

}  // namespace sm2::cpu::jit
