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
#include "cpu/ppc/ppc_state.h"

#include <vector>

// PowerPC block IR: produced by the decoder, run by the interpreter, translated
// by JIT backends.
//
// Blocks: straight-line code from IrBlock::pc, ending at a branch, at an
// instruction that changes mode or must be seen by the run loop (mtmsr, rfi,
// sc, tw/twi, isync, icbi, mtfsf*/mtfsb*, invalid), after kMaxInsns, or before
// an unmapped page. The op list ends with exactly one terminator (B, Bc, Bcr,
// Exit). insn_first_op[i] is the first op of guest instruction i (at pc + 4*i;
// insn_first_op[num_insns] == ops.size()); op_insn[k] is the owning
// instruction of op k. One cycle per instruction.
//
// Native code (NativeBlockFn) runs the ops, stores the next pc in st->pc and
// returns the number of instructions completed. It is entered only when the
// whole block fits st->budget. It may go on into following blocks of the same
// key while each fits what is left of the budget, storing st->block_cycles as
// each starts (and st->block_pc before a Helper), and returns the total; it
// must not continue after a block that ran a Helper (DEC, MSR or FPSCR may
// have changed). Exits:
//   - terminator: return num_insns;
//   - after every ppc_rt_* call, if st->exit_flags & kExitBreak: finish the
//     current instruction's remaining ops, st->pc = pc + 4*(i+1), return i+1
//     (do not clear the flag);
//   - Helper returned kHelperExit: pc already set, return i+1.
// Blocks may stop at any instruction boundary, so no op may be dropped because
// a later op overwrites its result. Store the instruction index in
// st->cur_insn before ppc_rt_read*/write*. Temporaries T0..T3 are dead at
// exits; all other state must be in PpcState before any runtime call.
//
// Operands: 8-byte ops (op d a b imm). Integer d/a/b are slots (s[n] =
// st->slot[n]; kSlotZero reads 0); FP d/a/b are FPR numbers with a third FPR
// "c" in imm. CR0/CR1/XER updates are always explicit ops.
//
// Runtime calls (ppc.h): ppc_rt_read8/16/32/64, ppc_rt_write8/16/32/64 (slow
// path, big-endian values) and ppc_rt_helper(st, insn, index). Fast path:
// host = st->read_table[ea >> 12] (write_table for stores); if non-null and
// the access fits in the page, unaligned host access + byte swap; otherwise
// the runtime call. Code pages are absent from write_table.
//
// FP: doubles in fpr[]. Single ops round the double result to float. FMA ops
// are fused. All FP ops round in the block's FPSCR[RN] (key & kKeyRnMask),
// which only block-ending Helpers change. Arithmetic does not update FPSCR
// sticky bits or FR/FI; FPRF only via Fprf. No FP exceptions.
namespace sm2::cpu::ppc {

enum class IrOp : u8 {
    // ---- integer moves and arithmetic ---------------------------------------
    Mov,     ///< s[d] = s[a]
    Li,      ///< s[d] = imm
    Add,     ///< s[d] = s[a] + s[b]
    Addi,    ///< s[d] = s[a] + imm
    Sub,     ///< s[d] = s[a] - s[b]
    AddC,    ///< s[d] = s[a] + s[b];        CA = unsigned carry out
    AddCi,   ///< s[d] = s[a] + imm;         CA = unsigned carry out
    AddE,    ///< s[d] = s[a] + s[b] + CA;   CA = carry out
    SubfC,   ///< s[d] = s[b] - s[a];        CA = (s[b] >= s[a]) (no borrow)
    SubfCi,  ///< s[d] = imm - s[a];         CA = (imm >= s[a])
    SubfE,   ///< s[d] = ~s[a] + s[b] + CA;  CA = carry out
    Neg,     ///< s[d] = -s[a]
    Mullw,   ///< s[d] = low 32 bits of s[a] * s[b]
    Mulli,   ///< s[d] = s[a] * (s32)imm
    Mulhw,   ///< s[d] = high 32 bits of (s64)(s32)s[a] * (s32)s[b]
    Mulhwu,  ///< s[d] = high 32 bits of (u64)s[a] * s[b]
    Divw,    ///< s[d] = (s32)s[a] / (s32)s[b]. If s[b] == 0: 0 when s[a] >= 0
             ///<        (as signed), else 0xffffffff. 0x80000000 / -1 = 0xffffffff.
    Divwu,   ///< s[d] = s[a] / s[b]; 0 if s[b] == 0.

