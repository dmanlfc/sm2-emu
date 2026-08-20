// SPDX-License-Identifier: BSD-3-Clause
//
// Fujitsu MB86235 "TGPx4" DSP core — interpreter.
//
// Derived from MAME's src/devices/cpu/mb86235/mb86235ops.cpp (the interpreter)
// and src/devices/cpu/mb86235/mb86235.cpp (the execute loop and reset), which
// are BSD-3-Clause, copyright-holders Angelo Salese, ElSemi, Ville Linde,
// Matthew Daniels. The dynamic recompiler is not ported; it is disabled upstream
// (ENABLE_DRC is 0) and the interpreter is what actually runs.
//
// MAME's own TODO list for the part, kept because it records what is known to be
// missing rather than what merely happens to be unwritten:
//
//   - the ALU integer/floating point paths want rewriting with templates;
//   - the FIFO hookups want moving out of the opcodes;
//   - illegal delay slots are unsupported, and nobody knows what the hardware
//     does with them;
//   - PDR wants externalising (it drives the error LEDs on Model 2).
//
// Individual departures from upstream are marked "Deviation:" where they occur.
// The structural ones, in one place so they are easy to audit:
//
//   * every fatalerror() becomes a fault(), which records a message and stops
//     the core rather than killing the process. The seven unimplemented_*() /
//     pcs_*() reporters that upstream only wires into the recompiler are used
//     here as the entry points for the equivalent interpreter conditions.
//   * the arithmetic condition flags live in the Flags struct and are folded
//     into ST on demand by pack_st(); upstream keeps them only in ST. RP, LP,
//     F0-F2 and the FIFO status bits have no struct member and stay in ST.
//   * MAME reaches the host FIFOs through a device that calls back into
//     stall()/halt on an access that cannot complete. Here the Bus exposes the
//     empty/full state and the core tests it before accessing, which moves the
//     decision inline but keeps the same stall semantics.

#include "cpu/mb86235/mb86235.h"

#include "core/log.h"

#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>

