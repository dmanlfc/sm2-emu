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

#include <initializer_list>

// Typed x86-64 encoder. Operand size comes from the registers (or Mem::size
// for mem,imm forms). Encodings match LLVM's byte for byte. 8-bit registers:
// AL..BL, SPL..DIL, R8B..R15B (no AH..BH). R11 is scratch for call()/jump().

namespace sm2::cpu::jit::x64 {

// -- Registers -----------------------------------------------------------------

struct Gp {
    u8 id   = 0;
    u8 size = 8;  ///< Operand size in bytes: 1, 2, 4 or 8.

    [[nodiscard]] constexpr Gp r64() const { return Gp{id, 8}; }
    [[nodiscard]] constexpr Gp r32() const { return Gp{id, 4}; }
    [[nodiscard]] constexpr Gp r16() const { return Gp{id, 2}; }
    [[nodiscard]] constexpr Gp r8() const { return Gp{id, 1}; }
    constexpr bool operator==(const Gp&) const = default;
};

#define SM2_X64_GP(n, q, d, w, b)                         \
    inline constexpr Gp q{n, 8}, d{n, 4}, w{n, 2}, b{n, 1};
SM2_X64_GP(0, RAX, EAX, AX, AL)
SM2_X64_GP(1, RCX, ECX, CX, CL)
SM2_X64_GP(2, RDX, EDX, DX, DL)
SM2_X64_GP(3, RBX, EBX, BX, BL)
SM2_X64_GP(4, RSP, ESP, SP, SPL)
SM2_X64_GP(5, RBP, EBP, BP, BPL)
SM2_X64_GP(6, RSI, ESI, SI, SIL)
SM2_X64_GP(7, RDI, EDI, DI, DIL)
SM2_X64_GP(8, R8, R8D, R8W, R8B)
SM2_X64_GP(9, R9, R9D, R9W, R9B)
SM2_X64_GP(10, R10, R10D, R10W, R10B)
SM2_X64_GP(11, R11, R11D, R11W, R11B)
SM2_X64_GP(12, R12, R12D, R12W, R12B)
SM2_X64_GP(13, R13, R13D, R13W, R13B)
SM2_X64_GP(14, R14, R14D, R14W, R14B)
SM2_X64_GP(15, R15, R15D, R15W, R15B)
#undef SM2_X64_GP

struct Xmm {
    u8 id = 0;
    constexpr bool operator==(const Xmm&) const = default;
};
inline constexpr Xmm XMM0{0}, XMM1{1}, XMM2{2}, XMM3{3}, XMM4{4}, XMM5{5}, XMM6{6}, XMM7{7}, XMM8{8},
    XMM9{9}, XMM10{10}, XMM11{11}, XMM12{12}, XMM13{13}, XMM14{14}, XMM15{15};

enum class Cond : u8 {
    O, NO, B, AE, E, NE, BE, A, S, NS, P, NP, L, GE, LE, G,
    C = B, NC = AE, Z = E, NZ = NE, NA = BE, NBE = A, PE = P, PO = NP, NGE = L, NL = GE, NG = LE, NLE = G,
};
[[nodiscard]] constexpr Cond invert(Cond c) { return static_cast<Cond>(static_cast<u8>(c) ^ 1U); }

// -- Memory operands ---------------------------------------------------------------

struct Mem {
    Gp    base{};
    Gp    index{};
    u8    scale     = 1;
    s32   disp      = 0;
    u8    size      = 0;  ///< 0 = take from the register operand.
    bool  has_base  = false;
    bool  has_index = false;
    bool  rip       = false;  ///< [rip + disp32] to `target` or `label`.
    const void* target = nullptr;
    Label label{};
};

[[nodiscard]] constexpr Mem ptr(Gp base, s32 disp = 0)
{
    Mem m;
    m.base     = base.r64();
    m.disp     = disp;
    m.has_base = true;
    return m;
}
[[nodiscard]] constexpr Mem ptr(Gp base, Gp index, u8 scale, s32 disp = 0)
{
    Mem m       = ptr(base, disp);
    m.index     = index.r64();
    m.scale     = scale;
    m.has_index = true;
    return m;
}
/// [index * scale + disp32] with no base.
[[nodiscard]] constexpr Mem ptr_index(Gp index, u8 scale, s32 disp = 0)
{
    Mem m;
    m.index     = index.r64();
    m.scale     = scale;
    m.disp      = disp;
    m.has_index = true;
    return m;
}
/// RIP-relative to an absolute address (must be within +-2 GiB) or a label.
[[nodiscard]] constexpr Mem rip(const void* target)
{
    Mem m;
    m.rip    = true;
    m.target = target;
    return m;
}
[[nodiscard]] constexpr Mem rip(Label l)
{
    Mem m;
    m.rip   = true;
    m.label = l;
    return m;
}
[[nodiscard]] constexpr Mem sized(Mem m, u8 size)
{
    m.size = size;
    return m;
}
template <typename... A> [[nodiscard]] constexpr Mem byte_ptr(A... a) { return sized(ptr(a...), 1); }
template <typename... A> [[nodiscard]] constexpr Mem word_ptr(A... a) { return sized(ptr(a...), 2); }
template <typename... A> [[nodiscard]] constexpr Mem dword_ptr(A... a) { return sized(ptr(a...), 4); }
template <typename... A> [[nodiscard]] constexpr Mem qword_ptr(A... a) { return sized(ptr(a...), 8); }

/// Can a rel32 branch at `from` (the address after the instruction) reach `to`?
[[nodiscard]] bool rel32_reachable(const void* from, const void* to);
/// The 5-byte `jmp rel32` at execution address `at` jumping to `to`.
void encode_jmp_rel32(u8 out[5], const void* at, const void* to);

// -- CPU features -------------------------------------------------------------------

/// What the host x86-64 supports beyond the baseline (SSE2). All false on
/// non-x86 hosts, where generated x86 code is only ever inspected, not run.
struct Features {
    bool sse41  = false;
    bool popcnt = false;
    bool lzcnt  = false;  ///< ABM / LZCNT
    bool bmi1   = false;  ///< TZCNT, ANDN
    bool bmi2   = false;
    bool movbe  = false;
    bool avx    = false;
    bool fma    = false;
};
[[nodiscard]] const Features& host_features();

// -- The emitter ---------------------------------------------------------------------

enum class AluOp : u8 { Add = 0, Or = 1, Adc = 2, Sbb = 3, And = 4, Sub = 5, Xor = 6, Cmp = 7 };
enum class ShiftOp : u8 { Rol = 0, Ror = 1, Rcl = 2, Rcr = 3, Shl = 4, Shr = 5, Sar = 7 };

class Emitter : public EmitterBase {
public:
    using EmitterBase::EmitterBase;