    // ---- logical ----------------------------------------------------------
    And,     ///< s[d] = s[a] & s[b]
    Andi,    ///< s[d] = s[a] & imm
    Andc,    ///< s[d] = s[a] & ~s[b]
    Or,      ///< s[d] = s[a] | s[b]
    Ori,     ///< s[d] = s[a] | imm
    Orc,     ///< s[d] = s[a] | ~s[b]
    Xor,     ///< s[d] = s[a] ^ s[b]
    Xori,    ///< s[d] = s[a] ^ imm
    Nand,    ///< s[d] = ~(s[a] & s[b])
    Nor,     ///< s[d] = ~(s[a] | s[b])
    Eqv,     ///< s[d] = ~(s[a] ^ s[b])
    Extsb,   ///< s[d] = (s32)(s8)s[a]
    Extsh,   ///< s[d] = (s32)(s16)s[a]
    Cntlzw,  ///< s[d] = count of leading zeros of s[a] (32 for 0)

    // ---- shifts and rotates -----------------------------------------------
    Slw,     ///< n = s[b] & 63; s[d] = n > 31 ? 0 : s[a] << n
    Srw,     ///< n = s[b] & 63; s[d] = n > 31 ? 0 : s[a] >> n
    Sraw,    ///< n = s[b] & 63; s[d] = (s32)s[a] >> min(n, 31);
             ///<   CA = s[a] negative and any 1 bits shifted out (all of them if n > 31)
    Srawi,   ///< as Sraw with the literal shift n = b (0..31), not a slot
    Rlwinm,  ///< s[d] = rotl32(s[a], b) & imm          (b is the literal shift 0..31)
    Rlwnm,   ///< s[d] = rotl32(s[a], s[b] & 31) & imm
    Rlwimi,  ///< s[d] = (rotl32(s[a], b) & imm) | (s[d] & ~imm)   (b literal; reads d)

    // ---- condition register -------------------------------------------------
    Cr0,     ///< cr[0] = (s32)s[a] <0: LT, >0: GT, ==0: EQ; plus SO = xer_so
    Cmp,     ///< cr[d] = signed compare of s[a] with s[b], plus SO   (d = field 0..7)
    Cmpi,    ///< cr[d] = signed compare of s[a] with (s32)imm, plus SO
    Cmpl,    ///< cr[d] = unsigned compare of s[a] with s[b], plus SO
    Cmpli,   ///< cr[d] = unsigned compare of s[a] with imm, plus SO
    CrLogic, ///< CR bit d = (imm >> ((bit a << 1) | bit b)) & 1. d, a, b are CR bit
             ///<   numbers 0..31 (bit n is field n/4, mask 8 >> (n%4)). imm is the 4-bit
             ///<   truth table: crand 8, crandc 4, creqv 9, crnand 7, crnor 1, cror 14,
             ///<   crorc 13, crxor 6.
    Mcrf,    ///< cr[d] = cr[a]   (fields)
    Mfcr,    ///< s[d] = the 32-bit CR (cr[0] in bits 28..31)
    Mtcrf,   ///< CR = (CR & ~imm) | (s[a] & imm); imm is the FXM field mask expanded to
             ///<   nibbles (FXM bit 0x80 -> 0xf0000000, ...)
    Mfxer,   ///< s[d] = SO<<31 | OV<<30 | CA<<29 | bc
    Mtxer,   ///< SO, OV, CA, bc = from s[a]
    Mftb,    ///< s[d] = timebase low (a = 0) or high (a = 1) word at this instruction.
             ///<   imm = the raw instruction: a backend may just call ppc_rt_helper.

    // ---- loads and stores ---------------------------------------------------
    // EA = s[a] + s[b] + imm (32-bit wrap). Update forms are decoded as the
    // access followed by an explicit Add/Addi into rA. Integer stores store s[d].
    Lbz,     ///< s[d] = zero-extended byte
    Lhz,     ///< s[d] = zero-extended big-endian halfword
    Lha,     ///< s[d] = sign-extended big-endian halfword
    Lwz,     ///< s[d] = big-endian word
    Lhbrx,   ///< s[d] = zero-extended little-endian halfword
    Lwbrx,   ///< s[d] = little-endian word
    Stb,     ///< byte s[d]
    Sth,     ///< big-endian halfword s[d]
    Stw,     ///< big-endian word s[d]
    Sthbrx,  ///< little-endian halfword s[d]
    Stwbrx,  ///< little-endian word s[d]
    Lfs,     ///< f[d] = single at EA converted to double, bit-exactly (ppc_fpu.h
             ///<   single_to_double: SNaNs stay signalling)
    Lfd,     ///< fpr[d] = big-endian doubleword at EA
    Stfs,    ///< single at EA = double_to_single(fpr[d]) (truncating conversion,
             ///<   PEM D.7; ppc_fpu.h)
    Stfd,    ///< big-endian doubleword at EA = fpr[d]
    Stfiwx,  ///< big-endian word at EA = low 32 bits of fpr[d]
    Lmw,     ///< for r = d..31: s[r] = word at EA + 4*(r-d)
    Stmw,    ///< for r = d..31: word at EA + 4*(r-d) = s[r]

