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

// IR interpreter, the reference engine for ppc_ir.h. Blocks with a non-nearest
// RN set the host rounding mode around the whole block.

#include "cpu/ppc/ppc.h"
#include "cpu/ppc/ppc_fpu.h"
#include "cpu/ppc/ppc_internal.h"

#include <algorithm>
#include <bit>
#include <cfenv>
#include <cmath>

namespace sm2::cpu::ppc {

namespace {

class RoundingScope {
public:
    explicit RoundingScope(u32 rn)
    {
        if (rn != 0) {
            static constexpr int kModes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
            m_active = true;
            std::fesetround(kModes[rn & 3]);
        }
    }
    ~RoundingScope()
    {
        if (m_active) {
            std::fesetround(FE_TONEAREST);
        }
    }
    RoundingScope(const RoundingScope&)            = delete;
    RoundingScope& operator=(const RoundingScope&) = delete;

private:
    bool m_active = false;
};

[[nodiscard]] inline u8 cmp_signed(s32 a, s32 b, u8 so)
{
    return static_cast<u8>((a < b ? kCrLT : a > b ? kCrGT : kCrEQ) | so);
}

[[nodiscard]] inline u8 cmp_unsigned(u32 a, u32 b, u8 so)
{
    return static_cast<u8>((a < b ? kCrLT : a > b ? kCrGT : kCrEQ) | so);
}

[[nodiscard]] inline u32 cr_bit(const PpcState& st, u32 n)
{
    return (st.cr[n >> 2] >> (3 - (n & 3))) & 1u;
}

[[nodiscard]] inline bool branch_taken(PpcState& st, u32 bo, u32 bi)
{
    u32& ctr = st.slot[kSlotCTR];
    if ((bo & 4) == 0) {
        --ctr;
    }
    const bool ctr_ok  = (bo & 4) != 0 || ((ctr != 0) != ((bo & 2) != 0));
    const bool cond_ok = (bo & 16) != 0 || (cr_bit(st, bi) == ((bo >> 3) & 1u));
    return ctr_ok && cond_ok;
}

}  // namespace

u32 Ppc::interpret(IrBlock& b, u32 max_insns)
{
    PpcState& st = m_st;
    u32* const s = st.slot;
    const IrInst* const ops = b.ops.data();
    u8* const* const rtab = st.read_table;
    u8* const* const wtab = st.write_table;

    u32 stop_insn = std::min(max_insns, b.num_insns);
    u32 end = max_insns >= b.num_insns ? static_cast<u32>(b.ops.size()) : b.insn_first_op[max_insns];

    const RoundingScope rounding(b.key & kKeyRnMask);

    auto f  = [&st](u32 n) { return std::bit_cast<double>(st.fpr[n]); };
    auto sf = [&st](u32 n, double v) { st.fpr[n] = std::bit_cast<u64>(v); };

    auto break_after = [&](u32 i) {
        stop_insn = b.op_insn[i] + 1u;
        end = b.insn_first_op[stop_insn];
    };

#define PPC_LOAD(T, slow, ea_expr, dst)                                               \
    do {                                                                              \
        const u32 ea_  = (ea_expr);                                                   \
        u8* const pg_  = rtab[ea_ >> PageMap::kPageBits];                             \
        const u32 off_ = ea_ & PageMap::kPageMask;                                    \
        T v_;                                                                         \
        if (pg_ != nullptr && off_ <= PageMap::kPageSize - sizeof(T)) [[likely]] {    \
            std::memcpy(&v_, pg_ + off_, sizeof(T));                                  \
            v_ = byteswap_any(v_);                                                    \
        } else {                                                                      \
            st.cur_insn = b.op_insn[i];                                               \
            v_ = slow(ea_);                                                           \
            if (st.exit_flags != 0) {                                                 \
                break_after(i);                                                       \
            }                                                                         \
        }                                                                             \
        dst = v_;                                                                     \
    } while (0)

#define PPC_STORE(T, slow, ea_expr, val)                                              \
    do {                                                                              \
        const u32 ea_  = (ea_expr);                                                   \
        const T   v_   = static_cast<T>(val);                                         \
        u8* const pg_  = wtab[ea_ >> PageMap::kPageBits];                             \
        const u32 off_ = ea_ & PageMap::kPageMask;                                    \
        if (pg_ != nullptr && off_ <= PageMap::kPageSize - sizeof(T)) [[likely]] {    \
            const T sw_ = byteswap_any(v_);                                           \
            std::memcpy(pg_ + off_, &sw_, sizeof(T));                                 \
        } else {                                                                      \
            st.cur_insn = b.op_insn[i];                                               \
            slow(ea_, v_);                                                            \
            if (st.exit_flags != 0) {                                                 \
                break_after(i);                                                       \
            }                                                                         \
        }                                                                             \
    } while (0)

    for (u32 i = 0; i < end; ++i) {
        const IrInst o = ops[i];
        switch (o.op) {
            // ---- integer --------------------------------------------------
            case IrOp::Mov:  s[o.d] = s[o.a]; break;
            case IrOp::Li:   s[o.d] = o.imm; break;
            case IrOp::Add:  s[o.d] = s[o.a] + s[o.b]; break;
            case IrOp::Addi: s[o.d] = s[o.a] + o.imm; break;
            case IrOp::Sub:  s[o.d] = s[o.a] - s[o.b]; break;
            case IrOp::AddC: {
                const u32 a = s[o.a], r = a + s[o.b];
                st.xer_ca = r < a;
                s[o.d] = r;
                break;
            }
            case IrOp::AddCi: {
                const u32 a = s[o.a], r = a + o.imm;
                st.xer_ca = r < a;
                s[o.d] = r;
                break;
            }
            case IrOp::AddE: {
                const u64 r = u64{s[o.a]} + s[o.b] + st.xer_ca;
                st.xer_ca = static_cast<u8>(r >> 32);
                s[o.d] = static_cast<u32>(r);
                break;
            }
            case IrOp::SubfC: {
                const u32 a = s[o.a], bb = s[o.b];
                st.xer_ca = bb >= a;
                s[o.d] = bb - a;
                break;
            }
            case IrOp::SubfCi: {
                const u32 a = s[o.a];
                st.xer_ca = o.imm >= a;
                s[o.d] = o.imm - a;
                break;
            }
            case IrOp::SubfE: {
                const u64 r = u64{~s[o.a]} + s[o.b] + st.xer_ca;
                st.xer_ca = static_cast<u8>(r >> 32);
                s[o.d] = static_cast<u32>(r);
                break;
            }
            case IrOp::Neg:   s[o.d] = 0u - s[o.a]; break;
            case IrOp::Mullw: s[o.d] = s[o.a] * s[o.b]; break;
            case IrOp::Mulli: s[o.d] = s[o.a] * o.imm; break;
            case IrOp::Mulhw:
                s[o.d] = static_cast<u32>(static_cast<u64>(s64{static_cast<s32>(s[o.a])} *
                                                           s64{static_cast<s32>(s[o.b])}) >> 32);
                break;
            case IrOp::Mulhwu: s[o.d] = static_cast<u32>((u64{s[o.a]} * s[o.b]) >> 32); break;
            case IrOp::Divw: {
                const u32 a = s[o.a], bb = s[o.b];
                if (bb == 0) {
                    s[o.d] = a < 0x80000000u ? 0u : 0xffffffffu;
                } else if (bb == 0xffffffffu && a == 0x80000000u) {
                    s[o.d] = 0xffffffffu;
                } else {
                    s[o.d] = static_cast<u32>(static_cast<s32>(a) / static_cast<s32>(bb));
                }
                break;
            }
            case IrOp::Divwu: {
                const u32 bb = s[o.b];
                s[o.d] = bb == 0 ? 0u : s[o.a] / bb;
                break;
            }

            // ---- logical ----------------------------------------------------
            case IrOp::And:    s[o.d] = s[o.a] & s[o.b]; break;
            case IrOp::Andi:   s[o.d] = s[o.a] & o.imm; break;
            case IrOp::Andc:   s[o.d] = s[o.a] & ~s[o.b]; break;
            case IrOp::Or:     s[o.d] = s[o.a] | s[o.b]; break;
            case IrOp::Ori:    s[o.d] = s[o.a] | o.imm; break;
            case IrOp::Orc:    s[o.d] = s[o.a] | ~s[o.b]; break;
            case IrOp::Xor:    s[o.d] = s[o.a] ^ s[o.b]; break;
            case IrOp::Xori:   s[o.d] = s[o.a] ^ o.imm; break;
            case IrOp::Nand:   s[o.d] = ~(s[o.a] & s[o.b]); break;
            case IrOp::Nor:    s[o.d] = ~(s[o.a] | s[o.b]); break;
            case IrOp::Eqv:    s[o.d] = ~(s[o.a] ^ s[o.b]); break;
            case IrOp::Extsb:  s[o.d] = static_cast<u32>(static_cast<s32>(static_cast<s8>(s[o.a]))); break;
            case IrOp::Extsh:  s[o.d] = static_cast<u32>(static_cast<s32>(static_cast<s16>(s[o.a]))); break;
            case IrOp::Cntlzw: s[o.d] = static_cast<u32>(std::countl_zero(s[o.a])); break;

            // ---- shifts and rotates -----------------------------------------
            case IrOp::Slw: {
                const u32 n = s[o.b] & 63;
                s[o.d] = n > 31 ? 0u : s[o.a] << n;
                break;
            }
            case IrOp::Srw: {
                const u32 n = s[o.b] & 63;
                s[o.d] = n > 31 ? 0u : s[o.a] >> n;
                break;
            }
            case IrOp::Sraw:
            case IrOp::Srawi: {
                const u32 n = o.op == IrOp::Sraw ? (s[o.b] & 63) : o.b;
                const u32 a = s[o.a];
                const s32 v = static_cast<s32>(a);
                if (n > 31) {
                    st.xer_ca = v < 0;
                    s[o.d] = static_cast<u32>(v >> 31);
                } else {
                    st.xer_ca = v < 0 && (a & ((1u << n) - 1u)) != 0;
                    s[o.d] = static_cast<u32>(v >> n);
                }
                break;
            }
            case IrOp::Rlwinm: s[o.d] = std::rotl(s[o.a], o.b) & o.imm; break;
            case IrOp::Rlwnm:  s[o.d] = std::rotl(s[o.a], static_cast<int>(s[o.b] & 31)) & o.imm; break;
            case IrOp::Rlwimi: s[o.d] = (std::rotl(s[o.a], o.b) & o.imm) | (s[o.d] & ~o.imm); break;

            // ---- condition register -----------------------------------------
            case IrOp::Cr0:   st.cr[0] = cmp_signed(static_cast<s32>(s[o.a]), 0, st.xer_so); break;
            case IrOp::Cmp:   st.cr[o.d] = cmp_signed(static_cast<s32>(s[o.a]), static_cast<s32>(s[o.b]), st.xer_so); break;
            case IrOp::Cmpi:  st.cr[o.d] = cmp_signed(static_cast<s32>(s[o.a]), static_cast<s32>(o.imm), st.xer_so); break;
            case IrOp::Cmpl:  st.cr[o.d] = cmp_unsigned(s[o.a], s[o.b], st.xer_so); break;
            case IrOp::Cmpli: st.cr[o.d] = cmp_unsigned(s[o.a], o.imm, st.xer_so); break;
            case IrOp::CrLogic: {
                const u32 r = (o.imm >> ((cr_bit(st, o.a) << 1) | cr_bit(st, o.b))) & 1u;
                const u8 m = static_cast<u8>(8u >> (o.d & 3));
                u8& field = st.cr[o.d >> 2];
                field = static_cast<u8>(r ? (field | m) : (field & ~m));
                break;
            }
            case IrOp::Mcrf:  st.cr[o.d] = st.cr[o.a]; break;
            case IrOp::Mfcr:  s[o.d] = st.get_cr(); break;
            case IrOp::Mtcrf: {
                const u32 v = s[o.a];
                for (unsigned k = 0; k < 8; ++k) {
                    const unsigned sh = 28 - 4 * k;
                    if ((o.imm >> sh) & 0xf) {
                        st.cr[k] = static_cast<u8>((v >> sh) & 0xf);
                    }
                }
                break;
            }
            case IrOp::Mfxer: s[o.d] = st.get_xer(); break;
            case IrOp::Mtxer: st.set_xer(s[o.a]); break;
            case IrOp::Mftb: {
                const u64 tb = read_tb(m_st.block_cycles + b.op_insn[i]);
                s[o.d] = static_cast<u32>(o.a ? tb >> 32 : tb);
                break;
            }

            // ---- loads and stores -------------------------------------------
            case IrOp::Lbz: PPC_LOAD(u8, slow_read8, s[o.a] + s[o.b] + o.imm, s[o.d]); break;
            case IrOp::Lhz: PPC_LOAD(u16, slow_read16, s[o.a] + s[o.b] + o.imm, s[o.d]); break;
            case IrOp::Lha: {
                u16 v;
                PPC_LOAD(u16, slow_read16, s[o.a] + s[o.b] + o.imm, v);
                s[o.d] = static_cast<u32>(static_cast<s32>(static_cast<s16>(v)));
                break;
            }
            case IrOp::Lwz: PPC_LOAD(u32, slow_read32, s[o.a] + s[o.b] + o.imm, s[o.d]); break;
            case IrOp::Lhbrx: {
                u16 v;
                PPC_LOAD(u16, slow_read16, s[o.a] + s[o.b] + o.imm, v);
                s[o.d] = byteswap_any(v);
                break;
            }
            case IrOp::Lwbrx: {
                u32 v;
                PPC_LOAD(u32, slow_read32, s[o.a] + s[o.b] + o.imm, v);
                s[o.d] = byteswap_any(v);
                break;
            }
            case IrOp::Stb: PPC_STORE(u8, slow_write8, s[o.a] + s[o.b] + o.imm, s[o.d]); break;
            case IrOp::Sth: PPC_STORE(u16, slow_write16, s[o.a] + s[o.b] + o.imm, s[o.d]); break;
            case IrOp::Stw: PPC_STORE(u32, slow_write32, s[o.a] + s[o.b] + o.imm, s[o.d]); break;
            case IrOp::Sthbrx:
                PPC_STORE(u16, slow_write16, s[o.a] + s[o.b] + o.imm, byteswap_any(static_cast<u16>(s[o.d])));
                break;
            case IrOp::Stwbrx:
                PPC_STORE(u32, slow_write32, s[o.a] + s[o.b] + o.imm, byteswap_any(s[o.d]));
                break;
            case IrOp::Lfs: {
                u32 v;
                PPC_LOAD(u32, slow_read32, s[o.a] + s[o.b] + o.imm, v);
                st.fpr[o.d] = fpu::single_to_double(v);
                break;
            }
            case IrOp::Lfd: PPC_LOAD(u64, slow_read64, s[o.a] + s[o.b] + o.imm, st.fpr[o.d]); break;
            case IrOp::Stfs:
                PPC_STORE(u32, slow_write32, s[o.a] + s[o.b] + o.imm, fpu::double_to_single(st.fpr[o.d]));
                break;
            case IrOp::Stfd: PPC_STORE(u64, slow_write64, s[o.a] + s[o.b] + o.imm, st.fpr[o.d]); break;
            case IrOp::Stfiwx:
                PPC_STORE(u32, slow_write32, s[o.a] + s[o.b] + o.imm, static_cast<u32>(st.fpr[o.d]));
                break;
            case IrOp::Lmw: {
                const u32 base = s[o.a] + s[o.b] + o.imm;
                for (u32 r = o.d; r < 32; ++r) {
                    PPC_LOAD(u32, slow_read32, base + 4 * (r - o.d), s[r]);
                }
                break;
            }
            case IrOp::Stmw: {
                const u32 base = s[o.a] + s[o.b] + o.imm;
                for (u32 r = o.d; r < 32; ++r) {
                    PPC_STORE(u32, slow_write32, base + 4 * (r - o.d), s[r]);
                }
                break;
            }

            // ---- floating point ---------------------------------------------
            case IrOp::Fmr:   st.fpr[o.d] = st.fpr[o.a]; break;
            case IrOp::Fneg:  st.fpr[o.d] = st.fpr[o.a] ^ fpu::kSign; break;
            case IrOp::Fabs:  st.fpr[o.d] = st.fpr[o.a] & ~fpu::kSign; break;
            case IrOp::Fnabs: st.fpr[o.d] = st.fpr[o.a] | fpu::kSign; break;
            case IrOp::Fadd:  sf(o.d, fpu::add(f(o.a), f(o.b))); break;
            case IrOp::Fsub:  sf(o.d, f(o.a) - f(o.b)); break;
            case IrOp::Fmul:  sf(o.d, fpu::mul(f(o.a), f(o.b))); break;
            case IrOp::Fdiv:  sf(o.d, f(o.a) / f(o.b)); break;
            case IrOp::Fmadd:  sf(o.d, fpu::fma(f(o.a), f(o.imm), f(o.b))); break;
            case IrOp::Fmsub:  sf(o.d, fpu::fma(f(o.a), f(o.imm), -f(o.b))); break;
            case IrOp::Fnmadd: sf(o.d, fpu::negate_unless_nan(fpu::fma(f(o.a), f(o.imm), f(o.b)))); break;
            case IrOp::Fnmsub: sf(o.d, fpu::negate_unless_nan(fpu::fma(f(o.a), f(o.imm), -f(o.b)))); break;
            case IrOp::Fadds:  sf(o.d, fpu::round_single(fpu::add(f(o.a), f(o.b)))); break;
            case IrOp::Fsubs:  sf(o.d, fpu::round_single(f(o.a) - f(o.b))); break;
            case IrOp::Fmuls:  sf(o.d, fpu::round_single(fpu::mul(f(o.a), f(o.b)))); break;
            case IrOp::Fdivs:  sf(o.d, fpu::round_single(f(o.a) / f(o.b))); break;
            case IrOp::Fmadds: sf(o.d, fpu::round_single(fpu::fma(f(o.a), f(o.imm), f(o.b)))); break;
            case IrOp::Fmsubs: sf(o.d, fpu::round_single(fpu::fma(f(o.a), f(o.imm), -f(o.b)))); break;
            case IrOp::Fnmadds:
                sf(o.d, fpu::round_single(fpu::negate_unless_nan(fpu::fma(f(o.a), f(o.imm), f(o.b)))));
                break;
            case IrOp::Fnmsubs:
                sf(o.d, fpu::round_single(fpu::negate_unless_nan(fpu::fma(f(o.a), f(o.imm), -f(o.b)))));
                break;
            case IrOp::Frsp:    sf(o.d, fpu::round_single(f(o.a))); break;
            case IrOp::Fres:    sf(o.d, static_cast<double>(static_cast<float>(1.0 / f(o.a)))); break;
            case IrOp::Frsqrte: sf(o.d, 1.0 / std::sqrt(f(o.a))); break;
            case IrOp::Fsqrt:   sf(o.d, std::sqrt(f(o.a))); break;
            case IrOp::Fsqrts:  sf(o.d, fpu::round_single(std::sqrt(f(o.a)))); break;
            case IrOp::Fsel:    st.fpr[o.d] = f(o.a) >= 0.0 ? st.fpr[o.imm] : st.fpr[o.b]; break;
            case IrOp::Fctiw:   st.fpr[o.d] = fpu::fctiw_bits(std::nearbyint(f(o.a))); break;
            case IrOp::Fctiwz:  st.fpr[o.d] = fpu::fctiw_bits(std::trunc(f(o.a))); break;
            case IrOp::Fcmpu: {
                const u8 c = fpu::compare(f(o.a), f(o.b));
                st.cr[o.d] = c;
                u32 fs = (st.fpscr & ~kFpscrFPRF) | (u32{c} << 12);
                if (c == 1 && (fpu::is_snan(st.fpr[o.a]) || fpu::is_snan(st.fpr[o.b]))) {
                    fs = fpu::fpscr_summarise(fs | kFpscrVXSNAN | kFpscrFX);
                }
                st.fpscr = fs;
                break;
            }
            case IrOp::Fprf: st.fpscr = (st.fpscr & ~kFpscrFPRF) | fpu::fprf_of(st.fpr[o.d]); break;
            case IrOp::Cr1:  st.cr[1] = static_cast<u8>(st.fpscr >> 28); break;

            // ---- escape hatch -------------------------------------------------
            case IrOp::Helper: {
                const u32 idx = u32{o.a} | (u32{o.b} << 8);
                st.cur_insn = idx;
                if (helper(o.imm, idx) == kHelperExit) {
                    return idx + 1;
                }
                if (st.exit_flags != 0) {
                    break_after(i);
                }
                break;
            }

            // ---- terminators ----------------------------------------------------
            case IrOp::B:
                st.pc = o.imm;
                return b.num_insns;
            case IrOp::Bc:
                st.pc = branch_taken(st, o.d, o.a) ? o.imm : b.end_pc();
                return b.num_insns;
            case IrOp::Bcr: {
                const u32 target = s[o.b] & ~3u;
                st.pc = branch_taken(st, o.d, o.a) ? target : b.end_pc();
                return b.num_insns;
            }
            case IrOp::Exit:
                st.pc = o.imm;
                return b.num_insns;

            case IrOp::Count:
                break;
        }
    }

#undef PPC_LOAD
#undef PPC_STORE

    st.pc = b.pc + 4 * stop_insn;
    return stop_insn;
}

}  // namespace sm2::cpu::ppc