    // ---- Integer ALU --------------------------------------------------------

    void alu(AluOp op, Gp d, Gp s);
    void alu(AluOp op, Gp d, const Mem& s);
    void alu(AluOp op, const Mem& d, Gp s);
    void alu(AluOp op, Gp d, s32 imm);
    void alu(AluOp op, const Mem& d, s32 imm);

#define SM2_X64_ALU(name, op)                                                  \
    void name(Gp d, Gp s) { alu(AluOp::op, d, s); }                            \
    void name(Gp d, const Mem& s) { alu(AluOp::op, d, s); }                    \
    void name(const Mem& d, Gp s) { alu(AluOp::op, d, s); }                    \
    void name(Gp d, s32 imm) { alu(AluOp::op, d, imm); }                       \
    void name(const Mem& d, s32 imm) { alu(AluOp::op, d, imm); }
    SM2_X64_ALU(add, Add)
    SM2_X64_ALU(or_, Or)
    SM2_X64_ALU(adc, Adc)
    SM2_X64_ALU(sbb, Sbb)
    SM2_X64_ALU(and_, And)
    SM2_X64_ALU(sub, Sub)
    SM2_X64_ALU(xor_, Xor)
    SM2_X64_ALU(cmp, Cmp)
#undef SM2_X64_ALU

    void test(Gp a, Gp b);
    void test(const Mem& a, Gp b);
    void test(Gp a, s32 imm);
    void test(const Mem& a, s32 imm);