    // ---- floating point (d, a, b FPRs; c = imm) -----------------------------
    Fmr,     ///< fpr[d] = fpr[a]                     (bit copy)
    Fneg,    ///< fpr[d] = fpr[a] ^ sign bit
    Fabs,    ///< fpr[d] = fpr[a] & ~sign bit
    Fnabs,   ///< fpr[d] = fpr[a] | sign bit
    Fadd,    ///< f[d] = f[a] + f[b]
    Fsub,    ///< f[d] = f[a] - f[b]
    Fmul,    ///< f[d] = f[a] * f[b]       (the decoder puts the guest's frC in b)
    Fdiv,    ///< f[d] = f[a] / f[b]
    Fmadd,   ///< f[d] = fma(f[a], f[c], f[b])
    Fmsub,   ///< f[d] = fma(f[a], f[c], -f[b])
    Fnmadd,  ///< f[d] = -fma(f[a], f[c], f[b]),  NaN results not negated
    Fnmsub,  ///< f[d] = -fma(f[a], f[c], -f[b]), NaN results not negated
    Fadds,   ///< the eight ops above with the result rounded to single
    Fsubs,
    Fmuls,
    Fdivs,
    Fmadds,
    Fmsubs,
    Fnmadds,
    Fnmsubs,
    Frsp,    ///< f[d] = (double)(float)f[a]
    Fres,    ///< f[d] = (double)(float)(1.0 / f[a])   (exact reciprocal, single)
    Frsqrte, ///< f[d] = 1.0 / sqrt(f[a])              (exact, double)
    Fsqrt,   ///< f[d] = sqrt(f[a])
    Fsqrts,  ///< f[d] = (double)(float)sqrt(f[a])
    Fsel,    ///< f[d] = (f[a] >= 0.0, false for NaN) ? f[c] : f[b]   (-0.0 >= 0)
    Fctiw,   ///< fpr[d] = f[a] rounded in the block's RN mode to s32, saturating;
             ///<   NaN -> 0x80000000. Upper 32 bits: 0xffffffff if the result
             ///<   saturated negative, else 0.
    Fctiwz,  ///< as Fctiw, rounding toward zero
    Fcmpu,   ///< cr[d] and FPSCR[FPCC] = compare f[a] with f[b]: LT 8, GT 4, EQ 2,
             ///<   unordered 1. Unordered with an SNaN operand sets VXSNAN and FX.
             ///<   Used for fcmpo too.
    Fprf,    ///< FPSCR[FPRF] = class of fpr[d] (ppc_fpu.h fprf_of)
    Cr1,     ///< cr[1] = FPSCR >> 28   (FX FEX VX OX)

    // ---- escape hatch ------------------------------------------------------
    Helper,  ///< r = ppc_rt_helper(st, imm = raw instruction word, insn index = a | b << 8).
             ///<   The helper interprets the whole guest instruction. r is kHelperExit
             ///<   or kHelperContinue; test exit_flags after.

    // ---- terminators (exactly one, last) ------------------------------------
    B,       ///< st->pc = imm
    Bc,      ///< conditional branch, d = BO, a = BI, imm = target; fallthrough is
             ///<   IrBlock::end_pc(). Semantics (PEM bc):
             ///<     if !(BO & 4): CTR -= 1
             ///<     ctr_ok  = (BO & 4)  || ((CTR != 0) != ((BO & 2) != 0))
             ///<     cond_ok = (BO & 16) || (CR bit BI == ((BO & 8) != 0))
             ///<     st->pc  = ctr_ok && cond_ok ? imm : end_pc
             ///<   The decoder turns "branch always" encodings into B.
    Bcr,     ///< as Bc with target s[b] & ~3 (LR, CTR or a temporary holding the
             ///<   old LR for bclrl). For bcctr the decoder forces BO & 4.
    Exit,    ///< st->pc = imm (fall out of the block: length limit or sync point)

