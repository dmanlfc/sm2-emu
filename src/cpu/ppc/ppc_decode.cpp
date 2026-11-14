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

// Instruction word -> IR. Rare and supervisor instructions become Helper ops.

#include "cpu/ppc/ppc_ir.h"


namespace sm2::cpu::ppc {

namespace {

struct Fields {
    u32 w;
    [[nodiscard]] u32 opcd() const { return w >> 26; }
    [[nodiscard]] u8 rd() const { return static_cast<u8>((w >> 21) & 31); }
    [[nodiscard]] u8 ra() const { return static_cast<u8>((w >> 16) & 31); }
    [[nodiscard]] u8 rb() const { return static_cast<u8>((w >> 11) & 31); }
    [[nodiscard]] u8 rc() const { return static_cast<u8>((w >> 6) & 31); }
    [[nodiscard]] u32 simm() const { return static_cast<u32>(static_cast<s32>(static_cast<s16>(w & 0xffff))); }
    [[nodiscard]] u32 uimm() const { return w & 0xffff; }
    [[nodiscard]] u32 xo10() const { return (w >> 1) & 0x3ff; }
    [[nodiscard]] u32 xo9() const { return (w >> 1) & 0x1ff; }
    [[nodiscard]] u32 xo5() const { return (w >> 1) & 0x1f; }
    [[nodiscard]] bool rc_bit() const { return (w & 1) != 0; }
    [[nodiscard]] bool oe() const { return (w & 0x400) != 0; }
    [[nodiscard]] u8 crfd() const { return static_cast<u8>((w >> 23) & 7); }
    [[nodiscard]] u8 crfs() const { return static_cast<u8>((w >> 18) & 7); }
    [[nodiscard]] u32 spr() const { return ((w >> 16) & 0x1f) | ((w >> 6) & 0x3e0); }
    /// (rA|0): the zero slot when rA is r0.
    [[nodiscard]] u8 ra0() const { return ra() == 0 ? kSlotZero : ra(); }
};

u32 rotate_mask(u32 mb, u32 me)
{
    const u32 a = 0xffffffffu >> mb;
    const u32 b = me >= 31 ? 0u : (0xffffffffu >> (me + 1));
    return mb <= me ? (a & ~b) : (a | ~b);
}

class Emitter {
public:
    Emitter(std::vector<IrInst>& out, u32 index) : m_out(out), m_index(index) {}