    void inc(Gp r) { unary(0xFE, 0, r); }
    void dec(Gp r) { unary(0xFE, 1, r); }
    void not_(Gp r) { unary(0xF6, 2, r); }
    void neg(Gp r) { unary(0xF6, 3, r); }
    /// RDX:RAX = RAX * r (unsigned / signed), and the divide counterparts.
    void mul(Gp r) { unary(0xF6, 4, r); }
    void imul(Gp r) { unary(0xF6, 5, r); }
    void div(Gp r) { unary(0xF6, 6, r); }
    void idiv(Gp r) { unary(0xF6, 7, r); }
    void imul(Gp d, Gp s);
    void imul(Gp d, const Mem& s);
    void imul(Gp d, Gp s, s32 imm);
    void cdq() { emit8(0x99); }
    void cmc() { emit8(0xF5); }
    void cqo() { emit8(0x48); emit8(0x99); }

    // ---- Shifts and rotates ---------------------------------------------------

    void shift(ShiftOp op, Gp r, u8 amount);
    /// Shift by CL.
    void shift_cl(ShiftOp op, Gp r);
    void shl(Gp r, u8 n) { shift(ShiftOp::Shl, r, n); }
    void shr(Gp r, u8 n) { shift(ShiftOp::Shr, r, n); }
    void sar(Gp r, u8 n) { shift(ShiftOp::Sar, r, n); }
    void rol(Gp r, u8 n) { shift(ShiftOp::Rol, r, n); }
    void ror(Gp r, u8 n) { shift(ShiftOp::Ror, r, n); }
    void rcl(Gp r, u8 n) { shift(ShiftOp::Rcl, r, n); }
    void rcr(Gp r, u8 n) { shift(ShiftOp::Rcr, r, n); }
    void shl_cl(Gp r) { shift_cl(ShiftOp::Shl, r); }
    void shr_cl(Gp r) { shift_cl(ShiftOp::Shr, r); }
    void sar_cl(Gp r) { shift_cl(ShiftOp::Sar, r); }
    void rol_cl(Gp r) { shift_cl(ShiftOp::Rol, r); }
    void ror_cl(Gp r) { shift_cl(ShiftOp::Ror, r); }
    /// Double-precision shifts: d = d:s shifted.
    void shld(Gp d, Gp s, u8 n);
    void shrd(Gp d, Gp s, u8 n);

    // ---- Moves ------------------------------------------------------------------

    void mov(Gp d, Gp s);
    void mov(Gp d, const Mem& s);
    void mov(const Mem& d, Gp s);
    void mov(const Mem& d, s32 imm);
    /// Shortest MOV for the immediate (never XOR: flags are preserved).
    void mov(Gp d, u64 imm);
    /// Absolute address: LEA rip-relative when within +-2 GiB, else MOVABS.
    void mov_address(Gp d, const void* p);
    void movabs(Gp d, u64 imm);
    void movzx(Gp d, Gp s);
    void movzx(Gp d, const Mem& s);  ///< s.size must be 1 or 2.
    void movsx(Gp d, Gp s);
    void movsx(Gp d, const Mem& s);  ///< s.size must be 1, 2 or 4 (MOVSXD).
    void lea(Gp d, const Mem& s);
    void xchg(Gp a, Gp b);
    void cmov(Cond c, Gp d, Gp s);
    void cmov(Cond c, Gp d, const Mem& s);
    void set(Cond c, Gp d8);

    // ---- Bit operations ---------------------------------------------------------

