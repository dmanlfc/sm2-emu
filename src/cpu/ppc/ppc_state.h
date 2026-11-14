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

#include <cstddef>

// Architectural state shared by the interpreter and generated code (which
// addresses it by offsetof). Host-endian values.

namespace sm2::cpu::ppc {

// Integer IR operands index slot[].
enum Slot : u8 {
    kSlotR0   = 0,   ///< r0..r31 are slots 0..31.
    kSlotLR   = 32,  ///< Link register.
    kSlotCTR  = 33,  ///< Count register.
    kSlotZero = 34,  ///< Always 0. Never written. Stands in for "(rA|0)" addressing.
    kSlotT0   = 35,  ///< Block-local temporaries T0..T3: dead at every block exit.
    kSlotT1   = 36,
    kSlotT2   = 37,
    kSlotT3   = 38,
    kSlotCount = 40,
};

// -- MSR bits (PEM 2.3.1, 603e UM 2.1.2.3) -------------------------------------
inline constexpr u32 kMsrPOW = 0x00040000;
inline constexpr u32 kMsrTGPR = 0x00020000;  ///< 603e: temporary GPR remapping (TLB miss).
inline constexpr u32 kMsrILE = 0x00010000;
inline constexpr u32 kMsrEE  = 0x00008000;
inline constexpr u32 kMsrPR  = 0x00004000;
inline constexpr u32 kMsrFP  = 0x00002000;
inline constexpr u32 kMsrME  = 0x00001000;
inline constexpr u32 kMsrFE0 = 0x00000800;
inline constexpr u32 kMsrSE  = 0x00000400;
inline constexpr u32 kMsrBE  = 0x00000200;
inline constexpr u32 kMsrFE1 = 0x00000100;
inline constexpr u32 kMsrIP  = 0x00000040;
inline constexpr u32 kMsrIR  = 0x00000020;
inline constexpr u32 kMsrDR  = 0x00000010;
inline constexpr u32 kMsrRI  = 0x00000002;
inline constexpr u32 kMsrLE  = 0x00000001;

// -- FPSCR bits (PEM 2.1.4) ----------------------------------------------------
inline constexpr u32 kFpscrFX     = 0x80000000;
inline constexpr u32 kFpscrFEX    = 0x40000000;
inline constexpr u32 kFpscrVX     = 0x20000000;
inline constexpr u32 kFpscrOX     = 0x10000000;
inline constexpr u32 kFpscrUX     = 0x08000000;
inline constexpr u32 kFpscrZX     = 0x04000000;
inline constexpr u32 kFpscrXX     = 0x02000000;
inline constexpr u32 kFpscrVXSNAN = 0x01000000;
inline constexpr u32 kFpscrVXISI  = 0x00800000;
inline constexpr u32 kFpscrVXIDI  = 0x00400000;
inline constexpr u32 kFpscrVXZDZ  = 0x00200000;
inline constexpr u32 kFpscrVXIMZ  = 0x00100000;
inline constexpr u32 kFpscrVXVC   = 0x00080000;
inline constexpr u32 kFpscrFR     = 0x00040000;
inline constexpr u32 kFpscrFI     = 0x00020000;
inline constexpr u32 kFpscrFPRF   = 0x0001f000;  ///< C and FPCC (FL FG FE FU).
inline constexpr u32 kFpscrVXSOFT = 0x00000400;
inline constexpr u32 kFpscrVXSQRT = 0x00000200;
inline constexpr u32 kFpscrVXCVI  = 0x00000100;
inline constexpr u32 kFpscrVE     = 0x00000080;
inline constexpr u32 kFpscrOE     = 0x00000040;
inline constexpr u32 kFpscrUE     = 0x00000020;
inline constexpr u32 kFpscrZE     = 0x00000010;
inline constexpr u32 kFpscrXE     = 0x00000008;
inline constexpr u32 kFpscrNI     = 0x00000004;
inline constexpr u32 kFpscrRN     = 0x00000003;
/// All the VX* invalid-operation causes, whose OR is VX.
inline constexpr u32 kFpscrVXAll = kFpscrVXSNAN | kFpscrVXISI | kFpscrVXIDI | kFpscrVXZDZ |
                                   kFpscrVXIMZ | kFpscrVXVC | kFpscrVXSOFT | kFpscrVXSQRT |
                                   kFpscrVXCVI;

// -- CR field bits, as stored in PpcState::cr[n] -------------------------------
inline constexpr u8 kCrLT = 8;
inline constexpr u8 kCrGT = 4;
inline constexpr u8 kCrEQ = 2;
inline constexpr u8 kCrSO = 1;

// -- SPR numbers used by the 603e (603e UM table 2-2) --------------------------
enum Spr : u32 {
    kSprXER   = 1,
    kSprLR    = 8,
    kSprCTR   = 9,
    kSprDSISR = 18,
    kSprDAR   = 19,
    kSprDEC   = 22,
    kSprSDR1  = 25,
    kSprSRR0  = 26,
    kSprSRR1  = 27,
    kSprTBLr  = 268,  ///< mftb encoding; accepted by mfspr too.
    kSprTBUr  = 269,
    kSprSPRG0 = 272,
    kSprSPRG1 = 273,
    kSprSPRG2 = 274,
    kSprSPRG3 = 275,
    kSprEAR   = 282,
    kSprTBLw  = 284,
    kSprTBUw  = 285,
    kSprPVR   = 287,
    kSprIBAT0U = 528,  ///< IBAT0U..IBAT3L are 528..535, DBAT0U..DBAT3L 536..543.
    kSprDBAT0U = 536,
    kSprDMISS = 976,
    kSprDCMP  = 977,
    kSprHASH1 = 978,
    kSprHASH2 = 979,
    kSprIMISS = 980,
    kSprICMP  = 981,
    kSprRPA   = 982,
    kSprHID0  = 1008,
    kSprHID1  = 1009,
    kSprIABR  = 1010,
    kSprHID2  = 1011,  ///< 603ev / 603r only; accepted on every model.
    kSprDABR  = 1013,
};

enum ExitFlag : u32 {
    kExitBreak     = 1u << 0,  ///< Leave the block after the current instruction.
    kExitEndSlice  = 1u << 1,  ///< ...and return from run() too.
};

struct PpcState {
    // ---- integer slots (see Slot) --------------------------------------------
    u32 slot[kSlotCount];

