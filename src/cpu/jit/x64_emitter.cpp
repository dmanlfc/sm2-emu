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
#include "cpu/jit/x64_emitter.h"

#include <bit>
#include <cstdint>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#  if defined(_MSC_VER) && !defined(__clang__)
#    include <intrin.h>
#  else
#    include <cpuid.h>
#  endif
#  define SM2_JIT_HOST_X64 1
#else
#  define SM2_JIT_HOST_X64 0
#endif

namespace sm2::cpu::jit::x64 {

namespace {

bool fits8(s64 v)
{
    return v >= -128 && v <= 127;
}

bool fits32(s64 v)
{
    return v >= INT32_MIN && v <= INT32_MAX;
}

}  // namespace

bool rel32_reachable(const void* from, const void* to)
{
    return fits32(reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from));
}

void encode_jmp_rel32(u8 out[5], const void* at, const void* to)
{
    const auto rel = static_cast<s32>(reinterpret_cast<intptr_t>(to) - (reinterpret_cast<intptr_t>(at) + 5));
    out[0]         = 0xE9;
    std::memcpy(out + 1, &rel, 4);
}

// -- CPU features ---------------------------------------------------------------------

const Features& host_features()
{
    static const Features features = [] {
        Features f;
#if SM2_JIT_HOST_X64
        auto cpuid = [](u32 leaf, u32 sub, u32 r[4]) {
#  if defined(_MSC_VER) && !defined(__clang__)
            int out[4];
            __cpuidex(out, static_cast<int>(leaf), static_cast<int>(sub));
            for (int i = 0; i < 4; ++i) r[i] = static_cast<u32>(out[i]);
#  else
            __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
#  endif
        };
        u32 r[4] = {};
        cpuid(0, 0, r);
        const u32 max_leaf = r[0];
        cpuid(1, 0, r);
        f.sse41  = (r[2] >> 19) & 1;
        f.popcnt = (r[2] >> 23) & 1;
        f.movbe  = (r[2] >> 22) & 1;
        f.avx    = (r[2] >> 28) & 1;
        f.fma    = (r[2] >> 12) & 1;
        if (max_leaf >= 7) {
            cpuid(7, 0, r);
            f.bmi1 = (r[1] >> 3) & 1;
            f.bmi2 = (r[1] >> 8) & 1;
        }
        cpuid(0x80000000U, 0, r);
        if (r[0] >= 0x80000001U) {
            cpuid(0x80000001U, 0, r);
            f.lzcnt = (r[2] >> 5) & 1;
        }
#endif
        return f;
    }();
    return features;
}

// -- Core encoder -----------------------------------------------------------------------