    void bswap(Gp r);
    void movbe(Gp d, const Mem& s);
    void movbe(const Mem& d, Gp s);
    void bsr(Gp d, Gp s) { rm_op(0, {0x0F, 0xBD}, d, s); }
    void bsf(Gp d, Gp s) { rm_op(0, {0x0F, 0xBC}, d, s); }
    void lzcnt(Gp d, Gp s) { rm_op(0xF3, {0x0F, 0xBD}, d, s); }
    void tzcnt(Gp d, Gp s) { rm_op(0xF3, {0x0F, 0xBC}, d, s); }
    void popcnt(Gp d, Gp s) { rm_op(0xF3, {0x0F, 0xB8}, d, s); }
    void bt(Gp a, Gp bitreg) { bt_rr(0xA3, a, bitreg); }
    void bts(Gp a, Gp bitreg) { bt_rr(0xAB, a, bitreg); }
    void btr(Gp a, Gp bitreg) { bt_rr(0xB3, a, bitreg); }
    void btc(Gp a, Gp bitreg) { bt_rr(0xBB, a, bitreg); }
    void bt(Gp a, u8 bit) { bt_ri(4, a, bit); }
    void bts(Gp a, u8 bit) { bt_ri(5, a, bit); }
    void btr(Gp a, u8 bit) { bt_ri(6, a, bit); }
    void btc(Gp a, u8 bit) { bt_ri(7, a, bit); }
    /// LZCNT, or BSR + fix-up without it. Clobbers flags.
    void clz(Gp d, Gp s, bool have_lzcnt);

    // ---- Stack, control flow ----------------------------------------------------

    void push(Gp r);
    void pop(Gp r);
    void ret() { emit8(0xC3); }
    void int3() { emit8(0xCC); }
    void ud2() { emit8(0x0F); emit8(0x0B); }
    void nop() { emit8(0x90); }
    /// Recommended multi-byte NOPs up to `n` bytes total.
    void nop(usize n);
    void align(usize alignment);
    void mfence() { emit8(0x0F); emit8(0xAE); emit8(0xF0); }
    void lfence() { emit8(0x0F); emit8(0xAE); emit8(0xE8); }
    void sfence() { emit8(0x0F); emit8(0xAE); emit8(0xF8); }

    /// Labels: rel8 when the (bound) target fits, rel32 otherwise and for all
    /// forward references.
    void jmp(Label l);
    void j(Cond c, Label l);
    void call(Label l);
    /// Forward rel8 jumps, for short skips the caller knows are short.
    void jmp_short(Label l);
    void j_short(Cond c, Label l);
    void jmp(Gp r);
    void call(Gp r);
    void jmp(const Mem& m);
    void call(const Mem& m);
    /// rel32 to an absolute address; error if out of range.
    void jmp(const void* target);
    void j(Cond c, const void* target);
    /// CALL rel32 if reachable, else MOVABS `scratch` + CALL `scratch`.
    void call(const void* fn, Gp scratch = R11);
    template <typename R, typename... A>
    void call(R (*fn)(A...), Gp scratch = R11)
    {
        call(reinterpret_cast<const void*>(fn), scratch);
    }
    /// JMP rel32 if reachable, else MOVABS `scratch` + JMP `scratch`.
    void jump(const void* target, Gp scratch = R11);

    // ---- SSE / SSE2 scalar floating point ----------------------------------------

