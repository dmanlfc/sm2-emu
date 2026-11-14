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
#include "cpu/jit/arm64_emitter.h"

#include <bit>
#include <cstdint>
#include <cstring>

namespace sm2::cpu::jit::a64 {

// -- Encoding utilities ----------------------------------------------------------

std::optional<u32> encode_bitmask(u64 value, bool is64)
{
    if (!is64) {
        value &= 0xFFFFFFFFULL;
        value |= value << 32;
    }
    if (value == 0 || value == ~u64{0}) return std::nullopt;

    // The smallest element size the value is a replication of.
    unsigned size = 64;
    while (size > 2) {
        const unsigned half = size / 2;
        const u64      mask = (u64{1} << half) - 1;
        if ((value & mask) != ((value >> half) & mask)) break;
        size = half;
    }
    const u64      mask = size == 64 ? ~u64{0} : (u64{1} << size) - 1;
    const u64      elem = value & mask;
    const unsigned ones = static_cast<unsigned>(std::popcount(elem));
    // 0 < ones < size here: an all-zero or all-one element would make the
    // whole value 0 or ~0, rejected above.
    const u64 run = (u64{1} << ones) - 1;

    // The element must be the run rotated right by some immr.
    for (unsigned r = 0; r < size; ++r) {
        const u64 rotated = r == 0 ? run : (((run >> r) | (run << (size - r))) & mask);
        if (rotated == elem) {
            const u32 n    = size == 64 ? 1U : 0U;
            const u32 imms = ((~(size - 1) << 1) & 0x3FU) | (ones - 1);
            return (n << 12) | (r << 6) | imms;
        }
    }
    return std::nullopt;
}

std::optional<u64> decode_bitmask(u32 n, u32 immr, u32 imms, bool is64)
{
    if (!is64 && n != 0) return std::nullopt;
    const u32 combined = (n << 6) | (~imms & 0x3FU);
    if (combined == 0) return std::nullopt;
    const unsigned len = 31 - static_cast<unsigned>(std::countl_zero(combined));
    if (len < 1) return std::nullopt;
    const u32 levels = (1U << len) - 1;
    if ((imms & levels) == levels) return std::nullopt;
    const unsigned s     = imms & levels;
    const unsigned r     = immr & levels;
    const unsigned esize = 1U << len;
    const u64      emask = esize == 64 ? ~u64{0} : (u64{1} << esize) - 1;
    const u64      welem = (s + 1 == 64) ? ~u64{0} : (u64{1} << (s + 1)) - 1;
    const u64      elem  = r == 0 ? welem : (((welem >> r) | (welem << (esize - r))) & emask);
    u64            out   = 0;
    for (unsigned i = 0; i < 64; i += esize) out |= elem << i;
    return is64 ? out : (out & 0xFFFFFFFFULL);
}

std::optional<u32> encode_fp_imm8(double value)
{
    const u64 want = std::bit_cast<u64>(value);
    for (u32 imm8 = 0; imm8 < 256; ++imm8) {
        const u64 sign = (imm8 >> 7) & 1;
        const u64 b    = (imm8 >> 6) & 1;
        const u64 cd   = (imm8 >> 4) & 3;
        const u64 efgh = imm8 & 15;
        const u64 exp  = ((b ^ 1) << 10) | (b != 0 ? (0xFFULL << 2) : 0) | cd;
        const u64 bits = (sign << 63) | (exp << 52) | (efgh << 48);
        if (bits == want) return imm8;
    }
    return std::nullopt;
}

bool branch26_reachable(const void* from, const void* to)
{
    const s64 delta = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from);
    return (delta & 3) == 0 && delta >= -(s64{1} << 27) && delta < (s64{1} << 27);
}

u32 encode_b(const void* from, const void* to)
{
    const s64 delta = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from);
    return 0x14000000U | (static_cast<u32>(delta >> 2) & 0x03FFFFFFU);
}

// -- Helpers ---------------------------------------------------------------------

u32 Emitter::ftype(VSize s)
{
    switch (s) {
    case VSize::S: return 0;
    case VSize::D: return 1;
    case VSize::H: return 3;
    default: return 0;
    }
}

void Emitter::check_same(Reg a, Reg b)
{
    if (a.x != b.x) m_error = true;
}

