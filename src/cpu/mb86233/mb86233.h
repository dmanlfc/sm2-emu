// SPDX-License-Identifier: BSD-3-Clause
//
// Fujitsu MB86233 / MB86234 "TGP" geometry coprocessor.
//
// Ported from MAME's src/devices/cpu/mb86233/mb86233.{cpp,h}, which are
// BSD-3-Clause, copyright-holders Olivier Galibert, building on Elsemi's original
// reverse engineering.
#pragma once

#include "core/types.h"
#include "cpu/mb86233/bus.h"

#include <string>

namespace sm2::cpu::mb86233 {

/// The geometry coprocessor of Model 1, Model 2 and Model 2A.
///
/// A 32-bit floating-point DSP. Three clocks per instruction, so the 50 MHz part
/// on a Model 2A retires about 16.7 million instructions a second.
///
/// Two behaviours matter more than the instruction set:
///
/// The part can be stalled. A read from a host FIFO that turns out to be empty
/// re-executes the whole instruction later rather than returning a value, so the
/// core has to be able to abandon an instruction after it has already had side
/// effects on the ALU. That is what stall() and the do_stall path are for.
///
/// The part can be halted from outside, which is how the host holds it while
/// uploading microcode and how the FIFO flow control blocks it. Halt is checked
/// every instruction, not only on entry, because a FIFO callback can assert it
/// part-way through a slice.
class MB86233 {
public:
    /// Clock divider: three clocks per instruction.
    static constexpr u32 kClocksPerCycle = 3;

    /// Status register flags, from MAME. Only a few are exercised by the Sega
    /// programs, but the ALU sets them all.
    enum StatusFlags : u32 {
        kZeroC       = 0x00000001,
        kZeroD       = 0x00000002,
        kSignC       = 0x00000004,
        kSignD       = 0x00000008,
        kCompareC    = 0x00000010,
        kCompareD    = 0x00000020,
        kOverflowC   = 0x00000040,
        kOverflowD   = 0x00000080,
        kUnderflowC  = 0x00000100,
        kUnderflowD  = 0x00000200,
        kDivZeroC    = 0x00000400,
        kDivZeroD    = 0x00000800,
        kCarry       = 0x00001000,
        kCompareP    = 0x00002000,
        kOverflowM   = 0x00004000,
        kUnderflowM  = 0x00008000,
        kSif0        = 0x00010000,
        kSif1        = 0x00020000,
        kSof0        = 0x00040000,
        kPif         = 0x00100000,
        kPof         = 0x00200000,
        kPaif        = 0x00400000,
        kPaof        = 0x00800000,
        kF0s         = 0x01000000,
        kF1s         = 0x02000000,
        kIt          = 0x04000000,
        kZeroX0      = 0x08000000,
        kZeroX1      = 0x10000000,
        kZeroX2      = 0x20000000,
        kZeroC0      = 0x40000000,
        kZeroC1      = 0x80000000,
    };

    explicit MB86233(Bus& bus);

    MB86233(const MB86233&)            = delete;
    MB86233& operator=(const MB86233&) = delete;

    void reset();

    /// Execute up to `cycles` instructions. Returns the number consumed, which is
    /// the whole budget when the core is halted.
    [[nodiscard]] s32 run(s32 cycles);

    /// Abandon the instruction in progress and execute it again next time.
    ///
    /// Called from a bus read that could not be satisfied. The instruction's
    /// effects on the ALU registers are discarded by virtue of the core reloading
    /// the program counter and starting over.
    void stall() { m_stall = true; }

    /// External halt, used while microcode is uploaded and by FIFO flow control.
    void set_halted(bool halted) { m_halted = halted; }
    [[nodiscard]] bool halted() const { return m_halted; }

    /// The four general-purpose input pins. Only gpio0 is used on Model 2, where
    /// the arctangent unit drives it with the result of a magnitude comparison.
    void set_gpio(u32 index, bool state);

    // -- state, for diagnostics and tests ----------------------------------

    [[nodiscard]] u16 pc() const { return m_pc; }
    [[nodiscard]] u16 previous_pc() const { return m_ppc; }
    void set_pc(u16 value) { m_pc = value; m_ppc = value; }

    [[nodiscard]] u32 status() const { return m_st; }
    [[nodiscard]] u32 reg_a() const { return m_a; }
    [[nodiscard]] u32 reg_b() const { return m_b; }
    [[nodiscard]] u32 reg_d() const { return m_d; }
    [[nodiscard]] u32 reg_p() const { return m_p; }
    [[nodiscard]] u16 sp() const { return m_sp; }
    [[nodiscard]] u64 instructions() const { return m_instructions; }