    void movss(Xmm d, Xmm s) { sse(0xF3, 0x10, d.id, s.id); }
    void movsd(Xmm d, Xmm s) { sse(0xF2, 0x10, d.id, s.id); }
    void movss(Xmm d, const Mem& s) { sse(0xF3, 0x10, d.id, s); }
    void movsd(Xmm d, const Mem& s) { sse(0xF2, 0x10, d.id, s); }
    void movss(const Mem& d, Xmm s) { sse(0xF3, 0x11, s.id, d); }
    void movsd(const Mem& d, Xmm s) { sse(0xF2, 0x11, s.id, d); }
    void movaps(Xmm d, Xmm s) { sse(0, 0x28, d.id, s.id); }
    void movapd(Xmm d, Xmm s) { sse(0x66, 0x28, d.id, s.id); }
    void movups(Xmm d, const Mem& s) { sse(0, 0x10, d.id, s); }
    void movups(const Mem& d, Xmm s) { sse(0, 0x11, s.id, d); }
    void movdqu(Xmm d, const Mem& s) { sse(0xF3, 0x6F, d.id, s); }
    void movdqu(const Mem& d, Xmm s) { sse(0xF3, 0x7F, s.id, d); }

#define SM2_X64_SSE_ARITH(name, pfx, op)                              \
    void name(Xmm d, Xmm s) { sse(pfx, op, d.id, s.id); }             \
    void name(Xmm d, const Mem& s) { sse(pfx, op, d.id, s); }
    SM2_X64_SSE_ARITH(addss, 0xF3, 0x58)
    SM2_X64_SSE_ARITH(addsd, 0xF2, 0x58)
    SM2_X64_SSE_ARITH(subss, 0xF3, 0x5C)
    SM2_X64_SSE_ARITH(subsd, 0xF2, 0x5C)
    SM2_X64_SSE_ARITH(mulss, 0xF3, 0x59)
    SM2_X64_SSE_ARITH(mulsd, 0xF2, 0x59)
    SM2_X64_SSE_ARITH(divss, 0xF3, 0x5E)
    SM2_X64_SSE_ARITH(divsd, 0xF2, 0x5E)
    SM2_X64_SSE_ARITH(sqrtss, 0xF3, 0x51)
    SM2_X64_SSE_ARITH(sqrtsd, 0xF2, 0x51)
    SM2_X64_SSE_ARITH(minss, 0xF3, 0x5D)
    SM2_X64_SSE_ARITH(minsd, 0xF2, 0x5D)
    SM2_X64_SSE_ARITH(maxss, 0xF3, 0x5F)
    SM2_X64_SSE_ARITH(maxsd, 0xF2, 0x5F)
    SM2_X64_SSE_ARITH(rcpss, 0xF3, 0x53)
    SM2_X64_SSE_ARITH(rsqrtss, 0xF3, 0x52)
    SM2_X64_SSE_ARITH(cvtss2sd, 0xF3, 0x5A)
    SM2_X64_SSE_ARITH(cvtsd2ss, 0xF2, 0x5A)
    SM2_X64_SSE_ARITH(ucomiss, 0, 0x2E)
    SM2_X64_SSE_ARITH(ucomisd, 0x66, 0x2E)
    SM2_X64_SSE_ARITH(comiss, 0, 0x2F)
    SM2_X64_SSE_ARITH(comisd, 0x66, 0x2F)
    SM2_X64_SSE_ARITH(andps, 0, 0x54)
    SM2_X64_SSE_ARITH(andpd, 0x66, 0x54)
    SM2_X64_SSE_ARITH(andnps, 0, 0x55)
    SM2_X64_SSE_ARITH(andnpd, 0x66, 0x55)
    SM2_X64_SSE_ARITH(orps, 0, 0x56)
    SM2_X64_SSE_ARITH(orpd, 0x66, 0x56)
    SM2_X64_SSE_ARITH(xorps, 0, 0x57)
    SM2_X64_SSE_ARITH(xorpd, 0x66, 0x57)
    SM2_X64_SSE_ARITH(pxor, 0x66, 0xEF)
#undef SM2_X64_SSE_ARITH

    /// Integer -> FP; the GPR's size (4 or 8) selects the source width.
    void cvtsi2ss(Xmm d, Gp s) { sse_w(0xF3, 0x2A, d.id, s); }
    void cvtsi2sd(Xmm d, Gp s) { sse_w(0xF2, 0x2A, d.id, s); }
    /// FP -> integer, truncating (cvtt) or using MXCSR rounding (cvt).
    void cvttss2si(Gp d, Xmm s) { sse_w(0xF3, 0x2C, s.id, d, true); }
    void cvttsd2si(Gp d, Xmm s) { sse_w(0xF2, 0x2C, s.id, d, true); }
    void cvtss2si(Gp d, Xmm s) { sse_w(0xF3, 0x2D, s.id, d, true); }
    void cvtsd2si(Gp d, Xmm s) { sse_w(0xF2, 0x2D, s.id, d, true); }
    /// GPR <-> XMM bit moves; the GPR's size picks MOVD (4) or MOVQ (8).
    void movd(Xmm d, Gp s) { sse_w(0x66, 0x6E, d.id, s); }
    void movd(Gp d, Xmm s) { sse_w(0x66, 0x7E, s.id, d); }
    /// SSE4.1 ROUNDSS / ROUNDSD; mode 0 nearest, 1 down, 2 up, 3 truncate,
    /// 4 MXCSR (| 8 to suppress the precision exception).
    void roundss(Xmm d, Xmm s, u8 mode);
    void roundsd(Xmm d, Xmm s, u8 mode);
    void ldmxcsr(const Mem& m);
    void stmxcsr(const Mem& m);
    /// FMA3: d = s2 * d + s3 (VEX.66.0F38.W1 A9).
    void vfmadd213sd(Xmm d, Xmm s2, Xmm s3);

protected:
    bool patch_fixup(usize pos, u32 kind, usize target) override;

private:
    static constexpr u32 kFixRel32 = 0, kFixRel8 = 1, kFixRip = 2;