void Emitter::check_same(Reg a, Reg b, Reg c)
{
    if (a.x != b.x || a.x != c.x) m_error = true;
}

u32 Emitter::tb(u32 base, Reg t, u32 bit)
{
    if (bit >= (t.x ? 64U : 32U)) m_error = true;
    return base | ((bit >> 5) << 31) | ((bit & 31) << 19) | t.id;
}

// -- Moves ---------------------------------------------------------------------

void Emitter::mov(Reg d, Reg n)
{
    check_same(d, n);
    if (d.sp || n.sp) {
        emit32(0x11000000U | sf(d) | (n.id << 5) | d.id);  // ADD d, n, #0
    } else {
        emit32(0x2A0003E0U | sf(d) | (n.id << 16) | d.id);  // ORR d, ZR, n
    }
}

void Emitter::movz(Reg d, u32 imm16, u32 shift)
{
    if ((shift & 15) != 0 || shift >= bits(d) || imm16 > 0xFFFF) m_error = true;
    emit32(0x52800000U | sf(d) | ((shift / 16) << 21) | ((imm16 & 0xFFFF) << 5) | d.id);
}

void Emitter::movn(Reg d, u32 imm16, u32 shift)
{
    if ((shift & 15) != 0 || shift >= bits(d) || imm16 > 0xFFFF) m_error = true;
    emit32(0x12800000U | sf(d) | ((shift / 16) << 21) | ((imm16 & 0xFFFF) << 5) | d.id);
}

void Emitter::movk(Reg d, u32 imm16, u32 shift)
{
    if ((shift & 15) != 0 || shift >= bits(d) || imm16 > 0xFFFF) m_error = true;
    emit32(0x72800000U | sf(d) | ((shift / 16) << 21) | ((imm16 & 0xFFFF) << 5) | d.id);
}

void Emitter::mov(Reg d, u64 imm)
{
    if (d.sp) {
        m_error = true;
        return;
    }
    const unsigned halves = d.x ? 4 : 2;
    if (!d.x) imm &= 0xFFFFFFFFULL;

    unsigned zero = 0;
    unsigned ones = 0;
    for (unsigned i = 0; i < halves; ++i) {
        const u32 h = static_cast<u32>(imm >> (16 * i)) & 0xFFFF;
        zero += h == 0 ? 1 : 0;
        ones += h == 0xFFFF ? 1 : 0;
    }

    // One instruction: MOVZ, MOVN, or ORR with a bitmask immediate.
    if (halves - zero <= 1) {
        for (unsigned i = 0; i < halves; ++i) {
            const u32 h = static_cast<u32>(imm >> (16 * i)) & 0xFFFF;
            if (h != 0) {
                movz(d, h, 16 * i);
                return;
            }
        }
        movz(d, 0, 0);
        return;
    }
    if (halves - ones <= 1) {
        for (unsigned i = 0; i < halves; ++i) {
            const u32 h = static_cast<u32>(imm >> (16 * i)) & 0xFFFF;
            if (h != 0xFFFF) {
                movn(d, ~h & 0xFFFF, 16 * i);
                return;
            }
        }
        movn(d, 0, 0);  // all ones; only reachable for W (ORR can't encode ~0)
        return;
    }
    if (const auto bm = encode_bitmask(imm, d.x)) {
        emit32(0x32000000U | sf(d) | (*bm << 10) | (31U << 5) | d.id);
        return;
    }

    // MOVZ or MOVN for the first interesting halfword, MOVK for the rest.
    const bool     use_n = ones > zero;
    const u32      skip  = use_n ? 0xFFFF : 0;
    bool           first = true;
    for (unsigned i = 0; i < halves; ++i) {
        const u32 h = static_cast<u32>(imm >> (16 * i)) & 0xFFFF;
        if (h == skip) continue;
        if (first) {
            if (use_n) {
                movn(d, ~h & 0xFFFF, 16 * i);
            } else {
                movz(d, h, 16 * i);
            }
            first = false;
        } else {
            movk(d, h, 16 * i);
        }
    }
}

