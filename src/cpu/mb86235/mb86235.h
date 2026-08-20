// SPDX-License-Identifier: BSD-3-Clause
//
// Fujitsu MB86235 "TGPx4" DSP core — interpreter.
//
// Derived from MAME's src/devices/cpu/mb86235/, which is BSD-3-Clause,
// copyright-holders Angelo Salese, ElSemi, Ville Linde, Matthew Daniels.
//
// Changes from upstream: MAME device/address-space plumbing replaced by a Bus
// interface; the (default-disabled) dynamic recompiler is not ported; state is
// kept in-class rather than in a cache-aligned heap block.
//
// Despite the "x4" in the marketing name this is a single core — Model 2C
// instantiates exactly one of them. The name refers to throughput, not to a
// core count.
#pragma once

#include "core/types.h"

#include <cstdint>
#include <string>

namespace sm2::cpu::mb86235 {

// ============================================================================
// Bus — the address spaces and FIFOs visible to the MB86235
// ============================================================================
//
// The MB86235 sees four spaces in MAME. Two of them (the A-bus and B-bus
// internal RAMs, 0x400 words each) are on-chip and are owned by the core
// itself, so they are absent here. This interface covers only what the host
// board supplies:
//
//   AS_PROGRAM  64-bit instruction words, 12-bit word address (4096 entries)
//   AS_DATA     the external bus, 32-bit words, 24-bit word address
//
// plus the two host FIFOs, which MAME wires as devices rather than as memory.
class Bus {
public:
    virtual ~Bus() = default;

    /// Program memory: 64-bit instruction words at a 12-bit word address.
    [[nodiscard]] virtual u64 program_read(u32 address) = 0;
    virtual void program_write(u32 address, u64 data) = 0;

    /// External bus (MAME's AS_DATA): 32-bit words at a 24-bit word address.
    /// On Model 2C this reaches buffer RAM and the copro_data ROM.
    [[nodiscard]] virtual u32 external_read(u32 address) = 0;
    virtual void external_write(u32 address, u32 data) = 0;

    // -- host FIFOs --------------------------------------------------------

    /// True when there is nothing for the core to read.
    [[nodiscard]] virtual bool fifo_in_empty() const = 0;

    /// True when the core cannot push another result.
    [[nodiscard]] virtual bool fifo_out_full() const = 0;

    /// The other two halves of the same picture. The MB86235 can branch on all
    /// four FIFO conditions (IFE/IFF/OFE/OFF), so all four have to be answerable
    /// or a microcode wait loop on one of them would never terminate.
    [[nodiscard]] virtual bool fifo_in_full() const = 0;
    [[nodiscard]] virtual bool fifo_out_empty() const = 0;

    /// Take one word from the input FIFO. Only called when it is not empty.
    [[nodiscard]] virtual u32 fifo_in_pop() = 0;

    /// Push one result word. Only called when the output FIFO is not full.
    virtual void fifo_out_push(u32 value) = 0;

    /// Discard everything queued. Backs the CLRFI/CLRFO control ops.
    virtual void fifo_in_clear() = 0;
    virtual void fifo_out_clear() = 0;
};

// ============================================================================
// MB86235 — the CPU core
// ============================================================================

class MB86235 {
public:
    /// Program memory is 4096 64-bit words (12-bit word address).
    static constexpr u32 kProgramWords = 0x1000;

    /// Each of the two internal data RAMs is 0x400 32-bit words.
    static constexpr u32 kInternalRamWords = 0x400;

    explicit MB86235(Bus& bus);

    MB86235(const MB86235&)            = delete;
    MB86235& operator=(const MB86235&) = delete;

    void reset();

    /// Execute up to `cycles` instructions. Returns the number actually
    /// consumed, which is never more than requested.
    [[nodiscard]] s32 run(s32 cycles);

    /// External halt — the host holds this while uploading microcode.
    void set_halted(bool halted) { m_halted = halted; }
    [[nodiscard]] bool halted() const { return m_halted; }

    /// Re-run the current instruction because a FIFO access could not complete
    /// (MAME: mb86235_device::stall).
    void stall() { m_fifo_state.has_stalled = true; }