void Emitter::encode(u8 prefix, u8 size, const Opcode& op, u8 reg, bool rm_is_mem, u8 rm_reg, const Mem* m,
                     usize imm_bytes, s64 imm, bool rex_w_from_size, bool force_rex)
{
    if (size == 2) emit8(0x66);
    if (prefix != 0) emit8(prefix);

    u8 rex = 0;
    if (size == 8 && rex_w_from_size) rex |= 0x08;
    if ((reg & 8) != 0) rex |= 0x04;
    if (rm_is_mem) {
        if (m->has_index && (m->index.id & 8) != 0) rex |= 0x02;
        if (m->has_base && (m->base.id & 8) != 0) rex |= 0x01;
    } else if ((rm_reg & 8) != 0) {
        rex |= 0x01;
    }
    if (rex != 0 || force_rex) emit8(static_cast<u8>(0x40 | rex));
    for (u8 i = 0; i < op.n; ++i) emit8(op.b[i]);

    const u8 r       = static_cast<u8>((reg & 7) << 3);
    usize    rip_pos = 0;
    if (!rm_is_mem) {
        emit8(static_cast<u8>(0xC0 | r | (rm_reg & 7)));
    } else if (m->rip) {
        emit8(static_cast<u8>(0x05 | r));
        rip_pos = m_pos;
        emit32(0);
    } else {
        u8 ss = 0;
        switch (m->scale) {
        case 1: ss = 0; break;
        case 2: ss = 1; break;
        case 4: ss = 2; break;
        case 8: ss = 3; break;
        default: m_error = true; break;
        }
        if (m->has_index && m->index.id == 4) m_error = true;  // RSP can't be an index
        if (!m->has_base) {
            emit8(static_cast<u8>(0x04 | r));
            emit8(m->has_index ? static_cast<u8>((ss << 6) | ((m->index.id & 7) << 3) | 5) : 0x25);
            emit32(static_cast<u32>(m->disp));
        } else {
            const u8 base = m->base.id & 7;
            u8       mod  = 2;
            if (m->disp == 0 && base != 5) {
                mod = 0;
            } else if (fits8(m->disp)) {
                mod = 1;
            }
            if (m->has_index || base == 4) {
                const u8 index = m->has_index ? (m->index.id & 7) : 4;
                emit8(static_cast<u8>((mod << 6) | r | 4));
                emit8(static_cast<u8>((ss << 6) | (index << 3) | base));
            } else {
                emit8(static_cast<u8>((mod << 6) | r | base));
            }
            if (mod == 1) emit8(static_cast<u8>(m->disp));
            if (mod == 2) emit32(static_cast<u32>(m->disp));
        }
    }

    switch (imm_bytes) {
    case 0: break;
    case 1: emit8(static_cast<u8>(imm)); break;
    case 2: emit16(static_cast<u16>(imm)); break;
    case 4: emit32(static_cast<u32>(imm)); break;
    case 8: emit64(static_cast<u64>(imm)); break;
    default: m_error = true; break;
    }

    if (rm_is_mem && m->rip) {
        const u32 tail = static_cast<u32>(4 + imm_bytes);
        if (m->label.valid()) {
            if (is_bound(m->label)) {
                patch_fixup(rip_pos, kFixRip | (tail << 8), label_offset(m->label));
            } else {
                add_fixup(rip_pos, m->label, kFixRip | (tail << 8));
            }
        } else {
            const s64 disp = reinterpret_cast<intptr_t>(m->target) - reinterpret_cast<intptr_t>(cursor());
            if (!fits32(disp)) m_error = true;
            if (in_span(rip_pos, 4)) write32_at(rip_pos, static_cast<u32>(disp));
        }
    }
}

bool Emitter::patch_fixup(usize pos, u32 kind, usize target)
{
    const s64 t = static_cast<s64>(target);
    switch (kind & 0xFF) {
    case kFixRel32: {
        const s64 rel = t - static_cast<s64>(pos + 4);
        if (!fits32(rel)) return false;
        write32_at(pos, static_cast<u32>(rel));
        return true;
    }
    case kFixRel8: {
        const s64 rel = t - static_cast<s64>(pos + 1);
        if (!fits8(rel)) return false;
        if (in_span(pos, 1)) m_span.rw[pos] = static_cast<u8>(rel);
        return true;
    }
    case kFixRip: {
        const s64 rel = t - static_cast<s64>(pos + (kind >> 8));
        if (!fits32(rel)) return false;
        write32_at(pos, static_cast<u32>(rel));
        return true;
    }
    default: return false;
    }
}

// -- Integer ALU ---------------------------------------------------------------------------

void Emitter::alu(AluOp op, Gp d, Gp s)
{
    if (d.size != s.size) m_error = true;
    const u8 o = static_cast<u8>((static_cast<u8>(op) << 3) | (d.size == 1 ? 0 : 1));
    enc_rr(0, d.size, o, s.id, d.id, 0, 0, byte_reg_needs_rex(d) || byte_reg_needs_rex(s));
}

void Emitter::alu(AluOp op, Gp d, const Mem& s)
{
    const u8 o = static_cast<u8>((static_cast<u8>(op) << 3) | (d.size == 1 ? 2 : 3));
    enc_rm(0, d.size, o, d.id, s, 0, 0, byte_reg_needs_rex(d));
}