void Emitter::mov_address(Reg d, const void* p)
{
    const auto target = reinterpret_cast<intptr_t>(p);
    const auto here   = reinterpret_cast<intptr_t>(cursor());
    const s64  delta  = target - here;
    if (delta >= -(s64{1} << 20) && delta < (s64{1} << 20)) {
        adr(d.as_x(), p);
        return;
    }
    const s64 pages = (target >> 12) - (here >> 12);
    if (pages >= -(s64{1} << 20) && pages < (s64{1} << 20)) {
        adrp(d.as_x(), p);
        if ((target & 0xFFF) != 0) add(d.as_x(), d.as_x(), static_cast<u32>(target & 0xFFF));
        return;
    }
    mov(d.as_x(), static_cast<u64>(target));
}

// -- Add / subtract ---------------------------------------------------------------

void Emitter::addsub_shift(u32 op, Reg d, Reg n, Reg m, Shift sh, u32 amount)
{
    if ((d.sp && (op & 1) == 0) || n.sp) {
        // Only the extended-register form takes SP.
        if (sh != Shift::LSL || amount > 4) m_error = true;
        addsub_ext(op, d, n, m, d.x ? Extend::UXTX : Extend::UXTW, amount);
        return;
    }
    check_same(d, n, m);
    if (sh == Shift::ROR || amount >= bits(d) || m.sp) m_error = true;
    emit32(0x0B000000U | (op << 29) | sf(d) | (static_cast<u32>(sh) << 22) | (m.id << 16) |
           ((amount & 63) << 10) | (n.id << 5) | d.id);
}

void Emitter::addsub_ext(u32 op, Reg d, Reg n, Reg m, Extend ext, u32 amount)
{
    if (amount > 4) m_error = true;
    emit32(0x0B200000U | (op << 29) | sf(d) | (m.id << 16) | (static_cast<u32>(ext) << 13) |
           ((amount & 7) << 10) | (n.id << 5) | d.id);
}

bool Emitter::addsub_imm_encodable(u64 imm)
{
    return imm < 4096 || ((imm & 0xFFF) == 0 && imm < (u64{1} << 24));
}

void Emitter::addsub_imm(u32 op, Reg d, Reg n, u32 imm)
{
    u32 sh = 0;
    if (imm >= 4096) {
        if ((imm & 0xFFF) != 0 || imm >= (1U << 24)) m_error = true;
        imm >>= 12;
        sh = 1;
    }
    emit32(0x11000000U | (op << 29) | sf(d) | (sh << 22) | ((imm & 0xFFF) << 10) | (n.id << 5) | d.id);
}

void Emitter::add_imm(Reg d, Reg n, s64 imm, Reg scratch)
{
    const bool neg = imm < 0;
    const u64  mag = neg ? static_cast<u64>(-(imm + 1)) + 1 : static_cast<u64>(imm);
    const u32  op  = neg ? 2 : 0;
    if (mag == 0) {
        if (d != n) mov(d, n);
        return;
    }
    if (mag < 4096 || ((mag & 0xFFF) == 0 && mag < (u64{1} << 24))) {
        addsub_imm(op, d, n, static_cast<u32>(mag));
        return;
    }
    if (mag < (u64{1} << 24)) {
        addsub_imm(op, d, n, static_cast<u32>(mag & 0xFFF000));
        addsub_imm(op, d, d, static_cast<u32>(mag & 0xFFF));
        return;
    }
    const Reg s = d.x ? scratch.as_x() : scratch.w();
    mov(s, static_cast<u64>(imm));
    if (d.sp || n.sp) {
        addsub_ext(0, d, n, s, d.x ? Extend::UXTX : Extend::UXTW, 0);
    } else {
        addsub_shift(0, d, n, s, Shift::LSL, 0);
    }
}

// -- Logical -----------------------------------------------------------------------

void Emitter::logic_shift(u32 base, Reg d, Reg n, Reg m, Shift sh, u32 amount)
{
    check_same(d, n, m);
    if (amount >= bits(d) || d.sp || n.sp || m.sp) m_error = true;
    emit32(base | sf(d) | (static_cast<u32>(sh) << 22) | (m.id << 16) | ((amount & 63) << 10) | (n.id << 5) |
           d.id);
}