    struct Opcode {
        u8 b[3];
        u8 n;
        Opcode(std::initializer_list<u8> l)
            : b{0, 0, 0}
            , n(static_cast<u8>(l.size()))
        {
            u8 i = 0;
            for (const u8 v : l) b[i++] = v;
        }
        Opcode(u8 v) : b{v, 0, 0}, n(1) {}
    };

    // The one place instructions are put together: [66] [prefix] [REX]
    // opcode ModRM [SIB] [disp] [imm].
    void encode(u8 prefix, u8 size, const Opcode& op, u8 reg, bool rm_is_mem, u8 rm_reg, const Mem* m,
                usize imm_bytes, s64 imm, bool rex_w_from_size = true, bool force_rex = false);
    void enc_rr(u8 prefix, u8 size, const Opcode& op, u8 reg, u8 rm, usize imm_bytes = 0, s64 imm = 0,
                bool force_rex = false)
    {
        encode(prefix, size, op, reg, false, rm, nullptr, imm_bytes, imm, true, force_rex);
    }
    void enc_rm(u8 prefix, u8 size, const Opcode& op, u8 reg, const Mem& m, usize imm_bytes = 0, s64 imm = 0,
                bool force_rex = false)
    {
        encode(prefix, size, op, reg, true, 0, &m, imm_bytes, imm, true, force_rex);
    }
    [[nodiscard]] static bool byte_reg_needs_rex(Gp r) { return r.size == 1 && r.id >= 4 && r.id < 8; }

    void unary(u8 op8, u8 ext, Gp r);
    void rm_op(u8 prefix, const Opcode& op, Gp d, Gp s);
    void bt_rr(u8 op, Gp a, Gp b);
    void bt_ri(u8 ext, Gp a, u8 bit);
    void sse(u8 prefix, u8 op, u8 reg, u8 rm);
    void sse(u8 prefix, u8 op, u8 reg, const Mem& m);
    void sse_w(u8 prefix, u8 op, u8 xmm, Gp g, bool gpr_is_reg = false);
    void jump_label(u8 short_op, const Opcode& near_op, Label l, bool force_short);
};

// -- Calling conventions ------------------------------------------------------------

enum class Abi : u8 { SysV, Win64 };
#if defined(_WIN32)
inline constexpr Abi kHostAbi = Abi::Win64;
#else
inline constexpr Abi kHostAbi = Abi::SysV;
#endif

struct AbiInfo {
    Gp  args[6];
    u8  num_args;
    u16 callee_saved_gprs;  ///< bit n = register n
    u16 callee_saved_xmms;
    u8  shadow_space;       ///< Bytes the caller reserves above the return address.
};
[[nodiscard]] const AbiInfo& abi_info(Abi abi = kHostAbi);

/// Frame: pushes RBP (frame pointer) and the callee-saved `gprs`, saves
/// `xmms` (Win64 XMM6-15), leaves RSP 16-aligned with shadow space, and
/// `locals` bytes at [RSP + locals_offset()].
class Frame {
public:
    Frame(Abi abi, u16 gprs, u16 xmms, u32 locals);

    [[nodiscard]] u32 locals_offset() const { return m_locals_offset; }
    void prologue(Emitter& a) const;
    void epilogue(Emitter& a, bool with_ret = true) const;

private:
    Abi m_abi;
    u16 m_gprs, m_xmms;
    u32 m_sub = 0;            ///< Bytes subtracted from RSP after the pushes.
    u32 m_xmm_offset    = 0;  ///< Where the XMM saves start, from RSP.
    u32 m_locals_offset = 0;
};

}  // namespace sm2::cpu::jit::x64
