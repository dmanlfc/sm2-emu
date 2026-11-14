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

// Helper interpreter: whole-instruction semantics for Helper ops.

#include "cpu/ppc/ppc.h"
#include "cpu/ppc/ppc_fpu.h"
#include "cpu/ppc/ppc_internal.h"

#include <cstdio>

namespace sm2::cpu::ppc {

namespace {

constexpr u32 kSrr1Trap = 0x00020000;  // SRR1 bit 14: program exception was a trap

[[nodiscard]] bool trap_condition(u32 to, u32 a, u32 b)
{
    const s32 sa = static_cast<s32>(a), sb = static_cast<s32>(b);
    return ((to & 0x10) && sa < sb) || ((to & 0x08) && sa > sb) || ((to & 0x04) && a == b) ||
           ((to & 0x02) && a < b) || ((to & 0x01) && a > b);
}

[[nodiscard]] u8 cr_of(s32 v, u8 so)
{
    return static_cast<u8>((v < 0 ? kCrLT : v > 0 ? kCrGT : kCrEQ) | so);
}

}  // namespace

void Ppc::unknown_insn(u32 insn)
{
    char msg[80];
    std::snprintf(msg, sizeof msg, "unimplemented instruction %08x", insn);
    fault(msg);
}

u32 Ppc::helper(u32 insn, u32 index)
{
    ++m_stats.helper_calls;
    PpcState& st = m_st;
    u32* const s = st.slot;
    const u32 pc = m_st.block_pc + 4 * index;

    const u32 opcd = insn >> 26;
    const u32 rd = (insn >> 21) & 31, ra = (insn >> 16) & 31, rb = (insn >> 11) & 31;
    const u32 xo10 = (insn >> 1) & 0x3ff;
    const bool rc = (insn & 1) != 0;
    const u32 ea_x = (ra != 0 ? s[ra] : 0u) + s[rb];  // X-form (rA|0) + rB

    auto set_cr1 = [&] {
        if (rc) {
            st.cr[1] = static_cast<u8>(st.fpscr >> 28);
        }
    };
    auto illegal = [&] {
        st.pc = pc;
        unknown_insn(insn);
        return kHelperExit;
    };

    switch (opcd) {
        case 3:  // twi
            if (trap_condition(rd, s[ra], static_cast<u32>(static_cast<s32>(static_cast<s16>(insn))))) {
                take_exception(0x700, pc, kSrr1Trap);
                return kHelperExit;
            }
            return kHelperContinue;

        case 17:  // sc
            take_exception(0xc00, pc + 4, 0);
            return kHelperExit;

        case 19:
            if (xo10 == 50) {  // rfi
                st.pc = st.srr0 & ~3u;
                set_msr(st.srr1);
                return kHelperExit;
            }
            return illegal();

        case 31:
            break;

        case 63:
            switch (xo10) {
                case 583:  // mffs
                    st.fpr[rd] = st.fpscr;
                    set_cr1();
                    return kHelperContinue;
                case 64: {  // mcrfs
                    const u32 crfd = rd >> 2, crfs = ra >> 2;
                    const u32 shift = 28 - 4 * crfs;
                    st.cr[crfd] = static_cast<u8>((st.fpscr >> shift) & 0xf);
                    const u32 clearable = (kFpscrFX | kFpscrOX | kFpscrUX | kFpscrZX | kFpscrXX |
                                           kFpscrVXAll);
                    st.fpscr = fpu::fpscr_summarise(st.fpscr & ~((0xfu << shift) & clearable));
                    return kHelperContinue;
                }
                case 38:   // mtfsb1
                case 70: {  // mtfsb0
                    const u32 bitn = rd;
                    if (bitn != 1 && bitn != 2) {
                        const u32 m = 0x80000000u >> bitn;
                        st.fpscr = xo10 == 38 ? (st.fpscr | m) : (st.fpscr & ~m);
                    }
                    st.fpscr = fpu::fpscr_summarise(st.fpscr);
                    set_cr1();
                    return kHelperContinue;
                }
                case 134: {  // mtfsfi
                    const u32 field = rd >> 2;
                    const u32 imm = (insn >> 12) & 0xf;
                    const u32 shift = 28 - 4 * field;
                    mtfpscr(imm << shift, 0xfu << shift);
                    set_cr1();
                    return kHelperContinue;
                }
                case 711: {  // mtfsf
                    const u32 fm = (insn >> 17) & 0xff;
                    u32 mask = 0;
                    for (unsigned i = 0; i < 8; ++i) {
                        if (fm & (0x80u >> i)) {
                            mask |= 0xf0000000u >> (4 * i);
                        }
                    }
                    mtfpscr(static_cast<u32>(st.fpr[rb]), mask);
                    set_cr1();
                    return kHelperContinue;
                }
                default:
                    return illegal();
            }

        default:
            return illegal();
    }

    // opcode 31: OE forms first
    if (insn & 0x400) {
        const u32 xo9 = (insn >> 1) & 0x1ff;
        const u32 a = s[ra], b = s[rb];
        u32  r  = 0;
        bool ov = false;
        bool handled = true;
        auto add_ov = [](u32 x, u32 y, u32 res) { return ((~(x ^ y) & (x ^ res)) >> 31) != 0; };
        switch (xo9) {
            case 266: r = a + b; ov = add_ov(a, b, r); break;                        // addo
            case 40:  r = b - a; ov = add_ov(~a, b, r); break;                       // subfo
            case 10:  r = a + b; st.xer_ca = r < a; ov = add_ov(a, b, r); break;    // addco
            case 8:   r = b - a; st.xer_ca = b >= a; ov = add_ov(~a, b, r); break;  // subfco
            case 138: {                                                              // addeo
                const u64 w = u64{a} + b + st.xer_ca;
                r = static_cast<u32>(w); st.xer_ca = static_cast<u8>(w >> 32); ov = add_ov(a, b, r);
                break;
            }
            case 136: {                                                              // subfeo
                const u64 w = u64{~a} + b + st.xer_ca;
                r = static_cast<u32>(w); st.xer_ca = static_cast<u8>(w >> 32); ov = add_ov(~a, b, r);
                break;
            }
            case 202: {                                                              // addzeo
                const u64 w = u64{a} + st.xer_ca;
                r = static_cast<u32>(w); st.xer_ca = static_cast<u8>(w >> 32); ov = add_ov(a, 0, r);
                break;
            }
            case 200: {                                                              // subfzeo
                const u64 w = u64{~a} + st.xer_ca;
                r = static_cast<u32>(w); st.xer_ca = static_cast<u8>(w >> 32); ov = add_ov(~a, 0, r);
                break;
            }
            case 234: {                                                              // addmeo
                const u64 w = u64{a} + 0xffffffffu + st.xer_ca;
                r = static_cast<u32>(w); st.xer_ca = static_cast<u8>(w >> 32); ov = add_ov(a, 0xffffffffu, r);
                break;
            }
            case 232: {                                                              // subfmeo
                const u64 w = u64{~a} + 0xffffffffu + st.xer_ca;
                r = static_cast<u32>(w); st.xer_ca = static_cast<u8>(w >> 32); ov = add_ov(~a, 0xffffffffu, r);
                break;
            }
            case 104: r = 0u - a; ov = a == 0x80000000u; break;                      // nego
            case 235: {                                                              // mullwo
                const s64 p = s64{static_cast<s32>(a)} * static_cast<s32>(b);
                r = static_cast<u32>(p); ov = p != static_cast<s32>(p);
                break;
            }
            case 491:                                                                // divwo
                if (b == 0 || (a == 0x80000000u && b == 0xffffffffu)) {
                    r = (b == 0 && a < 0x80000000u) ? 0u : 0xffffffffu;
                    ov = true;
                } else {
                    r = static_cast<u32>(static_cast<s32>(a) / static_cast<s32>(b));
                }
                break;
            case 459:                                                                // divwuo
                if (b == 0) { r = 0; ov = true; } else { r = a / b; }
                break;
            default:
                handled = false;
                break;
        }
        if (handled) {
            s[rd] = r;
            st.xer_ov = ov;
            st.xer_so |= static_cast<u8>(ov);
            if (rc) {
                st.cr[0] = cr_of(static_cast<s32>(r), st.xer_so);
            }
            return kHelperContinue;
        }
    }

    switch (xo10) {
        case 4:  // tw
            if (trap_condition(rd, s[ra], s[rb])) {
                take_exception(0x700, pc, kSrr1Trap);
                return kHelperExit;
            }
            return kHelperContinue;

        case 83:  // mfmsr
            s[rd] = st.msr;
            return kHelperContinue;
        case 146:  // mtmsr; the block ends after it, where interrupts are checked
            set_msr(s[rd]);
            return m_faulted ? kHelperExit : kHelperContinue;

        case 339: {  // mfspr
            const u32 spr = ((insn >> 16) & 0x1f) | ((insn >> 6) & 0x3e0);
            s[rd] = mfspr(spr, index);
            return m_faulted ? kHelperExit : kHelperContinue;
        }
        case 467: {  // mtspr
            const u32 spr = ((insn >> 16) & 0x1f) | ((insn >> 6) & 0x3e0);
            mtspr(spr, s[rd], index);
            return m_faulted ? kHelperExit : kHelperContinue;
        }
        case 371: {  // mftb
            const u32 tbr = ((insn >> 16) & 0x1f) | ((insn >> 6) & 0x3e0);
            const u64 tb  = read_tb(now_cycles(index));
            if (tbr == kSprTBLr) {
                s[rd] = static_cast<u32>(tb);
            } else if (tbr == kSprTBUr) {
                s[rd] = static_cast<u32>(tb >> 32);
            } else {
                return illegal();
            }
            return kHelperContinue;
        }

        case 210: st.sr[(insn >> 16) & 15] = s[rd]; return kHelperContinue;  // mtsr
        case 242: st.sr[s[rb] >> 28] = s[rd]; return kHelperContinue;        // mtsrin
        case 595: s[rd] = st.sr[(insn >> 16) & 15]; return kHelperContinue;  // mfsr
        case 659: s[rd] = st.sr[s[rb] >> 28]; return kHelperContinue;        // mfsrin

        case 512: {  // mcrxr
            st.cr[rd >> 2] = static_cast<u8>((st.get_xer() >> 28) & 0xf);
            st.xer_so = st.xer_ov = st.xer_ca = 0;
            return kHelperContinue;
        }

        case 20:  // lwarx
            st.reserved = 1;
            st.reserved_addr = ea_x;
            s[rd] = load<u32>(ea_x);
            return kHelperContinue;
        case 150:  // stwcx.
            if (st.reserved) {
                store<u32>(ea_x, s[rd]);
                st.reserved = 0;
                st.cr[0] = static_cast<u8>(kCrEQ | st.xer_so);
            } else {
                st.cr[0] = st.xer_so;
            }
            return kHelperContinue;

        case 597:    // lswi
        case 533: {  // lswx
            u32 ea = xo10 == 597 ? (ra != 0 ? s[ra] : 0u) : ea_x;
            u32 n  = xo10 == 597 ? (rb == 0 ? 32u : rb) : st.xer_bc;
            u32 r  = (rd + 31) & 31;
            u32 i  = 0;
            for (; n > 0; --n, ++ea) {
                if (i == 0) {
                    r = (r + 1) & 31;
                    s[r] = 0;
                }
                s[r] |= u32{load<u8>(ea)} << (24 - i);
                i = (i + 8) & 31;
            }
            return kHelperContinue;
        }
        case 725:    // stswi
        case 661: {  // stswx
            u32 ea = xo10 == 725 ? (ra != 0 ? s[ra] : 0u) : ea_x;
            u32 n  = xo10 == 725 ? (rb == 0 ? 32u : rb) : st.xer_bc;
            u32 r  = (rd + 31) & 31;
            u32 i  = 0;
            for (; n > 0; --n, ++ea) {
                if (i == 0) {
                    r = (r + 1) & 31;
                }
                store<u8>(ea, static_cast<u8>(s[r] >> (24 - i)));
                i = (i + 8) & 31;
            }
            return kHelperContinue;
        }

        case 1014: {  // dcbz: zero the 32-byte block
            const u32 ea = ea_x & ~31u;
            for (u32 k = 0; k < 32; k += 8) {
                store<u64>(ea + k, 0);
            }
            return kHelperContinue;
        }

        default:
            return illegal();
    }
}

void Ppc::mtfpscr(u32 value, u32 mask)
{
    mask &= ~(kFpscrFEX | kFpscrVX);
    m_st.fpscr = fpu::fpscr_summarise((m_st.fpscr & ~mask) | (value & mask));
}

u32 Ppc::mfspr(u32 spr, u32 index)
{
    PpcState& st = m_st;
    switch (spr) {
        case kSprXER:   return st.get_xer();
        case kSprLR:    return st.slot[kSlotLR];
        case kSprCTR:   return st.slot[kSlotCTR];
        case kSprSRR0:  return st.srr0;
        case kSprSRR1:  return st.srr1;
        case kSprSPRG0: case kSprSPRG1: case kSprSPRG2: case kSprSPRG3:
            return st.sprg[spr - kSprSPRG0];
        case kSprPVR:   return st.pvr;
        case kSprDEC:   return read_dec(now_cycles(index));
        case kSprTBLr: case kSprTBLw: return static_cast<u32>(read_tb(now_cycles(index)));
        case kSprTBUr: case kSprTBUw: return static_cast<u32>(read_tb(now_cycles(index)) >> 32);
        case kSprHID0:  return st.hid0;
        case kSprHID1:  return st.hid1;
        case kSprHID2:  return st.hid2;
        case kSprIABR:  return st.iabr;
        case kSprDABR:  return st.dabr;
        case kSprSDR1:  return st.sdr1;
        case kSprDSISR: return st.dsisr;
        case kSprDAR:   return st.dar;
        case kSprEAR:   return st.ear;
        case kSprDMISS: return st.dmiss;
        case kSprDCMP:  return st.dcmp;
        case kSprHASH1: return st.hash1;
        case kSprHASH2: return st.hash2;
        case kSprIMISS: return st.imiss;
        case kSprICMP:  return st.icmp;
        case kSprRPA:   return st.rpa;
        default:
            if (spr >= kSprIBAT0U && spr < kSprIBAT0U + 8) {
                return st.ibat[spr - kSprIBAT0U];
            }
            if (spr >= kSprDBAT0U && spr < kSprDBAT0U + 8) {
                return st.dbat[spr - kSprDBAT0U];
            }
            break;
    }
    char msg[64];
    std::snprintf(msg, sizeof msg, "mfspr from unknown SPR %u", spr);
    st.pc = m_st.block_pc + 4 * index;
    fault(msg);
    return 0;
}

void Ppc::mtspr(u32 spr, u32 v, u32 index)
{
    PpcState& st = m_st;
    switch (spr) {
        case kSprXER:   st.set_xer(v); return;
        case kSprLR:    st.slot[kSlotLR] = v; return;
        case kSprCTR:   st.slot[kSlotCTR] = v; return;
        case kSprSRR0:  st.srr0 = v; return;
        case kSprSRR1:  st.srr1 = v; return;
        case kSprSPRG0: case kSprSPRG1: case kSprSPRG2: case kSprSPRG3:
            st.sprg[spr - kSprSPRG0] = v;
            return;
        case kSprPVR:   return;  // read-only
        case kSprDEC:   write_dec(now_cycles(index), v); return;
        // Model 3 software writes TB through the read encodings too.
        case kSprTBLr: case kSprTBLw: write_tbl(now_cycles(index), v); return;
        case kSprTBUr: case kSprTBUw: write_tbu(now_cycles(index), v); return;
        case kSprHID0:  st.hid0 = v; return;
        case kSprHID1:  st.hid1 = v; return;
        case kSprHID2:  st.hid2 = v; return;
        case kSprIABR:  st.iabr = v; return;
        case kSprDABR:  st.dabr = v; return;
        case kSprSDR1:  st.sdr1 = v; return;
        case kSprDSISR: st.dsisr = v; return;
        case kSprDAR:   st.dar = v; return;
        case kSprEAR:   st.ear = v; return;
        case kSprDMISS: st.dmiss = v; return;
        case kSprDCMP:  st.dcmp = v; return;
        case kSprHASH1: st.hash1 = v; return;
        case kSprHASH2: st.hash2 = v; return;
        case kSprIMISS: st.imiss = v; return;
        case kSprICMP:  st.icmp = v; return;
        case kSprRPA:   st.rpa = v; return;
        default:
            if (spr >= kSprIBAT0U && spr < kSprIBAT0U + 8) {
                st.ibat[spr - kSprIBAT0U] = v;
                return;
            }
            if (spr >= kSprDBAT0U && spr < kSprDBAT0U + 8) {
                st.dbat[spr - kSprDBAT0U] = v;
                return;
            }
            break;
    }
    char msg[64];
    std::snprintf(msg, sizeof msg, "mtspr to unknown SPR %u", spr);
    st.pc = m_st.block_pc + 4 * index;
    fault(msg);
}

u32 Ppc::read_spr(u32 spr)
{
    if (!m_in_block) {
        m_st.block_cycles = m_cycles;
    }
    return mfspr(spr, 0);
}

void Ppc::write_spr(u32 spr, u32 value)
{
    if (!m_in_block) {
        m_st.block_cycles = m_cycles;
    }
    mtspr(spr, value, 0);
    if (!m_in_run) {
        check_interrupts();
    }
}

}  // namespace sm2::cpu::ppc