void Emitter::logic_imm(u32 base, Reg d, Reg n, u64 imm)
{
    check_same(d, n);
    const auto bm = encode_bitmask(imm, d.x);
    if (!bm) {
        m_error = true;
        emit32(0);
        return;
    }
    emit32(base | sf(d) | (*bm << 10) | (n.id << 5) | d.id);
}

void Emitter::logic_any(u32 opc, Reg d, Reg n, u64 imm, Reg scratch)
{
    static constexpr u32 kImm[4] = {0x12000000U, 0x32000000U, 0x52000000U, 0x72000000U};
    static constexpr u32 kReg[4] = {0x0A000000U, 0x2A000000U, 0x4A000000U, 0x6A000000U};
    if (encode_bitmask(imm, d.x)) {
        logic_imm(kImm[opc], d, n, imm);
        return;
    }
    const Reg s = d.x ? scratch.as_x() : scratch.w();
    mov(s, imm);
    logic_shift(kReg[opc], d, n, s, Shift::LSL, 0);
}

// -- Shifts, bitfields ---------------------------------------------------------------

void Emitter::lsl(Reg d, Reg n, u32 amount)
{
    const u32 size = bits(d);
    if (amount >= size) m_error = true;
    ubfm(d, n, (size - amount) % size, size - 1 - (amount % size));
}

void Emitter::lsr(Reg d, Reg n, u32 amount)
{
    if (amount >= bits(d)) m_error = true;
    ubfm(d, n, amount, bits(d) - 1);
}

void Emitter::asr(Reg d, Reg n, u32 amount)
{
    if (amount >= bits(d)) m_error = true;
    sbfm(d, n, amount, bits(d) - 1);
}

void Emitter::bitfield(u32 base, Reg d, Reg n, u32 immr, u32 imms)
{
    check_same(d, n);
    if (immr >= bits(d) || imms >= bits(d)) m_error = true;
    emit32(base | sf(d) | (d.x ? (1U << 22) : 0U) | ((immr & 63) << 16) | ((imms & 63) << 10) | (n.id << 5) |
           d.id);
}

void Emitter::extr(Reg d, Reg n, Reg m, u32 lsb)
{
    check_same(d, n, m);
    if (lsb >= bits(d)) m_error = true;
    emit32(0x13800000U | sf(d) | (d.x ? (1U << 22) : 0U) | (m.id << 16) | ((lsb & 63) << 10) | (n.id << 5) | d.id);
}

void Emitter::rr(u32 base, Reg d, Reg n)
{
    check_same(d, n);
    emit32(base | sf(d) | (n.id << 5) | d.id);
}

void Emitter::rrr(u32 base, Reg d, Reg n, Reg m)
{
    check_same(d, n, m);
    emit32(base | sf(d) | (m.id << 16) | (n.id << 5) | d.id);
}

void Emitter::rrrr(u32 base, Reg d, Reg n, Reg m, Reg a, bool x_only)
{
    check_same(d, n, m);
    if (x_only && !d.x) m_error = true;
    emit32(base | sf(d) | (m.id << 16) | (a.id << 10) | (n.id << 5) | d.id);
}

void Emitter::widening(u32 base, Reg d, Reg n, Reg m, Reg a)
{
    if (!d.x || !a.x || n.x || m.x) m_error = true;
    emit32(base | (m.id << 16) | (a.id << 10) | (n.id << 5) | d.id);
}

void Emitter::condsel(u32 base, Reg d, Reg n, Reg m, Cond c)
{
    check_same(d, n, m);
    emit32(base | sf(d) | (m.id << 16) | (static_cast<u32>(c) << 12) | (n.id << 5) | d.id);
}

void Emitter::condcmp(u32 base, Reg n, u32 m_or_imm, bool imm, u32 nzcv, Cond c, bool m_x)
{
    if (m_or_imm > 31 || nzcv > 15 || (!imm && m_x != n.x)) m_error = true;
    emit32(base | sf(n) | ((m_or_imm & 31) << 16) | (static_cast<u32>(c) << 12) | (n.id << 5) | (nzcv & 15));
}

// -- Loads and stores ------------------------------------------------------------------