void Emitter::alu(AluOp op, const Mem& d, Gp s)
{
    const u8 o = static_cast<u8>((static_cast<u8>(op) << 3) | (s.size == 1 ? 0 : 1));
    enc_rm(0, s.size, o, s.id, d, 0, 0, byte_reg_needs_rex(s));
}

void Emitter::alu(AluOp op, Gp d, s32 imm)
{
    const u8 ext = static_cast<u8>(op);
    if (d.size == 1) {
        if (d.id == 0) {
            emit8(static_cast<u8>((ext << 3) | 4));
            emit8(static_cast<u8>(imm));
        } else {
            enc_rr(0, 1, 0x80, ext, d.id, 1, imm, byte_reg_needs_rex(d));
        }
        return;
    }
    const usize iz = d.size == 2 ? 2 : 4;
    if (fits8(imm)) {
        enc_rr(0, d.size, 0x83, ext, d.id, 1, imm);
    } else if (d.id == 0) {
        if (d.size == 2) emit8(0x66);
        if (d.size == 8) emit8(0x48);
        emit8(static_cast<u8>((ext << 3) | 5));
        if (iz == 2) {
            emit16(static_cast<u16>(imm));
        } else {
            emit32(static_cast<u32>(imm));
        }
    } else {
        enc_rr(0, d.size, 0x81, ext, d.id, iz, imm);
    }
}

void Emitter::alu(AluOp op, const Mem& d, s32 imm)
{
    const u8 ext = static_cast<u8>(op);
    if (d.size == 0) m_error = true;
    if (d.size == 1) {
        enc_rm(0, 1, 0x80, ext, d, 1, imm);
    } else if (fits8(imm)) {
        enc_rm(0, d.size, 0x83, ext, d, 1, imm);
    } else {
        enc_rm(0, d.size, 0x81, ext, d, d.size == 2 ? 2 : 4, imm);
    }
}

void Emitter::test(Gp a, Gp b)
{
    if (a.size != b.size) m_error = true;
    enc_rr(0, a.size, a.size == 1 ? 0x84 : 0x85, b.id, a.id, 0, 0, byte_reg_needs_rex(a) || byte_reg_needs_rex(b));
}

void Emitter::test(const Mem& a, Gp b)
{
    enc_rm(0, b.size, b.size == 1 ? 0x84 : 0x85, b.id, a, 0, 0, byte_reg_needs_rex(b));
}

void Emitter::test(Gp a, s32 imm)
{
    if (a.size == 1) {
        if (a.id == 0) {
            emit8(0xA8);
            emit8(static_cast<u8>(imm));
        } else {
            enc_rr(0, 1, 0xF6, 0, a.id, 1, imm, byte_reg_needs_rex(a));
        }
        return;
    }
    const usize iz = a.size == 2 ? 2 : 4;
    if (a.id == 0) {
        if (a.size == 2) emit8(0x66);
        if (a.size == 8) emit8(0x48);
        emit8(0xA9);
        if (iz == 2) {
            emit16(static_cast<u16>(imm));
        } else {
            emit32(static_cast<u32>(imm));
        }
    } else {
        enc_rr(0, a.size, 0xF7, 0, a.id, iz, imm);
    }
}

void Emitter::test(const Mem& a, s32 imm)
{
    if (a.size == 0) m_error = true;
    enc_rm(0, a.size, a.size == 1 ? 0xF6 : 0xF7, 0, a, a.size == 1 ? 1 : a.size == 2 ? 2 : 4, imm);
}

void Emitter::unary(u8 op8, u8 ext, Gp r)
{
    enc_rr(0, r.size, static_cast<u8>(r.size == 1 ? op8 : op8 + 1), ext, r.id, 0, 0, byte_reg_needs_rex(r));
}

void Emitter::imul(Gp d, Gp s)
{
    if (d.size != s.size || d.size == 1) m_error = true;
    enc_rr(0, d.size, {0x0F, 0xAF}, d.id, s.id);
}