    void op(IrOp o, u32 d = 0, u32 a = 0, u32 b = 0, u32 imm = 0)
    {
        m_out.push_back(IrInst{o, static_cast<u8>(d), static_cast<u8>(a), static_cast<u8>(b), imm});
    }
    void helper(u32 insn)
    {
        op(IrOp::Helper, 0, m_index & 0xff, (m_index >> 8) & 0xff, insn);
    }

private:
    std::vector<IrInst>& m_out;
    u32 m_index;
};

void emit_dform(Emitter& e, IrOp op, const Fields& f, bool update)
{
    if (update) {
        e.op(op, f.rd(), f.ra(), kSlotZero, f.simm());
        e.op(IrOp::Addi, f.ra(), f.ra(), 0, f.simm());
    } else {
        e.op(op, f.rd(), f.ra0(), kSlotZero, f.simm());
    }
}

/// Integer update loads may have rD == rB, so form the EA first.
void emit_xform(Emitter& e, IrOp op, const Fields& f, bool update, bool int_dest)
{
    if (!update) {
        e.op(op, f.rd(), f.ra0(), f.rb(), 0);
        return;
    }
    if (int_dest && f.rd() == f.rb()) {
        e.op(IrOp::Add, kSlotT0, f.ra(), f.rb());
        e.op(op, f.rd(), kSlotT0, kSlotZero, 0);
        e.op(IrOp::Mov, f.ra(), kSlotT0);
        return;
    }
    e.op(op, f.rd(), f.ra(), f.rb(), 0);
    e.op(IrOp::Add, f.ra(), f.ra(), f.rb());
}

void emit_cr0(Emitter& e, const Fields& f, u8 dest)
{
    if (f.rc_bit()) {
        e.op(IrOp::Cr0, 0, dest);
    }
}

void emit_fp(Emitter& e, IrOp op, const Fields& f, u8 a, u8 b, u32 c = 0, bool fprf = true)
{
    e.op(op, f.rd(), a, b, c);
    if (fprf) {
        e.op(IrOp::Fprf, f.rd());
    }
    if (f.rc_bit()) {
        e.op(IrOp::Cr1);
    }
}

/// Opcode 19 (branch to register, CR logic, rfi, isync).
bool decode_19(Emitter& e, const Fields& f, u32 pc, bool& is_branch)
{
    switch (f.xo10()) {
        case 0:  // mcrf
            e.op(IrOp::Mcrf, f.crfd(), f.crfs());
            return false;
        case 16: {  // bclr
            const u8 bo = f.rd();
            u8 target = kSlotLR;
            if (f.rc_bit()) {
                e.op(IrOp::Mov, kSlotT0, kSlotLR);
                e.op(IrOp::Li, kSlotLR, 0, 0, pc + 4);
                target = kSlotT0;
            }
            e.op(IrOp::Bcr, bo, f.ra(), target);
            is_branch = true;
            return true;
        }
        case 528: {  // bcctr: a CTR-decrementing form is invalid, treat BO[2] as set
            const u8 bo = static_cast<u8>(f.rd() | 4);
            if (f.rc_bit()) {
                e.op(IrOp::Li, kSlotLR, 0, 0, pc + 4);
            }
            e.op(IrOp::Bcr, bo, f.ra(), kSlotCTR);
            is_branch = true;
            return true;
        }
        case 33:  e.op(IrOp::CrLogic, f.rd(), f.ra(), f.rb(), 1);  return false;  // crnor
        case 129: e.op(IrOp::CrLogic, f.rd(), f.ra(), f.rb(), 4);  return false;  // crandc
        case 193: e.op(IrOp::CrLogic, f.rd(), f.ra(), f.rb(), 6);  return false;  // crxor
        case 225: e.op(IrOp::CrLogic, f.rd(), f.ra(), f.rb(), 7);  return false;  // crnand
        case 257: e.op(IrOp::CrLogic, f.rd(), f.ra(), f.rb(), 8);  return false;  // crand
        case 289: e.op(IrOp::CrLogic, f.rd(), f.ra(), f.rb(), 9);  return false;  // creqv
        case 417: e.op(IrOp::CrLogic, f.rd(), f.ra(), f.rb(), 13); return false;  // crorc
        case 449: e.op(IrOp::CrLogic, f.rd(), f.ra(), f.rb(), 14); return false;  // cror
        case 150:  // isync: a context-synchronising point, nothing else to do
            return true;
        case 50:  // rfi
        default:
            e.helper(f.w);
            return true;
    }
}

/// Opcode-31 helpers after which the block can continue.
bool known_nonterminal_31(u32 xo10)
{
    switch (xo10) {
        case 83:    // mfmsr
        case 210:   // mtsr
        case 242:   // mtsrin
        case 595:   // mfsr
        case 659:   // mfsrin
        case 512:   // mcrxr
        case 20:    // lwarx
        case 150:   // stwcx.
        case 533:   // lswx
        case 597:   // lswi
        case 661:   // stswx
        case 725:   // stswi
        case 1014:  // dcbz
            return true;
        default:
            return false;
    }
}

/// Opcode 31.
bool decode_31(Emitter& e, const Fields& f)
{
    const u8 rd = f.rd(), ra = f.ra(), rb = f.rb();

    // XO-form arithmetic. The overflow-recording forms go to the helper.
    switch (f.xo9()) {
        case 266: case 40: case 10: case 8: case 138: case 136: case 202: case 200:
        case 234: case 232: case 104: case 235: case 491: case 459:
            if (f.oe()) {
                e.helper(f.w);
                return false;
            }
            break;
        default:
            break;
    }
    switch (f.xo9()) {
        case 266: e.op(IrOp::Add, rd, ra, rb);    emit_cr0(e, f, rd); return false;  // add
        case 40:  e.op(IrOp::Sub, rd, rb, ra);    emit_cr0(e, f, rd); return false;  // subf
        case 10:  e.op(IrOp::AddC, rd, ra, rb);   emit_cr0(e, f, rd); return false;  // addc
        case 8:   e.op(IrOp::SubfC, rd, ra, rb);  emit_cr0(e, f, rd); return false;  // subfc
        case 138: e.op(IrOp::AddE, rd, ra, rb);   emit_cr0(e, f, rd); return false;  // adde
        case 136: e.op(IrOp::SubfE, rd, ra, rb);  emit_cr0(e, f, rd); return false;  // subfe
        case 202: e.op(IrOp::AddE, rd, ra, kSlotZero);  emit_cr0(e, f, rd); return false;  // addze
        case 200: e.op(IrOp::SubfE, rd, ra, kSlotZero); emit_cr0(e, f, rd); return false;  // subfze
        case 234:  // addme
            e.op(IrOp::Li, kSlotT0, 0, 0, 0xffffffffu);
            e.op(IrOp::AddE, rd, ra, kSlotT0);
            emit_cr0(e, f, rd);
            return false;
        case 232:  // subfme
            e.op(IrOp::Li, kSlotT0, 0, 0, 0xffffffffu);
            e.op(IrOp::SubfE, rd, ra, kSlotT0);
            emit_cr0(e, f, rd);
            return false;
        case 104: e.op(IrOp::Neg, rd, ra);        emit_cr0(e, f, rd); return false;  // neg
        case 235: e.op(IrOp::Mullw, rd, ra, rb);  emit_cr0(e, f, rd); return false;  // mullw
        case 75:  e.op(IrOp::Mulhw, rd, ra, rb);  emit_cr0(e, f, rd); return false;  // mulhw
        case 11:  e.op(IrOp::Mulhwu, rd, ra, rb); emit_cr0(e, f, rd); return false;  // mulhwu
        case 491: e.op(IrOp::Divw, rd, ra, rb);   emit_cr0(e, f, rd); return false;  // divw
        case 459: e.op(IrOp::Divwu, rd, ra, rb);  emit_cr0(e, f, rd); return false;  // divwu
        default: break;
    }

    // X-form: rS is in the rD position, rA is the destination.
    const u8 rs = rd;
    switch (f.xo10()) {
        case 0:  e.op(IrOp::Cmp, f.crfd(), ra, rb);  return false;   // cmp
        case 32: e.op(IrOp::Cmpl, f.crfd(), ra, rb); return false;   // cmpl

        case 28:  e.op(IrOp::And, ra, rs, rb);  emit_cr0(e, f, ra); return false;
        case 60:  e.op(IrOp::Andc, ra, rs, rb); emit_cr0(e, f, ra); return false;
        case 444:  // or (mr when rS == rB)
            if (rs == rb) {
                e.op(IrOp::Mov, ra, rs);
            } else {
                e.op(IrOp::Or, ra, rs, rb);
            }
            emit_cr0(e, f, ra);
            return false;
        case 412: e.op(IrOp::Orc, ra, rs, rb);  emit_cr0(e, f, ra); return false;
        case 316: e.op(IrOp::Xor, ra, rs, rb);  emit_cr0(e, f, ra); return false;
        case 476: e.op(IrOp::Nand, ra, rs, rb); emit_cr0(e, f, ra); return false;
        case 124: e.op(IrOp::Nor, ra, rs, rb);  emit_cr0(e, f, ra); return false;
        case 284: e.op(IrOp::Eqv, ra, rs, rb);  emit_cr0(e, f, ra); return false;
        case 954: e.op(IrOp::Extsb, ra, rs);    emit_cr0(e, f, ra); return false;
        case 922: e.op(IrOp::Extsh, ra, rs);    emit_cr0(e, f, ra); return false;
        case 26:  e.op(IrOp::Cntlzw, ra, rs);   emit_cr0(e, f, ra); return false;
        case 24:  e.op(IrOp::Slw, ra, rs, rb);  emit_cr0(e, f, ra); return false;
        case 536: e.op(IrOp::Srw, ra, rs, rb);  emit_cr0(e, f, ra); return false;
        case 792: e.op(IrOp::Sraw, ra, rs, rb); emit_cr0(e, f, ra); return false;
        case 824: e.op(IrOp::Srawi, ra, rs, rb); emit_cr0(e, f, ra); return false;  // rB field = SH

        case 19: e.op(IrOp::Mfcr, rd); return false;
        case 144: {  // mtcrf
            const u32 fxm = (f.w >> 12) & 0xff;
            u32 mask = 0;
            for (unsigned i = 0; i < 8; ++i) {
                if (fxm & (0x80u >> i)) {
                    mask |= 0xf0000000u >> (4 * i);
                }
            }
            e.op(IrOp::Mtcrf, 0, rs, 0, mask);
            return false;
        }
        case 371: {  // mftb
            const u32 tbr = f.spr();
            if (tbr == kSprTBLr || tbr == kSprTBUr) {
                e.op(IrOp::Mftb, rd, tbr == kSprTBUr ? 1 : 0, 0, f.w);
            } else {
                e.helper(f.w);  // invalid TBR: faults
                return true;
            }
            return false;
        }
        case 339:  // mfspr
            switch (f.spr()) {
                case kSprLR:  e.op(IrOp::Mov, rd, kSlotLR);  return false;
                case kSprCTR: e.op(IrOp::Mov, rd, kSlotCTR); return false;
                case kSprXER: e.op(IrOp::Mfxer, rd);         return false;
                default:      e.helper(f.w);                 return false;
            }
        case 467:  // mtspr
            switch (f.spr()) {
                case kSprLR:  e.op(IrOp::Mov, kSlotLR, rs);  return false;
                case kSprCTR: e.op(IrOp::Mov, kSlotCTR, rs); return false;
                case kSprXER: e.op(IrOp::Mtxer, 0, rs);      return false;
                default:      e.helper(f.w);                 return false;
            }

        // loads and stores
        case 23:  emit_xform(e, IrOp::Lwz, f, false, true);  return false;  // lwzx
        case 55:  emit_xform(e, IrOp::Lwz, f, true, true);   return false;  // lwzux
        case 87:  emit_xform(e, IrOp::Lbz, f, false, true);  return false;  // lbzx
        case 119: emit_xform(e, IrOp::Lbz, f, true, true);   return false;  // lbzux
        case 279: emit_xform(e, IrOp::Lhz, f, false, true);  return false;  // lhzx
        case 311: emit_xform(e, IrOp::Lhz, f, true, true);   return false;  // lhzux
        case 343: emit_xform(e, IrOp::Lha, f, false, true);  return false;  // lhax
        case 375: emit_xform(e, IrOp::Lha, f, true, true);   return false;  // lhaux
        case 790: emit_xform(e, IrOp::Lhbrx, f, false, true); return false;  // lhbrx
        case 534: emit_xform(e, IrOp::Lwbrx, f, false, true); return false;  // lwbrx
        case 151: emit_xform(e, IrOp::Stw, f, false, false); return false;  // stwx
        case 183: emit_xform(e, IrOp::Stw, f, true, false);  return false;  // stwux
        case 215: emit_xform(e, IrOp::Stb, f, false, false); return false;  // stbx
        case 247: emit_xform(e, IrOp::Stb, f, true, false);  return false;  // stbux
        case 407: emit_xform(e, IrOp::Sth, f, false, false); return false;  // sthx
        case 439: emit_xform(e, IrOp::Sth, f, true, false);  return false;  // sthux
        case 918: emit_xform(e, IrOp::Sthbrx, f, false, false); return false;  // sthbrx
        case 662: emit_xform(e, IrOp::Stwbrx, f, false, false); return false;  // stwbrx
        case 535: emit_xform(e, IrOp::Lfs, f, false, false); return false;  // lfsx
        case 567: emit_xform(e, IrOp::Lfs, f, true, false);  return false;  // lfsux
        case 599: emit_xform(e, IrOp::Lfd, f, false, false); return false;  // lfdx
        case 631: emit_xform(e, IrOp::Lfd, f, true, false);  return false;  // lfdux
        case 663: emit_xform(e, IrOp::Stfs, f, false, false); return false;  // stfsx
        case 695: emit_xform(e, IrOp::Stfs, f, true, false);  return false;  // stfsux
        case 727: emit_xform(e, IrOp::Stfd, f, false, false); return false;  // stfdx
        case 759: emit_xform(e, IrOp::Stfd, f, true, false);  return false;  // stfdux
        case 983: emit_xform(e, IrOp::Stfiwx, f, false, false); return false;  // stfiwx

        // cache, TLB and ordering: nothing to model
        case 54:   // dcbst
        case 86:   // dcbf
        case 246:  // dcbtst
        case 278:  // dcbt
        case 470:  // dcbi
        case 758:  // dcba
        case 598:  // sync
        case 854:  // eieio
        case 306:  // tlbie
        case 370:  // tlbia
        case 566:  // tlbsync
            return false;

        case 982:  // icbi: end the block, like the isync that must follow it
            return true;

        // instructions that can change the mode, raise an exception or are
        // simply rare: the helper interpreter
        case 146:  // mtmsr
        case 4:    // tw
            e.helper(f.w);
            return true;

        default:  // mfmsr, mtsr(in), mfsr(in), mcrxr, lwarx, stwcx., string ops,
                  // dcbz, eciwx, ecowx, and invalid encodings (which fault)
            e.helper(f.w);
            return !known_nonterminal_31(f.xo10());
    }
}

/// Opcode 59: single-precision A-form arithmetic.
bool decode_59(Emitter& e, const Fields& f)
{
    const u8 a = f.ra(), b = f.rb(), c = f.rc();
    switch (f.xo5()) {
        case 18: emit_fp(e, IrOp::Fdivs, f, a, b); return false;
        case 20: emit_fp(e, IrOp::Fsubs, f, a, b); return false;
        case 21: emit_fp(e, IrOp::Fadds, f, a, b); return false;
        case 22: emit_fp(e, IrOp::Fsqrts, f, b, 0); return false;
        case 24: emit_fp(e, IrOp::Fres, f, b, 0); return false;
        case 25: emit_fp(e, IrOp::Fmuls, f, a, c); return false;
        case 28: emit_fp(e, IrOp::Fmsubs, f, a, b, c); return false;
        case 29: emit_fp(e, IrOp::Fmadds, f, a, b, c); return false;
        case 30: emit_fp(e, IrOp::Fnmsubs, f, a, b, c); return false;
        case 31: emit_fp(e, IrOp::Fnmadds, f, a, b, c); return false;
        default:
            e.helper(f.w);  // invalid: faults
            return true;
    }
}

/// Opcode 63: double-precision arithmetic, moves, compares, FPSCR.
bool decode_63(Emitter& e, const Fields& f)
{
    const u8 a = f.ra(), b = f.rb(), c = f.rc();
    switch (f.xo5()) {
        case 18: emit_fp(e, IrOp::Fdiv, f, a, b); return false;
        case 20: emit_fp(e, IrOp::Fsub, f, a, b); return false;
        case 21: emit_fp(e, IrOp::Fadd, f, a, b); return false;
        case 22: emit_fp(e, IrOp::Fsqrt, f, b, 0); return false;
        case 23: emit_fp(e, IrOp::Fsel, f, a, b, c, false); return false;
        case 25: emit_fp(e, IrOp::Fmul, f, a, c); return false;
        case 26: emit_fp(e, IrOp::Frsqrte, f, b, 0); return false;
        case 28: emit_fp(e, IrOp::Fmsub, f, a, b, c); return false;
        case 29: emit_fp(e, IrOp::Fmadd, f, a, b, c); return false;
        case 30: emit_fp(e, IrOp::Fnmsub, f, a, b, c); return false;
        case 31: emit_fp(e, IrOp::Fnmadd, f, a, b, c); return false;
        default: break;
    }
    switch (f.xo10()) {
        case 0:    // fcmpu
        case 32:   // fcmpo
            e.op(IrOp::Fcmpu, f.crfd(), a, b);
            return false;
        case 12:  emit_fp(e, IrOp::Frsp, f, b, 0); return false;
        case 14:  emit_fp(e, IrOp::Fctiw, f, b, 0, 0, false); return false;
        case 15:  emit_fp(e, IrOp::Fctiwz, f, b, 0, 0, false); return false;
        case 40:  emit_fp(e, IrOp::Fneg, f, b, 0, 0, false); return false;
        case 72:  emit_fp(e, IrOp::Fmr, f, b, 0, 0, false); return false;
        case 136: emit_fp(e, IrOp::Fnabs, f, b, 0, 0, false); return false;
        case 264: emit_fp(e, IrOp::Fabs, f, b, 0, 0, false); return false;
        case 583:  // mffs
        case 64:   // mcrfs
            e.helper(f.w);
            return false;
        case 38:   // mtfsb1
        case 70:   // mtfsb0
        case 134:  // mtfsfi
        case 711:  // mtfsf: may change the rounding mode, which is in the block key
        default:   // invalid: faults
            e.helper(f.w);
            return true;
    }
}

}  // namespace

bool decode_insn(std::vector<IrInst>& out, u32 insn, u32 pc, u32 index, u32 key,
                 bool& is_branch)
{
    (void)key;
    Emitter e(out, index);
    const Fields f{insn};
    is_branch = false;

    switch (f.opcd()) {
        case 3:   // twi
        case 17:  // sc
            e.helper(insn);
            return true;

        case 7:  e.op(IrOp::Mulli, f.rd(), f.ra(), 0, f.simm()); return false;
        case 8:  e.op(IrOp::SubfCi, f.rd(), f.ra(), 0, f.simm()); return false;
        case 10: e.op(IrOp::Cmpli, f.crfd(), f.ra(), 0, f.uimm()); return false;
        case 11: e.op(IrOp::Cmpi, f.crfd(), f.ra(), 0, f.simm()); return false;
        case 12: e.op(IrOp::AddCi, f.rd(), f.ra(), 0, f.simm()); return false;
        case 13:
            e.op(IrOp::AddCi, f.rd(), f.ra(), 0, f.simm());
            e.op(IrOp::Cr0, 0, f.rd());
            return false;
        case 14:  // addi / li
            if (f.ra() == 0) {
                e.op(IrOp::Li, f.rd(), 0, 0, f.simm());
            } else {
                e.op(IrOp::Addi, f.rd(), f.ra(), 0, f.simm());
            }
            return false;
        case 15:  // addis / lis
            if (f.ra() == 0) {
                e.op(IrOp::Li, f.rd(), 0, 0, f.uimm() << 16);
            } else {
                e.op(IrOp::Addi, f.rd(), f.ra(), 0, f.uimm() << 16);
            }
            return false;

        case 16: {  // bc
            const u8  bo     = f.rd();
            const u32 target = (insn & 0xfffc) | ((insn & 0x8000) ? 0xffff0000u : 0u);
            const u32 dest   = (insn & 2) ? target : pc + target;
            if (insn & 1) {
                e.op(IrOp::Li, kSlotLR, 0, 0, pc + 4);
            }
            if ((bo & 0x14) == 0x14) {
                e.op(IrOp::B, 0, 0, 0, dest);
            } else {
                e.op(IrOp::Bc, bo, f.ra(), 0, dest);
            }
            is_branch = true;
            return true;
        }
        case 18: {  // b
            u32 li = insn & 0x03fffffc;
            if (li & 0x02000000) {
                li |= 0xfc000000u;
            }
            const u32 dest = (insn & 2) ? li : pc + li;
            if (insn & 1) {
                e.op(IrOp::Li, kSlotLR, 0, 0, pc + 4);
            }
            e.op(IrOp::B, 0, 0, 0, dest);
            is_branch = true;
            return true;
        }
        case 19: return decode_19(e, f, pc, is_branch);

        case 20: {  // rlwimi
            e.op(IrOp::Rlwimi, f.ra(), f.rd(), f.rb(), rotate_mask(f.rc(), f.xo5()));
            emit_cr0(e, f, f.ra());
            return false;
        }
        case 21: {  // rlwinm
            e.op(IrOp::Rlwinm, f.ra(), f.rd(), f.rb(), rotate_mask(f.rc(), f.xo5()));
            emit_cr0(e, f, f.ra());
            return false;
        }
        case 23: {  // rlwnm
            e.op(IrOp::Rlwnm, f.ra(), f.rd(), f.rb(), rotate_mask(f.rc(), f.xo5()));
            emit_cr0(e, f, f.ra());
            return false;
        }

        case 24:  // ori (ori 0,0,0 is the canonical nop)
            if (f.uimm() == 0) {
                if (f.ra() != f.rd()) {
                    e.op(IrOp::Mov, f.ra(), f.rd());
                }
            } else {
                e.op(IrOp::Ori, f.ra(), f.rd(), 0, f.uimm());
            }
            return false;
        case 25: e.op(IrOp::Ori, f.ra(), f.rd(), 0, f.uimm() << 16); return false;
        case 26: e.op(IrOp::Xori, f.ra(), f.rd(), 0, f.uimm()); return false;
        case 27: e.op(IrOp::Xori, f.ra(), f.rd(), 0, f.uimm() << 16); return false;
        case 28:
            e.op(IrOp::Andi, f.ra(), f.rd(), 0, f.uimm());
            e.op(IrOp::Cr0, 0, f.ra());
            return false;
        case 29:
            e.op(IrOp::Andi, f.ra(), f.rd(), 0, f.uimm() << 16);
            e.op(IrOp::Cr0, 0, f.ra());
            return false;

        case 31: return decode_31(e, f);

        case 32: emit_dform(e, IrOp::Lwz, f, false); return false;
        case 33: emit_dform(e, IrOp::Lwz, f, true);  return false;
        case 34: emit_dform(e, IrOp::Lbz, f, false); return false;
        case 35: emit_dform(e, IrOp::Lbz, f, true);  return false;
        case 36: emit_dform(e, IrOp::Stw, f, false); return false;
        case 37: emit_dform(e, IrOp::Stw, f, true);  return false;
        case 38: emit_dform(e, IrOp::Stb, f, false); return false;
        case 39: emit_dform(e, IrOp::Stb, f, true);  return false;
        case 40: emit_dform(e, IrOp::Lhz, f, false); return false;
        case 41: emit_dform(e, IrOp::Lhz, f, true);  return false;
        case 42: emit_dform(e, IrOp::Lha, f, false); return false;
        case 43: emit_dform(e, IrOp::Lha, f, true);  return false;
        case 44: emit_dform(e, IrOp::Sth, f, false); return false;
        case 45: emit_dform(e, IrOp::Sth, f, true);  return false;
        case 46: e.op(IrOp::Lmw, f.rd(), f.ra0(), kSlotZero, f.simm()); return false;
        case 47: e.op(IrOp::Stmw, f.rd(), f.ra0(), kSlotZero, f.simm()); return false;
        case 48: emit_dform(e, IrOp::Lfs, f, false); return false;
        case 49: emit_dform(e, IrOp::Lfs, f, true);  return false;
        case 50: emit_dform(e, IrOp::Lfd, f, false); return false;
        case 51: emit_dform(e, IrOp::Lfd, f, true);  return false;
        case 52: emit_dform(e, IrOp::Stfs, f, false); return false;
        case 53: emit_dform(e, IrOp::Stfs, f, true);  return false;
        case 54: emit_dform(e, IrOp::Stfd, f, false); return false;
        case 55: emit_dform(e, IrOp::Stfd, f, true);  return false;

        case 59: return decode_59(e, f);
        case 63: return decode_63(e, f);

        default:  // invalid or 64-bit only: the helper faults
            e.helper(insn);
            return true;
    }
}

const char* ir_op_name(IrOp op)
{
    static const char* const kNames[] = {
        "mov", "li", "add", "addi", "sub", "addc", "addci", "adde", "subfc", "subfci", "subfe",
        "neg", "mullw", "mulli", "mulhw", "mulhwu", "divw", "divwu",
        "and", "andi", "andc", "or", "ori", "orc", "xor", "xori", "nand", "nor", "eqv",
        "extsb", "extsh", "cntlzw",
        "slw", "srw", "sraw", "srawi", "rlwinm", "rlwnm", "rlwimi",
        "cr0", "cmp", "cmpi", "cmpl", "cmpli", "crlogic", "mcrf", "mfcr", "mtcrf", "mfxer", "mtxer", "mftb",
        "lbz", "lhz", "lha", "lwz", "lhbrx", "lwbrx", "stb", "sth", "stw", "sthbrx", "stwbrx",
        "lfs", "lfd", "stfs", "stfd", "stfiwx", "lmw", "stmw",
        "fmr", "fneg", "fabs", "fnabs", "fadd", "fsub", "fmul", "fdiv", "fmadd", "fmsub", "fnmadd",
        "fnmsub", "fadds", "fsubs", "fmuls", "fdivs", "fmadds", "fmsubs", "fnmadds", "fnmsubs",
        "frsp", "fres", "frsqrte", "fsqrt", "fsqrts", "fsel", "fctiw", "fctiwz", "fcmpu", "fprf", "cr1",
        "helper", "b", "bc", "bcr", "exit",
    };
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == static_cast<usize>(IrOp::Count));
    const auto i = static_cast<usize>(op);
    return i < static_cast<usize>(IrOp::Count) ? kNames[i] : "?";
}

}  // namespace sm2::cpu::ppc