void Emitter::ldst(u32 size, u32 opc, u32 rt, const Mem& m, bool simd)
{
    const u32 v     = simd ? (1U << 26) : 0U;
    const u32 scale = simd && size == 0 && opc >= 2 ? 4 : size;  // Q: 16 bytes
    const u32 head  = (size << 30) | v | (opc << 22) | (m.base.id << 5) | rt;
    switch (m.mode) {
    case Mem::Mode::Offset: {
        const s64 bytes = s64{1} << scale;
        if (m.offset >= 0 && (m.offset & (bytes - 1)) == 0 && (m.offset >> scale) < 4096) {
            emit32(0x39000000U | head | (static_cast<u32>(m.offset >> scale) << 10));
            return;
        }
        if (m.offset >= -256 && m.offset < 256) {
            emit32(0x38000000U | head | ((static_cast<u32>(m.offset) & 0x1FF) << 12));
            return;
        }
        m_error = true;
        emit32(0);
        return;
    }
    case Mem::Mode::PreIndex:
    case Mem::Mode::PostIndex:
        if (m.offset < -256 || m.offset >= 256) m_error = true;
        emit32(0x38000000U | head | ((static_cast<u32>(m.offset) & 0x1FF) << 12) |
               (m.mode == Mem::Mode::PreIndex ? 0xC00U : 0x400U));
        return;
    case Mem::Mode::RegOffset: {
        // UXTW and SXTW take a W index, LSL (UXTX) and SXTX an X index.
        const auto e = m.extend;
        if (e != Extend::UXTW && e != Extend::SXTW && e != Extend::UXTX && e != Extend::SXTX) m_error = true;
        const bool want_x = e == Extend::UXTX || e == Extend::SXTX;
        if (m.index.x != want_x) m_error = true;
        emit32(0x38200800U | head | (m.index.id << 16) | (static_cast<u32>(e) << 13) | (m.shifted ? (1U << 12) : 0U));
        return;
    }
    }
}

namespace {
// size and opc fields for a SIMD&FP load or store of this register size.
void vsize_fields(VSize s, u32 load, u32& size, u32& opc)
{
    switch (s) {
    case VSize::B: size = 0; opc = load; return;
    case VSize::H: size = 1; opc = load; return;
    case VSize::S: size = 2; opc = load; return;
    case VSize::D: size = 3; opc = load; return;
    case VSize::Q: size = 0; opc = 2 + load; return;
    }
}
}  // namespace

void Emitter::ldr(VReg t, const Mem& m)
{
    u32 size = 0, opc = 0;
    vsize_fields(t.size, 1, size, opc);
    ldst(size, opc, t.id, m, true);
}

void Emitter::str(VReg t, const Mem& m)
{
    u32 size = 0, opc = 0;
    vsize_fields(t.size, 0, size, opc);
    ldst(size, opc, t.id, m, true);
}

void Emitter::pair(u32 opc, u32 load, bool simd, u32 t1, u32 t2, u32 scale, const Mem& m)
{
    u32 idx = 2;
    switch (m.mode) {
    case Mem::Mode::Offset: idx = 2; break;
    case Mem::Mode::PreIndex: idx = 3; break;
    case Mem::Mode::PostIndex: idx = 1; break;
    case Mem::Mode::RegOffset: m_error = true; break;
    }
    const s64 imm = m.offset / static_cast<s64>(scale);
    if (m.offset % static_cast<s64>(scale) != 0 || imm < -64 || imm > 63) m_error = true;
    emit32((opc << 30) | 0x28000000U | (simd ? (1U << 26) : 0U) | (idx << 23) | (load << 22) |
           ((static_cast<u32>(imm) & 0x7F) << 15) | (t2 << 10) | (m.base.id << 5) | t1);
}

void Emitter::ldp(VReg t1, VReg t2, const Mem& m)
{
    const u32 opc   = t1.size == VSize::S ? 0 : t1.size == VSize::D ? 1 : 2;
    const u32 scale = t1.size == VSize::S ? 4 : t1.size == VSize::D ? 8 : 16;
    if (t1.size != t2.size || t1.size == VSize::B || t1.size == VSize::H) m_error = true;
    pair(opc, 1, true, t1.id, t2.id, scale, m);
}

