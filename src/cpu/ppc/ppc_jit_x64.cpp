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

// PowerPC IR -> x86-64, the arm64 backend's twin.
//
// Pinned: RBX state, R14D budget left, R15 block_cycles of the running block.
// Guest GPRs, LR, CTR, CR fields, SO, OV and CA are cached in RBP, R12, R13,
// RCX, RDX, RSI, RDI, R8, R9 and FPRs in XMM0-XMM12 within a block (loaded on
// first use, written back before runtime calls and at exits; slow paths push
// the live caller-saved ones). FPRF is computed lazily from a copy of the
// last result in XMM13. Scratch: RAX (the effective address), R10, R11,
// XMM14, XMM15; RCX and RDX are freed for the ops that need CL or EDX:EAX.
// The page tables are read from the state. Rounding comes from FPSCR[RN] via
// MXCSR, set on entry and restored on return. FMA3 when the host has it, else
// the interpreter's fma(); FP NaN precedence matches ppc_fpu.h's asm.
//
// Direct exits are patchable jumps to the target's chain entry, which checks
// the budget; indirect exits probe a direct-mapped table. Blocks that ran a
// Helper always return to the run loop.

#include "cpu/ppc/ppc_jit_x64.h"

#if defined(__x86_64__) || defined(_M_X64)

#include "core/log.h"
#include "cpu/jit/code_buffer.h"
#include "cpu/jit/x64_emitter.h"
#include "cpu/page_map.h"
#include "cpu/ppc/ppc.h"
#include "cpu/ppc/ppc_fpu.h"
#include "cpu/ppc/ppc_internal.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace sm2::cpu::ppc {