void Emitter::imul(Gp d, const Mem& s)
{
    if (d.size == 1) m_error = true;
    enc_rm(0, d.size, {0x0F, 0xAF}, d.id, s);
}

void Emitter::imul(Gp d, Gp s, s32 imm)
{
    if (d.size != s.size || d.size == 1) m_error = true;
    if (fits8(imm)) {
        enc_rr(0, d.size, 0x6B, d.id, s.id, 1, imm);
    } else {
        enc_rr(0, d.size, 0x69, d.id, s.id, d.size == 2 ? 2 : 4, imm);
    }
}

// -- Shifts -------------------------------------------------------------------------------

void Emitter::shift(ShiftOp op, Gp r, u8 amount)
{
    const u8   ext   = static_cast<u8>(op);
    const bool byte  = r.size == 1;
    const bool force = byte_reg_needs_rex(r);
    if (amount == 1) {
        enc_rr(0, r.size, byte ? 0xD0 : 0xD1, ext, r.id, 0, 0, force);
    } else {
        enc_rr(0, r.size, byte ? 0xC0 : 0xC1, ext, r.id, 1, amount, force);
    }
}

void Emitter::shift_cl(ShiftOp op, Gp r)
{
    enc_rr(0, r.size, r.size == 1 ? 0xD2 : 0xD3, static_cast<u8>(op), r.id, 0, 0, byte_reg_needs_rex(r));
}

void Emitter::shld(Gp d, Gp s, u8 n)
{
    if (d.size != s.size || d.size == 1) m_error = true;
    enc_rr(0, d.size, {0x0F, 0xA4}, s.id, d.id, 1, n);
}

void Emitter::shrd(Gp d, Gp s, u8 n)
{
    if (d.size != s.size || d.size == 1) m_error = true;
    enc_rr(0, d.size, {0x0F, 0xAC}, s.id, d.id, 1, n);
}

// -- Moves -------------------------------------------------------------------------------

void Emitter::mov(Gp d, Gp s)
{
    if (d.size != s.size) m_error = true;
    enc_rr(0, d.size, d.size == 1 ? 0x88 : 0x89, s.id, d.id, 0, 0, byte_reg_needs_rex(d) || byte_reg_needs_rex(s));
}

void Emitter::mov(Gp d, const Mem& s)
{
    enc_rm(0, d.size, d.size == 1 ? 0x8A : 0x8B, d.id, s, 0, 0, byte_reg_needs_rex(d));
}

void Emitter::mov(const Mem& d, Gp s)
{
    enc_rm(0, s.size, s.size == 1 ? 0x88 : 0x89, s.id, d, 0, 0, byte_reg_needs_rex(s));
}

void Emitter::mov(const Mem& d, s32 imm)
{
    if (d.size == 0) m_error = true;
    enc_rm(0, d.size, d.size == 1 ? 0xC6 : 0xC7, 0, d, d.size == 1 ? 1 : d.size == 2 ? 2 : 4, imm);
}

void Emitter::mov(Gp d, u64 imm)
{
    // B0+r / B8+r carry the register in the opcode byte.
    auto opreg = [&](u8 base, Gp r, bool w) {
        if (r.size == 2) emit8(0x66);
        u8 rex = 0;
        if (w) rex |= 0x08;
        if ((r.id & 8) != 0) rex |= 0x01;
        if (rex != 0 || byte_reg_needs_rex(r)) emit8(static_cast<u8>(0x40 | rex));
        emit8(static_cast<u8>(base + (r.id & 7)));
    };
    switch (d.size) {
    case 1:
        opreg(0xB0, d, false);
        emit8(static_cast<u8>(imm));
        return;
    case 2:
        opreg(0xB8, d, false);
        emit16(static_cast<u16>(imm));
        return;
    case 4:
        opreg(0xB8, d, false);
        emit32(static_cast<u32>(imm));
        return;
    default:
        if (imm <= 0xFFFFFFFFULL) {
            opreg(0xB8, d.r32(), false);
            emit32(static_cast<u32>(imm));
        } else if (fits32(static_cast<s64>(imm))) {
            enc_rr(0, 8, 0xC7, 0, d.id, 4, static_cast<s64>(imm));
        } else {
            opreg(0xB8, d, true);
            emit64(imm);
        }
        return;
    }
}