    Count
};

/// Helper results.
inline constexpr u32 kHelperContinue = 0;
inline constexpr u32 kHelperExit     = 1;

/// Block key bits.
inline constexpr u32 kKeyRnMask = 0x3;  ///< FPSCR[RN] the block was translated for.

struct IrInst {
    IrOp op;
    u8   d;
    u8   a;
    u8   b;
    u32  imm;
};
static_assert(sizeof(IrInst) == 8);

using NativeBlockFn = u32 (*)(PpcState* st);

struct IrBlock {
    static constexpr u32 kMaxInsns = 64;

    u32 pc  = 0;          ///< Guest address of the first instruction.
    u32 key = 0;          ///< Mode bits (kKey*).
    u32 num_insns = 0;

    std::vector<IrInst> ops;
    std::vector<u16>    insn_first_op;  ///< num_insns + 1 entries.
    std::vector<u16>    op_insn;        ///< One per op.
    std::vector<u32>    raw;            ///< The guest instruction words.

    /// Set by a backend; the run loop calls it instead of interpreting.
    NativeBlockFn native = nullptr;
    void*         native_handle = nullptr;  ///< Backend-private.

    bool valid = true;    ///< Cleared on invalidation (the block may still be running).

    [[nodiscard]] u32 end_pc() const { return pc + 4 * num_insns; }
};

/// Interface for a JIT backend. The core owns blocks; the backend translates
/// them and is told when one dies, so it can free its code.
class BlockBackend {
public:
    virtual ~BlockBackend() = default;

    /// Translate `block` and store the entry in block.native (leave it null to
    /// have the block interpreted). Called once per block, after decoding.
    virtual void compile(IrBlock& block) = 0;

    /// `block` is being destroyed; release whatever compile() made.
    virtual void release(IrBlock& block) = 0;

    /// `block` was invalidated (it may still be running): stop entering it.
    virtual void invalidated(IrBlock& block) { (void)block; }

    /// compile() ran out of code space: the core kills every block, calls
    /// reset_code() and compiles again.
    [[nodiscard]] virtual bool full() const { return false; }
    virtual void reset_code() {}
};

/// Decode the block at `pc`. `fetch(addr, &word)` returns false if the address
/// cannot be fetched (unmapped). Returns false if not even the first
/// instruction could be fetched.
template <class Fetch>
bool decode_block(IrBlock& out, u32 pc, u32 key, Fetch&& fetch);

/// Decode one instruction into `out` (without the terminator logic of
/// decode_block). Returns true if the instruction ends a block.
bool decode_insn(std::vector<IrInst>& out, u32 insn, u32 pc, u32 index, u32 key,
                 bool& is_branch);

/// Short mnemonic of an IR op, for dumps.
[[nodiscard]] const char* ir_op_name(IrOp op);

template <class Fetch>
bool decode_block(IrBlock& out, u32 pc, u32 key, Fetch&& fetch)
{
    out.pc  = pc;
    out.key = key;
    out.num_insns = 0;
    out.ops.clear();
    out.insn_first_op.clear();
    out.op_insn.clear();
    out.raw.clear();

    u32  addr = pc;
    bool ended = false;
    while (!ended && out.num_insns < IrBlock::kMaxInsns) {
        u32 word = 0;
        if (!fetch(addr, &word)) {
            break;
        }
        const usize first = out.ops.size();
        bool is_branch = false;
        ended = decode_insn(out.ops, word, addr, out.num_insns, key, is_branch);
        out.insn_first_op.push_back(static_cast<u16>(first));
        for (usize i = first; i < out.ops.size(); ++i) {
            out.op_insn.push_back(static_cast<u16>(out.num_insns));
        }
        out.raw.push_back(word);
        ++out.num_insns;
        addr += 4;
        (void)is_branch;
    }
    if (out.num_insns == 0) {
        return false;
    }
    // Every block ends in a terminator; give the non-branch endings an Exit,
    // owned by the last instruction.
    if (out.ops.empty() || (out.ops.back().op != IrOp::B && out.ops.back().op != IrOp::Bc &&
                            out.ops.back().op != IrOp::Bcr)) {
        out.ops.push_back(IrInst{IrOp::Exit, 0, 0, 0, addr});
        out.op_insn.push_back(static_cast<u16>(out.num_insns - 1));
    }
    out.insn_first_op.push_back(static_cast<u16>(out.ops.size()));
    return true;
}

}  // namespace sm2::cpu::ppc