namespace {

using namespace sm2::cpu::jit;
using namespace sm2::cpu::jit::x64;

#define OFF(f) static_cast<s32>(offsetof(PpcState, f))

constexpr Gp kSt  = RBX;
constexpr Gp kBud = R14D;
constexpr Gp kCyc = R15;
constexpr Gp kEa  = EAX;

constexpr u32 kItemCr = 40;
constexpr u32 kItemSo = 48;
constexpr u32 kItemOv = 49;
constexpr u32 kItemCa = 50;
constexpr u32 kItems  = 51;

constexpr u8  kGprPool[] = {12, 13, 5, 6, 7, 1, 2, 8, 9};
constexpr u8  kFprPool[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
constexpr Xmm kFprfSrc   = XMM13;
constexpr Xmm kF0        = XMM14;
constexpr Xmm kF1        = XMM15;

constexpr u32 kFastBits = 16;

// Frame locals.
constexpr s32 kLocMxcsr = 0, kLocRnSet = 4, kLocEntry = 8, kLocFast = 16, kLocTmp = 24;
constexpr u32 kLocals = 32;

// Constant pool, 16-byte entries.
constexpr s32 kConstSign = 0, kConstAbs = 16, kConstOne = 32, kConstIntMax = 48, kConstTwo52 = 64;

struct FastEntry {
    u64         key;
    const void* host;
};

[[nodiscard]] s32 item_off(u32 it)
{
    if (it < kItemCr) return OFF(slot) + 4 * static_cast<s32>(it);
    if (it < kItemSo) return OFF(cr) + static_cast<s32>(it - kItemCr);
    if (it == kItemSo) return OFF(xer_so);
    if (it == kItemOv) return OFF(xer_ov);
    return OFF(xer_ca);
}

[[nodiscard]] bool is_temp(u32 it) { return it >= kSlotT0 && it <= kSlotT3; }

[[nodiscard]] bool is_terminator(IrOp op)
{
    return op == IrOp::B || op == IrOp::Bc || op == IrOp::Bcr || op == IrOp::Exit;
}

// ---- runtime helpers for the few ops not emitted inline ---------------------

void jit_lmw(PpcState* st, u32 ea, u32 d, u32 idx)
{
    for (u32 r = d; r < 32; ++r) {
        const u32 a   = ea + 4 * (r - d);
        u8* const pg  = st->read_table[a >> PageMap::kPageBits];
        const u32 off = a & PageMap::kPageMask;
        if (pg != nullptr && off <= PageMap::kPageSize - 4) {
            u32 v;
            std::memcpy(&v, pg + off, 4);
            st->slot[r] = byteswap_any(v);
        } else {
            st->cur_insn = idx;
            st->slot[r]  = ppc_rt_read32(st, a);
        }
    }
}

void jit_stmw(PpcState* st, u32 ea, u32 d, u32 idx)
{
    for (u32 r = d; r < 32; ++r) {
        const u32 a   = ea + 4 * (r - d);
        u8* const pg  = st->write_table[a >> PageMap::kPageBits];
        const u32 off = a & PageMap::kPageMask;
        if (pg != nullptr && off <= PageMap::kPageSize - 4) {
            const u32 v = byteswap_any(st->slot[r]);
            std::memcpy(pg + off, &v, 4);
        } else {
            st->cur_insn = idx;
            ppc_rt_write32(st, a, st->slot[r]);
        }
    }
}

u32 jit_fcmpu_unordered(u32 fs, u64 a, u64 b)
{
    if (fpu::is_snan(a) || fpu::is_snan(b)) {
        fs = fpu::fpscr_summarise(fs | kFpscrVXSNAN | kFpscrFX);
    }
    return fs;
}

double jit_fma(double a, double c, double b)
{
    return fpu::fma(a, c, b);
}

// ---- register cache -----------------------------------------------------------

struct Cache {
    s8   host[kItems];
    s8   item[16];
    s8   fhost[32];
    s8   fitem[16];
    u32  stamp[16];
    u32  fstamp[16];
    u64  dirty = 0, pend = 0;
    u32  fdirty = 0, fpend = 0;
    u32  lock = 0, flock = 0;
    u32  clock = 0;
    bool fprf  = false;

    Cache()
    {
        std::memset(host, -1, sizeof host);
        std::memset(item, -1, sizeof item);
        std::memset(fhost, -1, sizeof fhost);
        std::memset(fitem, -1, sizeof fitem);
        std::memset(stamp, 0, sizeof stamp);
        std::memset(fstamp, 0, sizeof fstamp);
    }
};

class JitX64;

struct BlockInfo {
    struct Exit {
        u8* site;
        u8* fallback;
        u64 target;
    };
    u64               key   = 0;
    u8*               chain = nullptr;
    std::vector<Exit> exits;
    bool              live = true;
};

class JitX64 final : public BlockBackend {
public:
    bool init();
    void compile(IrBlock& block) override;
    void release(IrBlock& block) override;
    void invalidated(IrBlock& block) override;
    [[nodiscard]] bool full() const override { return m_full; }
    void reset_code() override;

    CodeBuffer     m_code;
    const u8*      m_enter = nullptr;
    const u8*      m_ret   = nullptr;
    const u8*      m_fprf  = nullptr;
    const u8*      m_const = nullptr;
    u32            m_loc   = 0;
    const AbiInfo* m_abi   = nullptr;
    bool           m_fma = false, m_sse41 = false, m_movbe = false, m_lzcnt = false;
    std::unique_ptr<FastEntry[]> m_fast;

private:
    void patch(const BlockInfo::Exit& e, const void* to);
    void clear_fast();

    std::unordered_map<u64, BlockInfo*>                                m_live;
    std::unordered_map<u64, std::vector<std::pair<BlockInfo*, u32>>>   m_links;
    bool m_full = false;
};

// ---- the block compiler ---------------------------------------------------------

class Compiler {
public:
    Compiler(JitX64& be, IrBlock& b, Emitter& a) : m_be(be), m_b(b), m_a(a)
    {
        m_chain = true;
        for (const IrInst& o : b.ops) {
            if (o.op == IrOp::Helper) m_chain = false;
        }
        m_key = b.key;
        for (u32 i = 0; i < 4; ++i) m_arg[i] = be.m_abi->args[i];
    }

    bool run();

    struct ExitRec {
        Label site;
        Label fb;
        u32   target;
    };
    std::vector<ExitRec> m_exits;
    Label                m_chain_label;

private:
    // ---- cache ----
    void begin_op()
    {
        m_c.lock = m_c.flock = 0;
        m_c.pend = 0;
        m_c.fpend = 0;
    }
    void end_op()
    {
        m_c.dirty |= m_c.pend;
        m_c.fdirty |= m_c.fpend;
        m_c.pend = 0;
        m_c.fpend = 0;
        m_c.lock = m_c.flock = 0;
    }

    void store_item(u32 it, u8 h)
    {
        if (it >= kItemCr) {
            m_a.mov(ptr(kSt, item_off(it)), Gp{h, 1});
        } else {
            m_a.mov(ptr(kSt, item_off(it)), Gp{h, 4});
        }
    }
    void load_item(u32 it, u8 h)
    {
        if (it >= kItemCr) {
            m_a.movzx(Gp{h, 4}, sized(ptr(kSt, item_off(it)), 1));
        } else {
            m_a.mov(Gp{h, 4}, ptr(kSt, item_off(it)));
        }
    }
    void store_fpr(u32 n, u8 h) { m_a.movsd(ptr(kSt, OFF(fpr) + 8 * static_cast<s32>(n)), Xmm{h}); }
    void load_fpr(u32 n, u8 h) { m_a.movsd(Xmm{h}, ptr(kSt, OFF(fpr) + 8 * static_cast<s32>(n))); }

    void evict(u8 h)
    {
        const s8 it = m_c.item[h];
        if (it < 0) return;
        if (m_c.dirty & (1ull << it)) store_item(static_cast<u32>(it), h);
        m_c.dirty &= ~(1ull << it);
        m_c.pend &= ~(1ull << it);
        m_c.host[it] = -1;
        m_c.item[h]  = -1;
    }
    void fevict(u8 h)
    {
        const s8 n = m_c.fitem[h];
        if (n < 0) return;
        if (m_c.fdirty & (1u << n)) store_fpr(static_cast<u32>(n), h);
        m_c.fdirty &= ~(1u << n);
        m_c.fpend &= ~(1u << n);
        m_c.fhost[n] = -1;
        m_c.fitem[h] = -1;
    }

    u8 alloc_gpr()
    {
        for (u8 h : kGprPool) {
            if (m_c.item[h] < 0 && !(m_c.lock & (1u << h))) return h;
        }
        int best = -1;
        u32 bs   = ~0u;
        for (u8 h : kGprPool) {
            if (!(m_c.lock & (1u << h)) && m_c.stamp[h] < bs) {
                best = h;
                bs   = m_c.stamp[h];
            }
        }
        if (best < 0) {
            m_a.set_error();
            return kGprPool[0];
        }
        evict(static_cast<u8>(best));
        return static_cast<u8>(best);
    }
    u8 alloc_fpr()
    {
        for (u8 h : kFprPool) {
            if (m_c.fitem[h] < 0 && !(m_c.flock & (1u << h))) return h;
        }
        int best = -1;
        u32 bs   = ~0u;
        for (u8 h : kFprPool) {
            if (!(m_c.flock & (1u << h)) && m_c.fstamp[h] < bs) {
                best = h;
                bs   = m_c.fstamp[h];
            }
        }
        if (best < 0) {
            m_a.set_error();
            return kFprPool[0];
        }
        fevict(static_cast<u8>(best));
        return static_cast<u8>(best);
    }

    Gp get(u32 it, bool load, bool write)
    {
        s8 h = m_c.host[it];
        if (h < 0) {
            h            = static_cast<s8>(alloc_gpr());
            m_c.host[it] = h;
            m_c.item[h]  = static_cast<s8>(it);
            if (load) load_item(it, static_cast<u8>(h));
        }
        m_c.lock |= 1u << h;
        m_c.stamp[h] = ++m_c.clock;
        if (write) m_c.pend |= 1ull << it;
        return Gp{static_cast<u8>(h), 4};
    }
    Gp r(u32 it) { return get(it, true, false); }
    Gp w(u32 it) { return get(it, false, true); }
    Gp rw(u32 it) { return get(it, true, true); }

    Xmm fget(u32 n, bool load, bool write)
    {
        s8 h = m_c.fhost[n];
        if (h < 0) {
            h            = static_cast<s8>(alloc_fpr());
            m_c.fhost[n] = h;
            m_c.fitem[h] = static_cast<s8>(n);
            if (load) load_fpr(n, static_cast<u8>(h));
        }
        m_c.flock |= 1u << h;
        m_c.fstamp[h] = ++m_c.clock;
        if (write) m_c.fpend |= 1u << n;
        return Xmm{static_cast<u8>(h)};
    }
    Xmm fr(u32 n) { return fget(n, true, false); }
    Xmm fw(u32 n) { return fget(n, false, true); }

    /// Free pool register `h` for scratch use by this op.
    void claim(Gp h)
    {
        evict(h.id);
        m_c.lock |= 1u << h.id;
    }

    void drop(u32 it)
    {
        const s8 h = m_c.host[it];
        if (h < 0) return;
        m_c.dirty &= ~(1ull << it);
        m_c.pend &= ~(1ull << it);
        m_c.host[it] = -1;
        m_c.item[h]  = -1;
    }
    void drop_all()
    {
        const u32 clock = m_c.clock;
        m_c             = Cache{};
        m_c.clock       = clock;
    }

    /// Store every dirty non-temporary of `s` (state unchanged).
    void writeback(const Cache& s)
    {
        for (u32 it = 0; it < kItems; ++it) {
            if ((s.dirty & (1ull << it)) && !is_temp(it)) store_item(it, static_cast<u8>(s.host[it]));
        }
        for (u32 n = 0; n < 32; ++n) {
            if (s.fdirty & (1u << n)) store_fpr(n, static_cast<u8>(s.fhost[n]));
        }
    }
    /// writeback() on the main path, after which the items are clean.
    void writeback_clean()
    {
        writeback(m_c);
        for (u32 it = 0; it < kItems; ++it) {
            if (!is_temp(it)) m_c.dirty &= ~(1ull << it);
        }
        m_c.fdirty = 0;
    }
    void fprf_now()
    {
        if (m_c.fprf) m_a.call(m_be.m_fprf);
    }
    void fprf_materialize()
    {
        fprf_now();
        m_c.fprf = false;
    }

    struct Saved {
        u32 g, f, sub;
    };
    /// Save the live caller-saved registers of `s` around a call; RSP stays
    /// 16-aligned and Win64 shadow space is reserved.
    Saved push_live(const Cache& s, u32 excl_g = 0, u32 excl_f = 0)
    {
        const AbiInfo& abi = *m_be.m_abi;
        u32 g = 0, f = 0;
        for (u8 h : kGprPool) {
            if (s.item[h] >= 0 && !(abi.callee_saved_gprs & (1u << h))) g |= 1u << h;
        }
        for (u8 h : kFprPool) {
            if (s.fitem[h] >= 0 && !(abi.callee_saved_xmms & (1u << h))) f |= 1u << h;
        }
        if (s.fprf && !(abi.callee_saved_xmms & (1u << kFprfSrc.id))) f |= 1u << kFprfSrc.id;
        g &= ~excl_g;
        f &= ~excl_f;
        const u32 pushes = static_cast<u32>(std::popcount(g));
        const u32 nx     = static_cast<u32>(std::popcount(f));
        u32       sub    = 16 * nx + abi.shadow_space;
        if (((8 * pushes + sub) & 15) != 0) sub += 8;
        for (unsigned h = 0; h < 16; ++h) {
            if (g & (1u << h)) m_a.push(Gp{static_cast<u8>(h), 8});
        }
        if (sub != 0) m_a.sub(RSP, static_cast<s32>(sub));
        s32 off = static_cast<s32>(abi.shadow_space);
        for (unsigned h = 0; h < 16; ++h) {
            if (f & (1u << h)) {
                m_a.movdqu(ptr(RSP, off), Xmm{static_cast<u8>(h)});
                off += 16;
            }
        }
        return {g, f, sub};
    }
    void pop_live(Saved sv)
    {
        s32 off = static_cast<s32>(m_be.m_abi->shadow_space);
        for (unsigned h = 0; h < 16; ++h) {
            if (sv.f & (1u << h)) {
                m_a.movdqu(Xmm{static_cast<u8>(h)}, ptr(RSP, off));
                off += 16;
            }
        }
        if (sv.sub != 0) m_a.add(RSP, static_cast<s32>(sv.sub));
        for (int h = 15; h >= 0; --h) {
            if (sv.g & (1u << h)) m_a.pop(Gp{static_cast<u8>(h), 8});
        }
    }

    void set_cur_insn(u32 i) { m_a.mov(dword_ptr(kSt, OFF(cur_insn)), static_cast<s32>(i)); }
    [[nodiscard]] Mem konst(s32 off) const { return rip(m_be.m_const + off); }
    void mov_if(Gp d, Gp s)
    {
        if (d != s) m_a.mov(d, s);
    }
    /// d = x op y without disturbing x or y.
    void arith(AluOp op, Gp d, Gp x, Gp y, bool commutative)
    {
        if (d == x) {
            m_a.alu(op, d, y);
        } else if (d == y) {
            if (commutative) {
                m_a.alu(op, d, x);
            } else {
                m_a.mov(R10D, x);
                m_a.alu(op, R10D, y);
                m_a.mov(d, R10D);
            }
        } else {
            m_a.mov(d, x);
            m_a.alu(op, d, y);
        }
    }
    void set_bool(Cond c, Gp d)
    {
        m_a.set(c, d.r8());
        m_a.movzx(d, d.r8());
    }

    // ---- exits ----
    void exit_link(u32 target)
    {
        if (m_chain) {
            ExitRec e{m_a.new_label(), m_a.new_label(), target};
            m_a.bind(e.site);
            m_a.jmp(e.fb);
            m_a.bind(e.fb);
            m_exits.push_back(e);
        }
        m_a.mov(dword_ptr(kSt, OFF(pc)), static_cast<s32>(target));
        m_a.jump(m_be.m_ret);
    }
    void charge(u32 n)
    {
        m_a.add(kCyc, static_cast<s32>(n));
        m_a.sub(kBud, static_cast<s32>(n));
    }
    /// pc already in EAX.
    void exit_indirect()
    {
        m_a.mov(ptr(kSt, OFF(pc)), kEa);
        if (!m_chain) {
            m_a.jump(m_be.m_ret);
            return;
        }
        m_a.mov(R11, ptr(RSP, static_cast<s32>(m_be.m_loc) + kLocFast));
        m_a.mov(R10D, EAX);
        m_a.shr(R10D, 2);
        m_a.and_(R10D, static_cast<s32>((1u << kFastBits) - 1));
        m_a.shl(R10, 4);
        m_a.add(R11, R10);
        if (m_key != 0) {
            m_a.mov(R10, u64{m_key} << 32);
            m_a.or_(R10, RAX);
        } else {
            m_a.mov(R10, RAX);
        }
        m_a.cmp(R10, ptr(R11));
        const Label miss = m_a.new_label();
        m_a.j(Cond::NE, miss);
        m_a.jmp(ptr(R11, 8));
        m_a.bind(miss);
        m_a.jump(m_be.m_ret);
    }
    /// Leave after instruction i (break or helper exit).
    void exit_after(u32 i, bool set_pc)
    {
        if (set_pc) m_a.mov(dword_ptr(kSt, OFF(pc)), static_cast<s32>(m_b.pc + 4 * (i + 1)));
        m_a.add(kCyc, static_cast<s32>(i + 1));
        m_a.jump(m_be.m_ret);
    }

    void emit_op(u32 k, bool stub);
    void queue_break_stub(u32 k, Label l);

    // ---- op groups ----
    void ea(const IrInst& o, Gp ra, Gp rb);
    enum MemKind : u8 { kU8, kU16, kS16, kU32, kR16, kR32, kF32, kF64, kFS32, kFI32 };
    Label page_lookup(u32 size, s32 table_off);
    void emit_load(u32 k, const IrInst& o, MemKind kind, bool stub);
    void emit_store(u32 k, const IrInst& o, MemKind kind, bool stub);
    void emit_multiple(u32 k, const IrInst& o, bool load, bool stub);
    void flags_check(Label brk, bool stub)
    {
        if (stub) return;
        m_a.cmp(dword_ptr(kSt, OFF(exit_flags)), 0);
        m_a.j(Cond::NE, brk);
    }
    void lfs_convert(Xmm fd);
    void stfs_convert(Xmm fs);
    void cr_result(Cond gt, Cond lt, Gp so, Gp d);
    void round_single(Xmm dst, Xmm src)
    {
        m_a.cvtsd2ss(kF1, src);
        m_a.cvtss2sd(dst, kF1);
    }
    void neg_unless_nan(Xmm dst, Xmm src)
    {
        const Label done = m_a.new_label();
        m_a.ucomisd(src, src);
        if (dst != src) m_a.movapd(dst, src);
        m_a.j_short(Cond::P, done);
        m_a.xorpd(dst, konst(kConstSign));
        m_a.bind(done);
    }
    void emit_fp(const IrInst& o);
    void emit_branch(const IrInst& o);

    JitX64&   m_be;
    IrBlock&  m_b;
    Emitter&  m_a;
    Cache     m_c;
    bool      m_chain;
    u32       m_key;
    Gp        m_arg[4];
    Label     m_brk{};
    std::vector<std::function<void()>> m_ool;
};

void Compiler::ea(const IrInst& o, Gp ra, Gp rb)
{
    const s32 imm = static_cast<s32>(o.imm);
    if (o.b != kSlotZero) {
        if (o.a == kSlotZero) {
            if (imm != 0) {
                m_a.lea(kEa, ptr(rb, imm));
            } else {
                m_a.mov(kEa, rb);
            }
        } else {
            m_a.lea(kEa, ptr(ra, rb, 1, imm));
        }
    } else if (o.a == kSlotZero) {
        m_a.mov(kEa, u64{o.imm});
    } else if (imm != 0) {
        m_a.lea(kEa, ptr(ra, imm));
    } else {
        m_a.mov(kEa, ra);
    }
}

/// R10D (the single's bits) -> fd. Clobbers RAX, R11.
void Compiler::lfs_convert(Xmm fd)
{
    const Label special = m_a.new_label(), done = m_a.new_label();
    m_a.mov(R11D, R10D);
    m_a.and_(R11D, 0x7f800000);
    m_a.cmp(R11D, 0x7f800000);
    m_a.j(Cond::E, special);
    m_a.movd(fd, R10D);
    m_a.cvtss2sd(fd, fd);
    m_a.bind(done);
    m_ool.push_back([this, special, done, fd] {
        m_a.bind(special);
        m_a.mov(R11D, R10D);
        m_a.and_(R11D, 0x007fffff);
        m_a.shl(R11, 29);
        m_a.and_(R10D, static_cast<s32>(0x80000000u));
        m_a.shl(R10, 32);
        m_a.or_(R11, R10);
        m_a.mov(RAX, 0x7ff0000000000000ull);
        m_a.or_(R11, RAX);
        m_a.movd(fd, R11);
        m_a.jmp(done);
    });
}

/// double_to_single(fs) -> EAX. Clobbers R10 (RCX is saved around the
/// denormal path).
void Compiler::stfs_convert(Xmm fs)
{
    const Label small = m_a.new_label(), done = m_a.new_label();
    m_a.movd(R10, fs);
    m_a.mov(RAX, R10);
    m_a.shr(RAX, 52);
    m_a.and_(EAX, 0x7ff);
    m_a.cmp(EAX, 896);
    m_a.j(Cond::BE, small);
    m_a.mov(RAX, R10);
    m_a.shr(RAX, 29);
    m_a.and_(EAX, 0x3fffffff);
    m_a.shr(R10, 32);
    m_a.and_(R10D, static_cast<s32>(0xc0000000u));
    m_a.or_(EAX, R10D);
    m_a.bind(done);
    m_ool.push_back([this, small, done] {
        const Label out = m_a.new_label();
        m_a.bind(small);
        m_a.push(RCX);
        m_a.mov(ECX, u64{926});
        m_a.sub(ECX, EAX);
        m_a.mov(RAX, R10);
        m_a.shr(RAX, 32);
        m_a.and_(EAX, static_cast<s32>(0x80000000u));
        m_a.cmp(ECX, 52);
        m_a.j(Cond::A, out);
        m_a.shl(R10, 12);
        m_a.shr(R10, 12);
        m_a.bts(R10, 52);
        m_a.shr_cl(R10);
        m_a.or_(EAX, R10D);
        m_a.bind(out);
        m_a.pop(RCX);
        m_a.jmp(done);
    });
}

/// EAX = address: R11 = page or jump to the returned label; EAX becomes the
/// page offset. Clobbers R10.
Label Compiler::page_lookup(u32 size, s32 table_off)
{
    const Label slow = m_a.new_label();
    m_a.mov(R10D, kEa);
    m_a.shr(R10D, static_cast<u8>(PageMap::kPageBits));
    m_a.mov(R11, ptr(kSt, table_off));
    m_a.mov(R11, ptr(R11, R10, 8));
    m_a.test(R11, R11);
    m_a.j(Cond::E, slow);
    if (size > 1) {
        m_a.test(kEa, static_cast<s32>(size - 1));
        m_a.j(Cond::NE, slow);
    }
    m_a.and_(kEa, static_cast<s32>(PageMap::kPageMask));
    return slow;
}

void Compiler::emit_load(u32 k, const IrInst& o, MemKind kind, bool stub)
{
    static constexpr u32 kSize[] = {1, 2, 2, 4, 2, 4, 4, 8, 4, 4};
    const u32  size = kSize[kind];
    const bool fp   = kind == kF32 || kind == kF64;
    const Gp   ra   = r(o.a);
    const Gp   rb   = r(o.b);
    ea(o, ra, rb);
    Gp  d{};
    Xmm fd{};
    if (fp) {
        fd = fw(o.d);
    } else {
        d = w(o.d);
    }
    const Cache snap   = m_c;
    const Label resume = m_a.new_label();
    const Label slow   = page_lookup(size, OFF(read_table));
    const Mem   m      = ptr(R11, RAX, 1);
    switch (kind) {
        case kU8: m_a.movzx(d, sized(m, 1)); break;
        case kU16: m_a.movzx(d, sized(m, 2)); m_a.rol(d.r16(), 8); break;
        case kS16: m_a.movzx(d, sized(m, 2)); m_a.rol(d.r16(), 8); m_a.movsx(d, d.r16()); break;
        case kU32:
            if (m_be.m_movbe) {
                m_a.movbe(d, m);
            } else {
                m_a.mov(d, m);
                m_a.bswap(d);
            }
            break;
        case kR16: m_a.movzx(d, sized(m, 2)); break;
        case kR32: m_a.mov(d, m); break;
        case kF32:
            if (m_be.m_movbe) {
                m_a.movbe(R10D, m);
            } else {
                m_a.mov(R10D, m);
                m_a.bswap(R10D);
            }
            lfs_convert(fd);
            break;
        case kF64:
            if (m_be.m_movbe) {
                m_a.movbe(R10, m);
            } else {
                m_a.mov(R10, m);
                m_a.bswap(R10);
            }
            m_a.movd(fd, R10);
            break;
        default: m_a.set_error(); break;
    }
    m_a.bind(resume);

    const u32   i   = m_b.op_insn[k];
    const Label brk = stub ? Label{} : m_a.new_label();
    if (!stub) m_brk = brk;
    m_ool.push_back([this, snap, slow, resume, brk, stub, kind, size, fp, d, fd, i] {
        m_c = snap;
        m_a.bind(slow);
        writeback(snap);
        if (snap.fprf) m_a.call(m_be.m_fprf);
        const Saved sv = push_live(snap, fp ? 0u : (1u << d.id), fp ? (1u << fd.id) : 0u);
        set_cur_insn(i);
        m_a.mov(m_arg[1].r32(), kEa);
        m_a.mov(m_arg[0], kSt);
        switch (size) {
            case 1: m_a.call(&ppc_rt_read8); break;
            case 2: m_a.call(&ppc_rt_read16); break;
            case 4: m_a.call(&ppc_rt_read32); break;
            default: m_a.call(&ppc_rt_read64); break;
        }
        pop_live(sv);
        switch (kind) {
            case kU8: m_a.movzx(d, AL); break;
            case kU16: m_a.movzx(d, AX); break;
            case kS16: m_a.movsx(d, AX); break;
            case kU32: m_a.mov(d, EAX); break;
            case kR16: m_a.rol(AX, 8); m_a.movzx(d, AX); break;
            case kR32: m_a.bswap(EAX); m_a.mov(d, EAX); break;
            case kF32: m_a.mov(R10D, EAX); lfs_convert(fd); break;
            case kF64: m_a.movd(fd, RAX); break;
            default: break;
        }
        flags_check(brk, stub);
        m_a.jmp(resume);
    });
}

void Compiler::emit_store(u32 k, const IrInst& o, MemKind kind, bool stub)
{
    static constexpr u32 kSize[] = {1, 2, 2, 4, 2, 4, 4, 8, 4, 4};
    const u32  size = kSize[kind];
    const bool fp   = kind == kF64 || kind == kFS32 || kind == kFI32;
    const Gp   ra   = r(o.a);
    const Gp   rb   = r(o.b);
    Gp         v{};
    Xmm        fv{};
    if (fp) {
        fv = fr(o.d);
    } else {
        v = r(o.d);
    }
    ea(o, ra, rb);
    const Cache snap   = m_c;
    const Label resume = m_a.new_label();
    const Label slow   = page_lookup(size, OFF(write_table));
    m_a.add(R11, RAX);
    switch (kind) {
        case kU8: m_a.mov(ptr(R11), v.r8()); break;
        case kU16:
            if (m_be.m_movbe) {
                m_a.movbe(ptr(R11), v.r16());
            } else {
                m_a.mov(R10D, v);
                m_a.rol(R10W, 8);
                m_a.mov(ptr(R11), R10W);
            }
            break;
        case kU32:
            if (m_be.m_movbe) {
                m_a.movbe(ptr(R11), v);
            } else {
                m_a.mov(R10D, v);
                m_a.bswap(R10D);
                m_a.mov(ptr(R11), R10D);
            }
            break;
        case kR16: m_a.mov(ptr(R11), v.r16()); break;
        case kR32: m_a.mov(ptr(R11), v); break;
        case kF64:
            m_a.movd(R10, fv);
            m_a.bswap(R10);
            m_a.mov(ptr(R11), R10);
            break;
        case kFS32:
            stfs_convert(fv);
            m_a.bswap(EAX);
            m_a.mov(ptr(R11), EAX);
            break;
        case kFI32:
            m_a.movd(R10D, fv);
            m_a.bswap(R10D);
            m_a.mov(ptr(R11), R10D);
            break;
        default: m_a.set_error(); break;
    }
    m_a.bind(resume);

    const u32   i   = m_b.op_insn[k];
    const Label brk = stub ? Label{} : m_a.new_label();
    if (!stub) m_brk = brk;
    m_ool.push_back([this, snap, slow, resume, brk, stub, kind, size, v, fv, i] {
        m_c = snap;
        m_a.bind(slow);
        writeback(snap);
        if (snap.fprf) m_a.call(m_be.m_fprf);
        const Saved sv = push_live(snap);
        set_cur_insn(i);
        if (kind == kFS32) {
            m_a.mov(m_arg[1].r32(), kEa);
            stfs_convert(fv);
            m_a.mov(m_arg[2].r32(), EAX);
        } else {
            switch (kind) {
                case kU8: m_a.movzx(R11D, v.r8()); break;
                case kU16: m_a.movzx(R11D, v.r16()); break;
                case kR16: m_a.mov(R11D, v); m_a.rol(R11W, 8); m_a.movzx(R11D, R11W); break;
                case kR32: m_a.mov(R11D, v); m_a.bswap(R11D); break;
                case kF64: m_a.movd(R11, fv); break;
                case kFI32: m_a.movd(R11D, fv); break;
                default: m_a.mov(R11D, v); break;
            }
            m_a.mov(m_arg[1].r32(), kEa);
            m_a.mov(m_arg[2], R11);
        }
        m_a.mov(m_arg[0], kSt);
        switch (size) {
            case 1: m_a.call(&ppc_rt_write8); break;
            case 2: m_a.call(&ppc_rt_write16); break;
            case 4: m_a.call(&ppc_rt_write32); break;
            default: m_a.call(&ppc_rt_write64); break;
        }
        pop_live(sv);
        flags_check(brk, stub);
        m_a.jmp(resume);
    });
}

void Compiler::emit_multiple(u32 k, const IrInst& o, bool load, bool stub)
{
    fprf_now();
    const Gp ra = r(o.a);
    const Gp rb = r(o.b);
    ea(o, ra, rb);
    writeback_clean();
    const Saved sv = push_live(m_c);
    m_a.mov(m_arg[1].r32(), kEa);
    m_a.mov(m_arg[2].r32(), u64{o.d});
    m_a.mov(m_arg[3].r32(), u64{m_b.op_insn[k]});
    m_a.mov(m_arg[0], kSt);
    if (load) {
        m_a.call(&jit_lmw);
    } else {
        m_a.call(&jit_stmw);
    }
    pop_live(sv);
    if (load) {
        for (u32 n = o.d; n < 32; ++n) drop(n);
    }
    if (!stub) {
        m_brk = m_a.new_label();
        flags_check(m_brk, false);
    }
}

/// Flags from a compare -> CR field value in d.
void Compiler::cr_result(Cond gt, Cond lt, Gp so, Gp d)
{
    m_a.mov(d, u64{2});
    m_a.mov(R10D, u64{4});
    m_a.cmov(gt, d, R10D);
    m_a.mov(R10D, u64{8});
    m_a.cmov(lt, d, R10D);
    m_a.or_(d, so);
}

void Compiler::emit_fp(const IrInst& o)
{
    Emitter& a = m_a;
    switch (o.op) {
        case IrOp::Fmr: {
            const Xmm s = fr(o.a);
            const Xmm d = fw(o.d);
            if (d != s) a.movapd(d, s);
            break;
        }
        case IrOp::Fneg: case IrOp::Fabs: case IrOp::Fnabs: {
            const Xmm s = fr(o.a);
            const Xmm d = fw(o.d);
            if (d != s) a.movapd(d, s);
            if (o.op == IrOp::Fneg) {
                a.xorpd(d, konst(kConstSign));
            } else if (o.op == IrOp::Fabs) {
                a.andpd(d, konst(kConstAbs));
            } else {
                a.orpd(d, konst(kConstSign));
            }
            break;
        }
        case IrOp::Fadd: case IrOp::Fsub: case IrOp::Fmul: case IrOp::Fdiv:
        case IrOp::Fadds: case IrOp::Fsubs: case IrOp::Fmuls: case IrOp::Fdivs: {
            const Xmm  fa = fr(o.a), fb = fr(o.b);
            const Xmm  d  = fw(o.d);
            const bool single = o.op >= IrOp::Fadds;
            auto op = [&](Xmm t) {
                switch (o.op) {
                    case IrOp::Fadd: case IrOp::Fadds: a.addsd(t, fb); break;
                    case IrOp::Fsub: case IrOp::Fsubs: a.subsd(t, fb); break;
                    case IrOp::Fmul: case IrOp::Fmuls: a.mulsd(t, fb); break;
                    default: a.divsd(t, fb); break;
                }
            };
            // The first operand must be the destination: NaN precedence.
            if (single || d == fb) {
                a.movapd(kF1, fa);
                op(kF1);
                if (single) {
                    round_single(d, kF1);
                } else {
                    a.movapd(d, kF1);
                }
            } else {
                if (d != fa) a.movapd(d, fa);
                op(d);
            }
            break;
        }
        case IrOp::Fmadd: case IrOp::Fmsub: case IrOp::Fnmadd: case IrOp::Fnmsub:
        case IrOp::Fmadds: case IrOp::Fmsubs: case IrOp::Fnmadds: case IrOp::Fnmsubs: {
            const Xmm  fa = fr(o.a), fb = fr(o.b), fc = fr(o.imm);
            const Xmm  d  = fw(o.d);
            const bool single = o.op >= IrOp::Fmadds;
            const bool neg    = o.op == IrOp::Fnmadd || o.op == IrOp::Fnmsub || o.op == IrOp::Fnmadds ||
                             o.op == IrOp::Fnmsubs;
            const bool sub = o.op == IrOp::Fmsub || o.op == IrOp::Fnmsub || o.op == IrOp::Fmsubs ||
                             o.op == IrOp::Fnmsubs;
            Xmm bb = fb;
            if (sub) {
                a.movapd(kF0, fb);
                a.xorpd(kF0, konst(kConstSign));
                bb = kF0;
            }
            if (m_be.m_fma) {
                a.movapd(kF1, fa);
                a.vfmadd213sd(kF1, fc, bb);
            } else {
                fprf_materialize();
                if (!sub) a.movapd(kF0, fb);
                a.movapd(kF1, fa);
                a.movapd(kFprfSrc, fc);
                const Saved sv = push_live(m_c, 0, 1u << d.id);
                a.movapd(XMM0, kF1);
                a.movapd(XMM1, kFprfSrc);
                a.movapd(XMM2, kF0);
                a.call(&jit_fma);
                a.movapd(kF1, XMM0);
                pop_live(sv);
            }
            if (neg) neg_unless_nan(kF1, kF1);
            if (single) {
                round_single(d, kF1);
            } else {
                a.movapd(d, kF1);
            }
            break;
        }
        case IrOp::Frsp: { const Xmm s = fr(o.a); round_single(fw(o.d), s); break; }
        case IrOp::Fres: {
            const Xmm s = fr(o.a);
            const Xmm d = fw(o.d);
            a.movsd(kF1, konst(kConstOne));
            a.divsd(kF1, s);
            round_single(d, kF1);
            break;
        }
        case IrOp::Frsqrte: {
            const Xmm s = fr(o.a);
            const Xmm d = fw(o.d);
            a.sqrtsd(kF1, s);
            a.movsd(kF0, konst(kConstOne));
            a.divsd(kF0, kF1);
            a.movapd(d, kF0);
            break;
        }
        case IrOp::Fsqrt: { const Xmm s = fr(o.a); a.sqrtsd(fw(o.d), s); break; }
        case IrOp::Fsqrts: {
            const Xmm s = fr(o.a);
            const Xmm d = fw(o.d);
            a.sqrtsd(kF1, s);
            round_single(d, kF1);
            break;
        }
        case IrOp::Fsel: {
            const Xmm fa = fr(o.a), fb = fr(o.b), fc = fr(o.imm);
            const Xmm d  = fw(o.d);
            a.xorpd(kF1, kF1);
            a.ucomisd(fa, kF1);
            a.movd(RAX, fc);
            a.movd(R10, fb);
            a.cmov(Cond::B, RAX, R10);
            a.movd(d, RAX);
            break;
        }
        case IrOp::Fctiw: case IrOp::Fctiwz: {
            const Xmm s = fr(o.a);
            const Xmm d = fw(o.d);
            Xmm t = s;
            if (o.op == IrOp::Fctiw) {
                t = kF1;
                if (m_be.m_sse41) {
                    a.roundsd(kF1, s, 4 | 8);
                } else {
                    // For |x| < 2^52, adding then subtracting copysign(2^52, x)
                    // rounds x in the current mode.
                    const Label big = a.new_label();
                    a.movapd(kF1, s);
                    a.movapd(kF0, s);
                    a.andpd(kF0, konst(kConstAbs));
                    a.ucomisd(kF0, konst(kConstTwo52));
                    a.j_short(Cond::AE, big);
                    a.movapd(kF0, s);
                    a.andpd(kF0, konst(kConstSign));
                    a.orpd(kF0, konst(kConstTwo52));
                    a.addsd(kF1, kF0);
                    a.subsd(kF1, kF0);
                    a.bind(big);
                }
            }
            a.cvttsd2si(RAX, t);
            a.mov(R11D, EAX);
            a.mov(R10, 0xffffffff80000000ull);
            a.cmp(RAX, R10);
            a.cmov(Cond::L, R11, R10);
            a.mov(R10D, u64{0x7fffffff});
            a.ucomisd(t, konst(kConstIntMax));
            a.cmov(Cond::A, R11, R10);
            a.movd(d, R11);
            break;
        }
        case IrOp::Fcmpu: {
            const Xmm   fa = fr(o.a), fb = fr(o.b);
            const Gp    d  = w(kItemCr + o.d);
            const Cache snap = m_c;
            const Label un = a.new_label(), set = a.new_label();
            a.ucomisd(fa, fb);
            a.mov(R10D, u64{1});
            a.mov(R11D, u64{2});
            a.cmov(Cond::E, R10D, R11D);
            a.mov(R11D, u64{4});
            a.cmov(Cond::A, R10D, R11D);
            a.mov(R11D, u64{8});
            a.cmov(Cond::B, R10D, R11D);
            a.j(Cond::P, un);
            a.mov(R11D, ptr(kSt, OFF(fpscr)));
            a.and_(R11D, static_cast<s32>(~kFpscrFPRF));
            a.mov(EAX, R10D);
            a.shl(EAX, 12);
            a.or_(R11D, EAX);
            a.mov(ptr(kSt, OFF(fpscr)), R11D);
            a.bind(set);
            a.mov(d, R10D);
            m_c.fprf = false;
            m_ool.push_back([this, snap, un, set, fa, fb, d] {
                m_a.bind(un);
                const Saved sv = push_live(snap, 1u << d.id);
                m_a.mov(m_arg[0].r32(), ptr(kSt, OFF(fpscr)));
                m_a.and_(m_arg[0].r32(), static_cast<s32>(~kFpscrFPRF));
                m_a.or_(m_arg[0].r32(), 1 << 12);
                m_a.movd(m_arg[1], fa);
                m_a.movd(m_arg[2], fb);
                m_a.call(&jit_fcmpu_unordered);
                m_a.mov(ptr(kSt, OFF(fpscr)), EAX);
                pop_live(sv);
                m_a.mov(R10D, u64{1});
                m_a.jmp(set);
            });
            break;
        }
        case IrOp::Fprf: {
            const Xmm s = fr(o.d);
            a.movapd(kFprfSrc, s);
            m_c.fprf = true;
            break;
        }
        default: a.set_error(); break;
    }
}

void Compiler::emit_branch(const IrInst& o)
{
    Emitter&  a  = m_a;
    const u32 n  = m_b.num_insns;
    const u32 bo = o.d, bi = o.a;
    Gp t{}, ctr{}, f{};
    if (o.op == IrOp::Bcr) t = r(o.b);
    if (!(bo & 4)) ctr = rw(kSlotCTR);
    if (!(bo & 16)) f = r(kItemCr + (bi >> 2));
    fprf_materialize();
    if (o.op == IrOp::Bcr) {
        a.mov(kEa, t);
        a.and_(kEa, -4);
    }
    if (!(bo & 4)) a.sub(ctr, 1);
    end_op();
    writeback(m_c);
    charge(n);
    const Label nt = a.new_label();
    if (!(bo & 4)) {
        a.test(ctr, ctr);
        a.j((bo & 2) ? Cond::NE : Cond::E, nt);
    }
    if (!(bo & 16)) {
        const u32 bit = 3 - (bi & 3);
        a.test(f, static_cast<s32>(1u << bit));
        a.j((bo & 8) ? Cond::E : Cond::NE, nt);
    }
    if (o.op == IrOp::Bcr) {
        exit_indirect();
    } else {
        exit_link(o.imm);
    }
    a.bind(nt);
    exit_link(m_b.end_pc());
}

void Compiler::queue_break_stub(u32 k, Label l)
{
    const Cache snap = m_c;
    m_ool.push_back([this, snap, k, l] {
        m_c = snap;
        m_a.bind(l);
        const u32 i = m_b.op_insn[k];
        for (u32 q = k + 1; q < m_b.insn_first_op[i + 1]; ++q) {
            if (is_terminator(m_b.ops[q].op)) continue;
            begin_op();
            emit_op(q, true);
            end_op();
        }
        fprf_materialize();
        writeback(m_c);
        exit_after(i, true);
    });
}

void Compiler::emit_op(u32 k, bool stub)
{
    Emitter&      a = m_a;
    const IrInst& o = m_b.ops[k];
    switch (o.op) {
        // ---- integer ----
        case IrOp::Mov: {
            const Gp s = r(o.a);
            mov_if(w(o.d), s);
            break;
        }
        case IrOp::Li: a.mov(w(o.d), u64{o.imm}); break;
        case IrOp::Add: { const Gp x = r(o.a), y = r(o.b); arith(AluOp::Add, w(o.d), x, y, true); break; }
        case IrOp::Addi: {
            if (o.a == kSlotZero) {
                a.mov(w(o.d), u64{o.imm});
                break;
            }
            const Gp x = r(o.a);
            const Gp d = w(o.d);
            if (o.imm == 0) {
                mov_if(d, x);
            } else {
                a.lea(d, ptr(x, static_cast<s32>(o.imm)));
            }
            break;
        }
        case IrOp::Sub: { const Gp x = r(o.a), y = r(o.b); arith(AluOp::Sub, w(o.d), x, y, false); break; }
        case IrOp::AddC: {
            const Gp x = r(o.a), y = r(o.b);
            const Gp ca = w(kItemCa);
            const Gp d  = w(o.d);
            arith(AluOp::Add, d, x, y, true);
            set_bool(Cond::C, ca);
            break;
        }
        case IrOp::AddCi: {
            const Gp x  = r(o.a);
            const Gp ca = w(kItemCa);
            const Gp d  = w(o.d);
            mov_if(d, x);
            a.add(d, static_cast<s32>(o.imm));
            set_bool(Cond::C, ca);
            break;
        }
        case IrOp::AddE: {
            const Gp x = r(o.a), y = r(o.b);
            const Gp ca = rw(kItemCa);
            const Gp d  = w(o.d);
            a.bt(ca, 0);
            arith(AluOp::Adc, d, x, y, true);
            set_bool(Cond::C, ca);
            break;
        }
        case IrOp::SubfE: {
            const Gp x = r(o.a), y = r(o.b);
            const Gp ca = rw(kItemCa);
            const Gp d  = w(o.d);
            a.bt(ca, 0);
            a.cmc();
            arith(AluOp::Sbb, d, y, x, false);
            set_bool(Cond::NC, ca);
            break;
        }
        case IrOp::SubfC: {
            const Gp x = r(o.a), y = r(o.b);
            const Gp ca = w(kItemCa);
            const Gp d  = w(o.d);
            arith(AluOp::Sub, d, y, x, false);
            set_bool(Cond::NC, ca);
            break;
        }
        case IrOp::SubfCi: {
            const Gp x  = r(o.a);
            const Gp ca = w(kItemCa);
            const Gp d  = w(o.d);
            a.mov(R10D, u64{o.imm});
            a.sub(R10D, x);
            set_bool(Cond::NC, ca);
            a.mov(d, R10D);
            break;
        }
        case IrOp::Neg: {
            const Gp x = r(o.a);
            const Gp d = w(o.d);
            mov_if(d, x);
            a.neg(d);
            break;
        }
        case IrOp::Mullw: {
            const Gp x = r(o.a), y = r(o.b);
            const Gp d = w(o.d);
            if (d == x) {
                a.imul(d, y);
            } else if (d == y) {
                a.imul(d, x);
            } else {
                a.mov(d, x);
                a.imul(d, y);
            }
            break;
        }
        case IrOp::Mulli: {
            const Gp x = r(o.a);
            a.imul(w(o.d), x, static_cast<s32>(o.imm));
            break;
        }
        case IrOp::Mulhw: case IrOp::Mulhwu: {
            const Gp x = r(o.a), y = r(o.b);
            const Gp d = w(o.d);
            if (o.op == IrOp::Mulhw) {
                a.movsx(R10, x);
                a.movsx(R11, y);
            } else {
                a.mov(R10D, x);
                a.mov(R11D, y);
            }
            a.imul(R10, R11);
            a.shr(R10, 32);
            a.mov(d, R10D);
            break;
        }
        case IrOp::Divw: {
            claim(RDX);
            const Gp x = r(o.a), y = r(o.b);
            const Gp d = w(o.d);
            const Label zero = a.new_label(), m1 = a.new_label(), done = a.new_label();
            a.test(y, y);
            a.j(Cond::E, zero);
            a.cmp(y, -1);
            a.j(Cond::E, m1);
            a.mov(EAX, x);
            a.cdq();
            a.idiv(y);
            a.mov(d, EAX);
            a.bind(done);
            m_ool.push_back([this, x, d, zero, m1, done] {
                const Label ovf = m_a.new_label();
                m_a.bind(zero);
                mov_if(d, x);
                m_a.sar(d, 31);
                m_a.jmp(done);
                m_a.bind(m1);
                m_a.mov(EAX, x);
                m_a.neg(EAX);
                m_a.j(Cond::O, ovf);
                m_a.mov(d, EAX);
                m_a.jmp(done);
                m_a.bind(ovf);
                m_a.mov(d, u64{0xffffffffu});
                m_a.jmp(done);
            });
            break;
        }
        case IrOp::Divwu: {
            claim(RDX);
            const Gp x = r(o.a), y = r(o.b);
            const Gp d = w(o.d);
            const Label zero = a.new_label(), done = a.new_label();
            a.test(y, y);
            a.j(Cond::E, zero);
            a.mov(EAX, x);
            a.xor_(EDX, EDX);
            a.div(y);
            a.mov(d, EAX);
            a.bind(done);
            m_ool.push_back([this, d, zero, done] {
                m_a.bind(zero);
                m_a.mov(d, u64{0});
                m_a.jmp(done);
            });
            break;
        }

        // ---- logical ----
        case IrOp::And: { const Gp x = r(o.a), y = r(o.b); arith(AluOp::And, w(o.d), x, y, true); break; }
        case IrOp::Andi: {
            const Gp x = r(o.a);
            const Gp d = w(o.d);
            mov_if(d, x);
            a.and_(d, static_cast<s32>(o.imm));
            break;
        }
        case IrOp::Andc: case IrOp::Orc: {
            const Gp x = r(o.a), y = r(o.b);
            const Gp d = w(o.d);
            const AluOp op = o.op == IrOp::Andc ? AluOp::And : AluOp::Or;
            if (x == y) {
                a.mov(d, u64{o.op == IrOp::Andc ? 0u : 0xffffffffu});
            } else if (d == y) {
                a.not_(d);
                a.alu(op, d, x);
            } else {
                a.mov(R10D, y);
                a.not_(R10D);
                mov_if(d, x);
                a.alu(op, d, R10D);
            }
            break;
        }
        case IrOp::Or: { const Gp x = r(o.a), y = r(o.b); arith(AluOp::Or, w(o.d), x, y, true); break; }
        case IrOp::Ori: case IrOp::Xori: {
            const Gp x = r(o.a);
            const Gp d = w(o.d);
            mov_if(d, x);
            if (o.imm != 0) a.alu(o.op == IrOp::Ori ? AluOp::Or : AluOp::Xor, d, static_cast<s32>(o.imm));
            break;
        }
        case IrOp::Xor: { const Gp x = r(o.a), y = r(o.b); arith(AluOp::Xor, w(o.d), x, y, true); break; }
        case IrOp::Nand: case IrOp::Nor: case IrOp::Eqv: {
            const Gp x = r(o.a), y = r(o.b);
            const Gp d = w(o.d);
            arith(o.op == IrOp::Nand ? AluOp::And : o.op == IrOp::Nor ? AluOp::Or : AluOp::Xor, d, x, y, true);
            a.not_(d);
            break;
        }
        case IrOp::Extsb: { const Gp x = r(o.a); a.movsx(w(o.d), x.r8()); break; }
        case IrOp::Extsh: { const Gp x = r(o.a); a.movsx(w(o.d), x.r16()); break; }
        case IrOp::Cntlzw: { const Gp x = r(o.a); a.clz(w(o.d), x, m_be.m_lzcnt); break; }

        // ---- shifts and rotates ----
        case IrOp::Slw: case IrOp::Srw: {
            claim(RCX);
            const Gp x = r(o.a), y = r(o.b);
            const Gp d = w(o.d);
            a.mov(ECX, y);
            a.mov(R10D, x);
            if (o.op == IrOp::Slw) {
                a.shl_cl(R10);
            } else {
                a.shr_cl(R10);
            }
            a.mov(d, R10D);
            break;
        }
        case IrOp::Sraw: {
            claim(RCX);
            const Gp x = r(o.a), y = r(o.b);
            const Gp ca = w(kItemCa);
            const Gp d  = w(o.d);
            a.mov(ECX, y);
            a.movsx(R10, x);
            a.mov(R11, R10);
            a.sar_cl(R10);
            a.mov(RAX, R10);
            a.shl_cl(RAX);
            a.cmp(RAX, R11);
            a.set(Cond::NE, AL);
            a.shr(R11, 63);
            a.and_(AL, R11B);
            a.movzx(ca, AL);
            a.mov(d, R10D);
            break;
        }
        case IrOp::Srawi: {
            const Gp x  = r(o.a);
            const Gp ca = w(kItemCa);
            const Gp d  = w(o.d);
            const u32 sh = o.b & 31;
            if (sh == 0) {
                a.mov(ca, u64{0});
                mov_if(d, x);
                break;
            }
            a.test(x, static_cast<s32>((1u << sh) - 1));
            a.set(Cond::NE, R11B);
            a.mov(R10D, x);
            a.shr(R10D, 31);
            a.and_(R11B, R10B);
            a.movzx(ca, R11B);
            mov_if(d, x);
            a.sar(d, static_cast<u8>(sh));
            break;
        }
        case IrOp::Rlwinm: {
            const Gp  x  = r(o.a);
            const Gp  d  = w(o.d);
            const u32 sh = o.b & 31, m = o.imm;
            mov_if(d, x);
            if (m == 0xffffffffu) {
                if (sh != 0) a.rol(d, static_cast<u8>(sh));
            } else if (sh == 0) {
                a.and_(d, static_cast<s32>(m));
            } else if (m == (0xffffffffu << sh)) {
                a.shl(d, static_cast<u8>(sh));
            } else if (m == (0xffffffffu >> (32 - sh))) {
                a.shr(d, static_cast<u8>(32 - sh));
            } else {
                a.rol(d, static_cast<u8>(sh));
                a.and_(d, static_cast<s32>(m));
            }
            break;
        }
        case IrOp::Rlwnm: {
            claim(RCX);
            const Gp x = r(o.a), y = r(o.b);
            const Gp d = w(o.d);
            a.mov(ECX, y);
            a.mov(R10D, x);
            a.rol_cl(R10D);
            if (o.imm != 0xffffffffu) a.and_(R10D, static_cast<s32>(o.imm));
            a.mov(d, R10D);
            break;
        }
        case IrOp::Rlwimi: {
            const Gp  x  = r(o.a);
            const Gp  d  = rw(o.d);
            const u32 sh = o.b & 31, m = o.imm;
            if (m == 0xffffffffu) {
                mov_if(d, x);
                if (sh != 0) a.rol(d, static_cast<u8>(sh));
                break;
            }
            a.mov(R10D, x);
            if (sh != 0) a.rol(R10D, static_cast<u8>(sh));
            a.and_(R10D, static_cast<s32>(m));
            a.and_(d, static_cast<s32>(~m));
            a.or_(d, R10D);
            break;
        }

        // ---- condition register ----
        case IrOp::Cr0: case IrOp::Cmp: case IrOp::Cmpi: case IrOp::Cmpl: case IrOp::Cmpli: {
            const Gp x = r(o.a);
            Gp       y{};
            if (o.op == IrOp::Cmp || o.op == IrOp::Cmpl) y = r(o.b);
            const Gp so = r(kItemSo);
            const Gp d  = w(kItemCr + (o.op == IrOp::Cr0 ? 0 : o.d));
            switch (o.op) {
                case IrOp::Cr0: a.test(x, x); break;
                case IrOp::Cmp: case IrOp::Cmpl: a.cmp(x, y); break;
                default: a.cmp(x, static_cast<s32>(o.imm)); break;
            }
            const bool sign = o.op == IrOp::Cr0 || o.op == IrOp::Cmp || o.op == IrOp::Cmpi;
            cr_result(sign ? Cond::G : Cond::A, sign ? Cond::L : Cond::B, so, d);
            break;
        }
        case IrOp::CrLogic: {
            const Gp  fa = r(kItemCr + (o.a >> 2));
            const Gp  fb = r(kItemCr + (o.b >> 2));
            const Gp  fd = rw(kItemCr + (o.d >> 2));
            const u32 pd = 3 - (o.d & 3);
            a.mov(R10D, fa);
            if ((o.a & 3) != 3) a.shr(R10D, static_cast<u8>(3 - (o.a & 3)));
            a.and_(R10D, 1);
            a.mov(R11D, fb);
            if ((o.b & 3) != 3) a.shr(R11D, static_cast<u8>(3 - (o.b & 3)));
            a.and_(R11D, 1);
            a.lea(R10D, ptr(R11, R10, 2));
            a.mov(R11D, u64{o.imm & 15});
            a.bt(R11D, R10D);
            a.set(Cond::C, R10B);
            a.movzx(R10D, R10B);
            if (pd != 0) a.shl(R10D, static_cast<u8>(pd));
            a.and_(fd, static_cast<s32>(~(1u << pd)));
            a.or_(fd, R10D);
            break;
        }
        case IrOp::Mcrf: {
            if (o.a == o.d) break;
            const Gp s = r(kItemCr + o.a);
            mov_if(w(kItemCr + o.d), s);
            break;
        }
        case IrOp::Mfcr: {
            for (u32 f = 0; f < 8; ++f) {
                Gp c;
                if (m_c.host[kItemCr + f] >= 0) {
                    c = Gp{static_cast<u8>(m_c.host[kItemCr + f]), 4};
                } else {
                    a.movzx(R11D, sized(ptr(kSt, item_off(kItemCr + f)), 1));
                    c = R11D;
                }
                if (f == 0) {
                    a.mov(R10D, c);
                    a.shl(R10D, 28);
                } else if (f == 7) {
                    a.or_(R10D, c);
                } else {
                    if (c != R11D) a.mov(R11D, c);
                    a.shl(R11D, static_cast<u8>(28 - 4 * f));
                    a.or_(R10D, R11D);
                }
            }
            a.mov(w(o.d), R10D);
            break;
        }
        case IrOp::Mtcrf: {
            const Gp v = r(o.a);
            for (u32 f = 0; f < 8; ++f) {
                if (((o.imm >> (28 - 4 * f)) & 0xf) == 0) continue;
                const Gp c = m_c.host[kItemCr + f] >= 0 ? w(kItemCr + f) : R10D;
                a.mov(c, v);
                if (f != 7) a.shr(c, static_cast<u8>(28 - 4 * f));
                a.and_(c, 15);
                if (c == R10D) a.mov(ptr(kSt, item_off(kItemCr + f)), R10B);
            }
            break;
        }
        case IrOp::Mfxer: {
            const Gp so = r(kItemSo), ov = r(kItemOv), ca = r(kItemCa);
            const Gp d  = w(o.d);
            a.movzx(R10D, sized(ptr(kSt, OFF(xer_bc)), 1));
            a.and_(R10D, 0x7f);
            a.mov(R11D, so);
            a.shl(R11D, 31);
            a.or_(R10D, R11D);
            a.mov(R11D, ov);
            a.shl(R11D, 30);
            a.or_(R10D, R11D);
            a.mov(R11D, ca);
            a.shl(R11D, 29);
            a.or_(R10D, R11D);
            a.mov(d, R10D);
            break;
        }
        case IrOp::Mtxer: {
            const Gp v  = r(o.a);
            const Gp so = w(kItemSo), ov = w(kItemOv), ca = w(kItemCa);
            a.mov(so, v);
            a.shr(so, 31);
            a.mov(ov, v);
            a.shr(ov, 30);
            a.and_(ov, 1);
            a.mov(ca, v);
            a.shr(ca, 29);
            a.and_(ca, 1);
            a.mov(R10D, v);
            a.and_(R10D, 0x7f);
            a.mov(ptr(kSt, OFF(xer_bc)), R10B);
            break;
        }
        case IrOp::Mftb: {
            claim(RDX);
            const Gp d = w(o.d);
            a.lea(RAX, ptr(kCyc, static_cast<s32>(m_b.op_insn[k])));
            a.mov(R10D, ptr(kSt, OFF(timer_ratio)));
            a.xor_(EDX, EDX);
            a.div(R10);
            a.add(RAX, ptr(kSt, OFF(tb_offset)));
            if (o.a) a.shr(RAX, 32);
            a.mov(d, EAX);
            break;
        }

        // ---- loads and stores ----
        case IrOp::Lbz: emit_load(k, o, kU8, stub); break;
        case IrOp::Lhz: emit_load(k, o, kU16, stub); break;
        case IrOp::Lha: emit_load(k, o, kS16, stub); break;
        case IrOp::Lwz: emit_load(k, o, kU32, stub); break;
        case IrOp::Lhbrx: emit_load(k, o, kR16, stub); break;
        case IrOp::Lwbrx: emit_load(k, o, kR32, stub); break;
        case IrOp::Lfs: emit_load(k, o, kF32, stub); break;
        case IrOp::Lfd: emit_load(k, o, kF64, stub); break;
        case IrOp::Stb: emit_store(k, o, kU8, stub); break;
        case IrOp::Sth: emit_store(k, o, kU16, stub); break;
        case IrOp::Stw: emit_store(k, o, kU32, stub); break;
        case IrOp::Sthbrx: emit_store(k, o, kR16, stub); break;
        case IrOp::Stwbrx: emit_store(k, o, kR32, stub); break;
        case IrOp::Stfs: emit_store(k, o, kFS32, stub); break;
        case IrOp::Stfd: emit_store(k, o, kF64, stub); break;
        case IrOp::Stfiwx: emit_store(k, o, kFI32, stub); break;
        case IrOp::Lmw: emit_multiple(k, o, true, stub); break;
        case IrOp::Stmw: emit_multiple(k, o, false, stub); break;

        // ---- floating point ----
        case IrOp::Fmr: case IrOp::Fneg: case IrOp::Fabs: case IrOp::Fnabs:
        case IrOp::Fadd: case IrOp::Fsub: case IrOp::Fmul: case IrOp::Fdiv:
        case IrOp::Fmadd: case IrOp::Fmsub: case IrOp::Fnmadd: case IrOp::Fnmsub:
        case IrOp::Fadds: case IrOp::Fsubs: case IrOp::Fmuls: case IrOp::Fdivs:
        case IrOp::Fmadds: case IrOp::Fmsubs: case IrOp::Fnmadds: case IrOp::Fnmsubs:
        case IrOp::Frsp: case IrOp::Fres: case IrOp::Frsqrte: case IrOp::Fsqrt: case IrOp::Fsqrts:
        case IrOp::Fsel: case IrOp::Fctiw: case IrOp::Fctiwz: case IrOp::Fcmpu: case IrOp::Fprf:
            emit_fp(o);
            break;
        case IrOp::Cr1: {
            const Gp d = w(kItemCr + 1);
            a.mov(d, ptr(kSt, OFF(fpscr)));
            a.shr(d, 28);
            break;
        }

        // ---- helper ----
        case IrOp::Helper: {
            const u32 idx = u32{o.a} | (u32{o.b} << 8);
            writeback_clean();
            fprf_materialize();
            set_cur_insn(idx);
            a.mov(dword_ptr(kSt, OFF(block_pc)), static_cast<s32>(m_b.pc));
            a.mov(m_arg[0], kSt);
            a.mov(m_arg[1].r32(), u64{o.imm});
            a.mov(m_arg[2].r32(), u64{idx});
            a.call(&ppc_rt_helper);
            drop_all();
            const Label hexit = a.new_label();
            a.test(EAX, EAX);
            a.j(Cond::NE, hexit);
            m_ool.push_back([this, hexit, idx] {
                m_a.bind(hexit);
                exit_after(idx, false);
            });
            if (!stub) {
                m_brk = a.new_label();
                flags_check(m_brk, false);
            }
            break;
        }

        // ---- terminators ----
        case IrOp::B: case IrOp::Exit:
            fprf_materialize();
            end_op();
            writeback(m_c);
            charge(m_b.num_insns);
            exit_link(o.imm);
            break;
        case IrOp::Bc: case IrOp::Bcr: emit_branch(o); break;

        case IrOp::Count: a.set_error(); break;
    }
}

bool Compiler::run()
{
    Emitter& a = m_a;
    const Label body = a.new_label(), nb = a.new_label();
    a.lea(RAX, rip(body));
    a.jump(m_be.m_enter);
    m_chain_label = a.new_label();
    a.bind(m_chain_label);
    a.cmp(kBud, static_cast<s32>(m_b.num_insns));
    a.j(Cond::B, nb);
    a.mov(ptr(kSt, OFF(block_cycles)), kCyc);
    a.bind(body);

    for (u32 k = 0; k < m_b.ops.size(); ++k) {
        begin_op();
        m_brk = Label{};
        emit_op(k, false);
        end_op();
        if (m_brk.valid()) queue_break_stub(k, m_brk);
        if (!a.ok() && a.overflowed()) return false;
    }

    a.bind(nb);
    a.mov(dword_ptr(kSt, OFF(pc)), static_cast<s32>(m_b.pc));
    a.jump(m_be.m_ret);

    for (usize q = 0; q < m_ool.size(); ++q) {
        const auto f = m_ool[q];
        f();
    }
    return a.ok();
}

// ---- backend ---------------------------------------------------------------------

bool JitX64::init()
{
    if (!m_code.init()) return false;
    m_fast = std::make_unique<FastEntry[]>(1u << kFastBits);
    clear_fast();
    m_abi   = &abi_info();
    m_fma   = fpu::host_has_fma();
    m_sse41 = host_features().sse41;
    m_movbe = host_features().movbe;
    m_lzcnt = host_features().lzcnt;

    CodeBuffer::WriteScope scope(m_code);
    Emitter     a(m_code.tail());
    const Frame frame(kHostAbi, m_abi->callee_saved_gprs, m_abi->callee_saved_xmms, kLocals);
    m_loc = frame.locals_offset();
    const s32 loc = static_cast<s32>(m_loc);

    a.align_bytes(16, 0xcc);
    const usize cst = a.size();
    for (int i = 0; i < 2; ++i) a.emit64(0x8000000000000000ull);
    for (int i = 0; i < 2; ++i) a.emit64(0x7fffffffffffffffull);
    for (int i = 0; i < 2; ++i) a.emit64(std::bit_cast<u64>(1.0));
    for (int i = 0; i < 2; ++i) a.emit64(std::bit_cast<u64>(2147483647.0));
    for (int i = 0; i < 2; ++i) a.emit64(std::bit_cast<u64>(4503599627370496.0));

    const usize enter = a.size();
    frame.prologue(a);
    a.mov(kSt, m_abi->args[0]);
    a.mov(kBud, ptr(kSt, OFF(budget)));
    a.mov(kCyc, ptr(kSt, OFF(block_cycles)));
    a.mov(ptr(RSP, loc + kLocEntry), kCyc);
    a.mov(R10, static_cast<u64>(reinterpret_cast<uintptr_t>(m_fast.get())));
    a.mov(ptr(RSP, loc + kLocFast), R10);
    a.stmxcsr(dword_ptr(RSP, loc + kLocMxcsr));
    a.mov(dword_ptr(RSP, loc + kLocRnSet), 0);
    a.mov(R10D, ptr(kSt, OFF(fpscr)));
    a.and_(R10D, 3);
    const Label go = a.new_label();
    a.j(Cond::E, go);  // PowerPC RN 1 (zero), 2 (+inf), 3 (-inf) -> MXCSR RC 3, 2, 1
    a.mov(R11D, u64{4});
    a.sub(R11D, R10D);
    a.shl(R11D, 13);
    a.mov(R10D, ptr(RSP, loc + kLocMxcsr));
    a.and_(R10D, ~0x6000);
    a.or_(R10D, R11D);
    a.mov(ptr(RSP, loc + kLocTmp), R10D);
    a.ldmxcsr(dword_ptr(RSP, loc + kLocTmp));
    a.mov(dword_ptr(RSP, loc + kLocRnSet), 1);
    a.bind(go);
    a.jmp(RAX);

    const usize ret = a.size();
    a.mov(RAX, kCyc);
    a.sub(RAX, ptr(RSP, loc + kLocEntry));
    a.cmp(dword_ptr(RSP, loc + kLocRnSet), 0);
    const Label same = a.new_label();
    a.j(Cond::E, same);
    a.ldmxcsr(dword_ptr(RSP, loc + kLocMxcsr));
    a.bind(same);
    frame.epilogue(a);

    // FPRF of XMM13 into FPSCR; R10, R11 and flags only.
    const usize fprf = a.size();
    {
        const Label zero = a.new_label(), den = a.new_label(), inf = a.new_label(), nan = a.new_label(),
                    st = a.new_label();
        a.movd(R10, kFprfSrc);
        a.mov(R11, R10);
        a.shl(R11, 1);
        a.j(Cond::E, zero);
        a.shr(R11, 53);
        a.j(Cond::E, den);
        a.cmp(R11, 0x7ff);
        a.j(Cond::E, inf);
        a.mov(R11D, u64{0x04});
        a.bt(R10, 63);
        a.j(Cond::NC, st);
        a.mov(R11D, u64{0x08});
        a.jmp(st);
        a.bind(zero);
        a.mov(R11D, u64{0x02});
        a.bt(R10, 63);
        a.j(Cond::NC, st);
        a.mov(R11D, u64{0x12});
        a.jmp(st);
        a.bind(den);
        a.mov(R11D, u64{0x14});
        a.bt(R10, 63);
        a.j(Cond::NC, st);
        a.mov(R11D, u64{0x18});
        a.jmp(st);
        a.bind(inf);
        a.mov(R11, R10);
        a.shl(R11, 12);
        a.j(Cond::NE, nan);
        a.mov(R11D, u64{0x05});
        a.bt(R10, 63);
        a.j(Cond::NC, st);
        a.mov(R11D, u64{0x09});
        a.jmp(st);
        a.bind(nan);
        a.mov(R11D, u64{0x11});
        a.bind(st);
        a.mov(R10D, ptr(kSt, OFF(fpscr)));
        a.and_(R10D, static_cast<s32>(~kFpscrFPRF));
        a.shl(R11D, 12);
        a.or_(R10D, R11D);
        a.mov(ptr(kSt, OFF(fpscr)), R10D);
        a.ret();
    }

    if (!a.ok()) return false;
    u8* const base = m_code.commit(a.size());
    if (base == nullptr) return false;
    m_const = base + cst;
    m_enter = base + enter;
    m_ret   = base + ret;
    m_fprf  = base + fprf;
    m_code.set_persistent_mark();
    return true;
}

void JitX64::clear_fast()
{
    for (u32 n = 0; n < (1u << kFastBits); ++n) m_fast[n] = FastEntry{~0ull, nullptr};
}

void JitX64::patch(const BlockInfo::Exit& e, const void* to)
{
    u8 b[5];
    encode_jmp_rel32(b, e.site, to);
    m_code.patch_bytes(e.site, b, sizeof b);
}

void JitX64::compile(IrBlock& b)
{
    if (m_full) return;
    CodeBuffer::WriteScope scope(m_code);
    const CodeSpan span = m_code.tail();
    Emitter        a(span);
    Compiler       c(*this, b, a);
    if (!c.run()) {
        if (a.overflowed()) {
            m_full = true;
        } else {
            SM2_WARN("ppc jit: could not compile the block at %08x", b.pc);
        }
        return;
    }
    u8* const base = m_code.commit(a.size());
    if (base == nullptr) {
        m_full = true;
        return;
    }
    auto* info  = new BlockInfo;
    info->key   = (u64{b.key} << 32) | b.pc;
    info->chain = base + a.label_offset(c.m_chain_label);
    for (const auto& e : c.m_exits) {
        info->exits.push_back({base + a.label_offset(e.site), base + a.label_offset(e.fb),
                               (u64{b.key} << 32) | e.target});
    }
    b.native        = reinterpret_cast<NativeBlockFn>(base);
    b.native_handle = info;

    m_live[info->key] = info;
    for (u32 n = 0; n < info->exits.size(); ++n) {
        const auto& e = info->exits[n];
        m_links[e.target].push_back({info, n});
        const auto it = m_live.find(e.target);
        if (it != m_live.end()) patch(e, it->second->chain);
    }
    const auto lk = m_links.find(info->key);
    if (lk != m_links.end()) {
        for (const auto& [owner, n] : lk->second) {
            if (owner != info) patch(owner->exits[n], info->chain);
        }
    }
    m_fast[(b.pc >> 2) & ((1u << kFastBits) - 1)] = FastEntry{info->key, info->chain};
}

void JitX64::invalidated(IrBlock& b)
{
    auto* info = static_cast<BlockInfo*>(b.native_handle);
    if (info == nullptr || !info->live) return;
    info->live = false;
    CodeBuffer::WriteScope scope(m_code);
    const auto it = m_live.find(info->key);
    if (it != m_live.end() && it->second == info) m_live.erase(it);
    for (const auto& e : info->exits) {
        const auto lk = m_links.find(e.target);
        if (lk == m_links.end()) continue;
        auto& v = lk->second;
        for (usize n = 0; n < v.size();) {
            if (v[n].first == info) {
                v[n] = v.back();
                v.pop_back();
            } else {
                ++n;
            }
        }
        if (v.empty()) m_links.erase(lk);
    }
    const auto lk = m_links.find(info->key);
    if (lk != m_links.end()) {
        for (const auto& [owner, n] : lk->second) patch(owner->exits[n], owner->exits[n].fallback);
    }
    FastEntry& f = m_fast[(b.pc >> 2) & ((1u << kFastBits) - 1)];
    if (f.host == info->chain) f = FastEntry{~0ull, nullptr};
}

void JitX64::release(IrBlock& b)
{
    auto* info = static_cast<BlockInfo*>(b.native_handle);
    if (info != nullptr && info->live) invalidated(b);
    delete info;
    b.native_handle = nullptr;
    b.native        = nullptr;
}

void JitX64::reset_code()
{
    m_live.clear();
    m_links.clear();
    clear_fast();
    m_code.clear();
    m_full = false;
}

}  // namespace

std::unique_ptr<BlockBackend> create_jit_x64()
{
    auto jit = std::make_unique<JitX64>();
    if (!jit->init()) {
        SM2_WARN("ppc jit: no executable memory; using the interpreter");
        return nullptr;
    }
    return jit;
}

}  // namespace sm2::cpu::ppc

#else

namespace sm2::cpu::ppc {

std::unique_ptr<BlockBackend> create_jit_x64()
{
    return nullptr;
}

}  // namespace sm2::cpu::ppc

#endif