void Emitter::stp(VReg t1, VReg t2, const Mem& m)
{
    const u32 opc   = t1.size == VSize::S ? 0 : t1.size == VSize::D ? 1 : 2;
    const u32 scale = t1.size == VSize::S ? 4 : t1.size == VSize::D ? 8 : 16;
    if (t1.size != t2.size || t1.size == VSize::B || t1.size == VSize::H) m_error = true;
    pair(opc, 0, true, t1.id, t2.id, scale, m);
}

void Emitter::literal(u32 base, u32 rt, Label l)
{
    branch_label(base | rt, l, kFix19);
}

void Emitter::ldr(VReg t, Label l)
{
    const u32 base = t.size == VSize::S ? 0x1C000000U : t.size == VSize::D ? 0x5C000000U : 0x9C000000U;
    if (t.size != VSize::S && t.size != VSize::D && t.size != VSize::Q) m_error = true;
    literal(base, t.id, l);
}

void Emitter::adr(Reg d, Label l)
{
    branch_label(0x10000000U | d.id, l, kFixAdr);
}

void Emitter::adr(Reg d, const void* p)
{
    branch_abs(0x10000000U | d.id, p, kFixAdr);
}

void Emitter::adrp(Reg d, const void* p)
{
    const s64 pages = (reinterpret_cast<intptr_t>(p) >> 12) - (reinterpret_cast<intptr_t>(cursor()) >> 12);
    if (pages < -(s64{1} << 20) || pages >= (s64{1} << 20)) m_error = true;
    const u32 imm = static_cast<u32>(pages) & 0x1FFFFF;
    emit32(0x90000000U | ((imm & 3) << 29) | ((imm >> 2) << 5) | d.id);
}

// -- Branches ----------------------------------------------------------------------

bool Emitter::encode_rel(u32& insn, u32 kind, s64 delta)
{
    switch (kind) {
    case kFix26:
        if ((delta & 3) != 0 || delta < -(s64{1} << 27) || delta >= (s64{1} << 27)) return false;
        insn = (insn & ~0x03FFFFFFU) | (static_cast<u32>(delta >> 2) & 0x03FFFFFFU);
        return true;
    case kFix19:
        if ((delta & 3) != 0 || delta < -(s64{1} << 20) || delta >= (s64{1} << 20)) return false;
        insn = (insn & ~(0x7FFFFU << 5)) | ((static_cast<u32>(delta >> 2) & 0x7FFFFU) << 5);
        return true;
    case kFix14:
        if ((delta & 3) != 0 || delta < -(s64{1} << 15) || delta >= (s64{1} << 15)) return false;
        insn = (insn & ~(0x3FFFU << 5)) | ((static_cast<u32>(delta >> 2) & 0x3FFFU) << 5);
        return true;
    case kFixAdr: {
        if (delta < -(s64{1} << 20) || delta >= (s64{1} << 20)) return false;
        const u32 imm = static_cast<u32>(delta) & 0x1FFFFF;
        insn          = (insn & ~((3U << 29) | (0x7FFFFU << 5))) | ((imm & 3) << 29) | ((imm >> 2) << 5);
        return true;
    }
    default: return false;
    }
}

bool Emitter::patch_fixup(usize pos, u32 kind, usize target)
{
    if (!in_span(pos, 4)) return true;  // overflowed; reported separately
    u32 insn = read32_at(pos);
    if (!encode_rel(insn, kind, static_cast<s64>(target) - static_cast<s64>(pos))) return false;
    write32_at(pos, insn);
    return true;
}

void Emitter::branch_label(u32 insn, Label l, u32 kind)
{
    if (is_bound(l)) {
        if (!encode_rel(insn, kind, static_cast<s64>(label_offset(l)) - static_cast<s64>(m_pos))) m_error = true;
    } else {
        add_fixup(m_pos, l, kind);
    }
    emit32(insn);
}

void Emitter::branch_abs(u32 insn, const void* target, u32 kind)
{
    const s64 delta = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(cursor());
    if (!encode_rel(insn, kind, delta)) m_error = true;
    emit32(insn);
}

void Emitter::jump(const void* target, Reg scratch)
{
    if (branch26_reachable(cursor(), target)) {
        b(target);
    } else {
        mov(scratch.as_x(), static_cast<u64>(reinterpret_cast<uintptr_t>(target)));
        br(scratch.as_x());
    }
}

