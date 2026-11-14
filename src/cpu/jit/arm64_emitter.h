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

#include "cpu/jit/emitter_base.h"

#include "core/types.h"

#include <optional>

// Typed A64 encoder. W/X operands select the 32/64-bit form. Unencodable
// operands set the error flag instead of emitting something else.
// X16/X17 are scratch for the multi-instruction helpers.

namespace sm2::cpu::jit::a64 {

// -- Registers -----------------------------------------------------------------

/// A general-purpose register. `id` 31 is ZR or SP depending on `sp`.
struct Reg {
    u8   id = 0;
    bool x  = true;   ///< 64-bit (X) view; false for W.
    bool sp = false;  ///< Register 31 means the stack pointer.

    [[nodiscard]] constexpr Reg w() const { return Reg{id, false, sp}; }
    [[nodiscard]] constexpr Reg as_x() const { return Reg{id, true, sp}; }
    [[nodiscard]] constexpr bool is_zr() const { return id == 31 && !sp; }
    constexpr bool operator==(const Reg&) const = default;
};

[[nodiscard]] constexpr Reg W(unsigned n) { return Reg{static_cast<u8>(n), false, false}; }
[[nodiscard]] constexpr Reg X(unsigned n) { return Reg{static_cast<u8>(n), true, false}; }

#define SM2_A64_REG(n) inline constexpr Reg W##n = W(n), X##n = X(n);
SM2_A64_REG(0) SM2_A64_REG(1) SM2_A64_REG(2) SM2_A64_REG(3) SM2_A64_REG(4) SM2_A64_REG(5)
SM2_A64_REG(6) SM2_A64_REG(7) SM2_A64_REG(8) SM2_A64_REG(9) SM2_A64_REG(10) SM2_A64_REG(11)
SM2_A64_REG(12) SM2_A64_REG(13) SM2_A64_REG(14) SM2_A64_REG(15) SM2_A64_REG(16) SM2_A64_REG(17)
SM2_A64_REG(18) SM2_A64_REG(19) SM2_A64_REG(20) SM2_A64_REG(21) SM2_A64_REG(22) SM2_A64_REG(23)
SM2_A64_REG(24) SM2_A64_REG(25) SM2_A64_REG(26) SM2_A64_REG(27) SM2_A64_REG(28) SM2_A64_REG(29)
SM2_A64_REG(30)
#undef SM2_A64_REG
inline constexpr Reg WZR{31, false, false};
inline constexpr Reg XZR{31, true, false};
inline constexpr Reg WSP{31, false, true};
inline constexpr Reg SP{31, true, true};
inline constexpr Reg FP = X29;
inline constexpr Reg LR = X30;

/// A SIMD&FP register viewed as a scalar of one size.
enum class VSize : u8 { B, H, S, D, Q };

struct VReg {
    u8    id   = 0;
    VSize size = VSize::D;
    constexpr bool operator==(const VReg&) const = default;
};

[[nodiscard]] constexpr VReg S(unsigned n) { return VReg{static_cast<u8>(n), VSize::S}; }
[[nodiscard]] constexpr VReg D(unsigned n) { return VReg{static_cast<u8>(n), VSize::D}; }
[[nodiscard]] constexpr VReg Q(unsigned n) { return VReg{static_cast<u8>(n), VSize::Q}; }
[[nodiscard]] constexpr VReg H(unsigned n) { return VReg{static_cast<u8>(n), VSize::H}; }

#define SM2_A64_VREG(n) inline constexpr VReg S##n = S(n), D##n = D(n), Q##n = Q(n);
SM2_A64_VREG(0) SM2_A64_VREG(1) SM2_A64_VREG(2) SM2_A64_VREG(3) SM2_A64_VREG(4) SM2_A64_VREG(5)
SM2_A64_VREG(6) SM2_A64_VREG(7) SM2_A64_VREG(8) SM2_A64_VREG(9) SM2_A64_VREG(10) SM2_A64_VREG(11)
SM2_A64_VREG(12) SM2_A64_VREG(13) SM2_A64_VREG(14) SM2_A64_VREG(15) SM2_A64_VREG(16)
SM2_A64_VREG(17) SM2_A64_VREG(18) SM2_A64_VREG(19) SM2_A64_VREG(20) SM2_A64_VREG(21)
SM2_A64_VREG(22) SM2_A64_VREG(23) SM2_A64_VREG(24) SM2_A64_VREG(25) SM2_A64_VREG(26)
SM2_A64_VREG(27) SM2_A64_VREG(28) SM2_A64_VREG(29) SM2_A64_VREG(30) SM2_A64_VREG(31)
#undef SM2_A64_VREG

// -- Operand modifiers -----------------------------------------------------------

enum class Cond : u8 {
    EQ, NE, HS, LO, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL, NV,
    CS = HS, CC = LO,
};
[[nodiscard]] constexpr Cond invert(Cond c) { return static_cast<Cond>(static_cast<u8>(c) ^ 1U); }

enum class Shift : u8 { LSL, LSR, ASR, ROR };
enum class Extend : u8 { UXTB, UXTH, UXTW, UXTX, SXTB, SXTH, SXTW, SXTX };

/// System registers reachable with MRS / MSR, as op0:op1:CRn:CRm:op2 with
/// op0's top bit implied.
enum class SysReg : u16 {
    NZCV       = 0x5A10,
    FPCR       = 0x5A20,
    FPSR       = 0x5A21,
    TPIDR_EL0  = 0x5E82,
    CNTVCT_EL0 = 0x5F02,
};

/// Barrier domains for DMB / DSB.
enum class Barrier : u8 {
    OSHLD = 1, OSHST = 2, OSH = 3, NSHLD = 5, NSHST = 6, NSH = 7,
    ISHLD = 9, ISHST = 10, ISH = 11, LD = 13, ST = 14, SY = 15,
};

/// A memory operand.
struct Mem {
    enum class Mode : u8 { Offset, PreIndex, PostIndex, RegOffset };
    Reg    base;
    s64    offset  = 0;
    Reg    index   = XZR;
    Extend extend  = Extend::UXTX;
    bool   shifted = false;  ///< Register offset scaled by the access size.
    Mode   mode    = Mode::Offset;
};

/// [base, #offset]
[[nodiscard]] constexpr Mem mem(Reg base, s64 offset = 0) { return Mem{base, offset}; }
/// [base, #offset]!
[[nodiscard]] constexpr Mem pre(Reg base, s64 offset)
{
    return Mem{base, offset, XZR, Extend::UXTX, false, Mem::Mode::PreIndex};
}
/// [base], #offset
[[nodiscard]] constexpr Mem post(Reg base, s64 offset)
{
    return Mem{base, offset, XZR, Extend::UXTX, false, Mem::Mode::PostIndex};
}
/// [base, index{, extend {#log2(size)}}]. A W index must be UXTW or SXTW; an
/// X index LSL (UXTX) or SXTX.
[[nodiscard]] constexpr Mem mem(Reg base, Reg index, Extend ext = Extend::UXTX, bool shifted = false)
{
    return Mem{base, 0, index, ext, shifted, Mem::Mode::RegOffset};
}

// -- Encoding utilities ----------------------------------------------------------

/// Logical (bitmask) immediate -> N:immr:imms, or nothing if unencodable.
[[nodiscard]] std::optional<u32> encode_bitmask(u64 value, bool is64);
/// DecodeBitMasks(); nothing for reserved encodings.
[[nodiscard]] std::optional<u64> decode_bitmask(u32 n, u32 immr, u32 imms, bool is64);

/// FMOV imm8 for `value` (+-n/16 * 2^r, 16 <= n <= 31, -3 <= r <= 4).
[[nodiscard]] std::optional<u32> encode_fp_imm8(double value);

/// Can a B / BL at `from` reach `to` (+-128 MiB)?
[[nodiscard]] bool branch26_reachable(const void* from, const void* to);
/// B at `from` to `to` (check reachability first).
[[nodiscard]] u32 encode_b(const void* from, const void* to);

// -- The emitter ---------------------------------------------------------------

class Emitter : public EmitterBase {
public:
    using EmitterBase::EmitterBase;