    [[nodiscard]] std::string state_string() const;

    /// Called after each instruction fetch with the instruction's own address.
    using TraceHook = void (*)(void* context, u16 address);
    void set_trace_hook(TraceHook hook, void* context)
    {
        m_trace_hook    = hook;
        m_trace_context = context;
    }

private:
    // Everything below this line keeps MAME's names so the ported body reads the
    // same as its original.

    // MAME's spellings for the flags above. They exist only so the ported ALU
    // stays textually identical to upstream and remains diffable against it.
    static constexpr u32 F_ZRC  = kZeroC;
    static constexpr u32 F_ZRD  = kZeroD;
    static constexpr u32 F_SGC  = kSignC;
    static constexpr u32 F_SGD  = kSignD;
    static constexpr u32 F_CPC  = kCompareC;
    static constexpr u32 F_CPD  = kCompareD;
    static constexpr u32 F_OVC  = kOverflowC;
    static constexpr u32 F_OVD  = kOverflowD;
    static constexpr u32 F_UNC  = kUnderflowC;
    static constexpr u32 F_UND  = kUnderflowD;
    static constexpr u32 F_DVZC = kDivZeroC;
    static constexpr u32 F_DVZD = kDivZeroD;
    static constexpr u32 F_CA   = kCarry;
    static constexpr u32 F_CPP  = kCompareP;
    static constexpr u32 F_OVM  = kOverflowM;
    static constexpr u32 F_UNM  = kUnderflowM;
    static constexpr u32 F_SIF0 = kSif0;
    static constexpr u32 F_SIF1 = kSif1;
    static constexpr u32 F_SOF0 = kSof0;
    static constexpr u32 F_PIF  = kPif;
    static constexpr u32 F_POF  = kPof;
    static constexpr u32 F_PAIF = kPaif;
    static constexpr u32 F_PAOF = kPaof;
    static constexpr u32 F_F0S  = kF0s;
    static constexpr u32 F_F1S  = kF1s;
    static constexpr u32 F_IT   = kIt;
    static constexpr u32 F_ZX0  = kZeroX0;
    static constexpr u32 F_ZX1  = kZeroX1;
    static constexpr u32 F_ZX2  = kZeroX2;
    static constexpr u32 F_ZC0  = kZeroC0;
    static constexpr u32 F_ZC1  = kZeroC1;

    static u32 set_exp(u32 val, u32 exp);
    static u32 set_mant(u32 val, u32 mant);
    static u32 get_exp(u32 val);
    static u32 get_mant(u32 val);

    void alu_update_st();
    void alu_pre(u32 alu);
    void alu_post_1(u32 alu);
    void alu_post_2(u32 alu);
    u16  ea_pre_0(u32 r);
    void ea_post_0(u32 r);
    u16  ea_pre_1(u32 r);
    void ea_post_1(u32 r);
    void pcs_push();
    void pcs_pop();
    inline void stset_set_sz_int(u32 val);
    inline void stset_set_sz_fp(u32 val);

    u32  read_reg(u32 r);
    void write_reg(u32 r, u32 v);
    void write_mem_internal_1(u32 r, u32 v, bool bank);
    void write_mem_external_1(u32 r, u32 v);
    void write_mem_io_1(u32 r, u32 v);

    void execute_run();

    Bus* m_bus = nullptr;

    int m_icount = 0;

    u32 m_st = 0, m_a = 0, m_b = 0, m_d = 0, m_p = 0;
    u32 m_alu_stmask = 0, m_alu_stset = 0, m_alu_r1 = 0, m_alu_r2 = 0;
    u16 m_ppc = 0, m_pc = 0, m_sp = 0, m_b0 = 0, m_b1 = 0, m_x0 = 0, m_x1 = 0;
    u16 m_i0 = 0, m_i1 = 0, m_vsmr = 0, m_pcs[4]{}, m_mask = 0, m_m = 0;
    u8  m_r = 0, m_rpc = 0, m_c0 = 0, m_c1 = 0, m_sft = 0, m_vsm = 0;
    bool m_gpio0 = false, m_gpio1 = false, m_gpio2 = false, m_gpio3 = false;

    bool m_stall  = false;
    bool m_halted = false;

    u64 m_instructions = 0;

    TraceHook m_trace_hook    = nullptr;
    void*     m_trace_context = nullptr;
};

}  // namespace sm2::cpu::mb86233
