// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Zilog Z80 CPU core — interpreter.
//
// Derived from MAME's src/devices/cpu/z80/, which is BSD-3-Clause,
// copyright-holders Juergen Buchmueller, Andrei I. Holub.
//
// Changes from upstream: MAME device/address-space plumbing replaced by a Bus
// interface; the daisy chain, the nomreq/refresh/busack pin callbacks and the
// BUSREQ line are not modelled; the T-state-resumable microcode state machine
// generated from z80.lst by z80make.py is collapsed into a plain per-instruction
// interpreter that charges the same T-states at the same points, because nothing
// on a Model 2 board can suspend this CPU mid-access.
//
// There is no DRC variant of MAME's Z80, so the interpreter is the whole core.
//
// Z80 CPU core, used by the model1io and model1io2 (TMPZ84C015) boards and
// Daytona's drive board. Peripherals and boards live elsewhere.
#pragma once

#include "core/types.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <string>
#include <type_traits>

namespace sm2 {
class Archive;
}

namespace sm2::cpu::z80 {

// ============================================================================
// Bus — the two address spaces visible to the Z80
// ============================================================================

class Bus {
public:
    virtual ~Bus() = default;

    /// Memory space (64 KB). MAME's separate opcode and stack spaces all route
    /// here, since no Model 2 board needs them.
    [[nodiscard]] virtual u8 read8(u16 address) = 0;
    virtual void write8(u16 address, u8 value) = 0;

    /// I/O space, 16-bit port address. `OUT (n),A` puts A on the high half.
    [[nodiscard]] virtual u8 io_read8(u16 port) = 0;
    virtual void io_write8(u16 port, u8 value) = 0;

    /// Fetched during an interrupt acknowledge in mode 0/2: the low half of the
    /// vector address in mode 2, an opcode in mode 0. An undriven bus reads 0xff
    /// (`RST 38h`).
    [[nodiscard]] virtual u8 interrupt_vector() { return 0xff; }

    /// RETI retired, so a daisy-chained peripheral can release its in-service
    /// latch.
    virtual void interrupt_return() {}
};

// ============================================================================
// Z80 — the CPU core
// ============================================================================

class Z80 {
public:
    explicit Z80(Bus& bus);

    Z80(const Z80&)            = delete;
    Z80& operator=(const Z80&) = delete;

    /// MAME's device_reset: leaves halt, clears PC/WZ/I/R/IFF and any pending
    /// NMI, but not the register file, IM or SP, as on hardware. A new core is
    /// in MAME's post-device_start state (IX = IY = 0xffff, Z set).
    void reset();

    /// Execute T-states until the budget runs out. Returns the cycles charged:
    /// exactly `cycles` for a positive budget, 0 otherwise. The instruction that
    /// overruns the slice is finished and its surplus carried into the next call
    /// (see `cycle_debt`) rather than dropped, which would run the CPU fast.
    /// Use `cycles()` for measurement.
    [[nodiscard]] s32 run(s32 cycles);

    // -- interrupt and control lines ----------------------------------------

    /// INT is level-sensitive, sampled at the end of every instruction.
    void set_irq_line(bool asserted);

    /// NMI is edge-triggered: the rising edge latches a pending NMI, taken at the
    /// next instruction boundary even if the line has dropped.
    void set_nmi_line(bool asserted);

    /// WAIT stalls the CPU. While held, `run` consumes its budget and retires
    /// nothing.
    void set_wait_line(bool asserted) { m_wait_state = asserted; }

    /// True between a HALT and the interrupt or reset that lifts it. A halted
    /// core consumes its budget in M1 cycles but retires nothing.
    [[nodiscard]] bool halted() const { return m_halt != 0; }

    // -- cycle timing configuration -----------------------------------------
    // Boards may stretch the three bus cycle types; defaults are the stock
    // 4/3/4. The setters enforce the hardware minima (MAME checks them in
    // device_validity_check): a zero M1 cycle would let `run` never finish.
    static constexpr u8 kMinM1Cycles   = 4;
    static constexpr u8 kMinMreqCycles = 3;
    static constexpr u8 kMinIorqCycles = 4;

    void set_m1_cycles(u8 cycles) { m_m1_cycles = std::max(cycles, kMinM1Cycles); }
    void set_mreq_cycles(u8 cycles) { m_mreq_cycles = std::max(cycles, kMinMreqCycles); }
    void set_iorq_cycles(u8 cycles) { m_iorq_cycles = std::max(cycles, kMinIorqCycles); }
    [[nodiscard]] u8 m1_cycles() const { return m_m1_cycles; }
    [[nodiscard]] u8 mreq_cycles() const { return m_mreq_cycles; }
    [[nodiscard]] u8 iorq_cycles() const { return m_iorq_cycles; }