void Emitter::call(const void* fn, Reg scratch)
{
    if (branch26_reachable(cursor(), fn)) {
        bl(fn);
    } else {
        mov(scratch.as_x(), static_cast<u64>(reinterpret_cast<uintptr_t>(fn)));
        blr(scratch.as_x());
    }
}

// -- Floating point ------------------------------------------------------------------

void Emitter::fp1(u32 base, VReg d, VReg n)
{
    if (d.size != n.size) m_error = true;
    emit32(base | (ftype(d.size) << 22) | (n.id << 5) | d.id);
}

void Emitter::fp2(u32 base, VReg d, VReg n, VReg m)
{
    if (d.size != n.size || d.size != m.size) m_error = true;
    emit32(base | (ftype(d.size) << 22) | (m.id << 16) | (n.id << 5) | d.id);
}

void Emitter::fp3(u32 base, VReg d, VReg n, VReg m, VReg a)
{
    if (d.size != n.size || d.size != m.size || d.size != a.size) m_error = true;
    emit32(base | (ftype(d.size) << 22) | (m.id << 16) | (a.id << 10) | (n.id << 5) | d.id);
}

void Emitter::fcvt(VReg d, VReg n)
{
    if (d.size == n.size) m_error = true;
    emit32(0x1E224000U | (ftype(n.size) << 22) | (ftype(d.size) << 15) | (n.id << 5) | d.id);
}

void Emitter::fcmp_(u32 base, VReg n, u32 m, VSize msize)
{
    if (n.size != msize) m_error = true;
    emit32(base | (ftype(n.size) << 22) | (m << 16) | (n.id << 5));
}

void Emitter::fccmp(VReg n, VReg m, u32 nzcv, Cond c)
{
    if (n.size != m.size || nzcv > 15) m_error = true;
    emit32(0x1E200400U | (ftype(n.size) << 22) | (m.id << 16) | (static_cast<u32>(c) << 12) | (n.id << 5) |
           (nzcv & 15));
}

void Emitter::fcsel(VReg d, VReg n, VReg m, Cond c)
{
    if (d.size != n.size || d.size != m.size) m_error = true;
    emit32(0x1E200C00U | (ftype(d.size) << 22) | (m.id << 16) | (static_cast<u32>(c) << 12) | (n.id << 5) | d.id);
}

void Emitter::fp_int(u32 base, Reg d, VReg n)
{
    emit32(base | sf(d) | (ftype(n.size) << 22) | (n.id << 5) | d.id);
}

void Emitter::int_fp(u32 base, VReg d, Reg n)
{
    emit32(base | sf(n) | (ftype(d.size) << 22) | (n.id << 5) | d.id);
}

void Emitter::fmov(Reg d, VReg n)
{
    if (d.x != (n.size == VSize::D)) m_error = true;
    emit32(0x1E260000U | sf(d) | (ftype(n.size) << 22) | (n.id << 5) | d.id);
}

void Emitter::fmov(VReg d, Reg n)
{
    if (n.x != (d.size == VSize::D)) m_error = true;
    emit32(0x1E270000U | sf(n) | (ftype(d.size) << 22) | (n.id << 5) | d.id);
}

void Emitter::fmov(VReg d, double imm)
{
    const auto imm8 = encode_fp_imm8(imm);
    if (!imm8) m_error = true;
    emit32(0x1E201000U | (ftype(d.size) << 22) | ((imm8.value_or(0) & 0xFF) << 13) | d.id);
}

void Emitter::fmov_const(VReg d, double value, Reg scratch)
{
    const bool is_d = d.size == VSize::D;
    const u64  bits = is_d ? std::bit_cast<u64>(value) : std::bit_cast<u32>(static_cast<float>(value));
    if (bits == 0) {
        fmov(d, is_d ? XZR : WZR);
        return;
    }
    if (encode_fp_imm8(value) && (is_d || static_cast<double>(static_cast<float>(value)) == value)) {
        fmov(d, value);
        return;
    }
    const Reg s = is_d ? scratch.as_x() : scratch.w();
    mov(s, bits);
    fmov(d, s);
}

void Emitter::simd_scalar1(u32 base, VReg d, VReg n)
{
    if (d.size != n.size || (d.size != VSize::S && d.size != VSize::D)) m_error = true;
    emit32(base | (d.size == VSize::D ? (1U << 22) : 0U) | (n.id << 5) | d.id);
}