    // -- state accessors for debugger / diagnostics -------------------------

    [[nodiscard]] u32 pc() const { return m_pc; }
    [[nodiscard]] u32 ppc() const { return m_ppc; }
    /// The status word as software sees it. The arithmetic condition flags are
    /// held in the Flags struct rather than in ST, so they are folded back in
    /// here; the signature and meaning are unchanged.
    [[nodiscard]] u32 st() const { return pack_st(); }
    [[nodiscard]] u32 mod() const { return m_mod; }
    [[nodiscard]] u32 sp() const { return m_sp; }
    [[nodiscard]] u32 aa(int index) const { return m_aa[index & 7]; }
    [[nodiscard]] u32 ab(int index) const { return m_ab[index & 7]; }
    [[nodiscard]] u32 ma(int index) const { return m_ma[index & 7]; }
    [[nodiscard]] u32 mb(int index) const { return m_mb[index & 7]; }
    [[nodiscard]] u32 ar(int index) const { return m_ar[index & 7]; }
    [[nodiscard]] u64 instructions() const { return m_instruction_count; }

    /// True once the core hit an opcode this port does not implement. The host
    /// can surface this rather than letting it pass silently, which is how the
    /// other cores in this project report the same condition.
    [[nodiscard]] bool faulted() const { return m_faulted; }
    [[nodiscard]] const std::string& fault_message() const { return m_fault_message; }

    [[nodiscard]] std::string state_string() const;

    using TraceHook = void (*)(void* context, u32 pc, u64 opcode);
    void set_trace_hook(TraceHook hook, void* context)
    {
        m_trace_hook    = hook;
        m_trace_context = context;
    }

private:
    // -- status word (ST) bits ---------------------------------------------
    // MAME keeps these both as a packed ST and as a struct of separate flags;
    // this port keeps only the struct and packs on demand.
    struct Flags {
        u32 az = 0;  // ALU zero
        u32 an = 0;  // ALU negative
        u32 av = 0;  // ALU overflow
        u32 au = 0;  // ALU underflow
        u32 ad = 0;  // ALU divide-by-zero
        u32 zc = 0;  // shifter carry
        u32 il = 0;  // illegal input
        u32 nr = 0;  // not reciprocal
        u32 zd = 0;  // zero divisor
        u32 mn = 0;  // multiplier negative
        u32 mz = 0;  // multiplier zero
        u32 mv = 0;  // multiplier overflow
        u32 mu = 0;  // multiplier underflow
        u32 md = 0;  // multiplier divide-by-zero
    };

    /// Tracks an instruction that could not complete because a FIFO was not
    /// ready, so the next dispatch re-runs it from the same PC.
    struct FifoState {
        u32  pc          = 0;
        bool has_stalled = false;
    };

    Bus* m_bus = nullptr;

    // -- on-chip data RAM ---------------------------------------------------
    // MAME's internal_abus/internal_bbus maps: 0x400 words each.
    u32 m_ram_a[kInternalRamWords]{};
    u32 m_ram_b[kInternalRamWords]{};

    // -- registers ----------------------------------------------------------
    u32 m_pc       = 0;
    u32 m_delay_pc = 0;
    u32 m_ppc      = 0;

    u32 m_aa[8]{};  // A-bus address registers
    u32 m_ab[8]{};  // B-bus address registers
    u32 m_ma[8]{};  // multiplier A operands
    u32 m_mb[8]{};  // multiplier B operands
    u32 m_ar[8]{};  // general registers

    u32 m_sp  = 0;
    u32 m_eb  = 0;
    u32 m_eo  = 0;
    u32 m_rpc = 0;
    u32 m_lpc = 0;

    u32 m_prp = 0;
    u32 m_pwp = 0;
    u32 m_pr[24]{};

    u32   m_mod = 0;
    Flags m_flags{};
    u32   m_st = 0;

    u32 m_pcp = 0;      // PC stack pointer
    u32 m_pcs[4]{};     // PC stack

    u32 m_pdr = 0;
    u32 m_ddr = 0;