    // -- state accessors for debugger / diagnostics -------------------------

    [[nodiscard]] u16 pc() const { return m_pc.w; }
    [[nodiscard]] u16 prev_pc() const { return m_prvpc.w; }
    [[nodiscard]] u16 sp() const { return m_sp.w; }
    [[nodiscard]] u16 af() const { return u16((m_af.b.h << 8) | get_f()); }
    [[nodiscard]] u16 bc() const { return m_bc.w; }
    [[nodiscard]] u16 de() const { return m_de.w; }
    [[nodiscard]] u16 hl() const { return m_hl.w; }
    [[nodiscard]] u16 ix() const { return m_ix.w; }
    [[nodiscard]] u16 iy() const { return m_iy.w; }
    [[nodiscard]] u16 wz() const { return m_wz.w; }
    [[nodiscard]] u16 af2() const { return m_af2.w; }
    [[nodiscard]] u16 bc2() const { return m_bc2.w; }
    [[nodiscard]] u16 de2() const { return m_de2.w; }
    [[nodiscard]] u16 hl2() const { return m_hl2.w; }
    [[nodiscard]] u8 a() const { return m_af.b.h; }
    [[nodiscard]] u8 b() const { return m_bc.b.h; }
    [[nodiscard]] u8 c() const { return m_bc.b.l; }
    [[nodiscard]] u8 d() const { return m_de.b.h; }
    [[nodiscard]] u8 e() const { return m_de.b.l; }
    [[nodiscard]] u8 h() const { return m_hl.b.h; }
    [[nodiscard]] u8 l() const { return m_hl.b.l; }
    [[nodiscard]] u8 i() const { return m_i; }
    /// The refresh register as software sees it: bit 7 does not increment.
    [[nodiscard]] u8 r() const { return u8((m_r & 0x7f) | (m_r2 & 0x80)); }
    [[nodiscard]] u8 im() const { return m_im; }
    [[nodiscard]] bool iff1() const { return m_iff1; }
    [[nodiscard]] bool iff2() const { return m_iff2; }

    /// F assembled from the deferred-flag representation. See `Flags`.
    [[nodiscard]] u8 get_f() const;

    void set_pc(u16 value);
    void set_sp(u16 value) { m_sp.w = value; }
    void set_af(u16 value);
    void set_bc(u16 value) { m_bc.w = value; }
    void set_de(u16 value) { m_de.w = value; }
    void set_hl(u16 value) { m_hl.w = value; }
    void set_ix(u16 value) { m_ix.w = value; }
    void set_iy(u16 value) { m_iy.w = value; }
    void set_a(u8 value) { m_af.b.h = value; }
    void set_i(u8 value) { m_i = value; }
    void set_im(u8 value) { m_im = value & 3; }
    void set_iff1(bool value) { m_iff1 = value; }
    void set_iff2(bool value) { m_iff2 = value; }

    /// Unpack F into the deferred-flag representation.
    void set_f(u8 f);

    /// Instructions retired. A prefix chain counts once, however long it is, and
    /// the refresh cycles run while halted do not count at all.
    [[nodiscard]] u64 instructions() const { return m_instruction_count; }

    /// T-states consumed since construction, including halted and WAIT cycles.
    /// Equals the sum of `run`'s returns plus the outstanding `cycle_debt`.
    [[nodiscard]] u64 cycles() const { return m_total_cycles; }

    /// T-states executed but not yet reported, owed to the next `run`.
    [[nodiscard]] s32 cycle_debt() const { return m_cycle_debt; }

    [[nodiscard]] std::string state_string() const;

    using TraceHook = void (*)(void* context, u16 pc, u8 opcode);
    void set_trace_hook(TraceHook hook, void* context)
    {
        m_trace_hook    = hook;
        m_trace_context = context;
    }

    /// Save/restore the whole register set (including the alternate set and the
    /// deferred-flag struct), interrupt/line state and cycle carry. Bus and
    /// trace hook are excluded.
    void serialize(Archive& ar);

private:
    // -- flag bits ----------------------------------------------------------
    static constexpr u8 CF  = 0x01;
    static constexpr u8 NF  = 0x02;
    static constexpr u8 PF  = 0x04;
    static constexpr u8 VF  = PF;
    static constexpr u8 HF  = 0x10;
    static constexpr u8 YXF = 0x28;
    static constexpr u8 ZF  = 0x40;
    static constexpr u8 SF  = 0x80;

    /// NMOS quirk: an interrupted `LD A,I` / `LD A,R` resets P/V even with IFF2
    /// set. Off, as in MAME, since the variant on each board is unknown.
    static constexpr bool kHasLdairQuirk = false;