void Emitter::movabs(Gp d, u64 imm)
{
    u8 rex = 0x48;
    if ((d.id & 8) != 0) rex |= 0x01;
    emit8(rex);
    emit8(static_cast<u8>(0xB8 + (d.id & 7)));
    emit64(imm);
}

void Emitter::mov_address(Gp d, const void* p)
{
    // LEA r64, [rip + disp32] is 7 bytes; the displacement is from its end.
    if (rel32_reachable(cursor() + 7, p)) {
        lea(d.r64(), rip(p));
    } else {
        movabs(d.r64(), static_cast<u64>(reinterpret_cast<uintptr_t>(p)));
    }
}

void Emitter::movzx(Gp d, Gp s)
{
    if (s.size != 1 && s.size != 2) m_error = true;
    enc_rr(0, d.size, {0x0F, static_cast<u8>(s.size == 1 ? 0xB6 : 0xB7)}, d.id, s.id, 0, 0, byte_reg_needs_rex(s));
}

void Emitter::movzx(Gp d, const Mem& s)
{
    if (s.size != 1 && s.size != 2) m_error = true;
    enc_rm(0, d.size, {0x0F, static_cast<u8>(s.size == 1 ? 0xB6 : 0xB7)}, d.id, s);
}

void Emitter::movsx(Gp d, Gp s)
{
    if (s.size == 4) {
        if (d.size != 8) m_error = true;
        enc_rr(0, 8, 0x63, d.id, s.id);
        return;
    }
    if (s.size != 1 && s.size != 2) m_error = true;
    enc_rr(0, d.size, {0x0F, static_cast<u8>(s.size == 1 ? 0xBE : 0xBF)}, d.id, s.id, 0, 0, byte_reg_needs_rex(s));
}

void Emitter::movsx(Gp d, const Mem& s)
{
    if (s.size == 4) {
        if (d.size != 8) m_error = true;
        enc_rm(0, 8, 0x63, d.id, s);
        return;
    }
    if (s.size != 1 && s.size != 2) m_error = true;
    enc_rm(0, d.size, {0x0F, static_cast<u8>(s.size == 1 ? 0xBE : 0xBF)}, d.id, s);
}

void Emitter::lea(Gp d, const Mem& s)
{
    enc_rm(0, d.size, 0x8D, d.id, s);
}

void Emitter::xchg(Gp a, Gp b)
{
    if (a.size != b.size) m_error = true;
    enc_rr(0, a.size, a.size == 1 ? 0x86 : 0x87, a.id, b.id, 0, 0, byte_reg_needs_rex(a) || byte_reg_needs_rex(b));
}

void Emitter::cmov(Cond c, Gp d, Gp s)
{
    if (d.size != s.size || d.size == 1) m_error = true;
    enc_rr(0, d.size, {0x0F, static_cast<u8>(0x40 + static_cast<u8>(c))}, d.id, s.id);
}

void Emitter::cmov(Cond c, Gp d, const Mem& s)
{
    if (d.size == 1) m_error = true;
    enc_rm(0, d.size, {0x0F, static_cast<u8>(0x40 + static_cast<u8>(c))}, d.id, s);
}

void Emitter::set(Cond c, Gp d8)
{
    if (d8.size != 1) m_error = true;
    enc_rr(0, 1, {0x0F, static_cast<u8>(0x90 + static_cast<u8>(c))}, 0, d8.id, 0, 0, byte_reg_needs_rex(d8));
}

// -- Bit operations ------------------------------------------------------------------------

void Emitter::bswap(Gp r)
{
    if (r.size != 4 && r.size != 8) m_error = true;
    u8 rex = 0;
    if (r.size == 8) rex |= 0x08;
    if ((r.id & 8) != 0) rex |= 0x01;
    if (rex != 0) emit8(static_cast<u8>(0x40 | rex));
    emit8(0x0F);
    emit8(static_cast<u8>(0xC8 + (r.id & 7)));
}