    bool m_delay_slot = false;

    FifoState m_fifo_state{};

    /// A result that could not be pushed because the output FIFO was full.
    ///
    /// MAME's FIFO device keeps exactly one such slot and halts the source until
    /// the FIFO drains. The Bus here forbids a push while full, so the slot lives
    /// in the core: run() retries it before the next instruction. Stalling
    /// instead would be wrong, because a re-run would repeat the address-register
    /// side effects the transfer's read phase has already applied.
    bool m_fifo_out_pending       = false;
    u32  m_fifo_out_pending_value = 0;

    s32 m_icount = 0;
    u64 m_instruction_count = 0;

    bool m_halted = false;

    bool        m_faulted = false;
    std::string m_fault_message;

    TraceHook m_trace_hook    = nullptr;
    void*     m_trace_context = nullptr;

    // -- fault reporting ----------------------------------------------------
    void fault(const char* what, u64 opcode);

    void unimplemented_op(u64 opcode);
    void unimplemented_alu(u64 opcode);
    void unimplemented_control(u64 opcode);
    void unimplemented_double_xfer1(u64 opcode);
    void unimplemented_double_xfer2(u64 opcode);
    void pcs_overflow();
    void pcs_underflow();

    // -- dispatch -----------------------------------------------------------
    [[nodiscard]] bool check_previous_op_stall();
    void handle_single_step_execution();

    void execute_op(u64 op);
    void do_alu1(u64 op);
    void do_alu2(u64 op);
    void do_alu2_trans2_1(u64 op);
    void do_alu2_trans1_1(u64 op);
    void do_alu1_trans2_2(u64 op);
    void do_alu1_trans1_2(u64 op);
    void do_trans1_3(u64 op);
    void do_alu_control(u64 op);

    // -- operand access -----------------------------------------------------
    [[nodiscard]] u32 get_prx(u8 which);
    [[nodiscard]] u32 get_constfloat(u8 which);
    [[nodiscard]] u32 get_constint(u8 which);
    [[nodiscard]] u32 get_alureg(u8 which, bool isfloatop);
    [[nodiscard]] u32 get_mulreg(u8 which, bool isfloatop);
    void set_alureg(u8 which, u32 value);
    void decode_aluop(u8 opcode, u32 src1, u32 src2, u8 imm, u8 dst_which);
    void decode_mulop(bool isfmul, u32 src1, u32 src2, u8 dst_which);
    [[nodiscard]] bool decode_branch_jump(u8 which);
    [[nodiscard]] u32 do_control_dst(u64 op);

    void push_pc(u32 pcval);
    [[nodiscard]] u32 pop_pc();
    void set_mod(u16 mod1, u16 mod2);

    [[nodiscard]] u32 get_transfer_reg(u8 which);
    void set_transfer_reg(u8 which, u32 value);
    [[nodiscard]] u32 decode_ea(u8 mode, u8 rx, u8 ry, u16 disp, bool isbbus);

    // -- memory -------------------------------------------------------------
    // The A-bus and B-bus each see on-chip RAM in their low 0x400 words and
    // the external bus above that, mirroring MAME's internal_abus/internal_bbus
    // overlaid on the external space.
    [[nodiscard]] u32 read_abus(u32 addr);
    [[nodiscard]] u32 read_bbus(u32 addr);
    void write_abus(u32 addr, u32 data);
    void write_bbus(u32 addr, u32 data);

    // -- PR ring pointers ---------------------------------------------------
    void increment_pwp();
    void increment_prp();
    void decrement_prp();
    void zero_prp();

    // -- flag setting -------------------------------------------------------
    void set_alu_flagsd(u32 val);
    void set_alu_flagsf(float val);
    void set_alu_flagsi(int val);
    [[nodiscard]] bool get_alu_second_src(u8 which);

    /// Pack the flag struct into the ST layout MAME exposes.
    [[nodiscard]] u32 pack_st() const;

    /// The inverse: split an ST written by software back out into the flag
    /// struct, so the two representations cannot drift apart.
    void unpack_st(u32 value);
};

}  // namespace sm2::cpu::mb86235
