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

// PowerPC IR -> arm64.
//
// Pinned: X19 state, W20 budget left, X21 block_cycles of the running block,
// X23 / X24 read / write tables. Guest GPRs, LR, CTR, CR fields, SO, OV, CA
// and FPRs are cached in host registers within a block (loaded on first use,
// written back before runtime calls and at exits; slow paths push the live
// caller-saved ones). FPRF is computed lazily from a copy of the last result
// in V31. Scratch: X14-X17, X30, V29, V30.
//
// Direct exits are patchable branches to the target's chain entry, which
// checks the budget; indirect exits probe a direct-mapped table. Blocks that
// ran a Helper always return to the run loop.

#include "cpu/ppc/ppc_jit_arm64.h"

#if defined(__aarch64__) || defined(_M_ARM64)

#include "core/log.h"
#include "cpu/jit/arm64_emitter.h"
#include "cpu/jit/code_buffer.h"
#include "cpu/page_map.h"
#include "cpu/ppc/ppc.h"
#include "cpu/ppc/ppc_fpu.h"
#include "cpu/ppc/ppc_internal.h"

#include <bit>
#include <cstddef>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace sm2::cpu::ppc {

namespace {

using namespace sm2::cpu::jit;
using namespace sm2::cpu::jit::a64;

#define OFF(f) static_cast<u32>(offsetof(PpcState, f))

constexpr Reg kSt   = X19;
constexpr Reg kBud  = W20;
constexpr Reg kCyc  = X21;
constexpr Reg kRtab = X23;
constexpr Reg kWtab = X24;

constexpr u32 kItemCr = 40;
constexpr u32 kItemSo = 48;
constexpr u32 kItemOv = 49;
constexpr u32 kItemCa = 50;
constexpr u32 kItems  = 51;

constexpr u8 kGprPool[] = {22, 25, 26, 27, 28, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
constexpr u8 kFprPool[] = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
constexpr u32 kCallerGprs = 0x3fff;  // X0-X13 of the pool
constexpr VReg kFprfSrc   = D31;

constexpr u32 kFastBits = 16;

struct FastEntry {
    u64         key;
    const void* host;
};

[[nodiscard]] u32 item_off(u32 it)
{
    if (it < kItemCr) return OFF(slot) + 4 * it;
    if (it < kItemSo) return OFF(cr) + (it - kItemCr);
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

// ---- register cache -----------------------------------------------------------

struct Cache {
    s8   host[kItems];
    s8   item[32];
    s8   fhost[32];
    s8   fitem[32];
    u32  stamp[32];
    u32  fstamp[32];
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

class JitArm64;

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

class JitArm64 final : public BlockBackend {
public:
    bool init();
    void compile(IrBlock& block) override;
    void release(IrBlock& block) override;
    void invalidated(IrBlock& block) override;
    [[nodiscard]] bool full() const override { return m_full; }
    void reset_code() override;

    CodeBuffer m_code;
    const u8*  m_enter = nullptr;
    const u8*  m_ret   = nullptr;
    const u8*  m_fprf  = nullptr;
    u32        m_loc   = 0;  ///< Frame locals: FPCR, entry cycles, fast table.
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
    Compiler(JitArm64& be, IrBlock& b, Emitter& a) : m_be(be), m_b(b), m_a(a)
    {
        m_chain = true;
        for (const IrInst& o : b.ops) {
            if (o.op == IrOp::Helper) m_chain = false;
        }
        m_key = b.key;
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
            m_a.strb(W(h), mem(kSt, item_off(it)));
        } else {
            m_a.str(W(h), mem(kSt, item_off(it)));
        }
    }
    void load_item(u32 it, u8 h)
    {
        if (it >= kItemCr) {
            m_a.ldrb(W(h), mem(kSt, item_off(it)));
        } else {
            m_a.ldr(W(h), mem(kSt, item_off(it)));
        }
    }

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
        if (m_c.fdirty & (1u << n)) m_a.str(D(h), mem(kSt, OFF(fpr) + 8 * static_cast<u32>(n)));
        m_c.fdirty &= ~(1u << n);
        m_c.fpend &= ~(1u << n);
        m_c.fhost[n] = -1;
        m_c.fitem[h] = -1;
    }

    u8 alloc_gpr()
    {
        for (u8 h : kGprPool) {
            if (m_c.item[h] < 0) return h;
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
            if (m_c.fitem[h] < 0) return h;
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

    Reg get(u32 it, bool load, bool write)
    {
        if (it == kSlotZero) return WZR;
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
        return W(static_cast<unsigned>(h));
    }
    Reg r(u32 it) { return get(it, true, false); }
    Reg w(u32 it) { return get(it, false, true); }
    Reg rw(u32 it) { return get(it, true, true); }

    VReg fget(u32 n, bool load, bool write)
    {
        s8 h = m_c.fhost[n];
        if (h < 0) {
            h            = static_cast<s8>(alloc_fpr());
            m_c.fhost[n] = h;
            m_c.fitem[h] = static_cast<s8>(n);
            if (load) m_a.ldr(D(static_cast<unsigned>(h)), mem(kSt, OFF(fpr) + 8 * n));
        }
        m_c.flock |= 1u << h;
        m_c.fstamp[h] = ++m_c.clock;
        if (write) m_c.fpend |= 1u << n;
        return D(static_cast<unsigned>(h));
    }
    VReg fr(u32 n) { return fget(n, true, false); }
    VReg fw(u32 n) { return fget(n, false, true); }

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
            if (s.fdirty & (1u << n)) m_a.str(D(static_cast<unsigned>(s.fhost[n])), mem(kSt, OFF(fpr) + 8 * n));
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
        u32 g, f;
    };
    Saved push_live(const Cache& s, u32 excl_g = 0, u32 excl_f = 0)
    {
        u32 g = 0, f = 0;
        for (u8 h : kGprPool) {
            if (s.item[h] >= 0 && ((1u << h) & kCallerGprs)) g |= 1u << h;
        }
        for (u8 h : kFprPool) {
            if (s.fitem[h] >= 0) f |= 1u << h;
        }
        if (s.fprf) f |= 1u << kFprfSrc.id;
        g &= ~excl_g;
        f &= ~excl_f;
        push_regs(m_a, g, f);
        return {g, f};
    }
    void pop_live(Saved sv) { pop_regs(m_a, sv.g, sv.f); }

    void set_cur_insn(u32 i)
    {
        if (i == 0) {
            m_a.str(WZR, mem(kSt, OFF(cur_insn)));
        } else {
            m_a.mov(W16, u64{i});
            m_a.str(W16, mem(kSt, OFF(cur_insn)));
        }
    }

    // ---- exits ----
    void exit_link(u32 target)
    {
        if (m_chain) {
            ExitRec e{m_a.new_label(), m_a.new_label(), target};
            m_a.bind(e.site);
            m_a.b(e.fb);
            m_a.bind(e.fb);
            m_exits.push_back(e);
        }
        m_a.mov(W16, u64{target});
        m_a.str(W16, mem(kSt, OFF(pc)));
        m_a.jump(m_be.m_ret);
    }
    void charge(u32 n)
    {
        m_a.add(kCyc, kCyc, n);
        m_a.sub(kBud, kBud, n);
    }
    /// pc already in W30.
    void exit_indirect()
    {
        m_a.str(W30, mem(kSt, OFF(pc)));
        if (!m_chain) {
            m_a.jump(m_be.m_ret);
            return;
        }
        m_a.ldr(X17, mem(SP, m_be.m_loc + 16));
        m_a.ubfx(W16, W30, 2, kFastBits);
        m_a.add(X17, X17, X16, Shift::LSL, 4);
        m_a.ldp(X16, X17, mem(X17));
        if (m_key != 0) m_a.orr(X30, X30, u64{m_key} << 32);
        m_a.cmp(X16, X30);
        const Label miss = m_a.new_label();
        m_a.b(Cond::NE, miss);
        m_a.br(X17);
        m_a.bind(miss);
        m_a.jump(m_be.m_ret);
    }
    /// Leave after instruction i (break or helper exit).
    void exit_after(u32 i, bool set_pc)
    {
        if (set_pc) {
            m_a.mov(W16, u64{m_b.pc + 4 * (i + 1)});
            m_a.str(W16, mem(kSt, OFF(pc)));
        }
        m_a.add(kCyc, kCyc, i + 1);
        m_a.jump(m_be.m_ret);
    }

    void emit_op(u32 k, bool stub);
    void queue_break_stub(u32 k, Label l);

    // ---- op groups ----
    void ea(const IrInst& o, Reg ra, Reg rb);
    enum MemKind : u8 { kU8, kU16, kS16, kU32, kR16, kR32, kF32, kF64, kFS32, kFI32 };
    void emit_load(u32 k, const IrInst& o, MemKind kind, bool stub);
    void emit_store(u32 k, const IrInst& o, MemKind kind, bool stub);
    void emit_multiple(u32 k, const IrInst& o, bool load, bool stub);
    void flags_check(Label brk, bool stub)
    {
        if (stub) return;
        m_a.ldr(W17, mem(kSt, OFF(exit_flags)));
        m_a.cbnz(W17, brk);
    }
    void lfs_convert(VReg fd);
    void stfs_convert(VReg fs);
    void cr_from_flags(Cond lt, Reg so, Reg d);
    void round_single(VReg dst, VReg src)
    {
        m_a.fcvt(S30, src);
        m_a.fcvt(dst, S30);
    }
    void neg_unless_nan(VReg dst, VReg src)
    {
        m_a.fneg(D29, src);
        m_a.fcmp(src, src);
        m_a.fcsel(dst, src, D29, Cond::VS);
    }
    void emit_fp(const IrInst& o);
    void emit_branch(const IrInst& o);

    JitArm64& m_be;
    IrBlock&  m_b;
    Emitter&  m_a;
    Cache     m_c;
    bool      m_chain;
    u32       m_key;
    Label     m_brk{};
    std::vector<std::function<void()>> m_ool;
};

void Compiler::ea(const IrInst& o, Reg ra, Reg rb)
{
    const s64 imm = static_cast<s32>(o.imm);
    if (o.b != kSlotZero) {
        if (o.a == kSlotZero) {
            m_a.mov(W30, rb);
        } else {
            m_a.add(W30, ra, rb);
        }
        if (imm != 0) m_a.add_imm(W30, W30, imm, W16);
    } else if (o.a == kSlotZero) {
        m_a.mov(W30, u64{o.imm});
    } else {
        m_a.add_imm(W30, ra, imm, W16);
        if (imm == 0) m_a.mov(W30, ra);
    }
}

void Compiler::lfs_convert(VReg fd)
{
    const Label special = m_a.new_label(), done = m_a.new_label();
    m_a.ubfx(W17, W16, 23, 8);
    m_a.cmp(W17, 0xff);
    m_a.b(Cond::EQ, special);
    m_a.fmov(S(fd.id), W16);
    m_a.fcvt(fd, S(fd.id));
    m_a.bind(done);
    m_ool.push_back([this, special, done, fd] {
        m_a.bind(special);
        m_a.ubfiz(X17, X16, 29, 23);
        m_a.and_(W16, W16, 0x80000000);
        m_a.orr(X17, X17, X16, Shift::LSL, 32);
        m_a.orr(X17, X17, 0x7ff0000000000000ull);
        m_a.fmov(fd, X17);
        m_a.b(done);
    });
}

/// double_to_single(fs) -> W14 (clobbers X15-X17).
void Compiler::stfs_convert(VReg fs)
{
    const Label small = m_a.new_label(), done = m_a.new_label();
    m_a.fmov(X15, fs);
    m_a.ubfx(X17, X15, 52, 11);
    m_a.cmp(W17, 896);
    m_a.b(Cond::LS, small);
    m_a.lsr(X16, X15, 29);
    m_a.and_(W16, W16, 0x3fffffff);
    m_a.lsr(X17, X15, 32);
    m_a.and_(W17, W17, 0xc0000000);
    m_a.orr(W14, W17, W16);
    m_a.bind(done);
    m_ool.push_back([this, small, done] {
        m_a.bind(small);
        m_a.lsr(X16, X15, 32);
        m_a.and_(W14, W16, 0x80000000);
        m_a.cmp(W17, 874);
        m_a.b(Cond::LO, done);
        m_a.mov(W16, 926);
        m_a.sub(W16, W16, W17);
        m_a.and_(X17, X15, 0x000fffffffffffffull);
        m_a.orr(X17, X17, 1ull << 52);
        m_a.lsr(X17, X17, X16);
        m_a.orr(W14, W14, W17);
        m_a.b(done);
    });
}

void Compiler::emit_load(u32 k, const IrInst& o, MemKind kind, bool stub)
{
    static constexpr u32 kSize[] = {1, 2, 2, 4, 2, 4, 4, 8, 4, 4};
    const u32  size = kSize[kind];
    const bool fp   = kind == kF32 || kind == kF64;
    const Reg  ra   = r(o.a);
    const Reg  rb   = r(o.b);
    ea(o, ra, rb);
    Reg  d{};
    VReg fd{};
    if (fp) {
        fd = fw(o.d);
    } else {
        d = w(o.d);
    }
    const Cache snap = m_c;
    const Label slow = m_a.new_label(), resume = m_a.new_label();
    m_a.lsr(W16, W30, PageMap::kPageBits);
    m_a.ldr(X16, mem(kRtab, X16, Extend::UXTX, true));
    m_a.cbz(X16, slow);
    if (size > 1) {
        m_a.tst(W30, size - 1);
        m_a.b(Cond::NE, slow);
    }
    m_a.and_(W17, W30, PageMap::kPageMask);
    const Mem m = mem(X16, X17);
    switch (kind) {
        case kU8: m_a.ldrb(d, m); break;
        case kU16: m_a.ldrh(d, m); m_a.rev16(d, d); break;
        case kS16: m_a.ldrh(d, m); m_a.rev16(d, d); m_a.sxth(d, d); break;
        case kU32: m_a.ldr(d, m); m_a.rev(d, d); break;
        case kR16: m_a.ldrh(d, m); break;
        case kR32: m_a.ldr(d, m); break;
        case kF32:
            m_a.ldr(W16, m);
            m_a.rev(W16, W16);
            lfs_convert(fd);
            break;
        case kF64:
            m_a.ldr(X16, m);
            m_a.rev(X16, X16);
            m_a.fmov(fd, X16);
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
        m_a.mov(W15, W30);
        writeback(snap);
        if (snap.fprf) m_a.call(m_be.m_fprf);
        const Saved sv = push_live(snap, fp ? 0u : (1u << d.id), fp ? (1u << fd.id) : 0u);
        set_cur_insn(i);
        m_a.mov(W1, W15);
        m_a.mov(X0, kSt);
        switch (size) {
            case 1: m_a.call(&ppc_rt_read8); break;
            case 2: m_a.call(&ppc_rt_read16); break;
            case 4: m_a.call(&ppc_rt_read32); break;
            default: m_a.call(&ppc_rt_read64); break;
        }
        if (size == 8) {
            m_a.mov(X16, X0);
        } else {
            m_a.mov(W16, W0);
        }
        pop_live(sv);
        switch (kind) {
            case kU8: m_a.and_(d, W16, 0xff); break;
            case kU16: m_a.and_(d, W16, 0xffff); break;
            case kS16: m_a.sxth(d, W16); break;
            case kU32: m_a.mov(d, W16); break;
            case kR16: m_a.rev16(W16, W16); m_a.and_(d, W16, 0xffff); break;
            case kR32: m_a.rev(d, W16); break;
            case kF32: lfs_convert(fd); break;
            case kF64: m_a.fmov(fd, X16); break;
            default: break;
        }
        flags_check(brk, stub);
        m_a.b(resume);
    });
}

void Compiler::emit_store(u32 k, const IrInst& o, MemKind kind, bool stub)
{
    static constexpr u32 kSize[] = {1, 2, 2, 4, 2, 4, 4, 8, 4, 4};
    const u32 size = kSize[kind];
    const Reg ra   = r(o.a);
    const Reg rb   = r(o.b);
    Reg       v{};
    if (kind == kF64 || kind == kFS32 || kind == kFI32) {
        const VReg fv = fr(o.d);
        ea(o, ra, rb);
        if (kind == kF64) {
            m_a.fmov(X14, fv);
            v = X14;
        } else if (kind == kFS32) {
            stfs_convert(fv);
            v = W14;
        } else {
            m_a.fmov(W14, S(fv.id));
            v = W14;
        }
    } else {
        v = r(o.d);
        ea(o, ra, rb);
    }
    const Cache snap = m_c;
    const Label slow = m_a.new_label(), resume = m_a.new_label();
    m_a.lsr(W16, W30, PageMap::kPageBits);
    m_a.ldr(X16, mem(kWtab, X16, Extend::UXTX, true));
    m_a.cbz(X16, slow);
    if (size > 1) {
        m_a.tst(W30, size - 1);
        m_a.b(Cond::NE, slow);
    }
    m_a.and_(W17, W30, PageMap::kPageMask);
    const Mem m = mem(X16, X17);
    switch (kind) {
        case kU8: m_a.strb(v, m); break;
        case kU16: m_a.rev16(W15, v); m_a.strh(W15, m); break;
        case kU32:
        case kFS32:
        case kFI32: m_a.rev(W15, v); m_a.str(W15, m); break;
        case kR16: m_a.strh(v, m); break;
        case kR32: m_a.str(v, m); break;
        case kF64: m_a.rev(X15, X14); m_a.str(X15, m); break;
        default: m_a.set_error(); break;
    }
    m_a.bind(resume);

    const u32   i   = m_b.op_insn[k];
    const Label brk = stub ? Label{} : m_a.new_label();
    if (!stub) m_brk = brk;
    m_ool.push_back([this, snap, slow, resume, brk, stub, kind, size, v, i] {
        m_c = snap;
        m_a.bind(slow);
        m_a.mov(W15, W30);
        writeback(snap);
        if (snap.fprf) m_a.call(m_be.m_fprf);
        const Saved sv = push_live(snap);
        set_cur_insn(i);
        switch (kind) {
            case kU8: m_a.and_(W17, v, 0xff); break;
            case kU16: m_a.and_(W17, v, 0xffff); break;
            case kR16: m_a.rev16(W17, v); m_a.and_(W17, W17, 0xffff); break;
            case kR32: m_a.rev(W17, v); break;
            case kF64: m_a.mov(X17, X14); break;
            default: m_a.mov(W17, v); break;
        }
        m_a.mov(W1, W15);
        m_a.mov(X0, kSt);
        m_a.mov(X2, X17);
        switch (size) {
            case 1: m_a.call(&ppc_rt_write8); break;
            case 2: m_a.call(&ppc_rt_write16); break;
            case 4: m_a.call(&ppc_rt_write32); break;
            default: m_a.call(&ppc_rt_write64); break;
        }
        pop_live(sv);
        flags_check(brk, stub);
        m_a.b(resume);
    });
}

void Compiler::emit_multiple(u32 k, const IrInst& o, bool load, bool stub)
{
    fprf_now();
    const Reg ra = r(o.a);
    const Reg rb = r(o.b);
    ea(o, ra, rb);
    writeback_clean();
    const Saved sv = push_live(m_c);
    m_a.mov(W1, W30);
    m_a.mov(X0, kSt);
    m_a.mov(W2, u64{o.d});
    m_a.mov(W3, u64{m_b.op_insn[k]});
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

void Compiler::cr_from_flags(Cond lt, Reg so, Reg d)
{
    m_a.cset(W16, Cond::NE);
    m_a.cinc(W16, W16, lt);
    m_a.mov(W17, 2);
    m_a.lsl(W16, W17, W16);
    m_a.orr(d, W16, so);
}

void Compiler::emit_fp(const IrInst& o)
{
    Emitter& a = m_a;
    switch (o.op) {
        case IrOp::Fmr: {
            const VReg s = fr(o.a);
            const VReg d = fw(o.d);
            if (d != s) a.fmov(d, s);
            break;
        }
        case IrOp::Fneg: { const VReg s = fr(o.a); a.fneg(fw(o.d), s); break; }
        case IrOp::Fabs: { const VReg s = fr(o.a); a.fabs(fw(o.d), s); break; }
        case IrOp::Fnabs: {
            const VReg s = fr(o.a);
            const VReg d = fw(o.d);
            a.fabs(d, s);
            a.fneg(d, d);
            break;
        }
        case IrOp::Fadd: case IrOp::Fsub: case IrOp::Fmul: case IrOp::Fdiv:
        case IrOp::Fadds: case IrOp::Fsubs: case IrOp::Fmuls: case IrOp::Fdivs: {
            const VReg fa = fr(o.a), fb = fr(o.b);
            const VReg d  = fw(o.d);
            const bool single = o.op >= IrOp::Fadds;
            const VReg t      = single ? D30 : d;
            switch (o.op) {
                case IrOp::Fadd: case IrOp::Fadds: a.fadd(t, fa, fb); break;
                case IrOp::Fsub: case IrOp::Fsubs: a.fsub(t, fa, fb); break;
                case IrOp::Fmul: case IrOp::Fmuls: a.fmul(t, fa, fb); break;
                default: a.fdiv(t, fa, fb); break;
            }
            if (single) round_single(d, D30);
            break;
        }
        case IrOp::Fmadd: case IrOp::Fmsub: case IrOp::Fnmadd: case IrOp::Fnmsub:
        case IrOp::Fmadds: case IrOp::Fmsubs: case IrOp::Fnmadds: case IrOp::Fnmsubs: {
            const VReg fa = fr(o.a), fb = fr(o.b), fc = fr(o.imm);
            const VReg d  = fw(o.d);
            const bool single = o.op >= IrOp::Fmadds;
            const bool neg    = o.op == IrOp::Fnmadd || o.op == IrOp::Fnmsub || o.op == IrOp::Fnmadds ||
                             o.op == IrOp::Fnmsubs;
            const bool sub = o.op == IrOp::Fmsub || o.op == IrOp::Fnmsub || o.op == IrOp::Fmsubs ||
                             o.op == IrOp::Fnmsubs;
            const VReg t = (single || neg) ? D30 : d;
            if (sub) {
                a.fnmsub(t, fa, fc, fb);
            } else {
                a.fmadd(t, fa, fc, fb);
            }
            if (neg) neg_unless_nan(single ? D30 : d, D30);
            if (single) round_single(d, D30);
            break;
        }
        case IrOp::Frsp: { const VReg s = fr(o.a); round_single(fw(o.d), s); break; }
        case IrOp::Fres: {
            const VReg s = fr(o.a);
            const VReg d = fw(o.d);
            a.fmov(D30, 1.0);
            a.fdiv(D30, D30, s);
            round_single(d, D30);
            break;
        }
        case IrOp::Frsqrte: {
            const VReg s = fr(o.a);
            const VReg d = fw(o.d);
            a.fsqrt(D30, s);
            a.fmov(D29, 1.0);
            a.fdiv(d, D29, D30);
            break;
        }
        case IrOp::Fsqrt: { const VReg s = fr(o.a); a.fsqrt(fw(o.d), s); break; }
        case IrOp::Fsqrts: {
            const VReg s = fr(o.a);
            const VReg d = fw(o.d);
            a.fsqrt(D30, s);
            round_single(d, D30);
            break;
        }
        case IrOp::Fsel: {
            const VReg fa = fr(o.a), fb = fr(o.b), fc = fr(o.imm);
            const VReg d  = fw(o.d);
            a.fcmp_zero(fa);
            a.fcsel(d, fc, fb, Cond::GE);
            break;
        }
        case IrOp::Fctiw: case IrOp::Fctiwz: {
            const VReg s = fr(o.a);
            const VReg d = fw(o.d);
            if (o.op == IrOp::Fctiw) {
                a.frinti(D30, s);
            } else {
                a.frintz(D30, s);
            }
            a.fcvtzs(X16, D30);
            a.mov(X17, 0x7fffffffull);
            a.cmp(X16, X17);
            a.csel(X16, X17, X16, Cond::GT);
            a.mov(X17, 0xffffffff80000000ull);
            a.mov(W30, W16);
            a.cmp(X16, X17);
            a.csel(X16, X17, X30, Cond::LT);
            a.fcmp(D30, D30);
            a.csel(X16, X17, X16, Cond::VS);
            a.fmov(d, X16);
            break;
        }
        case IrOp::Fcmpu: {
            const VReg fa = fr(o.a), fb = fr(o.b);
            const Reg  d  = w(kItemCr + o.d);
            const Cache snap = m_c;
            const Label un = m_a.new_label(), set = m_a.new_label();
            a.fcmp(fa, fb);
            a.mov(W16, 1);
            a.mov(W17, 2);
            a.csel(W16, W17, W16, Cond::EQ);
            a.mov(W17, 4);
            a.csel(W16, W17, W16, Cond::GT);
            a.mov(W17, 8);
            a.csel(W16, W17, W16, Cond::MI);
            a.b(Cond::VS, un);
            a.ldr(W17, mem(kSt, OFF(fpscr)));
            a.bfi(W17, W16, 12, 5);
            a.str(W17, mem(kSt, OFF(fpscr)));
            a.bind(set);
            a.mov(d, W16);
            m_c.fprf = false;
            m_ool.push_back([this, snap, un, set, fa, fb, d] {
                m_a.bind(un);
                const Saved sv = push_live(snap, 1u << d.id);
                m_a.ldr(W0, mem(kSt, OFF(fpscr)));
                m_a.mov(W17, 1);
                m_a.bfi(W0, W17, 12, 5);
                m_a.fmov(X1, fa);
                m_a.fmov(X2, fb);
                m_a.call(&jit_fcmpu_unordered);
                m_a.str(W0, mem(kSt, OFF(fpscr)));
                pop_live(sv);
                m_a.mov(W16, 1);
                m_a.b(set);
            });
            break;
        }
        case IrOp::Fprf: {
            const VReg s = fr(o.d);
            a.fmov(kFprfSrc, s);
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
    Reg t{}, ctr{}, f{};
    if (o.op == IrOp::Bcr) t = r(o.b);
    if (!(bo & 4)) ctr = rw(kSlotCTR);
    if (!(bo & 16)) f = r(kItemCr + (bi >> 2));
    fprf_materialize();
    if (o.op == IrOp::Bcr) a.and_(W30, t, 0xfffffffc);
    if (!(bo & 4)) a.sub(ctr, ctr, 1);
    end_op();
    writeback(m_c);
    charge(n);
    const Label nt = a.new_label();
    if (!(bo & 4)) {
        if (bo & 2) {
            a.cbnz(ctr, nt);
        } else {
            a.cbz(ctr, nt);
        }
    }
    if (!(bo & 16)) {
        const u32 bit = 3 - (bi & 3);
        if (bo & 8) {
            a.tbz(f, bit, nt);
        } else {
            a.tbnz(f, bit, nt);
        }
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
            const Reg s = r(o.a);
            const Reg d = w(o.d);
            if (d != s) a.mov(d, s);
            break;
        }
        case IrOp::Li: a.mov(w(o.d), u64{o.imm}); break;
        case IrOp::Add: { const Reg x = r(o.a), y = r(o.b); a.add(w(o.d), x, y); break; }
        case IrOp::Addi: {
            if (o.a == kSlotZero) {
                a.mov(w(o.d), u64{o.imm});
                break;
            }
            const Reg x = r(o.a);
            a.add_imm(w(o.d), x, static_cast<s32>(o.imm), W16);
            break;
        }
        case IrOp::Sub: { const Reg x = r(o.a), y = r(o.b); a.sub(w(o.d), x, y); break; }
        case IrOp::AddC: {
            const Reg x = r(o.a), y = r(o.b);
            const Reg ca = w(kItemCa);
            a.adds(w(o.d), x, y);
            a.cset(ca, Cond::CS);
            break;
        }
        case IrOp::AddCi: {
            const Reg x  = r(o.a);
            const Reg ca = w(kItemCa);
            const Reg d  = w(o.d);
            a.mov(W16, u64{o.imm});
            a.adds(d, x, W16);
            a.cset(ca, Cond::CS);
            break;
        }
        case IrOp::AddE: case IrOp::SubfE: {
            const Reg x = r(o.a), y = r(o.b);
            const Reg ca = rw(kItemCa);
            const Reg d  = w(o.d);
            a.cmp(ca, 1);
            if (o.op == IrOp::AddE) {
                a.adcs(d, x, y);
            } else {
                a.sbcs(d, y, x);
            }
            a.cset(ca, Cond::CS);
            break;
        }
        case IrOp::SubfC: {
            const Reg x = r(o.a), y = r(o.b);
            const Reg ca = w(kItemCa);
            a.subs(w(o.d), y, x);
            a.cset(ca, Cond::HS);
            break;
        }
        case IrOp::SubfCi: {
            const Reg x  = r(o.a);
            const Reg ca = w(kItemCa);
            const Reg d  = w(o.d);
            a.mov(W16, u64{o.imm});
            a.subs(d, W16, x);
            a.cset(ca, Cond::HS);
            break;
        }
        case IrOp::Neg: { const Reg x = r(o.a); a.neg(w(o.d), x); break; }
        case IrOp::Mullw: { const Reg x = r(o.a), y = r(o.b); a.mul(w(o.d), x, y); break; }
        case IrOp::Mulli: {
            const Reg x = r(o.a);
            a.mov(W16, u64{o.imm});
            a.mul(w(o.d), x, W16);
            break;
        }
        case IrOp::Mulhw: case IrOp::Mulhwu: {
            const Reg x = r(o.a), y = r(o.b);
            const Reg d = w(o.d);
            if (o.op == IrOp::Mulhw) {
                a.smull(X16, x, y);
            } else {
                a.umull(X16, x, y);
            }
            a.lsr(X16, X16, 32);
            a.mov(d, W16);
            break;
        }
        case IrOp::Divw: {
            const Reg x = r(o.a), y = r(o.b);
            const Reg d = w(o.d);
            const Label zero = a.new_label(), m1 = a.new_label(), done = a.new_label();
            a.cbz(y, zero);
            a.cmn(y, 1);
            a.b(Cond::EQ, m1);
            a.sdiv(d, x, y);
            a.bind(done);
            m_ool.push_back([this, x, d, zero, m1, done] {
                m_a.bind(zero);
                m_a.asr(d, x, 31);
                m_a.b(done);
                m_a.bind(m1);
                m_a.negs(W16, x);
                m_a.csinv(d, W16, WZR, Cond::VC);
                m_a.b(done);
            });
            break;
        }
        case IrOp::Divwu: { const Reg x = r(o.a), y = r(o.b); a.udiv(w(o.d), x, y); break; }

        // ---- logical ----
        case IrOp::And: { const Reg x = r(o.a), y = r(o.b); a.and_(w(o.d), x, y); break; }
        case IrOp::Andi: { const Reg x = r(o.a); a.and_imm(w(o.d), x, o.imm, W16); break; }
        case IrOp::Andc: { const Reg x = r(o.a), y = r(o.b); a.bic(w(o.d), x, y); break; }
        case IrOp::Or: { const Reg x = r(o.a), y = r(o.b); a.orr(w(o.d), x, y); break; }
        case IrOp::Ori: {
            const Reg x = r(o.a);
            const Reg d = w(o.d);
            if (o.imm == 0) {
                if (d != x) a.mov(d, x);
            } else {
                a.orr_imm(d, x, o.imm, W16);
            }
            break;
        }
        case IrOp::Orc: { const Reg x = r(o.a), y = r(o.b); a.orn(w(o.d), x, y); break; }
        case IrOp::Xor: { const Reg x = r(o.a), y = r(o.b); a.eor(w(o.d), x, y); break; }
        case IrOp::Xori: {
            const Reg x = r(o.a);
            const Reg d = w(o.d);
            if (o.imm == 0) {
                if (d != x) a.mov(d, x);
            } else {
                a.eor_imm(d, x, o.imm, W16);
            }
            break;
        }
        case IrOp::Nand: {
            const Reg x = r(o.a), y = r(o.b);
            const Reg d = w(o.d);
            a.and_(d, x, y);
            a.mvn(d, d);
            break;
        }
        case IrOp::Nor: {
            const Reg x = r(o.a), y = r(o.b);
            const Reg d = w(o.d);
            a.orr(d, x, y);
            a.mvn(d, d);
            break;
        }
        case IrOp::Eqv: { const Reg x = r(o.a), y = r(o.b); a.eon(w(o.d), x, y); break; }
        case IrOp::Extsb: { const Reg x = r(o.a); a.sxtb(w(o.d), x); break; }
        case IrOp::Extsh: { const Reg x = r(o.a); a.sxth(w(o.d), x); break; }
        case IrOp::Cntlzw: { const Reg x = r(o.a); a.clz(w(o.d), x); break; }

        // ---- shifts and rotates ----
        case IrOp::Slw: case IrOp::Srw: {
            const Reg x = r(o.a), y = r(o.b);
            const Reg d = w(o.d);
            a.and_(W16, y, 63);
            if (o.op == IrOp::Slw) {
                a.lsl(X16, x.as_x(), X16);
            } else {
                a.lsr(X16, x.as_x(), X16);
            }
            a.mov(d, W16);
            break;
        }
        case IrOp::Sraw: {
            const Reg x = r(o.a), y = r(o.b);
            const Reg ca = w(kItemCa);
            const Reg d  = w(o.d);
            a.sxtw(X16, x);
            a.and_(W17, y, 63);
            a.asr(X30, X16, X17);
            a.lsl(X17, X30, X17);
            a.cmp(X17, X16);
            a.cset(W17, Cond::NE);
            a.and_(ca, W17, W16, Shift::LSR, 31);
            a.mov(d, W30);
            break;
        }
        case IrOp::Srawi: {
            const Reg x  = r(o.a);
            const Reg ca = w(kItemCa);
            const Reg d  = w(o.d);
            const u32 sh = o.b & 31;
            if (sh == 0) {
                a.mov(ca, WZR);
                if (d != x) a.mov(d, x);
                break;
            }
            a.tst(x, (1ull << sh) - 1);
            a.cset(W17, Cond::NE);
            a.and_(ca, W17, x, Shift::LSR, 31);
            a.asr(d, x, sh);
            break;
        }
        case IrOp::Rlwinm: {
            const Reg x  = r(o.a);
            const Reg d  = w(o.d);
            const u32 sh = o.b & 31, m = o.imm;
            const u32 ror = (32 - sh) & 31;
            if (m == 0xffffffffu) {
                if (sh == 0) {
                    if (d != x) a.mov(d, x);
                } else {
                    a.ror(d, x, ror);
                }
            } else if (sh == 0) {
                a.and_imm(d, x, m, W16);
            } else if (m == (0xffffffffu << sh)) {
                a.lsl(d, x, sh);
            } else if (m == (0xffffffffu >> (32 - sh))) {
                a.lsr(d, x, 32 - sh);
            } else {
                a.ror(W16, x, ror);
                a.and_imm(d, W16, m, W17);
            }
            break;
        }
        case IrOp::Rlwnm: {
            const Reg x = r(o.a), y = r(o.b);
            const Reg d = w(o.d);
            a.neg(W16, y);
            a.ror(W16, x, W16);
            if (o.imm == 0xffffffffu) {
                a.mov(d, W16);
            } else {
                a.and_imm(d, W16, o.imm, W17);
            }
            break;
        }
        case IrOp::Rlwimi: {
            const Reg x  = r(o.a);
            const Reg d  = rw(o.d);
            const u32 sh = o.b & 31, m = o.imm;
            const u32 ror = (32 - sh) & 31;
            if (m == 0xffffffffu) {
                if (sh == 0) {
                    if (d != x) a.mov(d, x);
                } else {
                    a.ror(d, x, ror);
                }
                break;
            }
            const u32 lsb   = static_cast<u32>(std::countr_zero(m));
            const u32 width = static_cast<u32>(std::popcount(m));
            if (m != 0 && (m >> lsb) == (width == 32 ? 0xffffffffu : (1u << width) - 1)) {
                const u32 rot = (ror + lsb) & 31;
                if (rot == 0) {
                    a.bfi(d, x, lsb, width);
                } else {
                    a.ror(W16, x, rot);
                    a.bfi(d, W16, lsb, width);
                }
            } else {
                if (ror == 0) {
                    a.and_imm(W16, x, m, W17);
                } else {
                    a.ror(W16, x, ror);
                    a.and_imm(W16, W16, m, W17);
                }
                a.and_imm(d, d, ~m, W17);
                a.orr(d, d, W16);
            }
            break;
        }

        // ---- condition register ----
        case IrOp::Cr0: case IrOp::Cmp: case IrOp::Cmpi: case IrOp::Cmpl: case IrOp::Cmpli: {
            const Reg x = r(o.a);
            Reg       y{};
            if (o.op == IrOp::Cmp || o.op == IrOp::Cmpl) y = r(o.b);
            const Reg so = r(kItemSo);
            const Reg d  = w(kItemCr + (o.op == IrOp::Cr0 ? 0 : o.d));
            switch (o.op) {
                case IrOp::Cr0: a.cmp(x, 0); break;
                case IrOp::Cmp: case IrOp::Cmpl: a.cmp(x, y); break;
                case IrOp::Cmpi: {
                    const s32 v = static_cast<s32>(o.imm);
                    if (v >= 0 && v < 4096) {
                        a.cmp(x, static_cast<u32>(v));
                    } else if (v < 0 && v > -4096) {
                        a.cmn(x, static_cast<u32>(-v));
                    } else {
                        a.mov(W16, u64{o.imm});
                        a.cmp(x, W16);
                    }
                    break;
                }
                default:
                    if (o.imm < 4096) {
                        a.cmp(x, o.imm);
                    } else {
                        a.mov(W16, u64{o.imm});
                        a.cmp(x, W16);
                    }
                    break;
            }
            const bool sign = o.op == IrOp::Cr0 || o.op == IrOp::Cmp || o.op == IrOp::Cmpi;
            cr_from_flags(sign ? Cond::LT : Cond::LO, so, d);
            break;
        }
        case IrOp::CrLogic: {
            const Reg fa = r(kItemCr + (o.a >> 2));
            const Reg fb = r(kItemCr + (o.b >> 2));
            const Reg fd = rw(kItemCr + (o.d >> 2));
            a.ubfx(W16, fa, 3 - (o.a & 3), 1);
            a.ubfx(W17, fb, 3 - (o.b & 3), 1);
            a.orr(W16, W17, W16, Shift::LSL, 1);
            a.mov(W17, u64{o.imm & 15});
            a.lsr(W16, W17, W16);
            a.bfi(fd, W16, 3 - (o.d & 3), 1);
            break;
        }
        case IrOp::Mcrf: {
            if (o.a == o.d) break;
            const Reg s = r(kItemCr + o.a);
            a.mov(w(kItemCr + o.d), s);
            break;
        }
        case IrOp::Mfcr: {
            for (u32 f = 0; f < 8; ++f) {
                const Reg c = r(kItemCr + f);
                if (f == 0) {
                    a.lsl(W16, c, 28);
                } else {
                    a.orr(W16, W16, c, Shift::LSL, 28 - 4 * f);
                }
            }
            a.mov(w(o.d), W16);
            break;
        }
        case IrOp::Mtcrf: {
            const Reg v = r(o.a);
            for (u32 f = 0; f < 8; ++f) {
                if ((o.imm >> (28 - 4 * f)) & 0xf) a.ubfx(w(kItemCr + f), v, 28 - 4 * f, 4);
            }
            break;
        }
        case IrOp::Mfxer: {
            const Reg so = r(kItemSo), ov = r(kItemOv), ca = r(kItemCa);
            const Reg d  = w(o.d);
            a.ldrb(W16, mem(kSt, OFF(xer_bc)));
            a.and_(W16, W16, 0x7f);
            a.orr(W16, W16, so, Shift::LSL, 31);
            a.orr(W16, W16, ov, Shift::LSL, 30);
            a.orr(W16, W16, ca, Shift::LSL, 29);
            a.mov(d, W16);
            break;
        }
        case IrOp::Mtxer: {
            const Reg v = r(o.a);
            a.ubfx(w(kItemSo), v, 31, 1);
            a.ubfx(w(kItemOv), v, 30, 1);
            a.ubfx(w(kItemCa), v, 29, 1);
            a.and_(W16, v, 0x7f);
            a.strb(W16, mem(kSt, OFF(xer_bc)));
            break;
        }
        case IrOp::Mftb: {
            const Reg d = w(o.d);
            a.add(X16, kCyc, m_b.op_insn[k]);
            a.ldr(W17, mem(kSt, OFF(timer_ratio)));
            a.udiv(X16, X16, X17);
            a.ldr(X17, mem(kSt, OFF(tb_offset)));
            a.add(X16, X16, X17);
            if (o.a) a.lsr(X16, X16, 32);
            a.mov(d, W16);
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
            const Reg d = w(kItemCr + 1);
            a.ldr(W16, mem(kSt, OFF(fpscr)));
            a.lsr(d, W16, 28);
            break;
        }

        // ---- helper ----
        case IrOp::Helper: {
            const u32 idx = u32{o.a} | (u32{o.b} << 8);
            writeback_clean();
            fprf_materialize();
            set_cur_insn(idx);
            a.mov(W16, u64{m_b.pc});
            a.str(W16, mem(kSt, OFF(block_pc)));
            a.mov(X0, kSt);
            a.mov(W1, u64{o.imm});
            a.mov(W2, u64{idx});
            a.call(&ppc_rt_helper);
            drop_all();
            const Label hexit = a.new_label();
            a.cbnz(W0, hexit);
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
    a.adr(X1, body);
    a.jump(m_be.m_enter);
    m_chain_label = a.new_label();
    a.bind(m_chain_label);
    a.cmp(kBud, m_b.num_insns);
    a.b(Cond::LO, nb);
    a.str(kCyc, mem(kSt, OFF(block_cycles)));
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
    a.mov(W16, u64{m_b.pc});
    a.str(W16, mem(kSt, OFF(pc)));
    a.jump(m_be.m_ret);

    for (usize q = 0; q < m_ool.size(); ++q) {
        const auto f = m_ool[q];
        f();
    }
    return a.ok();
}

// ---- backend ---------------------------------------------------------------------

bool JitArm64::init()
{
    if (!m_code.init()) return false;
    m_fast = std::make_unique<FastEntry[]>(1u << kFastBits);
    clear_fast();

    CodeBuffer::WriteScope scope(m_code);
    Emitter     a(m_code.tail());
    const Frame frame(kCalleeSavedGprs, 0, 32);
    m_loc = frame.locals_offset();

    const usize enter = a.size();
    frame.prologue(a);
    a.mov(kSt, X0);
    a.ldr(kBud, mem(kSt, OFF(budget)));
    a.ldr(kCyc, mem(kSt, OFF(block_cycles)));
    a.str(kCyc, mem(SP, m_loc + 8));
    a.ldr(kRtab, mem(kSt, OFF(read_table)));
    a.ldr(kWtab, mem(kSt, OFF(write_table)));
    a.mov(X16, static_cast<u64>(reinterpret_cast<uintptr_t>(m_fast.get())));
    a.str(X16, mem(SP, m_loc + 16));
    a.mrs(X16, SysReg::FPCR);
    a.str(X16, mem(SP, m_loc));
    a.ldr(W17, mem(kSt, OFF(fpscr)));
    a.ands(W17, W17, 3);
    const Label rn = a.new_label();
    a.b(Cond::NE, rn);
    a.br(X1);
    a.bind(rn);  // PowerPC RN 1 (zero), 2 (+inf), 3 (-inf) -> FPCR RMode 3, 1, 2
    a.cmp(W17, 1);
    a.sub(W17, W17, 1);
    a.mov(W30, 3);
    a.csel(W17, W30, W17, Cond::EQ);
    a.bfi(X16, X17, 22, 2);
    a.msr(SysReg::FPCR, X16);
    a.br(X1);

    const usize ret = a.size();
    a.ldr(X16, mem(SP, m_loc + 8));
    a.sub(X0, kCyc, X16);
    a.ldr(X16, mem(SP, m_loc));
    a.mrs(X17, SysReg::FPCR);
    a.cmp(X16, X17);
    const Label same = a.new_label();
    a.b(Cond::EQ, same);
    a.msr(SysReg::FPCR, X16);
    a.bind(same);
    frame.epilogue(a);

    // FPRF of D31 into FPSCR; X16, X17 and flags only.
    const usize fprf = a.size();
    {
        const Label zero = a.new_label(), den = a.new_label(), inf = a.new_label(), nan = a.new_label(),
                    st = a.new_label();
        a.fmov(X16, kFprfSrc);
        a.lsl(X17, X16, 1);
        a.cbz(X17, zero);
        a.lsr(X17, X17, 53);
        a.cbz(X17, den);
        a.cmp(X17, 0x7ff);
        a.b(Cond::EQ, inf);
        a.mov(W17, 0x04);
        a.tbz(X16, 63, st);
        a.mov(W17, 0x08);
        a.b(st);
        a.bind(zero);
        a.mov(W17, 0x02);
        a.tbz(X16, 63, st);
        a.mov(W17, 0x12);
        a.b(st);
        a.bind(den);
        a.mov(W17, 0x14);
        a.tbz(X16, 63, st);
        a.mov(W17, 0x18);
        a.b(st);
        a.bind(inf);
        a.lsl(X17, X16, 12);
        a.cbnz(X17, nan);
        a.mov(W17, 0x05);
        a.tbz(X16, 63, st);
        a.mov(W17, 0x09);
        a.b(st);
        a.bind(nan);
        a.mov(W17, 0x11);
        a.bind(st);
        a.ldr(W16, mem(kSt, OFF(fpscr)));
        a.bfi(W16, W17, 12, 5);
        a.str(W16, mem(kSt, OFF(fpscr)));
        a.ret();
    }

    if (!a.ok()) return false;
    u8* const base = m_code.commit(a.size());
    if (base == nullptr) return false;
    m_enter = base + enter;
    m_ret   = base + ret;
    m_fprf  = base + fprf;
    m_code.set_persistent_mark();
    return true;
}

void JitArm64::clear_fast()
{
    for (u32 n = 0; n < (1u << kFastBits); ++n) m_fast[n] = FastEntry{~0ull, nullptr};
}

void JitArm64::patch(const BlockInfo::Exit& e, const void* to)
{
    m_code.patch32(e.site, encode_b(e.site, to));
}

void JitArm64::compile(IrBlock& b)
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

void JitArm64::invalidated(IrBlock& b)
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

void JitArm64::release(IrBlock& b)
{
    auto* info = static_cast<BlockInfo*>(b.native_handle);
    if (info != nullptr && info->live) invalidated(b);
    delete info;
    b.native_handle = nullptr;
    b.native        = nullptr;
}

void JitArm64::reset_code()
{
    m_live.clear();
    m_links.clear();
    clear_fast();
    m_code.clear();
    m_full = false;
}

}  // namespace

std::unique_ptr<BlockBackend> create_jit_arm64()
{
    auto jit = std::make_unique<JitArm64>();
    if (!jit->init()) {
        SM2_WARN("ppc jit: no executable memory; using the interpreter");
        return nullptr;
    }
    return jit;
}

}  // namespace sm2::cpu::ppc

#else

namespace sm2::cpu::ppc {

std::unique_ptr<BlockBackend> create_jit_arm64()
{
    return nullptr;
}

}  // namespace sm2::cpu::ppc

#endif