    // ---- condition register, one field per byte (LT=8 GT=4 EQ=2 SO=1) -------
    u8 cr[8];

    // ---- XER, split so carry and overflow are plain byte stores --------------
    u8 xer_so;   ///< 0 or 1.
    u8 xer_ov;   ///< 0 or 1.
    u8 xer_ca;   ///< 0 or 1.
    u8 xer_bc;   ///< Byte count for lswx/stswx (bits 25..31 of XER).

    u32 fpscr;

    /// Raw double bit patterns (moves never quiet an SNaN).
    alignas(8) u64 fpr[32];

    // ---- control -------------------------------------------------------------
    u32 pc;          ///< Next instruction to execute (at block boundaries).
    u32 exit_flags;  ///< ExitFlag bits; see above.
    /// Instruction index in the block, stored before ppc_rt_read*/write*.
    u32 cur_insn;

    // ---- supervisor state ----------------------------------------------------
    u32 msr;
    u32 srr0, srr1;
    u32 sprg[4];
    u32 dsisr, dar, sdr1, ear;
    u32 hid0, hid1, hid2;
    u32 iabr, dabr;
    u32 pvr;
    u32 sr[16];
    u32 ibat[8];  ///< IBAT0U, IBAT0L, IBAT1U, ... in SPR order.
    u32 dbat[8];
    u32 dmiss, dcmp, hash1, hash2, imiss, icmp, rpa;

    // ---- lwarx / stwcx. reservation ------------------------------------------
    u32 reserved;       ///< 0 or 1.
    u32 reserved_addr;

    // ---- fast-path memory tables (copies of PageMap's; see ppc_ir.h) --------
    u8* const* read_table;
    u8* const* write_table;

    void* cpu;

    // ---- run loop ------------------------------------------------------------
    u64 block_cycles;  ///< Cycles completed when the current block started.
    u32 block_pc;      ///< pc of the current block, for helpers.
    u32 budget;        ///< Cycles a native entry may run (>= its block's length).

    // ---- timebase: TB = tb_offset + cycles / timer_ratio ----------------------
    u64 tb_offset;
    u32 timer_ratio;

    // ---- accessors -----------------------------------------------------------
    [[nodiscard]] u32& r(unsigned n) { return slot[n]; }
    [[nodiscard]] u32 r(unsigned n) const { return slot[n]; }
    [[nodiscard]] double f(unsigned n) const { return std::bit_cast<double>(fpr[n]); }
    void set_f(unsigned n, double v) { fpr[n] = std::bit_cast<u64>(v); }

    /// The architectural 32-bit CR and XER.
    [[nodiscard]] u32 get_cr() const
    {
        u32 v = 0;
        for (unsigned i = 0; i < 8; ++i) {
            v = (v << 4) | (cr[i] & 0xf);
        }
        return v;
    }
    void set_cr(u32 v)
    {
        for (unsigned i = 0; i < 8; ++i) {
            cr[i] = static_cast<u8>((v >> (28 - 4 * i)) & 0xf);
        }
    }
    [[nodiscard]] u32 get_xer() const
    {
        return (u32{xer_so} << 31) | (u32{xer_ov} << 30) | (u32{xer_ca} << 29) | (xer_bc & 0x7fu);
    }
    void set_xer(u32 v)
    {
        xer_so = static_cast<u8>((v >> 31) & 1);
        xer_ov = static_cast<u8>((v >> 30) & 1);
        xer_ca = static_cast<u8>((v >> 29) & 1);
        xer_bc = static_cast<u8>(v & 0x7f);
    }
};

}  // namespace sm2::cpu::ppc