void Emitter::movbe(Gp d, const Mem& s)
{
    if (d.size == 1) m_error = true;
    enc_rm(0, d.size, {0x0F, 0x38, 0xF0}, d.id, s);
}

void Emitter::movbe(const Mem& d, Gp s)
{
    if (s.size == 1) m_error = true;
    enc_rm(0, s.size, {0x0F, 0x38, 0xF1}, s.id, d);
}

void Emitter::rm_op(u8 prefix, const Opcode& op, Gp d, Gp s)
{
    if (d.size != s.size || d.size == 1) m_error = true;
    // The size prefix (66) must come before a mandatory F3; encode() emits
    // them in that order.
    enc_rr(prefix, d.size, op, d.id, s.id);
}

void Emitter::bt_rr(u8 op, Gp a, Gp b)
{
    if (a.size != b.size || a.size == 1) m_error = true;
    enc_rr(0, a.size, {0x0F, op}, b.id, a.id);
}

void Emitter::bt_ri(u8 ext, Gp a, u8 bit)
{
    if (a.size == 1) m_error = true;
    enc_rr(0, a.size, {0x0F, 0xBA}, ext, a.id, 1, bit);
}

void Emitter::clz(Gp d, Gp s, bool have_lzcnt)
{
    if (have_lzcnt) {
        lzcnt(d, s);
        return;
    }
    // clz = (width - 1) - bsr, with bsr(0) taken as -1.
    const Label nonzero = new_label();
    bsr(d, s);
    j_short(Cond::NZ, nonzero);
    mov(d, static_cast<u64>(d.size == 8 ? ~u64{0} : 0xFFFFFFFFULL));
    bind(nonzero);
    neg(d);
    add(d, static_cast<s32>(d.size * 8 - 1));
}

// -- Stack and control flow ------------------------------------------------------------

void Emitter::push(Gp r)
{
    if ((r.id & 8) != 0) emit8(0x41);
    emit8(static_cast<u8>(0x50 + (r.id & 7)));
}

void Emitter::pop(Gp r)
{
    if ((r.id & 8) != 0) emit8(0x41);
    emit8(static_cast<u8>(0x58 + (r.id & 7)));
}