    /// Emit one raw instruction word.
    void dw(u32 insn) { emit32(insn); }

    // ---- Moves --------------------------------------------------------------

    /// Register move (ORR, or ADD #0 when either side is SP).
    void mov(Reg d, Reg n);
    /// Any immediate: MOVZ / MOVN / ORR, else MOVZ|MOVN + MOVKs.
    void mov(Reg d, u64 imm);
    /// Load an absolute host address: ADR, ADRP+ADD or a full mov().
    void mov_address(Reg d, const void* p);
    void movz(Reg d, u32 imm16, u32 shift = 0);
    void movn(Reg d, u32 imm16, u32 shift = 0);
    void movk(Reg d, u32 imm16, u32 shift = 0);

    // ---- Add / subtract -----------------------------------------------------

    void add(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { addsub_shift(0, d, n, m, sh, amount); }
    void adds(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { addsub_shift(1, d, n, m, sh, amount); }
    void sub(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { addsub_shift(2, d, n, m, sh, amount); }
    void subs(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { addsub_shift(3, d, n, m, sh, amount); }
    /// Extended-register form (and the only register form that accepts SP).
    void add(Reg d, Reg n, Reg m, Extend ext, u32 amount = 0) { addsub_ext(0, d, n, m, ext, amount); }
    void adds(Reg d, Reg n, Reg m, Extend ext, u32 amount = 0) { addsub_ext(1, d, n, m, ext, amount); }
    void sub(Reg d, Reg n, Reg m, Extend ext, u32 amount = 0) { addsub_ext(2, d, n, m, ext, amount); }
    void subs(Reg d, Reg n, Reg m, Extend ext, u32 amount = 0) { addsub_ext(3, d, n, m, ext, amount); }
    /// 12-bit immediate, optionally shifted left by 12 (taken automatically
    /// when the low 12 bits are zero and the value needs it).
    void add(Reg d, Reg n, u32 imm) { addsub_imm(0, d, n, imm); }
    void adds(Reg d, Reg n, u32 imm) { addsub_imm(1, d, n, imm); }
    void sub(Reg d, Reg n, u32 imm) { addsub_imm(2, d, n, imm); }
    void subs(Reg d, Reg n, u32 imm) { addsub_imm(3, d, n, imm); }
    [[nodiscard]] static bool addsub_imm_encodable(u64 imm);
    /// d = n + imm for any imm: ADD/SUB immediate (one or two), else via
    /// `scratch` (which must differ from n).
    void add_imm(Reg d, Reg n, s64 imm, Reg scratch = X16);
    void adc(Reg d, Reg n, Reg m) { rrr(0x1A000000, d, n, m); }
    void adcs(Reg d, Reg n, Reg m) { rrr(0x3A000000, d, n, m); }
    void sbc(Reg d, Reg n, Reg m) { rrr(0x5A000000, d, n, m); }
    void sbcs(Reg d, Reg n, Reg m) { rrr(0x7A000000, d, n, m); }
    void ngc(Reg d, Reg m) { sbc(d, zr(d), m); }
    void ngcs(Reg d, Reg m) { sbcs(d, zr(d), m); }
    void cmp(Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { subs(zr(n), n, m, sh, amount); }
    void cmp(Reg n, Reg m, Extend ext, u32 amount = 0) { subs(zr(n), n, m, ext, amount); }
    void cmp(Reg n, u32 imm) { subs(zr(n), n, imm); }
    void cmn(Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { adds(zr(n), n, m, sh, amount); }
    void cmn(Reg n, u32 imm) { adds(zr(n), n, imm); }
    void neg(Reg d, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { sub(d, zr(d), m, sh, amount); }
    void negs(Reg d, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { subs(d, zr(d), m, sh, amount); }

    // ---- Logical --------------------------------------------------------------

    void and_(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { logic_shift(0x0A000000, d, n, m, sh, amount); }
    void bic(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { logic_shift(0x0A200000, d, n, m, sh, amount); }
    void orr(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { logic_shift(0x2A000000, d, n, m, sh, amount); }
    void orn(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { logic_shift(0x2A200000, d, n, m, sh, amount); }
    void eor(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { logic_shift(0x4A000000, d, n, m, sh, amount); }
    void eon(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { logic_shift(0x4A200000, d, n, m, sh, amount); }
    void ands(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { logic_shift(0x6A000000, d, n, m, sh, amount); }
    void bics(Reg d, Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { logic_shift(0x6A200000, d, n, m, sh, amount); }
    void tst(Reg n, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { ands(zr(n), n, m, sh, amount); }
    void mvn(Reg d, Reg m, Shift sh = Shift::LSL, u32 amount = 0) { orn(d, zr(d), m, sh, amount); }
    /// Bitmask-immediate forms. Unencodable immediates are an error; check
    /// with encode_bitmask() or use the *_imm() helpers.
    void and_(Reg d, Reg n, u64 imm) { logic_imm(0x12000000, d, n, imm); }
    void orr(Reg d, Reg n, u64 imm) { logic_imm(0x32000000, d, n, imm); }
    void eor(Reg d, Reg n, u64 imm) { logic_imm(0x52000000, d, n, imm); }
    void ands(Reg d, Reg n, u64 imm) { logic_imm(0x72000000, d, n, imm); }
    void tst(Reg n, u64 imm) { ands(zr(n), n, imm); }
    /// Any immediate: the bitmask form when encodable, else via `scratch`.
    void and_imm(Reg d, Reg n, u64 imm, Reg scratch = X16) { logic_any(0, d, n, imm, scratch); }
    void orr_imm(Reg d, Reg n, u64 imm, Reg scratch = X16) { logic_any(1, d, n, imm, scratch); }
    void eor_imm(Reg d, Reg n, u64 imm, Reg scratch = X16) { logic_any(2, d, n, imm, scratch); }
    void ands_imm(Reg d, Reg n, u64 imm, Reg scratch = X16) { logic_any(3, d, n, imm, scratch); }

    // ---- Shifts, bitfields ------------------------------------------------------

    void lsl(Reg d, Reg n, u32 amount);
    void lsr(Reg d, Reg n, u32 amount);
    void asr(Reg d, Reg n, u32 amount);
    void ror(Reg d, Reg n, u32 amount) { extr(d, n, n, amount); }
    void lsl(Reg d, Reg n, Reg m) { rrr(0x1AC02000, d, n, m); }
    void lsr(Reg d, Reg n, Reg m) { rrr(0x1AC02400, d, n, m); }
    void asr(Reg d, Reg n, Reg m) { rrr(0x1AC02800, d, n, m); }
    void ror(Reg d, Reg n, Reg m) { rrr(0x1AC02C00, d, n, m); }

    void sbfm(Reg d, Reg n, u32 immr, u32 imms) { bitfield(0x13000000, d, n, immr, imms); }
    void bfm(Reg d, Reg n, u32 immr, u32 imms) { bitfield(0x33000000, d, n, immr, imms); }
    void ubfm(Reg d, Reg n, u32 immr, u32 imms) { bitfield(0x53000000, d, n, immr, imms); }
    void ubfx(Reg d, Reg n, u32 lsb, u32 width) { ubfm(d, n, lsb, lsb + width - 1); }
    void sbfx(Reg d, Reg n, u32 lsb, u32 width) { sbfm(d, n, lsb, lsb + width - 1); }
    void bfxil(Reg d, Reg n, u32 lsb, u32 width) { bfm(d, n, lsb, lsb + width - 1); }
    void bfi(Reg d, Reg n, u32 lsb, u32 width) { bfm(d, n, (bits(d) - lsb) % bits(d), width - 1); }
    void bfc(Reg d, u32 lsb, u32 width) { bfi(d, zr(d), lsb, width); }
    void ubfiz(Reg d, Reg n, u32 lsb, u32 width) { ubfm(d, n, (bits(d) - lsb) % bits(d), width - 1); }
    void sbfiz(Reg d, Reg n, u32 lsb, u32 width) { sbfm(d, n, (bits(d) - lsb) % bits(d), width - 1); }
    void uxtb(Reg d, Reg n) { ubfm(d.w(), n.w(), 0, 7); }
    void uxth(Reg d, Reg n) { ubfm(d.w(), n.w(), 0, 15); }
    /// SXTB / SXTH into a W or X destination; SXTW needs an X destination.
    void sxtb(Reg d, Reg n) { sbfm(d, d.x ? n.as_x() : n, 0, 7); }
    void sxth(Reg d, Reg n) { sbfm(d, d.x ? n.as_x() : n, 0, 15); }
    void sxtw(Reg d, Reg n) { sbfm(d, n.as_x(), 0, 31); }
    void extr(Reg d, Reg n, Reg m, u32 lsb);

    void rbit(Reg d, Reg n) { rr(0x5AC00000, d, n); }
    void rev16(Reg d, Reg n) { rr(0x5AC00400, d, n); }
    /// Byte-reverse the whole register (REV, 32 or 64 bits).
    void rev(Reg d, Reg n) { rr(d.x ? 0x5AC00C00 : 0x5AC00800, d, n); }
    /// Byte-reverse each 32-bit word of an X register.
    void rev32(Reg d, Reg n) { rr(0x5AC00800, d, n); }
    void clz(Reg d, Reg n) { rr(0x5AC01000, d, n); }
    void cls(Reg d, Reg n) { rr(0x5AC01400, d, n); }

    // ---- Multiply / divide ------------------------------------------------------

    void madd(Reg d, Reg n, Reg m, Reg a) { rrrr(0x1B000000, d, n, m, a); }
    void msub(Reg d, Reg n, Reg m, Reg a) { rrrr(0x1B008000, d, n, m, a); }
    void mul(Reg d, Reg n, Reg m) { madd(d, n, m, zr(d)); }
    void mneg(Reg d, Reg n, Reg m) { msub(d, n, m, zr(d)); }
    /// Widening 32x32->64: X destination and accumulator, W sources.
    void smaddl(Reg d, Reg n, Reg m, Reg a) { widening(0x9B200000, d, n, m, a); }
    void smsubl(Reg d, Reg n, Reg m, Reg a) { widening(0x9B208000, d, n, m, a); }
    void umaddl(Reg d, Reg n, Reg m, Reg a) { widening(0x9BA00000, d, n, m, a); }
    void umsubl(Reg d, Reg n, Reg m, Reg a) { widening(0x9BA08000, d, n, m, a); }
    void smull(Reg d, Reg n, Reg m) { smaddl(d, n, m, XZR); }
    void umull(Reg d, Reg n, Reg m) { umaddl(d, n, m, XZR); }
    /// High 64 bits of the 128-bit product (X registers only).
    void smulh(Reg d, Reg n, Reg m) { rrrr(0x9B407C00, d, n, m, XZR, true); }
    void umulh(Reg d, Reg n, Reg m) { rrrr(0x9BC07C00, d, n, m, XZR, true); }
    void sdiv(Reg d, Reg n, Reg m) { rrr(0x1AC00C00, d, n, m); }
    void udiv(Reg d, Reg n, Reg m) { rrr(0x1AC00800, d, n, m); }

    // ---- Conditional ------------------------------------------------------------

    void csel(Reg d, Reg n, Reg m, Cond c) { condsel(0x1A800000, d, n, m, c); }
    void csinc(Reg d, Reg n, Reg m, Cond c) { condsel(0x1A800400, d, n, m, c); }
    void csinv(Reg d, Reg n, Reg m, Cond c) { condsel(0x5A800000, d, n, m, c); }
    void csneg(Reg d, Reg n, Reg m, Cond c) { condsel(0x5A800400, d, n, m, c); }
    void cset(Reg d, Cond c) { csinc(d, zr(d), zr(d), invert(c)); }
    void csetm(Reg d, Cond c) { csinv(d, zr(d), zr(d), invert(c)); }
    void cinc(Reg d, Reg n, Cond c) { csinc(d, n, n, invert(c)); }
    void cinv(Reg d, Reg n, Cond c) { csinv(d, n, n, invert(c)); }
    void cneg(Reg d, Reg n, Cond c) { csneg(d, n, n, invert(c)); }
    /// Flags = (cond ? flags of n - m : nzcv).
    void ccmp(Reg n, Reg m, u32 nzcv, Cond c) { condcmp(0x7A400000, n, m.id, false, nzcv, c, m.x); }
    void ccmp(Reg n, u32 imm5, u32 nzcv, Cond c) { condcmp(0x7A400800, n, imm5, true, nzcv, c, n.x); }
    void ccmn(Reg n, Reg m, u32 nzcv, Cond c) { condcmp(0x3A400000, n, m.id, false, nzcv, c, m.x); }
    void ccmn(Reg n, u32 imm5, u32 nzcv, Cond c) { condcmp(0x3A400800, n, imm5, true, nzcv, c, n.x); }

    // ---- Loads and stores (offset: scaled imm12, else LDUR imm9) -----------

    void ldr(Reg t, const Mem& m) { ldst(t.x ? 3 : 2, 1, t.id, m); }
    void str(Reg t, const Mem& m) { ldst(t.x ? 3 : 2, 0, t.id, m); }
    void ldrb(Reg t, const Mem& m) { ldst(0, 1, t.id, m); }
    void ldrh(Reg t, const Mem& m) { ldst(1, 1, t.id, m); }
    void strb(Reg t, const Mem& m) { ldst(0, 0, t.id, m); }
    void strh(Reg t, const Mem& m) { ldst(1, 0, t.id, m); }
    /// Sign-extending loads into a W or X register.
    void ldrsb(Reg t, const Mem& m) { ldst(0, t.x ? 2 : 3, t.id, m); }
    void ldrsh(Reg t, const Mem& m) { ldst(1, t.x ? 2 : 3, t.id, m); }
    void ldrsw(Reg t, const Mem& m) { ldst(2, 2, t.id, m); }
    void ldr(VReg t, const Mem& m);
    void str(VReg t, const Mem& m);

    /// Pairs. Offset, PreIndex and PostIndex modes; offsets are multiples of
    /// the register size within +-64 elements.
    void ldp(Reg t1, Reg t2, const Mem& m) { pair(t1.x ? 2 : 0, 1, false, t1.id, t2.id, t1.x ? 8 : 4, m); }
    void stp(Reg t1, Reg t2, const Mem& m) { pair(t1.x ? 2 : 0, 0, false, t1.id, t2.id, t1.x ? 8 : 4, m); }
    void ldpsw(Reg t1, Reg t2, const Mem& m) { pair(1, 1, false, t1.id, t2.id, 4, m); }
    void ldp(VReg t1, VReg t2, const Mem& m);
    void stp(VReg t1, VReg t2, const Mem& m);

    /// PC-relative literal loads (+-1 MiB).
    void ldr(Reg t, Label l) { literal(t.x ? 0x58000000 : 0x18000000, t.id, l); }
    void ldrsw(Reg t, Label l) { literal(0x98000000, t.id, l); }
    void ldr(VReg t, Label l);

    void adr(Reg d, Label l);
    /// ADR to an absolute address within +-1 MiB (error otherwise).
    void adr(Reg d, const void* p);
    /// ADRP to the 4 KiB page of `p` within +-4 GiB (error otherwise).
    void adrp(Reg d, const void* p);

    // ---- Branches ------------------------------------------------------------

    void b(Label l) { branch_label(0x14000000, l, kFix26); }
    void bl(Label l) { branch_label(0x94000000, l, kFix26); }
    void b(Cond c, Label l) { branch_label(0x54000000 | static_cast<u32>(c), l, kFix19); }
    void cbz(Reg t, Label l) { branch_label(0x34000000 | sf(t) | t.id, l, kFix19); }
    void cbnz(Reg t, Label l) { branch_label(0x35000000 | sf(t) | t.id, l, kFix19); }
    void tbz(Reg t, u32 bit, Label l) { branch_label(tb(0x36000000, t, bit), l, kFix14); }
    void tbnz(Reg t, u32 bit, Label l) { branch_label(tb(0x37000000, t, bit), l, kFix14); }
    /// Branches to absolute execution addresses. Out of range is an error;
    /// use jump() / call() when the target may be far.
    void b(const void* target) { branch_abs(0x14000000, target, kFix26); }
    void bl(const void* target) { branch_abs(0x94000000, target, kFix26); }
    void b(Cond c, const void* target) { branch_abs(0x54000000 | static_cast<u32>(c), target, kFix19); }
    void cbz(Reg t, const void* target) { branch_abs(0x34000000 | sf(t) | t.id, target, kFix19); }
    void cbnz(Reg t, const void* target) { branch_abs(0x35000000 | sf(t) | t.id, target, kFix19); }
    void br(Reg n) { emit32(0xD61F0000 | (n.id << 5)); }
    void blr(Reg n) { emit32(0xD63F0000 | (n.id << 5)); }
    void ret(Reg n = X30) { emit32(0xD65F0000 | (n.id << 5)); }

    /// B if reachable, else materialise into `scratch` and BR.
    void jump(const void* target, Reg scratch = X16);
    /// BL if reachable, else mov `scratch` + BLR.
    void call(const void* fn, Reg scratch = X16);
    template <typename R, typename... A>
    void call(R (*fn)(A...), Reg scratch = X16)
    {
        call(reinterpret_cast<const void*>(fn), scratch);
    }

    // ---- Scalar floating point (operands of one size, except fcvt) -------

    void fadd(VReg d, VReg n, VReg m) { fp2(0x1E202800, d, n, m); }
    void fsub(VReg d, VReg n, VReg m) { fp2(0x1E203800, d, n, m); }
    void fmul(VReg d, VReg n, VReg m) { fp2(0x1E200800, d, n, m); }
    void fdiv(VReg d, VReg n, VReg m) { fp2(0x1E201800, d, n, m); }
    void fnmul(VReg d, VReg n, VReg m) { fp2(0x1E208800, d, n, m); }
    void fmax(VReg d, VReg n, VReg m) { fp2(0x1E204800, d, n, m); }
    void fmin(VReg d, VReg n, VReg m) { fp2(0x1E205800, d, n, m); }
    void fmaxnm(VReg d, VReg n, VReg m) { fp2(0x1E206800, d, n, m); }
    void fminnm(VReg d, VReg n, VReg m) { fp2(0x1E207800, d, n, m); }
    /// d = a + n*m (fused).
    void fmadd(VReg d, VReg n, VReg m, VReg a) { fp3(0x1F000000, d, n, m, a); }
    /// d = a - n*m.
    void fmsub(VReg d, VReg n, VReg m, VReg a) { fp3(0x1F008000, d, n, m, a); }
    /// d = -a - n*m.
    void fnmadd(VReg d, VReg n, VReg m, VReg a) { fp3(0x1F200000, d, n, m, a); }
    /// d = -a + n*m.
    void fnmsub(VReg d, VReg n, VReg m, VReg a) { fp3(0x1F208000, d, n, m, a); }
    void fmov(VReg d, VReg n) { fp1(0x1E204000, d, n); }
    void fabs(VReg d, VReg n) { fp1(0x1E20C000, d, n); }
    void fneg(VReg d, VReg n) { fp1(0x1E214000, d, n); }
    void fsqrt(VReg d, VReg n) { fp1(0x1E21C000, d, n); }
    void frintn(VReg d, VReg n) { fp1(0x1E244000, d, n); }
    void frintp(VReg d, VReg n) { fp1(0x1E24C000, d, n); }
    void frintm(VReg d, VReg n) { fp1(0x1E254000, d, n); }
    void frintz(VReg d, VReg n) { fp1(0x1E25C000, d, n); }
    void frinta(VReg d, VReg n) { fp1(0x1E264000, d, n); }
    void frintx(VReg d, VReg n) { fp1(0x1E274000, d, n); }
    void frinti(VReg d, VReg n) { fp1(0x1E27C000, d, n); }
    /// Convert between precisions (S <-> D, and H).
    void fcvt(VReg d, VReg n);
    /// Reciprocal / reciprocal square root estimate and Newton step
    /// (AdvSIMD scalar; S and D).
    void frecpe(VReg d, VReg n) { simd_scalar1(0x5EA1D800, d, n); }
    void frsqrte(VReg d, VReg n) { simd_scalar1(0x7EA1D800, d, n); }
    void frecpx(VReg d, VReg n) { simd_scalar1(0x5EA1F800, d, n); }
    void frecps(VReg d, VReg n, VReg m) { simd_scalar2(0x5E20FC00, d, n, m); }
    void frsqrts(VReg d, VReg n, VReg m) { simd_scalar2(0x5EA0FC00, d, n, m); }

    void fcmp(VReg n, VReg m) { fcmp_(0x1E202000, n, m.id, m.size); }
    void fcmpe(VReg n, VReg m) { fcmp_(0x1E202010, n, m.id, m.size); }
    /// Compare with +0.0.
    void fcmp_zero(VReg n) { fcmp_(0x1E202008, n, 0, n.size); }
    void fcmpe_zero(VReg n) { fcmp_(0x1E202018, n, 0, n.size); }
    void fccmp(VReg n, VReg m, u32 nzcv, Cond c);
    void fcsel(VReg d, VReg n, VReg m, Cond c);

    /// FP <-> integer conversions. The GPR may be W or X.
    void fcvtzs(Reg d, VReg n) { fp_int(0x1E380000, d, n); }
    void fcvtzu(Reg d, VReg n) { fp_int(0x1E390000, d, n); }
    void fcvtns(Reg d, VReg n) { fp_int(0x1E200000, d, n); }
    void fcvtnu(Reg d, VReg n) { fp_int(0x1E210000, d, n); }
    void fcvtps(Reg d, VReg n) { fp_int(0x1E280000, d, n); }
    void fcvtms(Reg d, VReg n) { fp_int(0x1E300000, d, n); }
    void fcvtas(Reg d, VReg n) { fp_int(0x1E240000, d, n); }
    void scvtf(VReg d, Reg n) { int_fp(0x1E220000, d, n); }
    void ucvtf(VReg d, Reg n) { int_fp(0x1E230000, d, n); }
    /// Bit moves between a GPR and an FP register of the same width.
    void fmov(Reg d, VReg n);
    void fmov(VReg d, Reg n);
    /// FMOV of an 8-bit-encodable constant (see encode_fp_imm8). Error if
    /// it is not; fmov_const() handles any value.
    void fmov(VReg d, double imm);
    /// Any constant: FMOV imm, FMOV from XZR for +0.0, or via `scratch`.
    void fmov_const(VReg d, double value, Reg scratch = X16);

    // ---- System -----------------------------------------------------------------

    void mrs(Reg t, SysReg r) { emit32(0xD5300000 | (static_cast<u32>(r) << 5) | t.id); }
    void msr(SysReg r, Reg t) { emit32(0xD5100000 | (static_cast<u32>(r) << 5) | t.id); }
    void dmb(Barrier o = Barrier::ISH) { emit32(0xD50330BF | (static_cast<u32>(o) << 8)); }
    void dsb(Barrier o = Barrier::ISH) { emit32(0xD503309F | (static_cast<u32>(o) << 8)); }
    void isb() { emit32(0xD5033FDF); }
    void nop() { emit32(0xD503201F); }
    void brk(u32 imm16 = 0) { emit32(0xD4200000 | ((imm16 & 0xFFFF) << 5)); }
    void udf(u32 imm16 = 0) { emit32(imm16 & 0xFFFF); }

    /// Pad with NOPs to a multiple of `alignment` bytes.
    void align(usize alignment)
    {
        while ((reinterpret_cast<uintptr_t>(cursor()) & (alignment - 1)) != 0) nop();
    }

protected:
    bool patch_fixup(usize pos, u32 kind, usize target) override;

private:
    static constexpr u32 kFix26 = 0, kFix19 = 1, kFix14 = 2, kFixAdr = 3;

    [[nodiscard]] static u32  sf(Reg r) { return r.x ? 0x80000000U : 0U; }
    [[nodiscard]] static Reg  zr(Reg like) { return Reg{31, like.x, false}; }
    [[nodiscard]] static u32  bits(Reg r) { return r.x ? 64U : 32U; }
    [[nodiscard]] static u32  ftype(VSize s);
    [[nodiscard]] u32         tb(u32 base, Reg t, u32 bit);
    void check_same(Reg a, Reg b);
    void check_same(Reg a, Reg b, Reg c);

    void addsub_shift(u32 op, Reg d, Reg n, Reg m, Shift sh, u32 amount);
    void addsub_ext(u32 op, Reg d, Reg n, Reg m, Extend ext, u32 amount);
    void addsub_imm(u32 op, Reg d, Reg n, u32 imm);
    void logic_shift(u32 base, Reg d, Reg n, Reg m, Shift sh, u32 amount);
    void logic_imm(u32 base, Reg d, Reg n, u64 imm);
    void logic_any(u32 opc, Reg d, Reg n, u64 imm, Reg scratch);
    void bitfield(u32 base, Reg d, Reg n, u32 immr, u32 imms);
    void rr(u32 base, Reg d, Reg n);
    void rrr(u32 base, Reg d, Reg n, Reg m);
    void rrrr(u32 base, Reg d, Reg n, Reg m, Reg a, bool x_only = false);
    void widening(u32 base, Reg d, Reg n, Reg m, Reg a);
    void condsel(u32 base, Reg d, Reg n, Reg m, Cond c);
    void condcmp(u32 base, Reg n, u32 m_or_imm, bool imm, u32 nzcv, Cond c, bool m_x);
    void ldst(u32 size, u32 opc, u32 rt, const Mem& m, bool simd = false);
    void pair(u32 opc, u32 load, bool simd, u32 t1, u32 t2, u32 scale, const Mem& m);
    void literal(u32 base, u32 rt, Label l);
    void branch_label(u32 insn, Label l, u32 kind);
    void branch_abs(u32 insn, const void* target, u32 kind);
    [[nodiscard]] static bool encode_rel(u32& insn, u32 kind, s64 delta);
    void fp1(u32 base, VReg d, VReg n);
    void fp2(u32 base, VReg d, VReg n, VReg m);
    void fp3(u32 base, VReg d, VReg n, VReg m, VReg a);
    void fcmp_(u32 base, VReg n, u32 m, VSize msize);
    void fp_int(u32 base, Reg d, VReg n);
    void int_fp(u32 base, VReg d, Reg n);
    void simd_scalar1(u32 base, VReg d, VReg n);
    void simd_scalar2(u32 base, VReg d, VReg n, VReg m);
};

// -- AAPCS64 helpers -----------------------------------------------------------

/// Registers a C++ call may clobber, as bit masks (bit n = Xn / Vn).
inline constexpr u32 kCallerSavedGprs = 0x0007FFFF;  ///< X0-X18 (never use X18)
inline constexpr u32 kCalleeSavedGprs = 0x1FF80000;  ///< X19-X28.
inline constexpr u32 kCallerSavedFprs = 0xFFFF00FF;  ///< V0-V7, V16-V31.
inline constexpr u32 kCalleeSavedFprs = 0x0000FF00;  ///< V8-V15 (low 64 bits).
/// AAPCS64 integer argument registers.
inline constexpr Reg kArgGprs[8] = {X0, X1, X2, X3, X4, X5, X6, X7};

/// AAPCS64 frame: saves FP/LR (and sets FP), the X19-X28 in `gprs`, the
/// D8-D15 in `fprs`, and reserves `locals` bytes at [SP, #locals_offset()].
class Frame {
public:
    Frame(u32 gprs, u32 fprs, u32 locals);

    [[nodiscard]] u32 size() const { return m_size; }
    [[nodiscard]] u32 locals_offset() const { return m_locals_offset; }

    void prologue(Emitter& a) const;
    /// Restore everything and RET (or not, if `with_ret` is false).
    void epilogue(Emitter& a, bool with_ret = true) const;

private:
    u32 m_gprs, m_fprs;
    u32 m_size          = 0;
    u32 m_locals_offset = 0;
};

/// Save / restore registers (bit n = Xn, Dn) around a call; SP stays aligned.
u32 push_regs(Emitter& a, u32 gprs, u32 fprs);
void pop_regs(Emitter& a, u32 gprs, u32 fprs);

}  // namespace sm2::cpu::jit::a64