void Emitter::simd_scalar2(u32 base, VReg d, VReg n, VReg m)
{
    if (d.size != n.size || d.size != m.size || (d.size != VSize::S && d.size != VSize::D)) m_error = true;
    emit32(base | (d.size == VSize::D ? (1U << 22) : 0U) | (m.id << 16) | (n.id << 5) | d.id);
}

// -- AAPCS64 frames ------------------------------------------------------------------

namespace {

u32 align16(u32 v)
{
    return (v + 15) & ~15U;
}

// Store or load the registers in `mask` (GPRs or FPRs) in pairs starting at
// [SP, #offset]. Returns the offset after the last one.
u32 save_restore(Emitter& a, u32 mask, bool fpr, bool load, u32 offset)
{
    int pending = -1;
    for (unsigned r = 0; r < 32; ++r) {
        if ((mask & (1U << r)) == 0) continue;
        if (pending < 0) {
            pending = static_cast<int>(r);
            continue;
        }
        if (fpr) {
            if (load) a.ldp(D(pending), D(r), mem(SP, offset));
            else      a.stp(D(pending), D(r), mem(SP, offset));
        } else {
            if (load) a.ldp(X(pending), X(r), mem(SP, offset));
            else      a.stp(X(pending), X(r), mem(SP, offset));
        }
        offset += 16;
        pending = -1;
    }
    if (pending >= 0) {
        if (fpr) {
            if (load) a.ldr(D(pending), mem(SP, offset));
            else      a.str(D(pending), mem(SP, offset));
        } else {
            if (load) a.ldr(X(pending), mem(SP, offset));
            else      a.str(X(pending), mem(SP, offset));
        }
        offset += 8;
    }
    return offset;
}

}  // namespace

Frame::Frame(u32 gprs, u32 fprs, u32 locals)
    : m_gprs(gprs & kCalleeSavedGprs)
    , m_fprs(fprs & kCalleeSavedFprs)
{
    const u32 ng    = static_cast<u32>(std::popcount(m_gprs));
    const u32 nf    = static_cast<u32>(std::popcount(m_fprs));
    m_locals_offset = align16(16 + 8 * ng + 8 * nf);
    m_size          = align16(m_locals_offset + locals);
}

void Frame::prologue(Emitter& a) const
{
    if (m_size <= 504) {
        a.stp(X29, X30, pre(SP, -static_cast<s64>(m_size)));
    } else {
        a.add_imm(SP, SP, -static_cast<s64>(m_size));
        a.stp(X29, X30, mem(SP, 0));
    }
    a.mov(X29, SP);
    const u32 off = save_restore(a, m_gprs, false, false, 16);
    save_restore(a, m_fprs, true, false, off);
}

void Frame::epilogue(Emitter& a, bool with_ret) const
{
    const u32 off = save_restore(a, m_gprs, false, true, 16);
    save_restore(a, m_fprs, true, true, off);
    if (m_size <= 504) {
        a.ldp(X29, X30, post(SP, static_cast<s64>(m_size)));
    } else {
        a.ldp(X29, X30, mem(SP, 0));
        a.add_imm(SP, SP, static_cast<s64>(m_size));
    }
    if (with_ret) a.ret();
}

u32 push_regs(Emitter& a, u32 gprs, u32 fprs)
{
    const u32 n     = static_cast<u32>(std::popcount(gprs) + std::popcount(fprs));
    const u32 bytes = align16(8 * n);
    if (bytes == 0) return 0;
    a.sub(SP, SP, bytes);
    const u32 off = save_restore(a, gprs, false, false, 0);
    save_restore(a, fprs, true, false, off);
    return bytes;
}

void pop_regs(Emitter& a, u32 gprs, u32 fprs)
{
    const u32 n     = static_cast<u32>(std::popcount(gprs) + std::popcount(fprs));
    const u32 bytes = align16(8 * n);
    if (bytes == 0) return;
    const u32 off = save_restore(a, gprs, false, true, 0);
    save_restore(a, fprs, true, true, off);
    a.add(SP, SP, bytes);
}

}  // namespace sm2::cpu::jit::a64