void Emitter::nop(usize n)
{
    static constexpr u8 kNops[9][9] = {
        {0x90},
        {0x66, 0x90},
        {0x0F, 0x1F, 0x00},
        {0x0F, 0x1F, 0x40, 0x00},
        {0x0F, 0x1F, 0x44, 0x00, 0x00},
        {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
        {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
        {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
    };
    while (n > 0) {
        const usize k = n > 9 ? 9 : n;
        emit_bytes(kNops[k - 1], k);
        n -= k;
    }
}

void Emitter::align(usize alignment)
{
    const usize mis = reinterpret_cast<uintptr_t>(cursor()) & (alignment - 1);
    if (mis != 0) nop(alignment - mis);
}

void Emitter::jump_label(u8 short_op, const Opcode& near_op, Label l, bool force_short)
{
    if (is_bound(l)) {
        const s64 target = static_cast<s64>(label_offset(l));
        const s64 rel8   = target - static_cast<s64>(m_pos + 2);
        if (short_op != 0 && fits8(rel8)) {
            emit8(short_op);
            emit8(static_cast<u8>(rel8));
            return;
        }
        if (force_short) m_error = true;
        for (u8 i = 0; i < near_op.n; ++i) emit8(near_op.b[i]);
        const s64 rel32 = target - static_cast<s64>(m_pos + 4);
        emit32(static_cast<u32>(rel32));
        return;
    }
    if (force_short) {
        emit8(short_op);
        add_fixup(m_pos, l, kFixRel8);
        emit8(0);
        return;
    }
    for (u8 i = 0; i < near_op.n; ++i) emit8(near_op.b[i]);
    add_fixup(m_pos, l, kFixRel32);
    emit32(0);
}

void Emitter::jmp(Label l)
{
    jump_label(0xEB, 0xE9, l, false);
}

void Emitter::j(Cond c, Label l)
{
    jump_label(static_cast<u8>(0x70 + static_cast<u8>(c)), {0x0F, static_cast<u8>(0x80 + static_cast<u8>(c))}, l,
               false);
}

void Emitter::call(Label l)
{
    jump_label(0, 0xE8, l, false);
}

void Emitter::jmp_short(Label l)
{
    jump_label(0xEB, 0xE9, l, true);
}

void Emitter::j_short(Cond c, Label l)
{
    jump_label(static_cast<u8>(0x70 + static_cast<u8>(c)), {0x0F, static_cast<u8>(0x80 + static_cast<u8>(c))}, l,
               true);
}

void Emitter::jmp(Gp r)
{
    enc_rr(0, 4, 0xFF, 4, r.id);
}

void Emitter::call(Gp r)
{
    enc_rr(0, 4, 0xFF, 2, r.id);
}

void Emitter::jmp(const Mem& m)
{
    enc_rm(0, 4, 0xFF, 4, m);
}

void Emitter::call(const Mem& m)
{
    enc_rm(0, 4, 0xFF, 2, m);
}

void Emitter::jmp(const void* target)
{
    emit8(0xE9);
    const s64 rel = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(cursor() + 4);
    if (!fits32(rel)) m_error = true;
    emit32(static_cast<u32>(rel));
}

void Emitter::j(Cond c, const void* target)
{
    emit8(0x0F);
    emit8(static_cast<u8>(0x80 + static_cast<u8>(c)));
    const s64 rel = reinterpret_cast<intptr_t>(target) - reinterpret_cast<intptr_t>(cursor() + 4);
    if (!fits32(rel)) m_error = true;
    emit32(static_cast<u32>(rel));
}

void Emitter::call(const void* fn, Gp scratch)
{
    if (rel32_reachable(cursor() + 5, fn)) {
        emit8(0xE8);
        emit32(static_cast<u32>(reinterpret_cast<intptr_t>(fn) - reinterpret_cast<intptr_t>(cursor() + 4)));
    } else {
        movabs(scratch.r64(), static_cast<u64>(reinterpret_cast<uintptr_t>(fn)));
        call(scratch.r64());
    }
}

void Emitter::jump(const void* target, Gp scratch)
{
    if (rel32_reachable(cursor() + 5, target)) {
        jmp(target);
    } else {
        movabs(scratch.r64(), static_cast<u64>(reinterpret_cast<uintptr_t>(target)));
        jmp(scratch.r64());
    }
}

// -- SSE --------------------------------------------------------------------------------------

void Emitter::sse(u8 prefix, u8 op, u8 reg, u8 rm)
{
    enc_rr(prefix, 4, {0x0F, op}, reg, rm);
}

void Emitter::sse(u8 prefix, u8 op, u8 reg, const Mem& m)
{
    enc_rm(prefix, 4, {0x0F, op}, reg, m);
}

void Emitter::sse_w(u8 prefix, u8 op, u8 xmm, Gp g, bool gpr_is_reg)
{
    if (g.size != 4 && g.size != 8) m_error = true;
    // REX.W, not the 66 operand-size prefix, selects the 64-bit GPR form;
    // `size` 8 gives exactly that and 4 gives nothing.
    if (gpr_is_reg) {
        enc_rr(prefix, g.size, {0x0F, op}, g.id, xmm);
    } else {
        enc_rr(prefix, g.size, {0x0F, op}, xmm, g.id);
    }
}

void Emitter::roundss(Xmm d, Xmm s, u8 mode)
{
    enc_rr(0x66, 4, {0x0F, 0x3A, 0x0A}, d.id, s.id, 1, mode);
}

void Emitter::roundsd(Xmm d, Xmm s, u8 mode)
{
    enc_rr(0x66, 4, {0x0F, 0x3A, 0x0B}, d.id, s.id, 1, mode);
}

void Emitter::ldmxcsr(const Mem& m)
{
    enc_rm(0, 4, {0x0F, 0xAE}, 2, m);
}

void Emitter::stmxcsr(const Mem& m)
{
    enc_rm(0, 4, {0x0F, 0xAE}, 3, m);
}

void Emitter::vfmadd213sd(Xmm d, Xmm s2, Xmm s3)
{
    emit8(0xC4);
    emit8(static_cast<u8>(((d.id & 8) != 0 ? 0 : 0x80) | 0x40 | ((s3.id & 8) != 0 ? 0 : 0x20) | 0x02));
    emit8(static_cast<u8>(0x80 | ((~s2.id & 0xF) << 3) | 0x01));
    emit8(0xA9);
    emit8(static_cast<u8>(0xC0 | ((d.id & 7) << 3) | (s3.id & 7)));
}

// -- Calling conventions -------------------------------------------------------------------

const AbiInfo& abi_info(Abi abi)
{
    static const AbiInfo kSysV{
        {RDI, RSI, RDX, RCX, R8, R9},
        6,
        static_cast<u16>((1U << 3) | (1U << 5) | 0xF000U),
        0,
        0,
    };
    static const AbiInfo kWin64{
        {RCX, RDX, R8, R9, RAX, RAX},
        4,
        static_cast<u16>((1U << 3) | (1U << 5) | (1U << 6) | (1U << 7) | 0xF000U),
        0xFFC0,
        32,
    };
    return abi == Abi::Win64 ? kWin64 : kSysV;
}

Frame::Frame(Abi abi, u16 gprs, u16 xmms, u32 locals)
    : m_abi(abi)
    , m_gprs(static_cast<u16>(gprs & abi_info(abi).callee_saved_gprs & ~(1U << 5)))
    , m_xmms(static_cast<u16>(xmms & abi_info(abi).callee_saved_xmms))
{
    const u32 pushes  = static_cast<u32>(std::popcount(m_gprs));
    const u32 nx      = static_cast<u32>(std::popcount(m_xmms));
    const u32 shadow  = abi_info(abi).shadow_space;
    m_locals_offset   = shadow;
    m_xmm_offset      = (shadow + locals + 15) & ~15U;
    m_sub             = m_xmm_offset + 16 * nx;
    // On entry RSP is 8 mod 16 (the return address); PUSH RBP makes it 0 and
    // each further push flips it.
    if (((8 * pushes + m_sub) & 15) != 0) m_sub += 8;
}

void Frame::prologue(Emitter& a) const
{
    a.push(RBP);
    a.mov(RBP, RSP);
    for (unsigned r = 0; r < 16; ++r) {
        if ((m_gprs & (1U << r)) != 0) a.push(Gp{static_cast<u8>(r), 8});
    }
    if (m_sub != 0) a.sub(RSP, static_cast<s32>(m_sub));
    u32 off = m_xmm_offset;
    for (unsigned r = 0; r < 16; ++r) {
        if ((m_xmms & (1U << r)) == 0) continue;
        a.movups(ptr(RSP, static_cast<s32>(off)), Xmm{static_cast<u8>(r)});
        off += 16;
    }
}

void Frame::epilogue(Emitter& a, bool with_ret) const
{
    u32 off = m_xmm_offset;
    for (unsigned r = 0; r < 16; ++r) {
        if ((m_xmms & (1U << r)) == 0) continue;
        a.movups(Xmm{static_cast<u8>(r)}, ptr(RSP, static_cast<s32>(off)));
        off += 16;
    }
    if (m_sub != 0) a.add(RSP, static_cast<s32>(m_sub));
    for (int r = 15; r >= 0; --r) {
        if ((m_gprs & (1U << r)) != 0) a.pop(Gp{static_cast<u8>(r), 8});
    }
    a.pop(RBP);
    if (with_ret) a.ret();
    (void)m_abi;
}

}  // namespace sm2::cpu::jit::x64