    // -- 16-bit register pair ----------------------------------------------
    // MAME's PAIR16. Byte order matches the host so `H`/`L` alias `HL`, letting
    // the ported bodies take a `u8&` to half of a pair.
    struct BytesLE {
        u8 l;
        u8 h;
    };
    struct BytesBE {
        u8 h;
        u8 l;
    };
    using Bytes = std::conditional_t<std::endian::native == std::endian::little, BytesLE, BytesBE>;
    static_assert(std::endian::native == std::endian::little
                      || std::endian::native == std::endian::big,
                  "mixed-endian hosts are not supported");

    union Pair16 {
        u16   w;
        Bytes b;
    };
    static_assert(sizeof(Pair16) == 2);

    // -- deferred flags -----------------------------------------------------
    //
    // Ported verbatim from MAME. Each flag keeps the value it derives from and F
    // is built only when read. SCF/CCF's X/Y flags depend on whether the
    // previous instruction wrote F, which `q`/`qtemp` track.
    struct Flags {
        u8   s_val  = 0;  ///< bit 7, other bits don't care
        u8   z_val  = 0;  ///< zero when the result was zero
        u8   yx_val = 0;  ///< bit 5 for Y, bit 3 for X
        u8   h_val  = 0;  ///< bit 4, other bits don't care
        u8   pv_val = 0;  ///< parity of this byte, or !overflow for arithmetic
        bool n      = false;
        bool c      = false;

        u8 q     = 0;  ///< SCF/CCF X/Y mask: 0 if the last instruction wrote F
        u8 qtemp = 0;

        [[nodiscard]] u8 s() const { return s_val & 0x80; }
        [[nodiscard]] u8 z() const { return z_val ? 0 : 0x40; }
        [[nodiscard]] u8 yx() const { return yx_val & 0x28; }
        [[nodiscard]] u8 h() const { return h_val & 0x10; }
        [[nodiscard]] u8 pv() const
        {
            u8 val = pv_val;
            val ^= val >> 4;
            val ^= val << 2;
            val ^= val >> 1;
            return ~val & 0x04;
        }
    };

    // -- service-step attention bits ---------------------------------------
    // Conditions to check before the next opcode, as one bitmap so the hot path
    // is a single test.
    static constexpr u8 SA_NMI_PENDING = 1;
    static constexpr u8 SA_IRQ_ON      = 2;
    static constexpr u8 SA_HALT        = 3;
    static constexpr u8 SA_AFTER_EI    = 4;
    static constexpr u8 SA_AFTER_LDAIR = 5;

    template <u8 Bit, bool State>
    void set_service_attention()
    {
        static_assert(Bit < 8, "out of range bit index");
        if (State) {
            m_service_attention |= u8(1 << Bit);
        } else {
            m_service_attention &= u8(~(1 << Bit));
        }
    }
    template <u8 Bit>
    [[nodiscard]] bool get_service_attention() const
    {
        static_assert(Bit < 8, "out of range bit index");
        return (m_service_attention & (1 << Bit)) != 0;
    }

    // -- bus access ---------------------------------------------------------
    // Each charges the same T-states as MAME's z80.lst macro.

    [[nodiscard]] u8 data_read(u16 addr);
    void data_write(u16 addr, u8 value);
    [[nodiscard]] u8 stack_read(u16 addr) { return data_read(addr); }
    void stack_write(u16 addr, u8 value) { data_write(addr, value); }
    [[nodiscard]] u8 opcode_read();
    [[nodiscard]] u8 arg_read();

    /// M1 cycle: fetch an opcode, bump PC and R, roll the Q shadow over.
    u8 rop();

    /// Immediate operand fetch. Lands in TDAT8, as the microcode step does.
    void arg();
    [[nodiscard]] u16 arg16();

    void rm(u16 addr);      ///< TDAT8 = [addr]
    void rm_reg(u16 addr);  ///< as rm, plus the one internal cycle a
                            ///< read-modify-write opcode spends on the value
    [[nodiscard]] u16 rm16(u16 addr);
    void wm(u16 addr);  ///< [addr] = TDAT8
    void wm16(u16 addr, u16 value);
    void wm_sp(u8 value);
    void wm16_sp(u16 value);
    [[nodiscard]] u16 pop16();
    void push16(u16 value);
    void in_port(u16 port);  ///< TDAT8 = port
    void out_port(u16 port);

    /// Internal cycles with `addr` on the bus but no bus cycle. `addr` is unused
    /// (MAME's nomreq callback) but documents what the part drives.
    void nomreq_addr(u16 addr, int count);
    void nomreq_ir(int count);