namespace sm2::cpu::mb86235 {

namespace {

// ---------------------------------------------------------------------------
// Status word (ST) bit assignments, from MAME
// ---------------------------------------------------------------------------

constexpr u32 AD  = 0x00000001;  // ALU divide-by-zero
constexpr u32 AU  = 0x00000002;  // ALU underflow
constexpr u32 AV  = 0x00000004;  // ALU overflow
constexpr u32 AZ  = 0x00000008;  // ALU zero
constexpr u32 AN  = 0x00000010;  // ALU negative
constexpr u32 ZC  = 0x00000100;  // shifter carry
constexpr u32 IL  = 0x00000200;  // illegal input
constexpr u32 NR  = 0x00000400;  // not reciprocal
constexpr u32 ZD  = 0x00000800;  // zero divisor
constexpr u32 RP  = 0x00004000;  // repeat in progress
constexpr u32 LP  = 0x00008000;  // loop in progress
constexpr u32 MD  = 0x00010000;  // multiplier divide-by-zero
constexpr u32 MU  = 0x00020000;  // multiplier underflow
constexpr u32 MV  = 0x00040000;  // multiplier overflow
constexpr u32 MZ  = 0x00080000;  // multiplier zero
constexpr u32 MN  = 0x00100000;  // multiplier negative
constexpr u32 OFF = 0x01000000;  // output FIFO full
constexpr u32 OFE = 0x02000000;  // output FIFO empty
constexpr u32 IFF = 0x04000000;  // input FIFO full
constexpr u32 IFE = 0x08000000;  // input FIFO empty
constexpr u32 F0  = 0x10000000;
constexpr u32 F1  = 0x20000000;
constexpr u32 F2  = 0x40000000;

/// The bits that pack_st() supplies from the Flags struct. Everything outside
/// this mask is held directly in m_st.
constexpr u32 kFlagBits = AD | AU | AV | AZ | AN | ZC | IL | NR | ZD | MD | MU | MV | MZ | MN;

// ---------------------------------------------------------------------------
// Float punning (MAME's corefloat.h)
// ---------------------------------------------------------------------------

[[nodiscard]] inline float u2f(u32 value)
{
    return std::bit_cast<float>(value);
}

[[nodiscard]] inline u32 f2u(float value)
{
    return std::bit_cast<u32>(value);
}

}  // namespace

// Instruction field accessors, spelled as upstream spells them. Kept as macros
// so the ported expressions below stay diffable against mb86235ops.cpp; the
// operands are parenthesised here, which upstream omits.
#define GETAOP(x) (((x) >> 56) & 0x1f)
#define GETAI1(x) (((x) >> 52) & 0x0f)
#define GETAI2(x) (((x) >> 47) & 0x1f)
#define GETAO(x)  (((x) >> 42) & 0x1f)

#define GETMOP(x) (((x) >> 41) & 0x01)
#define GETMI1(x) (((x) >> 37) & 0x0f)
#define GETMI2(x) (((x) >> 32) & 0x1f)
#define GETMO(x)  (((x) >> 27) & 0x1f)

// ============================================================================
// Construction and reset
// ============================================================================

MB86235::MB86235(Bus& bus) : m_bus(&bus)
{
    reset();
}

void MB86235::reset()
{
    // MAME's device_reset touches only these four. The register file and the two
    // internal RAMs come up as whatever they were, which is what the hardware
    // does: a reset pulse does not clear RAM, and the microcode the host has
    // just uploaded is expected to initialise what it uses.
    m_pc         = 0;
    m_delay_pc   = 0;
    m_ppc        = 0;
    m_delay_slot = false;

    // Port-specific state that has no upstream counterpart.
    m_fifo_state             = FifoState{};
    m_fifo_out_pending       = false;
    m_fifo_out_pending_value = 0;
    m_faulted                = false;
    m_fault_message.clear();
}

// ============================================================================
// Execution
// ============================================================================

void MB86235::handle_single_step_execution()
{
    if (m_fifo_state.has_stalled) {
        return;
    }

    // repeat opcode
    if (m_st & RP) {
        --m_rpc;
        if (m_rpc == 1) {
            m_st &= ~RP;
        }
    } else {  // normal operation
        m_pc++;
    }
}

bool MB86235::check_previous_op_stall()
{
    return m_fifo_state.has_stalled && ((m_st & RP) == 0);
}

s32 MB86235::run(s32 cycles)
{
    if (cycles <= 0) {
        return 0;
    }
    if (m_halted) {
        // The host holds HALT while it uploads microcode, and the FIFO flow
        // control asserts it when the core cannot make progress. Nothing is
        // consumed, so the caller decides what to do with the time.
        return 0;
    }
    if (m_faulted) {
        // Nothing after an unimplemented condition can be trusted.
        return 0;
    }

    m_icount = cycles;

    while (m_icount > 0) {
        if (m_halted) {
            // A Bus handler asserted halt part-way through the slice. Stop with
            // the program counter intact.
            break;
        }

        // Deviation: MAME's output FIFO device keeps one extra slot for a push
        // into a full FIFO and halts the source until it drains. The Bus here
        // forbids a push while full, so that slot lives in the core instead.
        // Either way no result is lost and the instruction does not re-run.
        if (m_fifo_out_pending) {
            if (m_bus->fifo_out_full()) {
                // Still no room. The core is stopped, but time passes.
                m_icount = 0;
                break;
            }
            m_bus->fifo_out_push(m_fifo_out_pending_value);
            m_fifo_out_pending = false;
        }

        const u32 curpc = check_previous_op_stall() ? m_fifo_state.pc : m_pc;

        // MAME's program space is 12 bits wide and truncates the address for it;
        // PC itself is never masked, so the mask goes on the fetch.
        const u64 opcode = m_bus->program_read(curpc & 0xfff);

        m_ppc = curpc;

        if (m_delay_slot) {
            m_pc         = m_delay_pc;
            m_delay_slot = false;
        } else {
            handle_single_step_execution();
        }

        m_fifo_state.has_stalled = false;

        if (m_trace_hook != nullptr) {
            m_trace_hook(m_trace_context, curpc, opcode);
        }

        execute_op(opcode);

        m_icount--;
        ++m_instruction_count;
    }

    // A control op charges itself extra cycles, so the budget can be overrun by
    // the last instruction of a slice. The surplus is dropped rather than
    // charged to the caller, who asked for a bounded amount of work.
    if (m_icount < 0) {
        m_icount = 0;
    }

    return cycles - m_icount;
}

void MB86235::execute_op(u64 op)
{
    switch ((op >> 61) & 7) {
        case 0:
            do_alu2_trans2_1(op);
            break;
        case 1:
            do_alu2_trans1_1(op);
            break;
        case 4:
            do_alu1_trans2_2(op);
            break;
        case 5:
            do_alu1_trans1_2(op);
            break;
        case 2:
        case 6:
            do_alu_control(op);
            break;
        case 7:
            do_trans1_3(op);
            break;
        default:  // type 3 has no encoding
            unimplemented_op(op);
            break;
    }
}

// ============================================================================
// Fault reporting
// ============================================================================
//
// Upstream calls fatalerror() from every one of these paths, which exits the
// process. Here the condition is recorded and execution stops, so the host can
// report it alongside the rest of the machine and keep running.

void MB86235::fault(const char* what, u64 opcode)
{
    if (!m_faulted) {
        m_faulted = true;

        char buffer[256];
        if (opcode != 0) {
            std::snprintf(buffer, sizeof(buffer), "%s at pc=%03x (opcode %04x%08x)", what, m_ppc,
                          static_cast<u32>(opcode >> 32), static_cast<u32>(opcode));
        } else {
            // The sites that report a decoded field rather than a whole word put
            // the value in the message and have no opcode to hand.
            std::snprintf(buffer, sizeof(buffer), "%s at pc=%03x", what, m_ppc);
        }
        m_fault_message = buffer;

        SM2_ERROR("mb86235: %s", m_fault_message.c_str());
        SM2_ERROR("mb86235: %s", state_string().c_str());
    }

    // Abandon the rest of the slice. The instruction in progress still unwinds,
    // which is why fault() is not allowed to leave the core in a state its
    // callers cannot cope with: every fault site returns a defined value.
    m_icount = 0;
}

void MB86235::unimplemented_op(u64 opcode)
{
    fault("unimplemented opcode type", opcode);
}

void MB86235::unimplemented_alu(u64 opcode)
{
    // The argument is the five-bit ALU op, matching MAME's cfunc_unimplemented_alu.
    fault("unimplemented ALU op", opcode);
}

void MB86235::unimplemented_control(u64 opcode)
{
    // The argument is the five-bit control op.
    fault("unimplemented control op", opcode);
}

void MB86235::unimplemented_double_xfer1(u64 opcode)
{
    fault("unimplemented double transfer type 1", opcode);
}

void MB86235::unimplemented_double_xfer2(u64 opcode)
{
    fault("unimplemented double transfer type 2", opcode);
}

void MB86235::pcs_overflow()
{
    fault("PC stack overflow", 0);
}

void MB86235::pcs_underflow()
{
    fault("PC stack underflow", 0);
}

// ============================================================================
// Status flags
// ============================================================================

u32 MB86235::pack_st() const
{
    u32 value = m_st & ~kFlagBits;

    if (m_flags.ad != 0) value |= AD;
    if (m_flags.au != 0) value |= AU;
    if (m_flags.av != 0) value |= AV;
    if (m_flags.az != 0) value |= AZ;
    if (m_flags.an != 0) value |= AN;
    if (m_flags.zc != 0) value |= ZC;
    if (m_flags.il != 0) value |= IL;
    if (m_flags.nr != 0) value |= NR;
    if (m_flags.zd != 0) value |= ZD;
    if (m_flags.md != 0) value |= MD;
    if (m_flags.mu != 0) value |= MU;
    if (m_flags.mv != 0) value |= MV;
    if (m_flags.mz != 0) value |= MZ;
    if (m_flags.mn != 0) value |= MN;

    // Deviation: upstream leaves the four FIFO status bits at whatever software
    // last wrote into ST, because a branch on a FIFO condition asks the FIFO
    // rather than reading ST. They are filled in live here so that microcode
    // reading ST directly sees the truth instead of a stale value. Only two of
    // the four are observable through the Bus; the other two read as clear.
    value &= ~(IFE | IFF | OFF | OFE);
    if (m_bus->fifo_in_empty()) value |= IFE;
    if (m_bus->fifo_out_full()) value |= OFF;

    return value;
}

void MB86235::unpack_st(u32 value)
{
    m_st = value;

    m_flags.ad = (value & AD) != 0;
    m_flags.au = (value & AU) != 0;
    m_flags.av = (value & AV) != 0;
    m_flags.az = (value & AZ) != 0;
    m_flags.an = (value & AN) != 0;
    m_flags.zc = (value & ZC) != 0;
    m_flags.il = (value & IL) != 0;
    m_flags.nr = (value & NR) != 0;
    m_flags.zd = (value & ZD) != 0;
    m_flags.md = (value & MD) != 0;
    m_flags.mu = (value & MU) != 0;
    m_flags.mv = (value & MV) != 0;
    m_flags.mz = (value & MZ) != 0;
    m_flags.mn = (value & MN) != 0;
}

void MB86235::set_alu_flagsd(u32 val)
{
    m_flags.an = (val & 0x80000000) != 0;
    m_flags.az = (val == 0);
}

void MB86235::set_alu_flagsf(float val)
{
    m_flags.an = (val < 0.0F);
    m_flags.az = (val == 0.0F);
}

void MB86235::set_alu_flagsi(int val)
{
    m_flags.an = (val < 0);
    m_flags.az = (val == 0);
}

// ============================================================================
// Memory
// ============================================================================
//
// The A bus and the B bus are not symmetric, and upstream's asymmetry is
// deliberate rather than an oversight: read_abus/write_abus fall through to the
// external space for anything at 0x400 or above, offset by the high bits of EB,
// while read_bbus/write_bbus only ever see the on-chip RAM. The B bus is where
// the stack lives, so it never needs to leave the chip.

u32 MB86235::read_abus(u32 addr)
{
    if ((addr & 0x3fff) >= 0x400) {
        return m_bus->external_read((addr & 0x3fff) + (m_eb & 0xffc000));
    }
    return m_ram_a[addr & 0x3ff];
}

u32 MB86235::read_bbus(u32 addr)
{
    return m_ram_b[addr & 0x3ff];
}

void MB86235::write_abus(u32 addr, u32 data)
{
    if ((addr & 0x3fff) >= 0x400) {
        m_bus->external_write((addr & 0x3fff) + (m_eb & 0xffc000), data);
    } else {
        m_ram_a[addr & 0x3ff] = data;
    }
}

void MB86235::write_bbus(u32 addr, u32 data)
{
    m_ram_b[addr & 0x3ff] = data;
}

u32 MB86235::decode_ea(u8 mode, u8 rx, u8 ry, u16 disp, bool isbbus)
{
    u32 res;

    // A stalled instruction is going to run again, so the auto-increment and
    // auto-decrement modes have to leave the address register alone: the re-run
    // applies it.
    switch (mode) {
        case 0x00:  // ARx
            return m_ar[rx];
        case 0x01:  // ARx++
            res = m_ar[rx];
            if (m_fifo_state.has_stalled) {
                return res;
            }
            m_ar[rx]++;
            m_ar[rx] &= 0x3fff;
            return res;
        case 0x02:  // ARx--
            res = m_ar[rx];
            if (m_fifo_state.has_stalled) {
                return res;
            }
            m_ar[rx]--;
            m_ar[rx] &= 0x3fff;
            return res;
        case 0x03:  // ARx++ disp14
            res = m_ar[rx];
            if (m_fifo_state.has_stalled) {
                return res;
            }
            m_ar[rx] += disp;
            m_ar[rx] &= 0x3fff;
            return res;
        case 0x04:  // ARx + ARy
            return m_ar[rx] + m_ar[ry];
        case 0x05:  // ARx + ARy++
            res = m_ar[ry];
            if (m_fifo_state.has_stalled) {
                return m_ar[rx] + res;
            }
            m_ar[ry]++;
            m_ar[ry] &= 0x3fff;
            return m_ar[rx] + res;
        case 0x06:  // ARx + ARy--
            res = m_ar[ry];
            if (m_fifo_state.has_stalled) {
                return m_ar[rx] + res;
            }
            m_ar[ry]--;
            m_ar[ry] &= 0x3fff;
            return m_ar[rx] + res;
        case 0x07:  // ARx + (ARy++ disp14)
            res = m_ar[ry];
            if (m_fifo_state.has_stalled) {
                return m_ar[rx] + res;
            }
            m_ar[ry] += disp;
            m_ar[ry] &= 0x3fff;
            return m_ar[rx] + res;
        case 0x08:  // ARx + ARyU/ARyL (A bus/B bus)
            return m_ar[rx] + (isbbus ? (m_ar[ry] & 0x7f) : (m_ar[ry] >> 7));
        case 0x09:  // ARx + ARyL/ARyU (A bus/B bus)
            return m_ar[rx] + (isbbus ? (m_ar[ry] >> 7) : (m_ar[ry] & 0x7f));
        case 0x0a:  // ARx + disp14
            return m_ar[rx] + disp;
        case 0x0b:  // ARx + ARy + disp14
            return m_ar[rx] + m_ar[ry] + disp;
        case 0x0d:  // ARx + [ARy++]
            res = m_ar[ry] & (0x1ff >> (7 - (isbbus ? ((m_mod >> 8) & 7) : ((m_mod >> 12) & 7))));
            if (m_fifo_state.has_stalled) {
                return m_ar[rx] + res;
            }
            m_ar[ry]++;
            m_ar[ry] &= 0x3fff;
            return m_ar[rx] + res;
        case 0x0e:  // ARx + [ARy--]
            res = m_ar[ry] & (0x1ff >> (7 - (isbbus ? ((m_mod >> 8) & 7) : ((m_mod >> 12) & 7))));
            if (m_fifo_state.has_stalled) {
                return m_ar[rx] + res;
            }
            m_ar[ry]--;
            m_ar[ry] &= 0x3fff;
            return m_ar[rx] + res;
        case 0x0f:  // ARx + [ARy++ disp14]
            res = m_ar[ry] & (0x1ff >> (7 - (isbbus ? ((m_mod >> 8) & 7) : ((m_mod >> 12) & 7))));
            if (m_fifo_state.has_stalled) {
                return m_ar[rx] + res;
            }
            m_ar[ry] += disp;
            m_ar[ry] &= 0x3fff;
            return m_ar[rx] + res;
        default:
            break;
    }

    char message[64];
    std::snprintf(message, sizeof(message), "illegal decode_ea type %02x", mode);
    fault(message, 0);
    return 0;
}

// ============================================================================
// PR ring pointers
// ============================================================================

void MB86235::increment_pwp()
{
    m_pwp++;
    if (m_pwp >= 24) {
        m_pwp = 0;
    }
}

void MB86235::increment_prp()
{
    m_prp++;
    if (m_prp >= 24) {
        m_prp = 0;
    }
}

void MB86235::decrement_prp()
{
    if (m_prp == 0) {
        m_prp = 24;
    }

    m_prp--;
}

void MB86235::zero_prp()
{
    m_prp = 0;
}

// ============================================================================
// ALU and multiplier
// ============================================================================
//
// Upstream writes the sign-extending casts as ((int(x) << 0) >> 0); the shifts
// are no-ops and are dropped here. Where upstream would rely on signed overflow
// or on shifting a negative value left, the arithmetic is done unsigned and cast
// back, which is the same result on any two's-complement machine without being
// undefined.

u32 MB86235::get_prx(u8 which)
{
    const u32 res = m_pr[m_prp];

    switch (which & 7) {
        case 0:
            break;
        case 1:
            increment_prp();
            break;
        case 2:
            decrement_prp();
            break;
        case 3:
            zero_prp();
            break;
        default: {
            char message[64];
            std::snprintf(message, sizeof(message), "illegal get_prx %02x", which & 7);
            fault(message, 0);
            break;
        }
    }

    return res;
}

u32 MB86235::get_constfloat(u8 which)
{
    static constexpr float float_table[8] = { -1.0F, 0.0F, 0.5F, 1.0F, 1.5F, 2.0F, 3.0F, 5.0F };
    return f2u(float_table[which & 7]);
}

u32 MB86235::get_constint(u8 which)
{
    switch (which & 7) {
        case 0:  // A0
            return 0;
        case 1:  // A1
            return 1;
        case 2:  // A2
            return 0xffffffff;  // -1
        default:
            break;
    }

    char message[64];
    std::snprintf(message, sizeof(message), "illegal get_constint %02x", which);
    fault(message, 0);
    return 0;
}

u32 MB86235::get_alureg(u8 which, bool isfloatop)
{
    switch (which >> 3) {
        case 0:  // AAx
            return m_aa[which & 7];
        case 1:  // ABx
            return m_ab[which & 7];
        case 2:  // PRx
            return get_prx(which & 7);
        case 3:  // constants
            return isfloatop ? get_constfloat(which & 7) : get_constint(which & 7);
        default:
            break;
    }

    // Unreachable: the widest field feeding this is five bits.
    char message[64];
    std::snprintf(message, sizeof(message), "illegal get_alureg %02x", which);
    fault(message, 0);
    return 0;
}

u32 MB86235::get_mulreg(u8 which, bool isfloatop)
{
    switch (which >> 3) {
        case 0:  // MAx
            return m_ma[which & 7];
        case 1:  // MBx
            return m_mb[which & 7];
        case 2:  // PRx
            return get_prx(which & 7);
        case 3:  // constants
            return isfloatop ? get_constfloat(which & 7) : get_constint(which & 7);
        default:
            break;
    }

    // Unreachable, as for get_alureg.
    char message[64];
    std::snprintf(message, sizeof(message), "illegal get_mulreg %02x", which);
    fault(message, 0);
    return 0;
}

void MB86235::set_alureg(u8 which, u32 value)
{
    switch (which >> 3) {
        case 0:  // MAx
            m_ma[which & 7] = value;
            break;
        case 1:  // MBx
            m_mb[which & 7] = value;
            break;
        case 2:  // AAx
            m_aa[which & 7] = value;
            break;
        case 3:  // ABx
            m_ab[which & 7] = value;
            break;
        default:
            break;
    }
}

void MB86235::decode_aluop(u8 opcode, u32 src1, u32 src2, u8 imm, u8 dst_which)
{
    switch (opcode) {
        // floating point ops
        case 0x00:  // FADD
        case 0x01:  // FADDZ
        case 0x02:  // FSUB
        case 0x03:  // FSUBZ
        {
            const float f1 = u2f(src1);
            const float f2 = u2f(src2);
            float       d;

            if (opcode & 2) {
                d = f2 - f1;
            } else {
                d = f1 + f2;
            }

            if (opcode & 1) {
                m_flags.zc = 0;
                if (d < 0.0F) {
                    m_flags.zc = 1;
                    d          = 0.0F;
                }
            }

            set_alu_flagsf(d);
            set_alureg(dst_which, f2u(d));
            break;
        }

        case 0x04:  // FCMP
        case 0x06:  // FABC
        {
            const float f1 = u2f(src1);
            const float f2 = u2f(src2);
            float       d;

            if (opcode & 2) {
                d = std::fabs(f2) - std::fabs(f1);
            } else {
                d = f2 - f1;
            }

            set_alu_flagsf(d);
            break;
        }

        case 0x05:  // FABS
        {
            float d = u2f(src1);
            d       = std::fabs(d);
            set_alu_flagsf(d);
            set_alureg(dst_which, f2u(d));
            break;
        }

        case 0x07:  // NOP
            break;

        case 0x08:  // FEA
        case 0x09:  // FES
        {
            u32 exp = (src1 >> 23) & 0xff;
            src1 &= 0x7f800000;
            if (opcode & 1) {
                exp -= imm;
            } else {
                exp += imm;
            }
            exp &= 0xff;
            src1 |= exp << 23;
            set_alu_flagsd(src1);
            set_alureg(dst_which, src1);
            break;
        }

        case 0x0a:  // FRCP
        {
            float f    = u2f(src1);
            m_flags.zd = 0;
            if (f == 0.0F) {
                m_flags.zd = 1;
            }
            f = 1.0F / f;
            set_alu_flagsf(f);
            set_alureg(dst_which, f2u(f));
            break;
        }

        case 0x0b:  // FRSQ
        {
            float f    = u2f(src1);
            m_flags.nr = 0;
            if (f <= 0.0F) {
                m_flags.nr = 1;
            }
            f = 1.0F / std::sqrt(f);
            set_alu_flagsf(f);
            set_alureg(dst_which, f2u(f));
            break;
        }

        case 0x0c:  // FLOG
        {
            float f    = u2f(src1);
            m_flags.il = 0;
            if (f <= 0.0F) {
                m_flags.il = 1;
            }
            // Upstream divides the natural log by log10(2) and labels the result
            // log2, which is not what that gives. Reproduced rather than fixed:
            // the microcode is calibrated against whatever the part does, and
            // this is the value MAME's rendering has been checked against.
            f = std::log(f) / 0.301030F;
            set_alu_flagsf(f);
            set_alureg(dst_which, f2u(f));
            break;
        }

        case 0x0d:  // CIF
        {
            const int   v = static_cast<int>(src1);
            const float f = static_cast<float>(v);
            set_alu_flagsf(f);
            set_alureg(dst_which, f2u(f));
            break;
        }

        case 0x0e:  // CFI
        {
            const float f = u2f(src1);
            const int   v = static_cast<int>(f);
            set_alu_flagsi(v);
            set_alureg(dst_which, static_cast<u32>(v));
            break;
        }

        case 0x0f:  // CFIB
        {
            const float f   = u2f(src1);
            u32         res = static_cast<u32>(f);
            if (f < 0.0F) {
                m_flags.au = 1;
                res        = 0;
            }
            m_flags.az = 0;
            if (res == 0) {
                m_flags.az = 1;
            }
            if (res > 0xff) {
                m_flags.av = 1;
                res        = 0xff;
            }
            set_alureg(dst_which, res);
            break;
        }

        // integer ops
        case 0x10:  // ADD
        case 0x11:  // ADDZ
        case 0x12:  // SUB
        case 0x13:  // SUBZ
        {
            const s32 v1 = static_cast<s32>(src1);
            const s32 v2 = static_cast<s32>(src2);
            s32       res;

            if (opcode & 2) {
                res = static_cast<s32>(static_cast<u32>(v2) - static_cast<u32>(v1));
            } else {
                res = static_cast<s32>(static_cast<u32>(v1) + static_cast<u32>(v2));
            }

            if (opcode & 1) {
                m_flags.zc = 0;
                if (res < 0) {
                    m_flags.zc = 1;
                    res        = 0;
                }
            }
            set_alu_flagsi(res);
            set_alureg(dst_which, static_cast<u32>(res));
            break;
        }

        case 0x14:  // CMP
        {
            const s32 v1  = static_cast<s32>(src1);
            const s32 v2  = static_cast<s32>(src2);
            const s32 res = static_cast<s32>(static_cast<u32>(v2) - static_cast<u32>(v1));
            set_alu_flagsi(res);
            break;
        }

        case 0x15:  // ABS
        {
            src1 &= 0x7fffffff;
            set_alu_flagsd(src1);
            set_alureg(dst_which, src1);
            break;
        }

        case 0x16:  // ATR
        case 0x17:  // ATRZ
        {
            if (opcode & 1) {
                m_flags.zc = 0;
                if (src1 & 0x80000000) {
                    m_flags.zc = 1;
                    src1       = 0;
                }
            }
            set_alureg(dst_which, src1);
            break;
        }

        // logical ops
        case 0x18:  // AND
        {
            const u32 res = src1 & src2;
            set_alu_flagsd(res);
            set_alureg(dst_which, res);
            break;
        }

        case 0x19:  // OR
        {
            const u32 res = src1 | src2;
            set_alu_flagsd(res);
            set_alureg(dst_which, res);
            break;
        }

        case 0x1a:  // XOR
        {
            const u32 res = src1 ^ src2;
            set_alu_flagsd(res);
            set_alureg(dst_which, res);
            break;
        }

        case 0x1b:  // NOT
        {
            const u32 res = ~src1;
            set_alu_flagsd(res);
            set_alureg(dst_which, res);
            break;
        }

        case 0x1c:  // LSR
        {
            const u32 res = src1 >> imm;
            set_alu_flagsd(res);
            set_alureg(dst_which, res);
            break;
        }

        case 0x1d:  // LSL
        {
            const u32 res = src1 << imm;
            set_alu_flagsd(res);
            set_alureg(dst_which, res);
            break;
        }

        case 0x1e:  // ASR
        {
            const s32 res = static_cast<s32>(src1) >> imm;
            set_alu_flagsi(res);
            set_alureg(dst_which, static_cast<u32>(res));
            break;
        }

        case 0x1f:  // ASL
        {
            const s32 res = static_cast<s32>(src1 << imm);
            set_alu_flagsi(res);
            set_alureg(dst_which, static_cast<u32>(res));
            break;
        }

        default:
            // Unreachable: every one of the 32 encodings is handled above.
            unimplemented_alu(opcode);
            break;
    }
}

void MB86235::decode_mulop(bool isfmul, u32 src1, u32 src2, u8 dst_which)
{
    if (isfmul) {  // FMUL
        const float f1  = u2f(src1);
        const float f2  = u2f(src2);
        const float res = f1 * f2;

        // MV and MU are not reset
        m_flags.mn = 0;
        m_flags.mz = 0;
        m_flags.md = 0;
        if (res < 0.0F) m_flags.mn = 1;
        if (res == 0.0F) m_flags.mz = 1;
        if (std::isinf(res)) m_flags.mv = 1;
        if (std::fabs(res) < std::numeric_limits<float>::min()) m_flags.mu = 1;
        if (std::isnan(res)) m_flags.md = 1;

        set_alureg(dst_which, f2u(res));
    } else {  // MUL
        const s32 v1  = static_cast<s32>(src1);
        const s32 v2  = static_cast<s32>(src2);
        const s32 res = static_cast<s32>(static_cast<u32>(v1) * static_cast<u32>(v2));

        m_flags.mn = 0;
        m_flags.mz = 0;
        if (res < 0) m_flags.mn = 1;
        if (res == 0) m_flags.mz = 1;

        set_alureg(dst_which, static_cast<u32>(res));
    }
}

bool MB86235::get_alu_second_src(u8 which)
{
    if ((which & 0x1c) == 0x1c) {  // logical ops
        return false;
    }

    if ((which & 0x1e) == 0x16) {  // ATRx
        return false;
    }

    if ((which & 0x0f) == 0x05) {  // ABS/FABS
        return false;
    }

    if ((which & 0x18) == 0x08) {  // floating point ops
        return false;
    }

    return true;
}

void MB86235::do_alu1(u64 op)
{
    // A stalled instruction is going to run again, so the ALU must not act on
    // operands it may have read from a FIFO that was not ready.
    if (m_fifo_state.has_stalled) {
        return;
    }

    if ((op >> 41) & 1) {  // ALU
        const u8  aluop   = GETAOP(op);
        const u32 alusrc1 = get_alureg(GETAI1(op), false);
        u32       alusrc2;
        if (get_alu_second_src(aluop)) {
            alusrc2 = get_alureg(GETAI2(op), (aluop & 0x10) == 0);
        } else {
            alusrc2 = 0;
        }
        decode_aluop(aluop, alusrc1, alusrc2, GETAI2(op), GETAO(op));
    } else {  // MUL
        const bool isfmul  = GETAOP(op) != 0;
        const u32  mulsrc1 = get_mulreg(GETAI1(op), false);
        const u32  mulsrc2 = get_mulreg(GETAI2(op), isfmul);
        decode_mulop(isfmul, mulsrc1, mulsrc2, GETAO(op));
    }
}

void MB86235::do_alu2(u64 op)
{
    if (m_fifo_state.has_stalled) {
        return;
    }

    // ALU
    const u8  aluop   = GETAOP(op);
    const u32 alusrc1 = get_alureg(GETAI1(op), false);
    u32       alusrc2;
    if (get_alu_second_src(aluop)) {
        alusrc2 = get_alureg(GETAI2(op), (aluop & 0x10) == 0);
    } else {
        alusrc2 = 0;
    }

    // MUL
    const bool isfmul  = GETMOP(op) != 0;
    const u32  mulsrc1 = get_mulreg(GETMI1(op), false);
    const u32  mulsrc2 = get_mulreg(GETMI2(op), isfmul);

    decode_aluop(aluop, alusrc1, alusrc2, GETAI2(op), GETAO(op));
    decode_mulop(isfmul, mulsrc1, mulsrc2, GETMO(op));
}

// ============================================================================
// Transfers
// ============================================================================

u32 MB86235::get_transfer_reg(u8 which)
{
    switch (which >> 3) {
        case 0:  // MAx
            return m_ma[which & 7];
        case 1:  // AAx
            return m_aa[which & 7];
        case 2:
            switch (which & 7) {
                case 0: return m_eb;
                case 1: return m_eb >> 14;
                case 2: return m_eb & 0x3fff;
                case 3: return m_eo;
                case 4: return m_sp;
                case 5: return pack_st();
                case 6: return m_mod;
                case 7: return m_lpc;
                default: break;
            }

            break;
        case 3:  // ARx
            return m_ar[which & 7];
        case 4:  // MBx
            return m_mb[which & 7];
        case 5:  // ABx
            return m_ab[which & 7];
        case 6:
        {
            switch (which & 7) {
                case 0:  // PRx
                    return m_pr[m_prp];

                case 1:  // FI
                {
                    // Deviation: MAME pops unconditionally and lets the FIFO
                    // device call stall() when it turns out to be empty. The Bus
                    // here promises nothing for a pop from an empty FIFO, so the
                    // check moves in-line. The stall bookkeeping below is
                    // upstream's, unchanged: recording the PC is what makes the
                    // next dispatch re-run this instruction.
                    u32 res = 0;
                    if (m_bus->fifo_in_empty()) {
                        stall();
                    } else {
                        res = m_bus->fifo_in_pop();
                    }

                    if (m_fifo_state.has_stalled) {
                        if ((m_st & RP) == 0) {
                            m_fifo_state.pc = m_ppc;
                        }
                        // else: upstream wonders what a stall inside a repeat
                        // should do, and leaves the question open.
                    }

                    return res;
                }
                case 4:  // PDR
                    return m_pdr;
                case 5:  // DDR
                    return m_ddr;
                case 6:  // PRP
                    return m_prp;
                case 7:  // PWP
                    return m_pwp;
                default:
                    break;
            }
            break;
        }
        default:
            break;
    }

    char message[64];
    std::snprintf(message, sizeof(message), "illegal get_transfer_reg %02x", which);
    fault(message, 0);
    return 0;
}

void MB86235::set_transfer_reg(u8 which, u32 value)
{
    switch (which >> 3) {
        case 0:  // MAx
            m_ma[which & 7] = value;
            break;
        case 1:  // AAx
            m_aa[which & 7] = value;
            break;
        case 2:
            switch (which & 7) {
                case 0: m_eb = value; break;
                case 1: m_eb = (m_eb & 0x3fff) | (value << 14); break;
                // Upstream shifts here too, which looks wrong for the low half
                // of EB, but it is what the interpreter and the recompiler both
                // do; left alone rather than second-guessed.
                case 2: m_eb = (m_eb & 0xffc000) | (value << 14); break;
                case 3: m_eo = value; break;
                case 4: m_sp = value; break;
                case 5: unpack_st(value); break;
                case 6: m_mod = value; break;
                case 7: m_lpc = value; break;
                default: break;
            }
            break;
        case 3:  // ARx
            m_ar[which & 7] = value & 0x3fff;
            break;
        case 4:  // MBx
            m_mb[which & 7] = value;
            break;
        case 5:  // ABx
            m_ab[which & 7] = value;
            break;
        case 6:
            switch (which & 7) {
                case 0:
                    m_pr[m_pwp] = value;
                    if (!m_fifo_state.has_stalled) {
                        increment_pwp();
                    }
                    break;
                case 2:  // FO0
                case 3:  // FO1 (same FIFO output buffer but sets the 33rd bit; not used by Model 2)
                    // Deviation: see the pending-push slot in run(). Upstream
                    // pushes regardless and relies on the FIFO device to hold
                    // the overflowing value and halt the core.
                    if (m_bus->fifo_out_full()) {
                        m_fifo_out_pending       = true;
                        m_fifo_out_pending_value = value;
                    } else {
                        m_bus->fifo_out_push(value);
                    }
                    break;
                case 4: m_pdr = value; break;
                case 5: m_ddr = value; break;
                case 6:
                    if (value >= 24) {
                        char message[64];
                        std::snprintf(message, sizeof(message),
                                      "attempting to set prp with a %02x", value);
                        fault(message, 0);
                        break;
                    }

                    m_prp = value;
                    break;
                case 7:
                    if (value >= 24) {
                        char message[64];
                        std::snprintf(message, sizeof(message),
                                      "attempting to set pwp with a %02x", value);
                        fault(message, 0);
                        break;
                    }

                    m_pwp = value;
                    break;
                default: {
                    char message[64];
                    std::snprintf(message, sizeof(message), "illegal set_transfer_reg %02x",
                                  which);
                    fault(message, 0);
                    break;
                }
            }

            break;
        default: {
            char message[64];
            std::snprintf(message, sizeof(message), "illegal set_transfer_reg dst %02x", which);
            fault(message, 0);
            break;
        }
    }
}

// double transfer type 1
void MB86235::do_alu2_trans2_1(u64 op)
{
    const u8 sd = (op >> 25) & 3;
    u32      ares = 0;
    u32      bres = 0;

    switch (sd) {
        case 0:
        case 1:
        {
            const u8 as = (op >> 20) & 0x1f;
            const u8 bs = (op >> 10) & 0xf;
            ares        = get_transfer_reg(as);
            bres        = get_transfer_reg(bs | 0x20);
            break;
        }
        case 2:
        {
            u32 addr = decode_ea(op & 0xf, (op >> 17) & 7, (op >> 14) & 7, 0, false);
            ares     = read_abus(addr);
            addr     = decode_ea(op & 0xf, (op >> 7) & 7, (op >> 4) & 7, 0, true);
            bres     = read_bbus(addr);
            break;
        }
        default:
            unimplemented_double_xfer1(op);
            return;
    }

    // do alu
    do_alu2(op);

    switch (sd) {
        case 0:
        case 2:
        {
            const u8 ad = (op >> 20) & 0x1f;
            const u8 bd = (op >> 10) & 0xf;
            set_transfer_reg(ad, ares);
            set_transfer_reg(bd | 0x20, bres);
            break;
        }
        case 1:
        {
            u32 addr = decode_ea(op & 0xf, (op >> 17) & 7, (op >> 14) & 7, 0, false);
            write_abus(addr, ares);
            addr = decode_ea(op & 0xf, (op >> 7) & 7, (op >> 4) & 7, 0, true);
            write_bbus(addr, bres);
            break;
        }
        default:
            break;
    }
}

// transfer type 1
void MB86235::do_alu2_trans1_1(u64 op)
{
    u8  sr;
    u8  dr;
    u32 res;

    if ((op >> 26) & 1) {  // External transfer
        if ((op >> 25) & 1) {  // ext -> int
            u32 addr = m_eb + m_eo;
            res      = m_bus->external_read(addr);

            // do alu
            do_alu2(op);

            dr = (op >> 12) & 0x7f;
            if (dr & 0x40) {
                const bool isbbus = (dr & 0x20) == 0x20;
                addr = decode_ea(op & 0xf, dr & 7, (op >> 4) & 7, (op >> 7) & 0x1f, isbbus) & 0x3ff;
                if (isbbus) {
                    write_bbus(addr, res);
                } else {
                    write_abus(addr, res);
                }
            } else {
                set_transfer_reg(dr, res);
            }

            s8 disp_offs = (op >> 19) & 0x3f;
            if (disp_offs & 0x20) {
                disp_offs -= 0x40;
            }
            m_eo += disp_offs;
        } else {  // int -> ext
            sr = (op >> 12) & 0x7f;
            if (sr & 0x40) {
                const bool isbbus = (sr & 0x20) == 0x20;
                const u32  addr =
                    decode_ea(op & 0xf, sr & 7, (op >> 4) & 7, (op >> 7) & 0x1f, isbbus) & 0x3ff;
                res = isbbus ? read_bbus(addr) : read_abus(addr);
            } else {
                res = get_transfer_reg(sr);
            }

            // do alu
            do_alu2(op);

            const u32 addr = m_eb + m_eo;
            m_bus->external_write(addr, res);

            s8 disp_offs = (op >> 19) & 0x3f;
            if (disp_offs & 0x20) {
                disp_offs -= 0x40;
            }
            m_eo += disp_offs;
        }
    } else {
        sr = (op >> 19) & 0x7f;
        if (sr & 0x40) {
            if (sr == 0x58) {
                res = op & 0xfff;
            } else {
                const bool isbbus = (sr & 0x20) == 0x20;
                const u32  addr =
                    decode_ea(op & 0xf, sr & 7, (op >> 4) & 7, (op >> 7) & 0x1f, isbbus);
                res = isbbus ? read_bbus(addr) : read_abus(addr);
            }
        } else {
            res = get_transfer_reg(sr);
        }

        // do alu
        do_alu2(op);

        dr = (op >> 12) & 0x7f;
        if (dr & 0x40) {
            if (dr == 0x58) {
                fault("illegal do_alu2_trans1_1 dr == 0x58", op);
                return;
            }

            const bool isbbus = (dr & 0x20) == 0x20;
            const u32  addr = decode_ea(op & 0xf, dr & 7, (op >> 4) & 7, (op >> 7) & 0x1f, isbbus);
            if (isbbus) {
                write_bbus(addr, res);
            } else {
                write_abus(addr, res);
            }
        } else {
            set_transfer_reg(dr, res);
        }
    }
}

// double transfer type 2
void MB86235::do_alu1_trans2_2(u64 op)
{
    const u8 sda = (op >> 38) & 3;
    const u8 sdb = (op >> 18) & 3;
    u32      ares = 0;
    u32      bres = 0;

    // A bus read
    switch (sda) {
        case 0:
        case 1:
        {
            const u8 as = (op >> 33) & 0x1f;
            ares        = get_transfer_reg(as);
            break;
        }
        case 2:
        {
            const u32 addr =
                decode_ea((op >> 20) & 0xf, (op >> 30) & 7, (op >> 27) & 7, (op >> 24) & 7, false);
            ares = read_abus(addr);
            break;
        }
        default:
            unimplemented_double_xfer2(op);
            return;
    }

    // B bus read
    switch (sdb) {
        case 0:
        case 1:
        {
            const u8 bs = (op >> 13) & 0x1f;
            bres        = get_transfer_reg(bs | 0x20);
            break;
        }
        case 2:
        {
            const u32 addr = decode_ea(op & 0xf, (op >> 10) & 7, (op >> 7) & 7, (op >> 4) & 7, true);
            bres           = read_bbus(addr);
            break;
        }
        default:
            unimplemented_double_xfer2(op);
            return;
    }

    // do alu
    do_alu1(op);

    // A bus write
    switch (sda) {
        case 0:
        case 2:
        {
            const u8 ad = (op >> 28) & 0x1f;
            set_transfer_reg(ad, ares);
            break;
        }
        case 1:
        {
            const u32 addr =
                decode_ea((op >> 20) & 0xf, (op >> 30) & 7, (op >> 27) & 7, (op >> 24) & 7, false);
            write_abus(addr, ares);
            break;
        }
        default:
            break;
    }

    // B bus write
    switch (sdb) {
        case 0:
        case 2:
        {
            const u8 bd = (op >> 8) & 0x1f;
            set_transfer_reg(bd | 0x20, bres);
            break;
        }
        case 1:
        {
            const u32 addr = decode_ea(op & 0xf, (op >> 10) & 7, (op >> 7) & 7, (op >> 4) & 7, true);
            write_bbus(addr, bres);
            break;
        }
        default:
            break;
    }
}

// transfer type 2
void MB86235::do_alu1_trans1_2(u64 op)
{
    u8  sr;
    u8  dr;
    u32 res;

    if ((op >> 38) & 1) {  // external transfer
        if ((op >> 37) & 1) {  // ext->int
            u32 addr = m_eb + m_eo;
            res      = m_bus->external_read(addr);

            // do alu
            do_alu1(op);

            dr = (op >> 24) & 0x7f;
            if (dr & 0x40) {
                const bool isbbus = (dr & 0x20) == 0x20;
                addr =
                    decode_ea(op & 0xf, dr & 7, (op >> 4) & 7, (op >> 7) & 0x3fff, isbbus) & 0x3ff;
                if (isbbus) {
                    write_bbus(addr, res);
                } else {
                    write_abus(addr, res);
                }
            } else {
                set_transfer_reg(dr, res);
            }

            s8 disp_offs = (op >> 31) & 0x3f;
            if (disp_offs & 0x20) {
                disp_offs -= 0x40;
            }
            m_eo += disp_offs;
        } else {  // int->ext
            sr = (op >> 24) & 0x7f;
            if (sr & 0x40) {
                if (sr == 0x58) {
                    res = op & 0xffffff;
                } else {
                    const bool isbbus = (sr & 0x20) == 0x20;
                    const u32  addr =
                        decode_ea(op & 0xf, sr & 7, (op >> 4) & 7, (op >> 7) & 0x3fff, isbbus)
                        & 0x3ff;
                    res = isbbus ? read_bbus(addr) : read_abus(addr);
                }
            } else {
                res = get_transfer_reg(sr);
            }

            // do alu
            do_alu1(op);

            const u32 addr = m_eb + m_eo;
            m_bus->external_write(addr, res);

            s8 disp_offs = (op >> 31) & 0x3f;
            if (disp_offs & 0x20) {
                disp_offs -= 0x40;
            }
            m_eo += disp_offs;
        }
    } else {
        sr = (op >> 31) & 0x7f;
        if (sr & 0x40) {
            if (sr == 0x58) {
                res = op & 0xffffff;
            } else {
                const bool isbbus = (sr & 0x20) == 0x20;
                const u32  addr =
                    decode_ea(op & 0xf, sr & 7, (op >> 4) & 7, (op >> 7) & 0x3fff, isbbus);
                res = isbbus ? read_bbus(addr) : read_abus(addr);
            }
        } else {
            res = get_transfer_reg(sr);
        }

        // do alu
        do_alu1(op);

        dr = (op >> 24) & 0x7f;
        if (dr & 0x40) {
            if (dr == 0x58) {
                fault("illegal do_alu1_trans1_2 dr == 0x58", op);
                return;
            }

            const bool isbbus = (dr & 0x20) == 0x20;
            const u32  addr =
                decode_ea(op & 0xf, dr & 7, (op >> 4) & 7, (op >> 7) & 0x3fff, isbbus);
            if (isbbus) {
                write_bbus(addr, res);
            } else {
                write_abus(addr, res);
            }
        } else {
            set_transfer_reg(dr, res);
        }
    }
}

// transfer type 3
void MB86235::do_trans1_3(u64 op)
{
    const u8  dr  = (op >> 19) & 0x7f;
    const u32 imm = (op >> 27) & 0xffffffff;

    if (dr & 0x40) {
        const bool isbbus = (dr & 0x20) == 0x20;
        const u32  addr = decode_ea(op & 0xf, dr & 7, (op >> 4) & 7, (op >> 7) & 0xfff, isbbus);
        if (isbbus) {
            write_bbus(addr, imm);
        } else {
            write_abus(addr, imm);
        }
    } else {  // direct imm reg
        set_transfer_reg(dr, imm);
    }
}

// ============================================================================
// Control
// ============================================================================

void MB86235::push_pc(u32 pcval)
{
    // Deviation: upstream's interpreter masks PCP to two bits, so a fifth nested
    // call silently overwrites the oldest return address (its own overflow check
    // is commented out and, because the mask is applied first, could never have
    // fired anyway). The recompiler checks properly, and a lost return address
    // sends control flow somewhere arbitrary, so the recompiler's check is used
    // here in preference to the wrap. PCP therefore counts 0..4 rather than
    // wrapping; nothing outside these two functions relies on its range.
    if (m_pcp >= 4) {
        pcs_overflow();
        return;
    }

    m_pcs[m_pcp++] = pcval;
}

u32 MB86235::pop_pc()
{
    if (m_pcp == 0) {
        pcs_underflow();
        return 0;
    }

    m_pcp--;

    return m_pcs[m_pcp];
}

u32 MB86235::do_control_dst(u64 op)
{
    u32 addr = 0;

    switch ((op >> 13) & 7) {
        case 0:  // immediate
            addr = op & 0xfff;
            break;
        case 1:  // register ARx
            addr = m_ar[(op >> 6) & 7];
            break;
        case 2:
            addr = ((op >> 11) & 1) ? m_ab[(op >> 6) & 7] : m_aa[(op >> 6) & 7];
            break;
        case 3:
            addr = ((op >> 11) & 1) ? m_mb[(op >> 6) & 7] : m_ma[(op >> 6) & 7];
            break;
        case 4:
            addr = read_abus(op & 0x3ff);
            break;
        case 5:
            addr = read_bbus(op & 0x3ff);
            break;
        case 6:
            addr = read_abus(m_ar[(op >> 6) & 7]);
            break;
        case 7:
            addr = read_bbus(m_ar[(op >> 6) & 7]);
            break;
        default:
            break;
    }

    if ((op >> 12) & 1) {
        m_icount--;
        return (m_pc + addr) & 0xfff;
    }

    return addr & 0xfff;
}

void MB86235::set_mod(u16 mod1, u16 mod2)
{
    m_mod &= ~static_cast<u32>(mod1);
    m_mod |= mod2;
}

bool MB86235::decode_branch_jump(u8 which)
{
    if (which < 19) {
        static constexpr u32 condition_table[19] = { MN, MZ, MV, MU, ZD, NR, IL, ZC, AN, AZ,
                                                     AV, AU, MD, AD, 0,  0,  F0, F1, F2 };

        return (pack_st() & condition_table[which]) != 0;
    }

    switch (which) {
        case 20:  // IFF
            return m_bus->fifo_in_full();
        case 21:  // IFE
            return m_bus->fifo_in_empty();
        case 22:  // OFF
            return m_bus->fifo_out_full();
        case 23:  // OFE
            return m_bus->fifo_out_empty();
        case 24:  // IF
            return false;  // bit 33 of input FIFO; not used by Model 2
        default:
            break;
    }

    char message[64];
    std::snprintf(message, sizeof(message), "illegal decode_branch_jump mode %02x", which);
    fault(message, 0);
    return false;
}

void MB86235::do_alu_control(u64 op)
{
    const u32 cop = (op >> 22) & 0x1f;
    const u32 ef1 = (op >> 16) & 0x3f;
    const u16 ef2 = op & 0xffff;

    switch (cop) {
        case 0x00:  // NOP
            break;
        case 0x01:  // REP
            if (ef1 == 0x3f) {
                m_rpc = m_ar[(ef2 >> 13) & 7];
            } else {
                m_rpc = ef2;
            }

            m_st |= RP;  // set repeat flag
            break;
        case 0x02:  // SETL
            if (ef1 == 0x3f) {
                m_lpc = m_ar[(ef2 >> 13) & 7];
            } else {
                m_lpc = ef2;
            }

            m_st |= LP;  // set loop flag
            break;
        case 0x03:  // CLRF
            if (ef1 & 1) {  // clear fifo in (CLRFI)
                m_bus->fifo_in_clear();
            }
            if (ef1 & 2) {  // clear fifo out (CLRFO)
                m_bus->fifo_out_clear();
                // The slot holding a result that could not be pushed is part of
                // the output path, so it goes too.
                m_fifo_out_pending = false;
            }
            break;
        case 0x04:  // PUSH
            m_sp--;
            m_sp &= 0x3ff;
            write_bbus(m_sp, get_transfer_reg((ef2 >> 6) & 0x3f));
            break;
        case 0x05:  // POP
            // wait until ALU operation(s) have finished before popping
            break;
        case 0x08:  // SETM
            set_mod(0xffff, ef2);
            break;
        case 0x09:  // SETMCBSA
            set_mod(0x7000, ef2);
            break;
        case 0x0a:  // SETMCBSB
            set_mod(0x0e00, ef2);
            break;
        case 0x0b:  // SETMRF
            set_mod(0x0080, ef2);
            break;
        case 0x0c:  // SETMRDY
            set_mod(0x0010, ef2);
            break;
        case 0x0d:  // SETMWAIT
            set_mod(0x0007, ef2);
            break;

        // control flow
        case 0x10:  // DBcc
        {
            if (decode_branch_jump(ef1)) {
                m_delay_slot = true;
                m_delay_pc   = do_control_dst(op);
            }
            m_icount--;
            break;
        }
        case 0x11:  // DBNcc
        {
            if (!decode_branch_jump(ef1)) {
                m_delay_slot = true;
                m_delay_pc   = do_control_dst(op);
            }
            m_icount--;
            break;
        }
        case 0x12:  // DJMP
        {
            m_delay_slot = true;
            m_delay_pc   = do_control_dst(op);
            m_icount--;
            break;
        }
        case 0x13:  // DBLP
        {
            if (m_st & LP) {
                m_delay_slot = true;
                // relative addressing only
                m_delay_pc = m_pc + (op & 0xfff);
                m_delay_pc &= 0xfff;
            }

            --m_lpc;
            if (m_lpc == 1) {
                m_st &= ~LP;
            }
            break;
        }
        case 0x14:  // DBBC
        {
            const bool result = (m_ar[(op >> 13) & 7] & (1u << ((op >> 16) & 0xf))) != 0;
            if (!result) {
                m_delay_slot = true;
                m_delay_pc   = m_pc + (op & 0xfff);
                m_delay_pc &= 0xfff;
            }
            m_icount -= 2;
            break;
        }
        case 0x15:  // DBBS
        {
            const bool result = (m_ar[(op >> 13) & 7] & (1u << ((op >> 16) & 0xf))) != 0;
            if (result) {
                m_delay_slot = true;
                m_delay_pc   = m_pc + (op & 0xfff);
                m_delay_pc &= 0xfff;
            }
            m_icount -= 2;
            break;
        }
        case 0x18:  // DCcc
        {
            if (decode_branch_jump(ef1)) {
                m_delay_slot = true;
                m_delay_pc   = do_control_dst(op);
                push_pc(m_pc + 1);
            }
            m_icount--;
            break;
        }
        case 0x19:  // DCNcc
        {
            if (!decode_branch_jump(ef1)) {
                m_delay_slot = true;
                m_delay_pc   = do_control_dst(op);
                push_pc(m_pc + 1);
            }
            m_icount--;
            break;
        }
        case 0x1a:  // DCALL
            m_delay_slot = true;
            m_delay_pc   = do_control_dst(op);
            push_pc(m_pc + 1);
            break;
        case 0x1b:  // DRET
            m_delay_slot = true;
            m_delay_pc   = pop_pc();
            break;
        default:
            unimplemented_control(cop);
            break;
    }

    // do alu
    if ((op >> 63) & 1) {
        do_alu1(op);
    } else {
        do_alu2(op);
    }

    // now we can safely pop if needed
    if (cop == 0x05) {
        set_transfer_reg((ef2 >> 6) & 0x3f, read_bbus(m_sp));
        m_sp++;
        m_sp &= 0x3ff;
    }
}

// ============================================================================
// Diagnostics
// ============================================================================

std::string MB86235::state_string() const
{
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "PC=%03x PPC=%03x ST=%08x MOD=%04x SP=%03x EB=%06x EO=%06x "
                  "RPC=%04x LPC=%04x PRP=%02x PWP=%02x PCP=%x "
                  "AR=%04x %04x %04x %04x %04x %04x %04x %04x",
                  m_pc & 0xfff, m_ppc & 0xfff, pack_st(), m_mod & 0xffff, m_sp & 0x3ff,
                  m_eb & 0xffffff, m_eo & 0xffffff, m_rpc & 0xffff, m_lpc & 0xffff, m_prp, m_pwp,
                  m_pcp, m_ar[0], m_ar[1], m_ar[2], m_ar[3], m_ar[4], m_ar[5], m_ar[6], m_ar[7]);
    return buffer;
}

#undef GETAOP
#undef GETAI1
#undef GETAI2
#undef GETAO
#undef GETMOP
#undef GETMI1
#undef GETMI2
#undef GETMO

}  // namespace sm2::cpu::mb86235