    // -- ALU / flag helpers (MAME z80_device members, unchanged) ------------
    void halt();
    void leave_halt();
    void inc(u8& r);
    void dec(u8& r);
    void rlca();
    void rrca();
    void rla();
    void rra();
    void add_a(u8 value);
    void adc_a(u8 value);
    void sub_a(u8 value);
    void sbc_a(u8 value);
    void neg();
    void daa();
    void and_a(u8 value);
    void or_a(u8 value);
    void xor_a(u8 value);
    void cp(u8 value);
    void exx();
    [[nodiscard]] u8 rlc(u8 value);
    [[nodiscard]] u8 rrc(u8 value);
    [[nodiscard]] u8 rl(u8 value);
    [[nodiscard]] u8 rr(u8 value);
    [[nodiscard]] u8 sla(u8 value);
    [[nodiscard]] u8 sra(u8 value);
    [[nodiscard]] u8 sll(u8 value);
    [[nodiscard]] u8 srl(u8 value);
    /// Dispatch for the eight CB-prefix rotate/shift operations.
    [[nodiscard]] u8 shift_op(int operation, u8 value);
    void bit(int bit_index, u8 value);
    void bit_hl(int bit_index, u8 value);
    void bit_xy(int bit_index, u8 value);
    [[nodiscard]] static u8 res(int bit_index, u8 value);
    [[nodiscard]] static u8 set(int bit_index, u8 value);
    void ei();
    void block_io_interrupted_flags();

    void add16(Pair16& dst, u16 src);
    void adc_hl(u16 src);
    void sbc_hl(u16 src);
    void ex_sp(Pair16& dst);

    /// MAME's @eax / @eay: read the signed displacement and latch IX/IY + d.
    void eax(Pair16& xy);

    void ldi();
    void cpi();
    void ini();
    void outi();
    void ldd();
    void cpd();
    void ind();
    void outd();

    void jp();
    void jp_cond(bool condition);
    void jr();
    void jr_cond(bool condition);
    void arg16_call();
    void call_cond(bool condition);
    void ret_cond(bool condition);
    void retn();
    void reti();
    void rst(u16 address);
    void ld_r_a();
    void ld_a_r();
    void ld_i_a();
    void ld_a_i();
    void rrd();
    void rld();

    /// Undocumented-opcode diagnostics. Deviation: MAME re-reads the bytes
    /// through the bus; these take what was already fetched, so logging cannot
    /// change bus cycles.
    void illegal_1(u8 prefix, u8 op);
    void illegal_2(u8 op);

    // -- dispatch -----------------------------------------------------------
    /// Points at the byte an opcode's 3-bit register field selects. Index 6 is
    /// the memory operand, which the CB/DDCB bodies stage in TDAT8.
    [[nodiscard]] u8* reg8_ptr(int index);

    void execute_one();
    void op_main(u8 op);
    void op_cb(u8 op);
    void op_ed(u8 op);
    void op_xycb(u8 op);
    /// DD/FD prefix. Returns false when `op` is not an index-register opcode;
    /// the prefix is then discarded (4 wasted T-states) and `op` runs from the
    /// main table. `prefix` is only for the diagnostic.
    [[nodiscard]] bool op_index(u8 prefix, u8 op, Pair16& xy);

    void check_interrupts();
    void take_nmi();
    void take_interrupt();

    // -- state --------------------------------------------------------------
    Bus* m_bus = nullptr;

    Pair16 m_prvpc{};
    Pair16 m_pc{};
    Pair16 m_sp{};
    Pair16 m_af{};
    Pair16 m_bc{};
    Pair16 m_de{};
    Pair16 m_hl{};
    Pair16 m_ix{};
    Pair16 m_iy{};
    Pair16 m_wz{};
    Pair16 m_af2{};
    Pair16 m_bc2{};
    Pair16 m_de2{};
    Pair16 m_hl2{};

    Flags m_f{};

    u8   m_r  = 0;  ///< refresh counter, low 7 bits
    u8   m_r2 = 0;  ///< bit 7 of R, which software can set but hardware never bumps
    u8   m_i  = 0;
    bool m_iff1 = false;
    bool m_iff2 = false;
    u8   m_halt = 0;
    u8   m_im   = 0;

    bool m_nmi_state  = false;
    bool m_irq_state  = false;
    bool m_wait_state = false;

    u16 m_ea = 0;
    u8  m_service_attention = 0;
    u32 m_tmp_irq_vector    = 0;

    /// MAME's m_shared_data / m_shared_data2: scratch passed between microcode
    /// steps, which some flag results read.
    Pair16 m_shared_data{};
    Pair16 m_shared_data2{};

    s32 m_icount     = 0;
    s32 m_cycle_debt = 0;

    u8 m_m1_cycles   = 4;
    u8 m_mreq_cycles = 3;
    u8 m_iorq_cycles = 4;

    u64 m_instruction_count = 0;
    u64 m_total_cycles      = 0;

    TraceHook m_trace_hook    = nullptr;
    void*     m_trace_context = nullptr;
};

}  // namespace sm2::cpu::z80
