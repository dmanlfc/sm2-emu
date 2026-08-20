// SPDX-License-Identifier: BSD-3-Clause
//
// Analog Devices ADSP-21062 "SHARC" DSP core — interpreter.
//
// Derived from MAME's src/devices/cpu/sharc/, which is BSD-3-Clause,
// copyright-holders Ville Linde.

#include "cpu/sharc/sharc.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace sm2::cpu::sharc {

// IEEE754 single-precision constants
static constexpr u32 FLOAT_CANONICAL_NAN  = 0xffffffff;
static constexpr u32 FLOAT_INFINITY       = 0x7f800000;
static constexpr u32 FLOAT_SIGN_MASK      = 0x80000000;
static constexpr u32 FLOAT_EXPONENT_MASK  = 0x7f800000;
static constexpr u32 FLOAT_MANTISSA_MASK  = 0x007fffff;
static constexpr unsigned FLOAT_EXPONENT_SHIFT = 23;
static constexpr unsigned FLOAT_EXPONENT_BITS  = 8;
static constexpr int FLOAT_EXPONENT_BIAS       = 127;

// Float classification helpers
static constexpr bool IS_FLOAT_ZERO(u32 r) { return (r & (FLOAT_EXPONENT_MASK | FLOAT_MANTISSA_MASK)) == 0; }
static constexpr bool IS_FLOAT_DENORMAL(u32 r) { return ((r & FLOAT_EXPONENT_MASK) == 0) && ((r & FLOAT_MANTISSA_MASK) != 0); }
static constexpr bool IS_FLOAT_DENORMAL_OR_ZERO(u32 r) { return (r & FLOAT_EXPONENT_MASK) == 0; }
static constexpr bool IS_FLOAT_NAN(u32 r) { return ((r & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK) && ((r & FLOAT_MANTISSA_MASK) != 0); }
static constexpr bool IS_FLOAT_INFINITY(u32 r) { return (r & (FLOAT_EXPONENT_MASK | FLOAT_MANTISSA_MASK)) == FLOAT_INFINITY; }
static constexpr u32 FLOAT_FLUSH_DENORMAL(u32 r) { return IS_FLOAT_DENORMAL_OR_ZERO(r) ? (r & FLOAT_SIGN_MASK) : r; }

static inline bool IS_FLOAT_NAN_ADD(u32 a, u32 b) {
    bool aemax = (a & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK;
    bool bemax = (b & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK;
    return (aemax && ((a & FLOAT_MANTISSA_MASK) != 0)) || (bemax && ((b & FLOAT_MANTISSA_MASK) != 0)) || (aemax && bemax && ((a ^ b) & FLOAT_SIGN_MASK));
}
static inline bool IS_FLOAT_NAN_SUB(u32 a, u32 b) {
    bool aemax = (a & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK;
    bool bemax = (b & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK;
    return (aemax && ((a & FLOAT_MANTISSA_MASK) != 0)) || (bemax && ((b & FLOAT_MANTISSA_MASK) != 0)) || (aemax && bemax && !((a ^ b) & FLOAT_SIGN_MASK));
}
static inline bool IS_FLOAT_NAN_MUL(u32 a, u32 b) {
    bool aemax = (a & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK;
    bool bemax = (b & FLOAT_EXPONENT_MASK) == FLOAT_EXPONENT_MASK;
    return (aemax && (((a & FLOAT_MANTISSA_MASK) != 0) || IS_FLOAT_ZERO(b))) || (bemax && (((b & FLOAT_MANTISSA_MASK) != 0) || IS_FLOAT_ZERO(a)));
}

static constexpr int float_get_unbiased_exponent(u32 f) {
    return int(s32((f >> FLOAT_EXPONENT_SHIFT) & ((1u << FLOAT_EXPONENT_BITS) - 1u))) - FLOAT_EXPONENT_BIAS;
}
static constexpr u32 float_make_biased_exponent(int exponent) {
    return u32((exponent + FLOAT_EXPONENT_BIAS) & ((1 << FLOAT_EXPONENT_BITS) - 1)) << FLOAT_EXPONENT_SHIFT;
}

// Sign extension helper
static constexpr s32 sext(u32 value, unsigned bits) {
    u32 mask = 1u << (bits - 1);
    u32 raw  = value & ((1u << bits) - 1u);
    return s32((raw ^ mask) - mask);
}

// Opcode field extraction helpers (matching MAME's)
static constexpr unsigned op_get_compute(u64 opcode)   { return unsigned((opcode >> 0) & 0x7fffff); }
static constexpr unsigned op_get_cond(u64 opcode)      { return unsigned((opcode >> 33) & 0x1f); }
static constexpr unsigned op_get_ureg_src(u64 opcode)  { return unsigned((opcode >> 36) & 0xff); }
static constexpr unsigned op_get_cond_ureg(u64 opcode) { return unsigned((opcode >> 31) & 0x1f); }
static constexpr unsigned op_get_ureg_dst(u64 opcode)  { return unsigned((opcode >> 23) & 0xff); }
static constexpr int      op_get_reladdr(u64 opcode)   { return sext(unsigned(opcode >> 27), 6); }
static constexpr unsigned op_get_dmi(u64 opcode)       { return unsigned((opcode >> 41) & 0x7); }
static constexpr unsigned op_get_dmm(u64 opcode)       { return unsigned((opcode >> 38) & 0x7); }
static constexpr unsigned op_get_pmi(u64 opcode)       { return unsigned((opcode >> 30) & 0x7); }
static constexpr unsigned op_get_pmm(u64 opcode)       { return unsigned((opcode >> 27) & 0x7); }
static constexpr unsigned op_get_jump_la(u64 opcode)   { return unsigned((opcode >> 38) & 1); }
static constexpr unsigned op_get_jump_ci(u64 opcode)   { return unsigned((opcode >> 24) & 1); }
static constexpr unsigned op_get_jump_j(u64 opcode)    { return unsigned((opcode >> 26) & 1); }
static constexpr unsigned op_get_jump_e(u64 opcode)    { return unsigned((opcode >> 25) & 1); }
static constexpr unsigned op_get_rs(u64 opcode)        { return unsigned((opcode >> 12) & 0xf); }
static constexpr unsigned op_get_ra(u64 opcode)        { return unsigned((opcode >> 8) & 0xf); }

// DMA packing modes
static constexpr int DMA_PMODE_NO_PACKING = 0;
static constexpr int DMA_PMODE_16_32      = 1;
// static constexpr int DMA_PMODE_16_48   = 2;  // unused for now
// static constexpr int DMA_PMODE_32_48   = 3;  // unused for now
static constexpr int DMA_PMODE_8_48       = 4;

// ============================================================================
// Lookup tables
// ============================================================================

const u32 SHARC::s_recips_mantissa_lookup[128] = {
    0x007f8000, 0x007e0000, 0x007c0000, 0x007a0000, 0x00780000, 0x00760000, 0x00740000, 0x00720000,
    0x00700000, 0x006f0000, 0x006d0000, 0x006b0000, 0x006a0000, 0x00680000, 0x00660000, 0x00650000,
    0x00630000, 0x00610000, 0x00600000, 0x005e0000, 0x005d0000, 0x005b0000, 0x005a0000, 0x00590000,
    0x00570000, 0x00560000, 0x00540000, 0x00530000, 0x00520000, 0x00500000, 0x004f0000, 0x004e0000,
    0x004c0000, 0x004b0000, 0x004a0000, 0x00490000, 0x00470000, 0x00460000, 0x00450000, 0x00440000,
    0x00430000, 0x00410000, 0x00400000, 0x003f0000, 0x003e0000, 0x003d0000, 0x003c0000, 0x003b0000,
    0x003a0000, 0x00390000, 0x00380000, 0x00370000, 0x00360000, 0x00350000, 0x00340000, 0x00330000,
    0x00320000, 0x00310000, 0x00300000, 0x002f0000, 0x002e0000, 0x002d0000, 0x002c0000, 0x002b0000,
    0x002a0000, 0x00290000, 0x00280000, 0x00280000, 0x00270000, 0x00260000, 0x00250000, 0x00240000,
    0x00230000, 0x00230000, 0x00220000, 0x00210000, 0x00200000, 0x001f0000, 0x001f0000, 0x001e0000,
    0x001d0000, 0x001c0000, 0x001c0000, 0x001b0000, 0x001a0000, 0x00190000, 0x00190000, 0x00180000,
    0x00170000, 0x00170000, 0x00160000, 0x00150000, 0x00140000, 0x00140000, 0x00130000, 0x00120000,
    0x00120000, 0x00110000, 0x00100000, 0x00100000, 0x000f0000, 0x000f0000, 0x000e0000, 0x000d0000,
    0x000d0000, 0x000c0000, 0x000c0000, 0x000b0000, 0x000a0000, 0x000a0000, 0x00090000, 0x00090000,
    0x00080000, 0x00070000, 0x00070000, 0x00060000, 0x00060000, 0x00050000, 0x00050000, 0x00040000,
    0x00040000, 0x00030000, 0x00030000, 0x00020000, 0x00020000, 0x00010000, 0x00010000, 0x00000000,
};

const u32 SHARC::s_rsqrts_mantissa_lookup[128] = {
    0x00350000, 0x00330000, 0x00320000, 0x00300000, 0x002f0000, 0x002e0000, 0x002d0000, 0x002b0000,
    0x002a0000, 0x00290000, 0x00280000, 0x00270000, 0x00260000, 0x00250000, 0x00230000, 0x00220000,
    0x00210000, 0x00200000, 0x001f0000, 0x001e0000, 0x001e0000, 0x001d0000, 0x001c0000, 0x001b0000,
    0x001a0000, 0x00190000, 0x00180000, 0x00170000, 0x00160000, 0x00160000, 0x00150000, 0x00140000,
    0x00130000, 0x00130000, 0x00120000, 0x00110000, 0x00100000, 0x00100000, 0x000f0000, 0x000e0000,
    0x000e0000, 0x000d0000, 0x000c0000, 0x000b0000, 0x000b0000, 0x000a0000, 0x000a0000, 0x00090000,
    0x00080000, 0x00080000, 0x00070000, 0x00070000, 0x00060000, 0x00050000, 0x00050000, 0x00040000,
    0x00040000, 0x00030000, 0x00030000, 0x00020000, 0x00020000, 0x00010000, 0x00010000, 0x00000000,
    0x007f8000, 0x007e0000, 0x007c0000, 0x007a0000, 0x00780000, 0x00760000, 0x00740000, 0x00730000,
    0x00710000, 0x006f0000, 0x006e0000, 0x006c0000, 0x006a0000, 0x00690000, 0x00670000, 0x00660000,
    0x00640000, 0x00630000, 0x00620000, 0x00600000, 0x005f0000, 0x005e0000, 0x005c0000, 0x005b0000,
    0x005a0000, 0x00590000, 0x00570000, 0x00560000, 0x00550000, 0x00540000, 0x00530000, 0x00520000,
    0x00510000, 0x004f0000, 0x004e0000, 0x004d0000, 0x004c0000, 0x004b0000, 0x004a0000, 0x00490000,
    0x00480000, 0x00470000, 0x00460000, 0x00450000, 0x00450000, 0x00440000, 0x00430000, 0x00420000,
    0x00410000, 0x00400000, 0x003f0000, 0x003e0000, 0x003e0000, 0x003d0000, 0x003c0000, 0x003b0000,
    0x003a0000, 0x003a0000, 0x00390000, 0x00380000, 0x00370000, 0x00370000, 0x00360000, 0x00350000,
};

// ============================================================================
// Opcode table
// ============================================================================

const SHARC::OpTableEntry SHARC::s_opcode_table[] = {
    { 0xe000, 0x2000, &SHARC::sharcop_compute_dreg_dm_dreg_pm },
    { 0xff00, 0x0100, &SHARC::sharcop_compute },
    { 0xf000, 0x4000, &SHARC::sharcop_compute_ureg_dmpm_premod },
    { 0xf000, 0x5000, &SHARC::sharcop_compute_ureg_dmpm_postmod },
    { 0xf180, 0x6000, &SHARC::sharcop_compute_dm_to_dreg_immmod },
    { 0xf180, 0x6080, &SHARC::sharcop_compute_dreg_to_dm_immmod },
    { 0xf180, 0x6100, &SHARC::sharcop_compute_pm_to_dreg_immmod },
    { 0xf180, 0x6180, &SHARC::sharcop_compute_dreg_to_pm_immmod },
    { 0xf000, 0x7000, &SHARC::sharcop_compute_ureg_to_ureg },
    { 0xf000, 0x8000, &SHARC::sharcop_imm_shift_dreg_dmpm },
    { 0xff00, 0x0200, &SHARC::sharcop_imm_shift },
    { 0xff00, 0x0400, &SHARC::sharcop_compute_modify },
    { 0xff80, 0x0600, &SHARC::sharcop_direct_jump },
    { 0xff80, 0x0680, &SHARC::sharcop_direct_call },
    { 0xff80, 0x0700, &SHARC::sharcop_relative_jump },
    { 0xff80, 0x0780, &SHARC::sharcop_relative_call },
    { 0xff80, 0x0800, &SHARC::sharcop_indirect_jump },
    { 0xff80, 0x0880, &SHARC::sharcop_indirect_call },
    { 0xff80, 0x0900, &SHARC::sharcop_relative_jump_compute },
    { 0xff80, 0x0980, &SHARC::sharcop_relative_call_compute },
    { 0xe000, 0xc000, &SHARC::sharcop_indirect_jump_compute_dreg_dm },
    { 0xe000, 0xe000, &SHARC::sharcop_relative_jump_compute_dreg_dm },
    { 0xff00, 0x0a00, &SHARC::sharcop_rts },
    { 0xff00, 0x0b00, &SHARC::sharcop_rti },
    { 0xff00, 0x0c00, &SHARC::sharcop_do_until_counter_imm },
    { 0xff00, 0x0d00, &SHARC::sharcop_do_until_counter_ureg },
    { 0xff00, 0x0e00, &SHARC::sharcop_do_until },
    { 0xff00, 0x1000, &SHARC::sharcop_dm_to_ureg_direct },
    { 0xff00, 0x1100, &SHARC::sharcop_ureg_to_dm_direct },
    { 0xff00, 0x1200, &SHARC::sharcop_pm_to_ureg_direct },
    { 0xff00, 0x1300, &SHARC::sharcop_ureg_to_pm_direct },
    { 0xf100, 0xa000, &SHARC::sharcop_dm_to_ureg_indirect },
    { 0xf100, 0xa100, &SHARC::sharcop_ureg_to_dm_indirect },
    { 0xf100, 0xb000, &SHARC::sharcop_pm_to_ureg_indirect },
    { 0xf100, 0xb100, &SHARC::sharcop_ureg_to_pm_indirect },
    { 0xf000, 0x9000, &SHARC::sharcop_imm_to_dmpm },
    { 0xff00, 0x0f00, &SHARC::sharcop_imm_to_ureg },
    { 0xff00, 0x1400, &SHARC::sharcop_sysreg_bitop },
    { 0xff80, 0x1600, &SHARC::sharcop_modify },
    { 0xff80, 0x1680, &SHARC::sharcop_bit_reverse },
    { 0xff00, 0x1700, &SHARC::sharcop_push_pop_stacks },
    { 0xff80, 0x0000, &SHARC::sharcop_nop },
    { 0xff80, 0x0080, &SHARC::sharcop_idle },
};

const int SHARC::s_num_ops = int(sizeof(s_opcode_table) / sizeof(s_opcode_table[0]));

// ============================================================================
// Constructor / Reset / Run
// ============================================================================

SHARC::SHARC(Bus& bus) : m_bus(&bus)
{
    build_opcode_table();
}

void SHARC::build_opcode_table()
{
    for (int i = 0; i < 512; i++) {
        m_sharc_op[i] = &SHARC::sharcop_unimplemented;
        u16 op = u16(i << 7);
        for (int j = 0; j < s_num_ops; j++) {
            if ((s_opcode_table[j].op_mask & op) == s_opcode_table[j].op_bits) {
                m_sharc_op[i] = s_opcode_table[j].handler;
                break;
            }
        }
    }
}

void SHARC::reset()
{
    std::memset(m_r, 0, sizeof(m_r));
    std::memset(m_reg_alt, 0, sizeof(m_reg_alt));
    std::memset(m_pcstack, 0, sizeof(m_pcstack));
    std::memset(m_lcstack, 0, sizeof(m_lcstack));
    std::memset(m_lastack, 0, sizeof(m_lastack));
    std::memset(m_flag, 0, sizeof(m_flag));
    std::memset(m_dma, 0, sizeof(m_dma));
    std::memset(m_dma_op, 0, sizeof(m_dma_op));
    std::memset(m_status_stack, 0, sizeof(m_status_stack));

    m_dag1 = DAG{};
    m_dag2 = DAG{};
    m_dag1_alt = DAG{};
    m_dag2_alt = DAG{};

    // Host boot mode: DMA channel 6 configured for program upload
    m_dma[6].int_index    = 0x20000;
    m_dma[6].int_modifier = 1;
    m_dma[6].int_count    = 0x100;
    m_dma[6].ext_index    = 0x400000;
    m_dma[6].ext_modifier = 1;
    m_dma[6].ext_count    = 0x600;
    m_dma[6].control      = 0xa1;

    m_pc     = 0x20004;
    m_daddr  = m_pc + 1;
    m_faddr  = m_daddr + 1;
    m_nfaddr = m_faddr + 1;

    m_idle = 0;
    m_mode1 = 0;
    m_mode2 = 0;
    m_astat = 0;
    m_stky = PCEM | SSEM | LSEM;
    m_irptl = 0;
    m_imask = 0x0003;
    m_ustat1 = 0;
    m_ustat2 = 0;
    m_imaskp = 0;

    m_pcstkp = 0;
    m_lstkp  = 0;
    m_pcstk  = 0x00ffffff;
    m_curlcntr = 0xffffffff;
    m_lcntr  = 0;
    m_laddr.unpack(0xffffffff);
    m_status_stkp = 0;
    m_interrupt_active = 0;

    m_syscon  = 0x00000010;
    m_sysstat = 0;
    m_iop_write_num = 0;
    m_iop_data = 0;
    m_extdma_shift = 0;
    m_dma_status = 0;

    m_mrf = 0;
    m_mrb = 0;
    m_px = 0;

    m_delay_slot1 = 0;
    m_delay_slot2 = 0;
    m_systemreg_latency_cycles = 0;
    m_systemreg_latency_reg = -1;
    m_astat_old = 0;
    m_astat_old_old = 0;
    m_astat_old_old_old = 0;

    m_halted = false;
    m_write_stalled = false;
    m_irq_pending = 0;
    m_active_irq_num = 0;
    m_instruction_count = 0;
}

void SHARC::set_pc(u32 value)
{
    m_pc     = value;
    m_daddr  = value + 1;
    m_faddr  = value + 2;
    m_nfaddr = value + 3;
}

s32 SHARC::run(s32 cycles)
{
    if (cycles <= 0) return 0;
    m_icount = cycles;

    if (m_write_stalled) {
        m_icount = 0;
        return cycles;
    }

    if (m_idle && m_irq_pending == 0) {
        // Run DMAs while idle
        s32 dma_count = m_icount;
        while (dma_count > 0 && (m_dma_status & ((1 << 6) | (1 << 7)))) {
            if (!m_write_stalled) {
                dma_run_cycle(6);
                dma_run_cycle(7);
            }
            dma_count--;
        }
        m_icount = 0;
        return cycles;
    }

    if (m_irq_pending != 0) {
        check_interrupts();
        m_idle = 0;
    }

    while (m_icount > 0 && !m_idle && !m_write_stalled) {
        m_pc     = m_daddr;
        m_daddr  = m_faddr;
        m_faddr  = m_nfaddr;
        m_nfaddr++;

        m_astat_old_old_old = m_astat_old_old;
        m_astat_old_old = m_astat_old;
        m_astat_old = m_astat;

        m_opcode = m_bus->pm_read48(m_pc);

        if (m_trace_hook)
            m_trace_hook(m_trace_context, m_pc, m_opcode);

        // Handle loop back-edge
        if (!(m_stky & LSEM) && (m_pc == m_laddr.addr)) {
            switch (m_laddr.loop_type) {
            case 0: { // arithmetic condition-based
                if ((m_pc - TOP_PC()) > 2)
                    m_astat = m_astat_old_old_old;
                if (DO_CONDITION_CODE(m_laddr.code)) {
                    POP_LOOP();
                    POP_PC();
                } else {
                    CHANGE_PC(TOP_PC());
                }
                m_astat = m_astat_old;
                break;
            }
            case 1: // counter-based length 1
            case 2: // counter-based length 2
            case 3: { // counter-based length >2
                --m_curlcntr;
                if (m_curlcntr == 0) {
                    POP_LOOP();
                    POP_PC();
                } else {
                    CHANGE_PC(TOP_PC());
                }
                break;
            }
            }
        }

        // Dispatch
        (this->*m_sharc_op[(m_opcode >> 39) & 0x1ff])();

        // System register latency
        if (m_systemreg_latency_cycles > 0) {
            --m_systemreg_latency_cycles;
            if (m_systemreg_latency_cycles <= 0)
                systemreg_write_latency_effect();
        }

        // DMA
        if (!m_write_stalled) {
            dma_run_cycle(6);
            dma_run_cycle(7);
        }

        --m_icount;
        ++m_instruction_count;
    }

    return cycles - m_icount;
}

// ============================================================================
// PC flow control
// ============================================================================

void SHARC::CHANGE_PC(u32 newpc)
{
    m_pc     = newpc;
    m_daddr  = newpc;
    m_faddr  = newpc + 1;
    m_nfaddr = newpc + 2;
}

void SHARC::CHANGE_PC_DELAYED(u32 newpc)
{
    m_nfaddr = newpc;
    m_delay_slot1 = m_pc;
    m_delay_slot2 = m_daddr;
}

// ============================================================================
// Stack operations
// ============================================================================

void SHARC::PUSH_PC()
{
    if (m_pcstkp > 0)
        m_pcstack[m_pcstkp - 1] = m_pcstk;
    m_pcstkp++;
    m_stky &= ~PCEM;
    if (m_pcstkp >= 30)
        m_stky |= PCFL;
}

u32 SHARC::POP_PC()
{
    u32 result = m_pcstk;
    if (m_pcstkp > 0) m_pcstkp--;
    if (m_pcstkp > 0) {
        m_pcstk = m_pcstack[m_pcstkp - 1];
    } else {
        m_pcstk = 0x00ffffff;
        m_stky |= PCEM;
    }
    m_stky &= ~PCFL;
    return result;
}

u32 SHARC::TOP_PC() { return m_pcstk; }

void SHARC::PUSH_LOOP()
{
    if (m_lstkp > 0) {
        m_lcstack[m_lstkp - 1] = m_curlcntr;
        m_lastack[m_lstkp - 1] = m_laddr.pack();
    }
    m_curlcntr = m_lcntr;
    if (m_lstkp < 6)
        m_laddr.unpack(m_lastack[m_lstkp]);
    m_lstkp++;
    if (m_lstkp < 6)
        m_lcntr = m_lcstack[m_lstkp];
    else
        m_lcntr = 0xffffffff;
    m_stky &= ~LSEM;
}

void SHARC::POP_LOOP()
{
    if (m_lstkp > 0) m_lstkp--;
    m_lcntr = m_curlcntr;
    m_lastack[m_lstkp] = m_laddr.pack();
    if (m_lstkp > 0) {
        m_curlcntr = m_lcstack[m_lstkp - 1];
        m_laddr.unpack(m_lastack[m_lstkp - 1]);
    } else {
        m_curlcntr = 0xffffffff;
        m_laddr.unpack(0xffffffff);
        m_stky |= LSEM;
    }
}

void SHARC::PUSH_STATUS_STACK()
{
    m_status_stkp++;
    if (m_status_stkp > 5) m_status_stkp = 5;
    m_status_stack[m_status_stkp - 1].mode1 = m_mode1;
    m_status_stack[m_status_stkp - 1].astat = m_astat;
    m_stky &= ~SSEM;
}

void SHARC::POP_STATUS_STACK()
{
    if (m_status_stkp <= 0) return;
    m_status_stkp--;
    u32 flags_mask = FLG0 | FLG1 | FLG2 | FLG3;
    SET_UREG(0x7b, m_status_stack[m_status_stkp].mode1);
    m_astat = (m_astat & flags_mask) | (m_status_stack[m_status_stkp].astat & ~flags_mask);
    if (m_status_stkp == 0) m_stky |= SSEM;
}

// ============================================================================
// Circular buffer update
// ============================================================================

void SHARC::update_circular_buffer_pm(int i)
{
    if (m_dag2.l[i] != 0) {
        if (m_dag2.i[i] > m_dag2.b[i] + m_dag2.l[i])
            m_dag2.i[i] -= m_dag2.l[i];
        else if (m_dag2.i[i] < m_dag2.b[i])
            m_dag2.i[i] += m_dag2.l[i];
    }
}

void SHARC::update_circular_buffer_dm(int i)
{
    if (m_dag1.l[i] != 0) {
        if (m_dag1.i[i] > m_dag1.b[i] + m_dag1.l[i])
            m_dag1.i[i] -= m_dag1.l[i];
        else if (m_dag1.i[i] < m_dag1.b[i])
            m_dag1.i[i] += m_dag1.l[i];
    }
}

// ============================================================================
// Condition codes
// ============================================================================

int SHARC::IF_CONDITION_CODE(int cond)
{
    auto COND_LT = [&]() -> bool {
        if (m_astat & AF)
            return (m_astat & AN) && !(m_astat & AZ);
        return (bool(m_astat & AN)) != ((m_astat & AV) && !(m_mode1 & MODE1_ALUSAT));
    };
    auto COND_LE = [&]() -> bool {
        return (m_astat & AZ) || COND_LT();
    };

    switch (cond) {
    case 0x00: return m_astat & AZ;
    case 0x01: return COND_LT();
    case 0x02: return COND_LE();
    case 0x03: return m_astat & AC;
    case 0x04: return m_astat & AV;
    case 0x05: return m_astat & MV;
    case 0x06: return m_astat & MN;
    case 0x07: return m_astat & SV;
    case 0x08: return m_astat & SZ;
    case 0x09: return m_flag[0] != 0;
    case 0x0a: return m_flag[1] != 0;
    case 0x0b: return m_flag[2] != 0;
    case 0x0c: return m_flag[3] != 0;
    case 0x0d: return m_astat & BTF;
    case 0x0e: return 0;
    case 0x0f: return m_curlcntr != 1;
    case 0x10: return !(m_astat & AZ);
    case 0x11: return !COND_LT();
    case 0x12: return !COND_LE();
    case 0x13: return !(m_astat & AC);
    case 0x14: return !(m_astat & AV);
    case 0x15: return !(m_astat & MV);
    case 0x16: return !(m_astat & MN);
    case 0x17: return !(m_astat & SV);
    case 0x18: return !(m_astat & SZ);
    case 0x19: return m_flag[0] == 0;
    case 0x1a: return m_flag[1] == 0;
    case 0x1b: return m_flag[2] == 0;
    case 0x1c: return m_flag[3] == 0;
    case 0x1d: return !(m_astat & BTF);
    case 0x1e: return 1;
    case 0x1f: return 1;
    }
    return 1;
}

int SHARC::DO_CONDITION_CODE(int cond)
{
    if (cond == 0x0f) return m_curlcntr == 1; // LCE
    if (cond == 0x1f) return 0;               // FALSE (FOREVER)
    return IF_CONDITION_CODE(cond);
}

// ============================================================================
// UREG access
// ============================================================================

u32 SHARC::GET_UREG(int ureg)
{
    int reg = ureg & 0xf;
    switch ((ureg >> 4) & 0xf) {
    case 0x0: return u32(m_r[reg].r);
    case 0x1:
        return (reg & 0x8) ? m_dag2.i[reg & 0x7] : m_dag1.i[reg & 0x7];
    case 0x2:
        if (reg & 0x8) {
            s32 r = s32(m_dag2.m[reg & 0x7]);
            if (r & 0x800000) r |= s32(0xff000000);
            return u32(r);
        }
        return m_dag1.m[reg & 0x7];
    case 0x3:
        return (reg & 0x8) ? m_dag2.l[reg & 0x7] : m_dag1.l[reg & 0x7];
    case 0x4:
        return (reg & 0x8) ? m_dag2.b[reg & 0x7] : m_dag1.b[reg & 0x7];
    case 0x6:
        switch (reg) {
        case 0x4: return m_pcstk;
        case 0x5: return m_pcstkp;
        case 0x7: return m_curlcntr;
        case 0x8: return m_lcntr;
        default:  return 0;
        }
    case 0x7:
        switch (reg) {
        case 0x0: return m_ustat1;
        case 0x1: return m_ustat2;
        case 0x9: return m_irptl;
        case 0xa: return m_mode2;
        case 0xb: return m_mode1;
        case 0xc: return m_astat;
        case 0xd: return m_imask;
        case 0xe: return m_stky;
        default:  return 0;
        }
    case 0xd:
        switch (reg) {
        case 0xb: return u32(m_px);
        case 0xc: return u16(m_px);
        case 0xd: return u32(m_px >> 16);
        default:  return 0;
        }
    default: return 0;
    }
}

void SHARC::SET_UREG(int ureg, u32 data)
{
    int reg = ureg & 0xf;
    switch ((ureg >> 4) & 0xf) {
    case 0x0: m_r[reg].r = s32(data); break;
    case 0x1:
        if (reg & 0x8) m_dag2.i[reg & 0x7] = data;
        else           m_dag1.i[reg & 0x7] = data;
        break;
    case 0x2:
        if (reg & 0x8) m_dag2.m[reg & 0x7] = data;
        else           m_dag1.m[reg & 0x7] = data;
        break;
    case 0x3:
        if (reg & 0x8) m_dag2.l[reg & 0x7] = data;
        else           m_dag1.l[reg & 0x7] = data;
        break;
    case 0x4:
        if (reg & 0x8) { m_dag2.b[reg & 0x7] = data; m_dag2.i[reg & 0x7] = data; }
        else           { m_dag1.b[reg & 0x7] = data; m_dag1.i[reg & 0x7] = data; }
        break;

    case 0x6:
        switch (reg) {
        case 0x4: if (m_pcstkp > 0 && m_pcstkp < 31) m_pcstk = data & 0x00ffffff; break;
        case 0x5: m_pcstkp = data & 0x1f; break;
        case 0x7: if (m_lstkp > 0 && m_lstkp < 7) m_curlcntr = data; break;
        case 0x8: if (m_lstkp < 6) m_lcntr = data; break;
        default: break;
        }
        break;
    case 0x7:
        switch (reg) {
        case 0x0: m_ustat1 = data; break;
        case 0x1: m_ustat2 = data; break;
        case 0x9: m_irptl = data; break;
        case 0xa: m_mode2 = data; break;
        case 0xb:
            add_systemreg_write_latency_effect(reg, data, m_mode1);
            m_mode1 = data;
            break;
        case 0xc: m_astat = data; break;
        case 0xd: check_interrupts(); m_imask = data; break;
        case 0xe:
            m_stky = (m_stky & (LSEM | LSOV | SSEM | SSOV | PCEM | PCFL))
                   | (data & ~(LSEM | LSOV | SSEM | SSOV | PCEM | PCFL));
            break;
        default: break;
        }
        break;
    case 0xd:
        switch (reg) {
        case 0xc: m_px = (m_px & 0xffffffffffff0000ULL) | (data & 0xffff); break;
        case 0xd: m_px = (m_px & 0x000000000000ffffULL) | (u64(data) << 16); break;
        default: break;
        }
        break;
    default: break;
    }
}

// ============================================================================
// System register latency
// ============================================================================

void SHARC::add_systemreg_write_latency_effect(int sysreg, u32 data, u32 prev_data)
{
    if (m_systemreg_latency_cycles > 0)
        systemreg_write_latency_effect();
    m_systemreg_latency_cycles = 2;
    m_systemreg_latency_reg = sysreg;
    m_systemreg_latency_data = data;
    m_systemreg_previous_data = prev_data;
}

void SHARC::systemreg_write_latency_effect()
{
    u32 data = m_systemreg_latency_data;
    u32 old_data = m_systemreg_previous_data;
    if (m_systemreg_latency_reg == 0xb) { // MODE1
        u32 diff = data ^ old_data;
        m_mode1 = data;
        if (diff & MODE1_SRD1H) {
            for (int i = 4; i < 8; i++) {
                std::swap(m_dag1.i[i], m_dag1_alt.i[i]);
                std::swap(m_dag1.m[i], m_dag1_alt.m[i]);
                std::swap(m_dag1.l[i], m_dag1_alt.l[i]);
                std::swap(m_dag1.b[i], m_dag1_alt.b[i]);
            }
        }
        if (diff & MODE1_SRD1L) {
            for (int i = 0; i < 4; i++) {
                std::swap(m_dag1.i[i], m_dag1_alt.i[i]);
                std::swap(m_dag1.m[i], m_dag1_alt.m[i]);
                std::swap(m_dag1.l[i], m_dag1_alt.l[i]);
                std::swap(m_dag1.b[i], m_dag1_alt.b[i]);
            }
        }
        if (diff & MODE1_SRD2H) {
            for (int i = 4; i < 8; i++) {
                std::swap(m_dag2.i[i], m_dag2_alt.i[i]);
                std::swap(m_dag2.m[i], m_dag2_alt.m[i]);
                std::swap(m_dag2.l[i], m_dag2_alt.l[i]);
                std::swap(m_dag2.b[i], m_dag2_alt.b[i]);
            }
        }
        if (diff & MODE1_SRD2L) {
            for (int i = 0; i < 4; i++) {
                std::swap(m_dag2.i[i], m_dag2_alt.i[i]);
                std::swap(m_dag2.m[i], m_dag2_alt.m[i]);
                std::swap(m_dag2.l[i], m_dag2_alt.l[i]);
                std::swap(m_dag2.b[i], m_dag2_alt.b[i]);
            }
        }
        if (diff & MODE1_SRRFH) {
            for (int i = 8; i < 16; i++)
                std::swap(m_r[i].r, m_reg_alt[i].r);
        }
        if (diff & MODE1_SRRFL) {
            for (int i = 0; i < 8; i++)
                std::swap(m_r[i].r, m_reg_alt[i].r);
        }
    }
    m_systemreg_latency_reg = -1;
}

// ============================================================================
// Interrupts
// ============================================================================

void SHARC::check_interrupts()
{
    if ((m_imask & m_irq_pending) && (m_mode1 & MODE1_IRPTEN) && !m_interrupt_active &&
        m_pc != m_delay_slot1 && m_pc != m_delay_slot2)
    {
        int which = 0;
        for (int i = 0; i < 32; i++) {
            if (m_irq_pending & (1 << i)) break;
            which++;
        }
        if (which >= 32) return;

        PUSH_PC();
        m_pcstk = m_idle ? (m_pc + 1) : m_daddr;
        m_irptl |= 1 << which;
        if (which >= 6 && which <= 8)
            PUSH_STATUS_STACK();
        CHANGE_PC(0x20000 + (which * 0x4));
        m_active_irq_num = which;
        m_irq_pending &= ~(1 << which);
        m_interrupt_active = 1;
    }
}

// ============================================================================
// DMA
// ============================================================================

void SHARC::schedule_dma_op(int channel, u32 src, u32 dst, s32 src_modifier,
                            s32 dst_modifier, s32 src_count, s32 dst_count, int pmode)
{
    m_dma_op[channel].src = src;
    m_dma_op[channel].dst = dst;
    m_dma_op[channel].src_modifier = src_modifier;
    m_dma_op[channel].dst_modifier = dst_modifier;
    m_dma_op[channel].src_count = src_count;
    m_dma_op[channel].dst_count = dst_count;
    m_dma_op[channel].pmode = pmode;
    m_dma_op[channel].chain_ptr = 0;
    m_dma_op[channel].chained = false;
    m_dma_op[channel].active = true;
    m_dma_status |= (1 << channel);
}

void SHARC::schedule_chained_dma_op(int channel, u32 dma_chain_ptr, int chained_direction)
{
    u32 op_ptr = 0x20000 + (dma_chain_ptr & 0x1ffff);
    u32 int_index    = m_bus->dm_read32(op_ptr - 0);
    u32 int_modifier = m_bus->dm_read32(op_ptr - 1);
    u32 int_count    = m_bus->dm_read32(op_ptr - 2);
    u32 chain_ptr    = m_bus->dm_read32(op_ptr - 3);
    u32 ext_index    = m_bus->dm_read32(op_ptr - 5);
    u32 ext_modifier = m_bus->dm_read32(op_ptr - 6);
    u32 ext_count    = m_bus->dm_read32(op_ptr - 7);

    if (chained_direction) {
        m_dma_op[channel].dst = ext_index;
        m_dma_op[channel].dst_modifier = s32(ext_modifier);
        m_dma_op[channel].dst_count = s32(ext_count);
        m_dma_op[channel].src = int_index;
        m_dma_op[channel].src_modifier = s32(int_modifier);
        m_dma_op[channel].src_count = s32(int_count);
    } else {
        m_dma_op[channel].src = ext_index;
        m_dma_op[channel].src_modifier = s32(ext_modifier);
        m_dma_op[channel].src_count = s32(ext_count);
        m_dma_op[channel].dst = int_index;
        m_dma_op[channel].dst_modifier = s32(int_modifier);
        m_dma_op[channel].dst_count = s32(int_count);
    }
    m_dma_op[channel].pmode = 0;
    m_dma_op[channel].chain_ptr = chain_ptr;
    m_dma_op[channel].chained_direction = chained_direction;
    m_dma_op[channel].chained = true;
    m_dma_op[channel].active = true;
    m_dma_status |= (1 << channel);
}

void SHARC::dma_run_cycle(int channel)
{
    if (!(m_dma_status & (1 << channel))) return;
    u32 src = m_dma_op[channel].src;
    u32 dst = m_dma_op[channel].dst;
    s32 src_modifier = m_dma_op[channel].src_modifier;
    s32 dst_modifier = m_dma_op[channel].dst_modifier;
    s32 src_count = m_dma_op[channel].src_count;
    int pmode = m_dma_op[channel].pmode;

    switch (pmode) {
    case DMA_PMODE_NO_PACKING: {
        u32 data = m_bus->dm_read32(src);
        m_bus->dm_write32(dst, data);
        src += u32(src_modifier);
        dst += u32(dst_modifier);
        src_count--;
        break;
    }
    case DMA_PMODE_16_32: {
        u32 data = ((m_bus->dm_read32(src) & 0xffff) << 16) | (m_bus->dm_read32(src + 1) & 0xffff);
        m_bus->dm_write32(dst, data);
        src += u32(src_modifier * 2);
        dst += u32(dst_modifier);
        src_count -= 2;
        break;
    }
    case DMA_PMODE_8_48: {
        u64 data = (u64(m_bus->dm_read32(src + 0) & 0xff) <<  0) |
                   (u64(m_bus->dm_read32(src + 1) & 0xff) <<  8) |
                   (u64(m_bus->dm_read32(src + 2) & 0xff) << 16) |
                   (u64(m_bus->dm_read32(src + 3) & 0xff) << 24) |
                   (u64(m_bus->dm_read32(src + 4) & 0xff) << 32) |
                   (u64(m_bus->dm_read32(src + 5) & 0xff) << 40);
        m_bus->pm_write48(dst, data);
        src += u32(src_modifier * 6);
        dst += u32(dst_modifier);
        src_count -= 6;
        break;
    }
    default: break;
    }

    m_dma_op[channel].src_count = src_count;
    m_dma_op[channel].src = src;
    m_dma_op[channel].dst = dst;

    if (src_count <= 0) {
        m_dma_status &= ~(1u << channel);
        m_dma_op[channel].active = false;
        if (!m_dma_op[channel].chained || (m_dma_op[channel].chain_ptr & 0x20000)) {
            m_irptl |= (1 << (channel + 10));
            if (m_imask & (1 << (channel + 10)))
                m_irq_pending |= 1 << (channel + 10);
        }
        if ((m_dma_op[channel].chain_ptr & 0x1ffff) != 0)
            schedule_chained_dma_op(channel, m_dma_op[channel].chain_ptr, m_dma_op[channel].chained_direction);
    }
}

void SHARC::dma_op(int channel)
{
    u32 src = m_dma_op[channel].src;
    u32 dst = m_dma_op[channel].dst;
    s32 src_modifier = m_dma_op[channel].src_modifier;
    s32 dst_modifier = m_dma_op[channel].dst_modifier;
    s32 src_count = m_dma_op[channel].src_count;
    int pmode = m_dma_op[channel].pmode;

    switch (pmode) {
    case DMA_PMODE_NO_PACKING:
        for (s32 i = 0; i < src_count; i++) {
            m_bus->dm_write32(dst, m_bus->dm_read32(src));
            src += u32(src_modifier); dst += u32(dst_modifier);
        }
        break;
    case DMA_PMODE_8_48: {
        s32 length = src_count / 6;
        for (s32 i = 0; i < length; i++) {
            u64 data = (u64(m_bus->dm_read32(src+0) & 0xff) <<  0) |
                       (u64(m_bus->dm_read32(src+1) & 0xff) <<  8) |
                       (u64(m_bus->dm_read32(src+2) & 0xff) << 16) |
                       (u64(m_bus->dm_read32(src+3) & 0xff) << 24) |
                       (u64(m_bus->dm_read32(src+4) & 0xff) << 32) |
                       (u64(m_bus->dm_read32(src+5) & 0xff) << 40);
            m_bus->pm_write48(dst, data);
            src += u32(src_modifier * 6); dst += u32(dst_modifier);
        }
        break;
    }
    default: break;
    }
    m_dma_status &= ~(1u << channel);
    m_dma_op[channel].active = false;
    m_irptl |= (1 << (channel + 10));
    if (m_imask & (1 << (channel + 10)))
        m_irq_pending |= 1 << (channel + 10);
}

void SHARC::sharc_dma_exec(int channel)
{
    int tran  = (m_dma[channel].control >> 2) & 0x1;
    int dtype = (m_dma[channel].control >> 5) & 0x1;
    int pmode = (m_dma[channel].control >> 6) & 0x3;
    int chen  = (m_dma[channel].control >> 1) & 0x1;

    if (chen) {
        u32 dma_chain_ptr = m_dma[channel].chain_ptr & 0x1ffff;
        schedule_chained_dma_op(channel, dma_chain_ptr, tran);
    } else {
        u32 src, dst;
        u32 src_modifier, dst_modifier, src_count, dst_count;
        if (tran) {
            dst = m_dma[channel].ext_index;
            dst_modifier = m_dma[channel].ext_modifier;
            dst_count = m_dma[channel].ext_count;
            src = (m_dma[channel].int_index & 0x1ffff) | 0x20000;
            src_modifier = m_dma[channel].int_modifier;
            src_count = m_dma[channel].int_count;
        } else {
            src = m_dma[channel].ext_index;
            src_modifier = m_dma[channel].ext_modifier;
            src_count = m_dma[channel].ext_count;
            dst = (m_dma[channel].int_index & 0x1ffff) | 0x20000;
            dst_modifier = m_dma[channel].int_modifier;
            dst_count = m_dma[channel].int_count;
        }
        if (dtype) pmode = DMA_PMODE_8_48;
        schedule_dma_op(channel, src, dst, s32(src_modifier), s32(dst_modifier),
                        s32(src_count), s32(dst_count), pmode);
    }
}

// ============================================================================
// External host interface
// ============================================================================

void SHARC::external_iop_write(u32 address, u32 data)
{
    if ((m_syscon & 0x10) && address != 0x04) {
        if ((m_iop_write_num++ & 1) == 0) {
            m_iop_data = data & 0xffff;
            return;
        } else {
            m_iop_data |= (data & 0xffff) << 16;
        }
    } else {
        m_iop_data = data;
    }

    if (address == 0x1c) {
        m_dma[6].control = m_iop_data;
    } else {
        iop_write(address, m_iop_data);
    }
}

void SHARC::external_dma_write(u32 address, u64 data)
{
    u32 index = (m_dma[6].int_index & 0x1ffff) | 0x20000;
    unsigned pmode = (m_dma[6].control >> 6) & 0x3;
    unsigned mswf  = (m_dma[6].control >> 8) & 0x1;
    unsigned dtype = (m_dma[6].control >> 5) & 0x1;
    (void)dtype;

    switch (pmode) {
    case 0: { // no packing
        m_bus->pm_write32(index, u32(data));
        m_dma[6].int_index += m_dma[6].int_modifier;
        break;
    }
    case 2: { // 16/48 packing
        unsigned word = address % 3;
        unsigned shift = (mswf ? (2 - word) : word) * 16;
        u64 r = m_bus->pm_read48(index);
        r &= ~(u64(0xffff) << shift);
        r |= (data & 0xffff) << shift;
        m_bus->pm_write48(index, r);
        if (word == 2)
            m_dma[6].int_index += m_dma[6].int_modifier;
        break;
    }
    default: break;
    }
}

void SHARC::set_flag_input(int flag_num, int state)
{
    if (flag_num >= 0 && flag_num < 4)
        m_flag[flag_num] = state ? 1 : 0;
}

void SHARC::iop_write(u32 offset, u32 data)
{
    switch (offset) {
    case 0x00: m_syscon = data; break;
    case 0x02: break;
    case 0x1c:
        m_dma[6].control = data;
        if (data & 0x1) sharc_dma_exec(6);
        break;
    case 0x1d:
        m_dma[7].control = data;
        if (data & 0x1) sharc_dma_exec(7);
        break;
    case 0x40: m_dma[6].int_index = data; break;
    case 0x41: m_dma[6].int_modifier = data; break;
    case 0x42: m_dma[6].int_count = data; break;
    case 0x43: m_dma[6].chain_ptr = data; break;
    case 0x44: m_dma[6].gen_purpose = data; break;
    case 0x45: m_dma[6].ext_index = data; break;
    case 0x46: m_dma[6].ext_modifier = data; break;
    case 0x47: m_dma[6].ext_count = data; break;
    case 0x48: m_dma[7].int_index = data; break;
    case 0x49: m_dma[7].int_modifier = data; break;
    case 0x4a: m_dma[7].int_count = data; break;
    case 0x4b: m_dma[7].chain_ptr = data; break;
    case 0x4c: m_dma[7].gen_purpose = data; break;
    case 0x4d: m_dma[7].ext_index = data; break;
    case 0x4e: m_dma[7].ext_modifier = data; break;
    case 0x4f: m_dma[7].ext_count = data; break;
    default: break;
    }
}

// ============================================================================
// ALU / Multiplier / Shifter — compute helpers
// ============================================================================

#define REG(x)  (m_r[x].r)
#define FREG(x) (m_r[x].f)
#define UIREG(x) u32(m_r[x].r)

#define CLEAR_ALU_FLAGS()       do { m_astat &= ~(AZ|AN|AV|AC|AS|AI); } while(0)
#define SET_FLAG_AZ(r)          do { if ((r) == 0) m_astat |= AZ; } while(0)
#define SET_FLAG_AN(r)          do { if ((r) & 0x80000000) m_astat |= AN; } while(0)
#define SET_FLAG_AC_ADD(r,a,b)  do { if (u32(r) < u32(a)) m_astat |= AC; } while(0)
#define SET_FLAG_AC_SUB(r,a,b)  do { if (u32(r) <= u32(a)) m_astat |= AC; } while(0)
#define SET_FLAG_AV_ADD(r,a,b)  do { if (~((a)^(b)) & ((a)^(r)) & 0x80000000) { m_astat |= AV; m_stky |= AOS; } } while(0)
#define SET_FLAG_AV_SUB(r,a,b)  do { if (((a)^(b)) & ((a)^(r)) & 0x80000000) { m_astat |= AV; m_stky |= AOS; } } while(0)
#define CLEAR_MULTIPLIER_FLAGS() do { m_astat &= ~(MN|MV|MU|MI); } while(0)
#define SET_FLAG_SZ(x)          do { if ((x) == 0) m_astat |= SZ; } while(0)

static inline void SATURATE(u32& r) { r = u32((s32(r) >> 31) ^ 0x80000000); }

// -- Float ALU helpers --

SHARC::REG_UNION SHARC::FADD(int fx, int fy) {
    REG_UNION r;
    if (IS_FLOAT_NAN_ADD(UIREG(fx), UIREG(fy))) {
        r.r = s32(FLOAT_CANONICAL_NAN); m_astat |= AI; m_stky |= AIS;
    } else {
        REG_UNION x, y;
        x.r = s32(FLOAT_FLUSH_DENORMAL(UIREG(fx)));
        y.r = s32(FLOAT_FLUSH_DENORMAL(UIREG(fy)));
        r.f = x.f + y.f;
        u32 ru = u32(r.r);
        if (IS_FLOAT_INFINITY(ru)) { m_astat |= AV; m_stky |= AVS; }
        else if (IS_FLOAT_DENORMAL_OR_ZERO(ru)) { m_astat |= AZ; r.r = s32(ru & FLOAT_SIGN_MASK); }
        if (ru & FLOAT_SIGN_MASK) m_astat |= AN;
    }
    m_astat |= AF;
    return r;
}

SHARC::REG_UNION SHARC::FSUB(int fx, int fy) {
    REG_UNION r;
    if (IS_FLOAT_NAN_SUB(UIREG(fx), UIREG(fy))) {
        r.r = s32(FLOAT_CANONICAL_NAN); m_astat |= AI; m_stky |= AIS;
    } else {
        REG_UNION x, y;
        x.r = s32(FLOAT_FLUSH_DENORMAL(UIREG(fx)));
        y.r = s32(FLOAT_FLUSH_DENORMAL(UIREG(fy)));
        r.f = x.f - y.f;
        u32 ru = u32(r.r);
        if (IS_FLOAT_INFINITY(ru)) { m_astat |= AV; m_stky |= AVS; }
        else if (IS_FLOAT_DENORMAL_OR_ZERO(ru)) { m_astat |= AZ; r.r = s32(ru & FLOAT_SIGN_MASK); }
        if (ru & FLOAT_SIGN_MASK) m_astat |= AN;
    }
    m_astat |= AF;
    return r;
}

SHARC::REG_UNION SHARC::FAVG(int fx, int fy) {
    REG_UNION r;
    if (IS_FLOAT_NAN_ADD(UIREG(fx), UIREG(fy))) {
        r.r = s32(FLOAT_CANONICAL_NAN); m_astat |= AI; m_stky |= AIS;
    } else {
        REG_UNION x, y;
        x.r = s32(FLOAT_FLUSH_DENORMAL(UIREG(fx)));
        y.r = s32(FLOAT_FLUSH_DENORMAL(UIREG(fy)));
        r.f = (x.f + y.f) * 0.5f;
        u32 ru = u32(r.r);
        if (IS_FLOAT_INFINITY(ru)) { m_astat |= AV; m_stky |= AVS; }
        else if (IS_FLOAT_DENORMAL_OR_ZERO(ru)) { m_astat |= AZ; r.r = s32(ru & FLOAT_SIGN_MASK); }
        if (ru & FLOAT_SIGN_MASK) m_astat |= AN;
    }
    m_astat |= AF;
    return r;
}

SHARC::REG_UNION SHARC::FABS(int fx) {
    REG_UNION r;
    if (IS_FLOAT_NAN(UIREG(fx))) {
        r.r = s32(FLOAT_CANONICAL_NAN); m_astat |= AI; m_stky |= AIS;
    } else {
        if (IS_FLOAT_DENORMAL_OR_ZERO(UIREG(fx))) { r.r = 0; m_astat |= AZ; }
        else r.r = s32(UIREG(fx) & ~FLOAT_SIGN_MASK);
        if (UIREG(fx) & FLOAT_SIGN_MASK) m_astat |= AS;
    }
    m_astat |= AF;
    return r;
}

SHARC::REG_UNION SHARC::FMIN(int fx, int fy) {
    REG_UNION r;
    if (IS_FLOAT_NAN(UIREG(fx)) || IS_FLOAT_NAN(UIREG(fy))) {
        r.r = s32(FLOAT_CANONICAL_NAN); m_astat |= AI; m_stky |= AIS;
    } else {
        r.f = std::min(FREG(fx), FREG(fy));
        if (r.f < 0.0f) m_astat |= AN;
        if (IS_FLOAT_ZERO(u32(r.r))) m_astat |= AZ;
    }
    m_astat |= AF;
    return r;
}

SHARC::REG_UNION SHARC::FMAX(int fx, int fy) {
    REG_UNION r;
    if (IS_FLOAT_NAN(UIREG(fx)) || IS_FLOAT_NAN(UIREG(fy))) {
        r.r = s32(FLOAT_CANONICAL_NAN); m_astat |= AI; m_stky |= AIS;
    } else {
        r.f = std::max(FREG(fx), FREG(fy));
        if (r.f < 0.0f) m_astat |= AN;
        if (IS_FLOAT_ZERO(u32(r.r))) m_astat |= AZ;
    }
    m_astat |= AF;
    return r;
}

std::pair<SHARC::REG_UNION, SHARC::REG_UNION> SHARC::FADD_FSUB(int fx, int fy) {
    std::pair<REG_UNION, REG_UNION> r;
    REG_UNION x, y;
    x.r = s32(FLOAT_FLUSH_DENORMAL(UIREG(fx)));
    y.r = s32(FLOAT_FLUSH_DENORMAL(UIREG(fy)));
    r.first.f = x.f + y.f;
    r.second.f = x.f - y.f;
    m_astat |= AF;
    return r;
}

u32 SHARC::SCALB(REG_UNION fx, int ry) {
    u32 mantissa = u32(fx.r) & FLOAT_MANTISSA_MASK;
    u32 sign = u32(fx.r) & FLOAT_SIGN_MASK;
    int exponent = float_get_unbiased_exponent(u32(fx.r)) + s32(REG(ry));
    if (exponent > 127) { m_astat |= AV; return sign | FLOAT_INFINITY; }
    if (exponent < -126) { m_astat |= AZ; return sign; }
    return sign | float_make_biased_exponent(exponent) | mantissa;
}

SHARC::REG_UNION SHARC::FMUL(int fx, int fy) {
    REG_UNION r;
    if (IS_FLOAT_NAN_MUL(UIREG(fx), UIREG(fy))) {
        r.r = s32(FLOAT_CANONICAL_NAN); m_astat |= MI; m_stky |= MIS;
    } else {
        r.f = FREG(fx) * FREG(fy);
        u32 ru = u32(r.r);
        if (ru & FLOAT_SIGN_MASK) m_astat |= MN;
        if (IS_FLOAT_INFINITY(ru)) { m_astat |= MV; m_stky |= MVS; }
        if (IS_FLOAT_DENORMAL(ru)) { m_astat |= MU; m_stky |= MUS; }
    }
    return r;
}

// -- Integer ALU operations --

void SHARC::compute_add(int rn, int rx, int ry) {
    u32 r = u32(REG(rx)) + u32(REG(ry));
    CLEAR_ALU_FLAGS(); SET_FLAG_AV_ADD(r, REG(rx), REG(ry)); SET_FLAG_AC_ADD(r, REG(rx), REG(ry));
    if ((m_mode1 & MODE1_ALUSAT) && (m_astat & AV)) SATURATE(r);
    SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_sub(int rn, int rx, int ry) {
    u32 r = u32(REG(rx)) - u32(REG(ry));
    CLEAR_ALU_FLAGS(); SET_FLAG_AV_SUB(r, REG(rx), REG(ry)); SET_FLAG_AC_SUB(r, REG(rx), REG(ry));
    if ((m_mode1 & MODE1_ALUSAT) && (m_astat & AV)) SATURATE(r);
    SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_add_ci(int rn, int rx, int ry) {
    int c = (m_astat & AC) ? 1 : 0;
    u32 r = u32(REG(rx)) + u32(REG(ry)) + u32(c);
    CLEAR_ALU_FLAGS(); SET_FLAG_AV_ADD(r, REG(rx), REG(ry)); SET_FLAG_AC_ADD(r, REG(rx), REG(ry));
    if (c == 1 && u32(REG(ry)) == 0xffffffff) m_astat |= AC;
    if ((m_mode1 & MODE1_ALUSAT) && (m_astat & AV)) SATURATE(r);
    SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_sub_ci(int rn, int rx, int ry) {
    int c = (m_astat & AC) ? 1 : 0;
    u32 r = u32(REG(rx)) - u32(REG(ry)) + u32(c) - 1;
    CLEAR_ALU_FLAGS(); SET_FLAG_AV_SUB(r, REG(rx), REG(ry));
    if (c != 0 || u32(REG(ry)) != 0xffffffff) SET_FLAG_AC_SUB(r, REG(rx), REG(ry));
    if ((m_mode1 & MODE1_ALUSAT) && (m_astat & AV)) SATURATE(r);
    SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_comp(int rx, int ry) {
    CLEAR_ALU_FLAGS();
    if (u32(REG(rx)) == u32(REG(ry))) m_astat |= AZ;
    else if (s32(REG(rx)) < s32(REG(ry))) m_astat |= AN;
    u32 comp_accum = (m_astat >> 1) & 0x7f000000;
    if ((m_astat & (AZ|AN)) == 0) comp_accum |= 0x80000000;
    m_astat = (m_astat & 0x00ffffff) | comp_accum;
    m_astat &= ~AF;
}
void SHARC::compute_add_ci(int rn, int rx) {
    int c = (m_astat & AC) ? 1 : 0;
    u32 r = u32(REG(rx)) + u32(c);
    CLEAR_ALU_FLAGS(); SET_FLAG_AV_ADD(r, REG(rx), 0); SET_FLAG_AC_ADD(r, REG(rx), 0);
    if ((m_mode1 & MODE1_ALUSAT) && (m_astat & AV)) SATURATE(r);
    SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_sub_ci(int rn, int rx) {
    int c = (m_astat & AC) ? 1 : 0;
    u32 r = u32(REG(rx)) + u32(c) - 1;
    CLEAR_ALU_FLAGS(); SET_FLAG_AV_SUB(r, REG(rx), 0); SET_FLAG_AC_SUB(r, REG(rx), 0);
    if ((m_mode1 & MODE1_ALUSAT) && (m_astat & AV)) SATURATE(r);
    SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_inc(int rn, int rx) {
    u32 r = u32(REG(rx)) + 1;
    CLEAR_ALU_FLAGS(); SET_FLAG_AV_ADD(r, REG(rx), 1); SET_FLAG_AC_ADD(r, REG(rx), 1);
    if ((m_mode1 & MODE1_ALUSAT) && (m_astat & AV)) SATURATE(r);
    SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_dec(int rn, int rx) {
    u32 r = u32(REG(rx)) - 1;
    CLEAR_ALU_FLAGS(); SET_FLAG_AV_SUB(r, REG(rx), 1); SET_FLAG_AC_SUB(r, REG(rx), 1);
    if ((m_mode1 & MODE1_ALUSAT) && (m_astat & AV)) SATURATE(r);
    SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_neg(int rn, int rx) {
    u32 r = u32(-s32(REG(rx)));
    CLEAR_ALU_FLAGS(); SET_FLAG_AV_SUB(r, 0, REG(rx)); SET_FLAG_AC_SUB(r, 0, REG(rx));
    if ((m_mode1 & MODE1_ALUSAT) && (m_astat & AV)) SATURATE(r);
    SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_abs(int rn, int rx) {
    u32 r = u32(std::abs(s32(REG(rx))));
    CLEAR_ALU_FLAGS();
    if (s32(r) < 0) { m_astat |= AV; m_stky |= AOS; if (m_mode1 & MODE1_ALUSAT) r = 0x7fffffff; }
    SET_FLAG_AN(r); SET_FLAG_AZ(r);
    if (s32(REG(rx)) < 0) m_astat |= AS;
    REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_pass(int rn, int rx) {
    CLEAR_ALU_FLAGS(); REG(rn) = REG(rx);
    if (REG(rn) == 0) m_astat |= AZ;
    if (u32(REG(rn)) & 0x80000000) m_astat |= AN;
    m_astat &= ~AF;
}
void SHARC::compute_and(int rn, int rx, int ry) {
    u32 r = UIREG(rx) & UIREG(ry);
    CLEAR_ALU_FLAGS(); SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_or(int rn, int rx, int ry) {
    u32 r = UIREG(rx) | UIREG(ry);
    CLEAR_ALU_FLAGS(); SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_xor(int rn, int rx, int ry) {
    u32 r = UIREG(rx) ^ UIREG(ry);
    CLEAR_ALU_FLAGS(); SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_not(int rn, int rx) {
    u32 r = ~UIREG(rx);
    CLEAR_ALU_FLAGS(); SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_min(int rn, int rx, int ry) {
    u32 r = u32(std::min(s32(REG(rx)), s32(REG(ry))));
    CLEAR_ALU_FLAGS(); SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_max(int rn, int rx, int ry) {
    u32 r = u32(std::max(s32(REG(rx)), s32(REG(ry))));
    CLEAR_ALU_FLAGS(); SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}
void SHARC::compute_clip(int rn, int rx, int ry) {
    s32 absry = std::abs(s32(REG(ry)));
    u32 r = u32(std::clamp(s32(REG(rx)), -absry, absry));
    CLEAR_ALU_FLAGS(); SET_FLAG_AN(r); SET_FLAG_AZ(r); REG(rn) = s32(r); m_astat &= ~AF;
}

// -- Floating-point ALU compute wrappers --

void SHARC::compute_fadd(int fn, int fx, int fy) { CLEAR_ALU_FLAGS(); FREG(fn) = FADD(fx, fy).f; }
void SHARC::compute_fsub(int fn, int fx, int fy) { CLEAR_ALU_FLAGS(); FREG(fn) = FSUB(fx, fy).f; }
void SHARC::compute_favg(int fn, int fx, int fy) { CLEAR_ALU_FLAGS(); FREG(fn) = FAVG(fx, fy).f; }
void SHARC::compute_fcomp(int fx, int fy) {
    CLEAR_ALU_FLAGS();
    if (FREG(fx) == FREG(fy)) m_astat |= AZ;
    if (FREG(fx) < FREG(fy)) m_astat |= AN;
    m_astat |= AF;
}
void SHARC::compute_fneg(int fn, int fx) {
    CLEAR_ALU_FLAGS(); FREG(fn) = -FREG(fx);
    if (FREG(fn) == 0.0f) m_astat |= AZ;
    if (u32(m_r[fn].r) & FLOAT_SIGN_MASK) m_astat |= AN;
    m_astat |= AF;
}
void SHARC::compute_fabs(int fn, int fx) { CLEAR_ALU_FLAGS(); m_r[fn] = FABS(fx); }
void SHARC::compute_fpass(int fn, int fx) {
    CLEAR_ALU_FLAGS();
    FREG(fn) = FREG(fx);
    if (IS_FLOAT_ZERO(UIREG(fn))) m_astat |= AZ;
    if (UIREG(fn) & FLOAT_SIGN_MASK) m_astat |= AN;
    m_astat |= AF;
}
void SHARC::compute_scalb(int fn, int fx, int ry) {
    CLEAR_ALU_FLAGS();
    REG(fn) = s32(SCALB(m_r[fx], ry));
    m_astat |= AF;
}
void SHARC::compute_logb(int rn, int fx) {
    CLEAR_ALU_FLAGS();
    int exp = float_get_unbiased_exponent(UIREG(fx));
    REG(rn) = exp;
    if (exp == 0) m_astat |= AZ;
    if (exp < 0) m_astat |= AN;
    m_astat |= AF;
}
void SHARC::compute_fix(int rn, int fx) {
    CLEAR_ALU_FLAGS();
    REG(rn) = s32(FREG(fx));
    if (REG(rn) == 0) m_astat |= AZ;
    if (REG(rn) < 0) m_astat |= AN;
    m_astat |= AF;
}
void SHARC::compute_float(int fn, int rx) {
    CLEAR_ALU_FLAGS();
    FREG(fn) = float(REG(rx));
    if (FREG(fn) == 0.0f) m_astat |= AZ;
    if (FREG(fn) < 0.0f) m_astat |= AN;
    m_astat |= AF;
}
void SHARC::compute_fix_scaled(int rn, int fx, int ry) {
    CLEAR_ALU_FLAGS();
    float scale = std::ldexp(1.0f, s32(REG(ry)));
    REG(rn) = s32(FREG(fx) * scale);
    if (REG(rn) == 0) m_astat |= AZ;
    if (REG(rn) < 0) m_astat |= AN;
    m_astat |= AF;
}
void SHARC::compute_float_scaled(int fn, int rx, int ry) {
    CLEAR_ALU_FLAGS();
    float scale = std::ldexp(1.0f, s32(REG(ry)));
    FREG(fn) = float(REG(rx)) * scale;
    if (FREG(fn) == 0.0f) m_astat |= AZ;
    if (FREG(fn) < 0.0f) m_astat |= AN;
    m_astat |= AF;
}
void SHARC::compute_recips(int fn, int fx) {
    CLEAR_ALU_FLAGS();
    u32 val = UIREG(fx);
    if (IS_FLOAT_NAN(val) || IS_FLOAT_ZERO(val)) { REG(fn) = s32(FLOAT_CANONICAL_NAN); m_astat |= AI; m_stky |= AIS; }
    else {
        int exp = -(float_get_unbiased_exponent(val) + 1);
        u32 mantissa = s_recips_mantissa_lookup[(val >> 16) & 0x7f];
        REG(fn) = s32((val & FLOAT_SIGN_MASK) | float_make_biased_exponent(exp) | mantissa);
    }
    m_astat |= AF;
}
void SHARC::compute_rsqrts(int fn, int fx) {
    CLEAR_ALU_FLAGS();
    u32 val = UIREG(fx);
    if (IS_FLOAT_NAN(val) || IS_FLOAT_ZERO(val) || (val & FLOAT_SIGN_MASK)) {
        REG(fn) = s32(FLOAT_CANONICAL_NAN); m_astat |= AI; m_stky |= AIS;
    } else {
        int exp = float_get_unbiased_exponent(val);
        int idx = ((val >> 16) & 0x3f) | ((exp & 1) ? 0x40 : 0);
        exp = -(exp / 2) - 1;
        u32 mantissa = s_rsqrts_mantissa_lookup[idx];
        REG(fn) = s32(float_make_biased_exponent(exp) | mantissa);
    }
    m_astat |= AF;
}
void SHARC::compute_fcopysign(int fn, int fx, int fy) {
    CLEAR_ALU_FLAGS();
    REG(fn) = s32((UIREG(fx) & ~FLOAT_SIGN_MASK) | (UIREG(fy) & FLOAT_SIGN_MASK));
    m_astat |= AF;
}
void SHARC::compute_fmin(int fn, int fx, int fy) { CLEAR_ALU_FLAGS(); m_r[fn] = FMIN(fx, fy); }
void SHARC::compute_fmax(int fn, int fx, int fy) { CLEAR_ALU_FLAGS(); m_r[fn] = FMAX(fx, fy); }
void SHARC::compute_fclip(int fn, int fx, int fy) {
    CLEAR_ALU_FLAGS();
    float absfy = std::fabs(FREG(fy));
    FREG(fn) = std::clamp(FREG(fx), -absfy, absfy);
    m_astat |= AF;
}
void SHARC::compute_fadd_abs(int fn, int fx, int fy) {
    CLEAR_ALU_FLAGS();
    REG_UNION a; a.r = s32(UIREG(fx) & ~FLOAT_SIGN_MASK);
    REG_UNION b; b.r = s32(UIREG(fy) & ~FLOAT_SIGN_MASK);
    // Swap in abs values, do add
    s32 save_x = REG(fx), save_y = REG(fy);
    REG(fx) = a.r; REG(fy) = b.r;
    FREG(fn) = FADD(fx, fy).f;
    REG(fx) = save_x; REG(fy) = save_y;
}
void SHARC::compute_fsub_abs(int fn, int fx, int fy) {
    CLEAR_ALU_FLAGS();
    s32 save_x = REG(fx), save_y = REG(fy);
    REG(fx) = s32(UIREG(fx) & ~FLOAT_SIGN_MASK);
    REG(fy) = s32(UIREG(fy) & ~FLOAT_SIGN_MASK);
    FREG(fn) = FSUB(fx, fy).f;
    REG(fx) = save_x; REG(fy) = save_y;
}

// -- Multiplier operations --

void SHARC::compute_mul_uuin(int rn, int rx, int ry) {
    CLEAR_MULTIPLIER_FLAGS();
    u64 r = u64(UIREG(rx)) * u64(UIREG(ry));
    REG(rn) = s32(u32(r));
    if (u32(r >> 32) != 0) { m_astat |= MV; m_stky |= MOS; }
    if (u32(r) & 0x80000000) m_astat |= MN;
}
void SHARC::compute_mul_ssin(int rn, int rx, int ry) {
    CLEAR_MULTIPLIER_FLAGS();
    s64 r = s64(s32(REG(rx))) * s64(s32(REG(ry)));
    REG(rn) = s32(u32(r));
    u32 hi = u32(u64(r) >> 32);
    if (hi != 0 && hi != 0xffffffff) { m_astat |= MV; m_stky |= MOS; }
    if (u32(r) & 0x80000000) m_astat |= MN;
}
u32 SHARC::compute_mrf_plus_mul_ssin(int rx, int ry) {
    CLEAR_MULTIPLIER_FLAGS();
    s64 r = s64(s32(REG(rx))) * s64(s32(REG(ry)));
    m_mrf = u64(s64(m_mrf) + r);
    return u32(m_mrf);
}
u32 SHARC::compute_mrb_plus_mul_ssin(int rx, int ry) {
    CLEAR_MULTIPLIER_FLAGS();
    s64 r = s64(s32(REG(rx))) * s64(s32(REG(ry)));
    m_mrb = u64(s64(m_mrb) + r);
    return u32(m_mrb);
}
void SHARC::compute_fmul(int fn, int fx, int fy) {
    CLEAR_MULTIPLIER_FLAGS();
    m_r[fn] = FMUL(fx, fy);
}

// -- Dual add/subtract --

void SHARC::compute_dual_add_sub(int ra, int rs, int rx, int ry) {
    u32 a = u32(REG(rx)) + u32(REG(ry));
    u32 s = u32(REG(rx)) - u32(REG(ry));
    CLEAR_ALU_FLAGS();
    SET_FLAG_AN(a); SET_FLAG_AZ(a);
    REG(ra) = s32(a); REG(rs) = s32(s);
    m_astat &= ~AF;
}
void SHARC::compute_dual_fadd_fsub(int fa, int fs, int fx, int fy) {
    CLEAR_ALU_FLAGS();
    auto [add_r, sub_r] = FADD_FSUB(fx, fy);
    m_r[fa] = add_r;
    m_r[fs] = sub_r;
}

// -- Multi-function (multiply + ALU) --

void SHARC::compute_mul_ssfr_add(int rm, int rxm, int rym, int ra, int rxa, int rya) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    s64 mul_r = s64(s32(REG(rxm))) * s64(s32(REG(rym)));
    REG(rm) = s32(u32(u64(mul_r) >> 31));
    u32 add = u32(REG(rxa)) + u32(REG(rya));
    REG(ra) = s32(add);
    m_astat &= ~AF;
}
void SHARC::compute_mul_ssfr_sub(int rm, int rxm, int rym, int ra, int rxa, int rya) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    s64 mul_r = s64(s32(REG(rxm))) * s64(s32(REG(rym)));
    REG(rm) = s32(u32(u64(mul_r) >> 31));
    u32 sub = u32(REG(rxa)) - u32(REG(rya));
    REG(ra) = s32(sub);
    m_astat &= ~AF;
}
void SHARC::compute_fmul_fadd(int fm, int fxm, int fym, int fa, int fxa, int fya) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    m_r[fm] = FMUL(fxm, fym);
    FREG(fa) = FADD(fxa, fya).f;
}
void SHARC::compute_fmul_fsub(int fm, int fxm, int fym, int fa, int fxa, int fya) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    m_r[fm] = FMUL(fxm, fym);
    FREG(fa) = FSUB(fxa, fya).f;
}
void SHARC::compute_fmul_float_scaled(int fm, int fxm, int fym, int fa, int rxa, int rya) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    m_r[fm] = FMUL(fxm, fym);
    compute_float_scaled(fa, rxa, rya);
}
void SHARC::compute_fmul_fix_scaled(int fm, int fxm, int fym, int ra, int fxa, int rya) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    m_r[fm] = FMUL(fxm, fym);
    compute_fix_scaled(ra, fxa, rya);
}
void SHARC::compute_fmul_favg(int fm, int fxm, int fym, int fa, int fxa, int fya) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    m_r[fm] = FMUL(fxm, fym);
    FREG(fa) = FAVG(fxa, fya).f;
}
void SHARC::compute_fmul_fabs(int fm, int fxm, int fym, int fa, int fxa) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    m_r[fm] = FMUL(fxm, fym);
    m_r[fa] = FABS(fxa);
}
void SHARC::compute_fmul_fmax(int fm, int fxm, int fym, int fa, int fxa, int fya) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    m_r[fm] = FMUL(fxm, fym);
    m_r[fa] = FMAX(fxa, fya);
}
void SHARC::compute_fmul_fmin(int fm, int fxm, int fym, int fa, int fxa, int fya) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    m_r[fm] = FMUL(fxm, fym);
    m_r[fa] = FMIN(fxa, fya);
}
void SHARC::compute_fmul_dual_fadd_fsub(int fm, int fxm, int fym, int fa, int fs, int fxa, int fya) {
    CLEAR_MULTIPLIER_FLAGS(); CLEAR_ALU_FLAGS();
    m_r[fm] = FMUL(fxm, fym);
    auto [add_r, sub_r] = FADD_FSUB(fxa, fya);
    m_r[fa] = add_r; m_r[fs] = sub_r;
}
void SHARC::compute_multi_mr_to_reg(int ai, int rk) {
    if (ai == 0) REG(rk) = s32(u32(m_mrf));
    else         REG(rk) = s32(u32(m_mrb));
}
void SHARC::compute_multi_reg_to_mr(int ai, int rk) {
    if (ai == 0) m_mrf = u64(s64(s32(REG(rk))));
    else         m_mrb = u64(s64(s32(REG(rk))));
}

// ============================================================================
// Shift operation (immediate)
// ============================================================================

void SHARC::SHIFT_OPERATION_IMM(int shiftop, int data, int rn, int rx)
{
    s8 shift = s8(data & 0xff);
    int bit = data & 0x3f;
    int len = (data >> 6) & 0x3f;
    m_astat &= ~(SZ|SV|SS);

    switch (shiftop) {
    case 0x00: // LSHIFT Rx BY <data8>
        if (shift < 0) REG(rn) = s32((shift > -32) ? (UIREG(rx) >> (-shift)) : 0);
        else { REG(rn) = s32((shift < 32) ? (UIREG(rx) << shift) : 0); if (shift > 0) m_astat |= SV; }
        SET_FLAG_SZ(UIREG(rn));
        break;
    case 0x01: // ASHIFT Rx BY <data8>
        if (shift < 0) REG(rn) = s32(REG(rx)) >> ((shift > -32) ? (-shift) : 31);
        else { REG(rn) = (shift < 32) ? (s32(REG(rx)) << shift) : 0; if (shift > 0) m_astat |= SV; }
        SET_FLAG_SZ(UIREG(rn));
        break;
    case 0x02: // ROT Rx BY <data8>
        REG(rn) = s32(std::rotl(UIREG(rx), int(shift)));
        SET_FLAG_SZ(UIREG(rn));
        break;
    case 0x08: { // Rn = Rn OR LSHIFT Rx BY <data8>
        u32 r = 0;
        if (shift < 0) r = (shift > -32) ? (UIREG(rx) >> (-shift)) : 0;
        else { r = (shift < 32) ? (UIREG(rx) << shift) : 0; if (shift > 0) m_astat |= SV; }
        SET_FLAG_SZ(r);
        REG(rn) = s32(UIREG(rn) | r);
        break;
    }
    case 0x10: // FEXT Rx BY <bit6>:<len6>
        if (len == 0 || bit >= 32) REG(rn) = 0;
        else REG(rn) = s32((UIREG(rx) >> bit) & ((1u << std::min(len, 32)) - 1u));
        SET_FLAG_SZ(UIREG(rn));
        if (bit + len > 32) m_astat |= SV;
        break;
    case 0x11: // FDEP Rx BY <bit6>:<len6>
        if (len == 0 || bit >= 32) REG(rn) = 0;
        else REG(rn) = s32((UIREG(rx) & ((1u << std::min(len, 32)) - 1u)) << bit);
        SET_FLAG_SZ(UIREG(rn));
        if (bit + len > 32) m_astat |= SV;
        break;
    case 0x12: // FEXT Rx BY <bit6>:<len6> (Sign Extended)
        if (len == 0 || bit >= 32) REG(rn) = 0;
        else if (bit + len > 32) REG(rn) = s32(UIREG(rx) >> bit);
        else REG(rn) = sext(UIREG(rx) >> bit, std::min(len, 32));
        SET_FLAG_SZ(UIREG(rn));
        if (bit + len > 32) m_astat |= SV;
        break;
    case 0x13: // FDEP Rx (SE)
        if (len == 0 || bit >= 32) REG(rn) = 0;
        else REG(rn) = sext(UIREG(rx), std::min(len, 32)) << bit;
        SET_FLAG_SZ(UIREG(rn));
        if (bit + len > 32) m_astat |= SV;
        break;
    case 0x19: // Rn = Rn OR FDEP Rx BY <bit6>:<len6>
        if (len != 0 && bit < 32)
            REG(rn) = s32(UIREG(rn) | ((UIREG(rx) & ((1u << std::min(len, 32)) - 1u)) << bit));
        SET_FLAG_SZ(UIREG(rn));
        if (bit + len > 32) m_astat |= SV;
        break;
    case 0x30: // BSET
        REG(rn) = REG(rx);
        if (data >= 0 && data < 32) REG(rn) = s32(UIREG(rn) | (1u << data));
        else m_astat |= SV;
        SET_FLAG_SZ(UIREG(rn));
        break;
    case 0x31: // BCLR
        REG(rn) = REG(rx);
        if (data >= 0 && data < 32) REG(rn) = s32(UIREG(rn) & ~(1u << data));
        else m_astat |= SV;
        SET_FLAG_SZ(UIREG(rn));
        break;
    case 0x32: // BTGL
        REG(rn) = REG(rx);
        if (data >= 0 && data < 32) REG(rn) = s32(UIREG(rn) ^ (1u << data));
        else m_astat |= SV;
        SET_FLAG_SZ(UIREG(rn));
        break;
    case 0x33: // BTST
        if (data >= 0 && data < 32) { u32 r = UIREG(rx) & (1u << data); SET_FLAG_SZ(r); }
        else m_astat |= SZ | SV;
        break;
    default: break;
    }
}

// ============================================================================
// COMPUTE dispatcher
// ============================================================================

void SHARC::COMPUTE(u32 opcode)
{
    int op = (opcode >> 12) & 0xff;
    int cu = (opcode >> 20) & 0x3;
    int rn = (opcode >> 8) & 0xf;
    int rx = (opcode >> 4) & 0xf;
    int ry = (opcode >> 0) & 0xf;

    if (opcode & 0x400000) { // Multi-function
        int fm  = (opcode >> 12) & 0xf;
        int fa  = (opcode >> 8) & 0xf;
        int fxm = (opcode >> 6) & 0x3;
        int fym = ((opcode >> 4) & 0x3) + 4;
        int fxa = ((opcode >> 2) & 0x3) + 8;
        int fya = (opcode & 0x3) + 12;
        int multiop = (opcode >> 16) & 0x3f;

        switch (multiop) {
        case 0x00: compute_multi_mr_to_reg(op & 0xf, rn); break;
        case 0x01: compute_multi_reg_to_mr(op & 0xf, rn); break;
        case 0x04: compute_mul_ssfr_add(fm, fxm, fym, fa, fxa, fya); break;
        case 0x05: compute_mul_ssfr_sub(fm, fxm, fym, fa, fxa, fya); break;
        case 0x18: compute_fmul_fadd(fm, fxm, fym, fa, fxa, fya); break;
        case 0x19: compute_fmul_fsub(fm, fxm, fym, fa, fxa, fya); break;
        case 0x1a: compute_fmul_float_scaled(fm, fxm, fym, fa, fxa, fya); break;
        case 0x1b: compute_fmul_fix_scaled(fm, fxm, fym, fa, fxa, fya); break;
        case 0x1c: compute_fmul_favg(fm, fxm, fym, fa, fxa, fya); break;
        case 0x1d: compute_fmul_fabs(fm, fxm, fym, fa, fxa); break;
        case 0x1e: compute_fmul_fmax(fm, fxm, fym, fa, fxa, fya); break;
        case 0x1f: compute_fmul_fmin(fm, fxm, fym, fa, fxa, fya); break;
        default:
            if (multiop >= 0x30) {
                int fs = (opcode >> 16) & 0xf;
                compute_fmul_dual_fadd_fsub(fm, fxm, fym, fa, fs, fxa, fya);
            }
            break;
        }
    } else { // Single-function
        switch (cu) {
        case 0: // ALU
            switch (op) {
            case 0x01: compute_add(rn, rx, ry); break;
            case 0x02: compute_sub(rn, rx, ry); break;
            case 0x05: compute_add_ci(rn, rx, ry); break;
            case 0x06: compute_sub_ci(rn, rx, ry); break;
            case 0x0a: compute_comp(rx, ry); break;
            case 0x21: compute_pass(rn, rx); break;
            case 0x22: compute_neg(rn, rx); break;
            case 0x25: compute_add_ci(rn, rx); break;
            case 0x26: compute_sub_ci(rn, rx); break;
            case 0x29: compute_inc(rn, rx); break;
            case 0x2a: compute_dec(rn, rx); break;
            case 0x30: compute_abs(rn, rx); break;
            case 0x40: compute_and(rn, rx, ry); break;
            case 0x41: compute_or(rn, rx, ry); break;
            case 0x42: compute_xor(rn, rx, ry); break;
            case 0x43: compute_not(rn, rx); break;
            case 0x61: compute_min(rn, rx, ry); break;
            case 0x62: compute_max(rn, rx, ry); break;
            case 0x63: compute_clip(rn, rx, ry); break;
            case 0x81: compute_fadd(rn, rx, ry); break;
            case 0x82: compute_fsub(rn, rx, ry); break;
            case 0x89: compute_favg(rn, rx, ry); break;
            case 0x8a: compute_fcomp(rx, ry); break;
            case 0x91: compute_fadd_abs(rn, rx, ry); break;
            case 0x92: compute_fsub_abs(rn, rx, ry); break;
            case 0xa1: compute_fpass(rn, rx); break;
            case 0xa2: compute_fneg(rn, rx); break;
            case 0xb0: compute_fabs(rn, rx); break;
            case 0xbd: compute_scalb(rn, rx, ry); break;
            case 0xc1: compute_logb(rn, rx); break;
            case 0xc4: compute_recips(rn, rx); break;
            case 0xc5: compute_rsqrts(rn, rx); break;
            case 0xc9: compute_fix(rn, rx); break;
            case 0xca: compute_float(rn, rx); break;
            case 0xd9: compute_fix_scaled(rn, rx, ry); break;
            case 0xda: compute_float_scaled(rn, rx, ry); break;
            case 0xe0: compute_fcopysign(rn, rx, ry); break;
            case 0xe1: compute_fmin(rn, rx, ry); break;
            case 0xe2: compute_fmax(rn, rx, ry); break;
            case 0xe3: compute_fclip(rn, rx, ry); break;
            default:
                if (op >= 0x70 && op <= 0x7f) { compute_dual_add_sub(op_get_ra(opcode), op_get_rs(opcode), rx, ry); }
                else if (op >= 0xf0) { compute_dual_fadd_fsub(op_get_ra(opcode), op_get_rs(opcode), rx, ry); }
                break;
            }
            break;
        case 1: // Multiplier
            switch (op) {
            case 0x14: m_mrf = 0; break;
            case 0x16: m_mrb = 0; break;
            case 0x30: compute_fmul(rn, rx, ry); break;
            case 0x40: compute_mul_uuin(rn, rx, ry); break;
            case 0x70: compute_mul_ssin(rn, rx, ry); break;
            case 0xb0: REG(rn) = s32(compute_mrf_plus_mul_ssin(rx, ry)); break;
            case 0xb2: REG(rn) = s32(compute_mrb_plus_mul_ssin(rx, ry)); break;
            default: break;
            }
            break;
        case 2: { // Shifter
            m_astat &= ~(SZ|SV|SS);
            int shift_op = op >> 2;
            switch (shift_op) {
            case 0x00: { // LSHIFT Rx BY Ry
                s32 shift = REG(ry);
                if (shift < 0) REG(rn) = s32((shift > -32) ? (UIREG(rx) >> (-shift)) : 0);
                else { REG(rn) = s32((shift < 32) ? (UIREG(rx) << shift) : 0); if (shift > 0) m_astat |= SV; }
                SET_FLAG_SZ(UIREG(rn));
                break;
            }
            case 0x02: { // ROT
                s32 shift = REG(ry);
                REG(rn) = s32(std::rotl(UIREG(rx), int(shift)));
                SET_FLAG_SZ(UIREG(rn));
                break;
            }
            case 0x08: { // Rn = Rn OR LSHIFT Rx BY Ry
                s8 shift = s8(REG(ry));
                u32 r;
                if (shift < 0) r = (shift > -32) ? (UIREG(rx) >> (-shift)) : 0;
                else { r = (shift < 32) ? (UIREG(rx) << shift) : 0; if (shift > 0) m_astat |= SV; }
                SET_FLAG_SZ(r);
                REG(rn) = s32(UIREG(rn) | r);
                break;
            }
            case 0x30: { // BSET Rx BY Ry
                u32 sh = UIREG(ry);
                REG(rn) = REG(rx);
                if (sh < 32) REG(rn) = s32(UIREG(rn) | (1u << sh));
                else m_astat |= SV;
                SET_FLAG_SZ(UIREG(rn));
                break;
            }
            case 0x31: { // BCLR Rx BY Ry
                u32 sh = UIREG(ry);
                REG(rn) = REG(rx);
                if (sh < 32) REG(rn) = s32(UIREG(rn) & ~(1u << sh));
                else m_astat |= SV;
                SET_FLAG_SZ(UIREG(rn));
                break;
            }
            case 0x33: { // BTST Rx BY Ry
                u32 sh = UIREG(ry);
                if (sh < 32) { u32 r = UIREG(rx) & (1u << sh); SET_FLAG_SZ(r); }
                else m_astat |= SZ | SV;
                break;
            }
            default: break;
            }
            break;
        }
        default: break;
        }
    }
}

// ============================================================================
// Opcode handlers
// ============================================================================

void SHARC::sharcop_compute_dreg_dm_dreg_pm()
{
    int pm_dreg = (m_opcode >> 23) & 0xf;
    int pmm = op_get_pmm(m_opcode);
    int pmi = op_get_pmi(m_opcode);
    int dm_dreg = (m_opcode >> 33) & 0xf;
    int dmm = op_get_dmm(m_opcode);
    int dmi = op_get_dmi(m_opcode);
    int pmd = (m_opcode >> 37) & 0x1;
    int dmd = (m_opcode >> 44) & 0x1;
    int compute = op_get_compute(m_opcode);

    u32 parallel_pm_dreg = UIREG(pm_dreg);
    u32 parallel_dm_dreg = UIREG(dm_dreg);

    if (compute) COMPUTE(compute);

    if (pmd) { m_bus->pm_write32(m_dag2.i[pmi], parallel_pm_dreg); }
    else     { REG(pm_dreg) = s32(m_bus->pm_read32(m_dag2.i[pmi])); }
    m_dag2.i[pmi] += m_dag2.m[pmm];
    update_circular_buffer_pm(pmi);

    if (dmd) { m_bus->dm_write32(m_dag1.i[dmi], parallel_dm_dreg); }
    else     { REG(dm_dreg) = s32(m_bus->dm_read32(m_dag1.i[dmi])); }
    m_dag1.i[dmi] += m_dag1.m[dmm];
    update_circular_buffer_dm(dmi);
}

void SHARC::sharcop_compute()
{
    int cond = op_get_cond(m_opcode);
    int compute = op_get_compute(m_opcode);
    if (IF_CONDITION_CODE(cond) && compute)
        COMPUTE(compute);
}

void SHARC::sharcop_compute_ureg_dmpm_premod()
{
    int i = (m_opcode >> 41) & 0x7;
    int m = (m_opcode >> 38) & 0x7;
    int cond = op_get_cond(m_opcode);
    int g = (m_opcode >> 32) & 0x1;
    int d = (m_opcode >> 31) & 0x1;
    int ureg = (m_opcode >> 23) & 0xff;
    int compute = op_get_compute(m_opcode);

    if (IF_CONDITION_CODE(cond)) {
        u32 parallel_ureg = GET_UREG(ureg);
        if (compute) COMPUTE(compute);
        if (g) {
            if (d) { if (ureg == 0xdb) m_bus->pm_write48(m_dag2.i[i]+m_dag2.m[m], m_px); else m_bus->pm_write32(m_dag2.i[i]+m_dag2.m[m], parallel_ureg); }
            else   { if (ureg == 0xdb) m_px = m_bus->pm_read48(m_dag2.i[i]+m_dag2.m[m]); else SET_UREG(ureg, m_bus->pm_read32(m_dag2.i[i]+m_dag2.m[m])); }
        } else {
            if (d) m_bus->dm_write32(m_dag1.i[i]+m_dag1.m[m], parallel_ureg);
            else   SET_UREG(ureg, m_bus->dm_read32(m_dag1.i[i]+m_dag1.m[m]));
        }
    }
}

void SHARC::sharcop_compute_ureg_dmpm_postmod()
{
    int i = (m_opcode >> 41) & 0x7;
    int m = (m_opcode >> 38) & 0x7;
    int cond = op_get_cond(m_opcode);
    int g = (m_opcode >> 32) & 0x1;
    int d = (m_opcode >> 31) & 0x1;
    int ureg = (m_opcode >> 23) & 0xff;
    int compute = op_get_compute(m_opcode);

    if (IF_CONDITION_CODE(cond)) {
        u32 parallel_ureg = GET_UREG(ureg);
        if (compute) COMPUTE(compute);
        if (g) {
            if (d) { if (ureg == 0xdb) m_bus->pm_write48(m_dag2.i[i], m_px); else m_bus->pm_write32(m_dag2.i[i], parallel_ureg); }
            else   { if (ureg == 0xdb) m_px = m_bus->pm_read48(m_dag2.i[i]); else SET_UREG(ureg, m_bus->pm_read32(m_dag2.i[i])); }
            m_dag2.i[i] += m_dag2.m[m]; update_circular_buffer_pm(i);
        } else {
            if (d) m_bus->dm_write32(m_dag1.i[i], parallel_ureg);
            else   SET_UREG(ureg, m_bus->dm_read32(m_dag1.i[i]));
            m_dag1.i[i] += m_dag1.m[m]; update_circular_buffer_dm(i);
        }
    }
}

void SHARC::sharcop_compute_dm_to_dreg_immmod() {
    int cond = op_get_cond(m_opcode); int u = (m_opcode >> 38) & 0x1;
    int dreg = (m_opcode >> 23) & 0xf; int i = (m_opcode >> 41) & 0x7;
    int mod = op_get_reladdr(m_opcode); int compute = op_get_compute(m_opcode);
    if (IF_CONDITION_CODE(cond)) {
        if (compute) COMPUTE(compute);
        if (u) { REG(dreg) = s32(m_bus->dm_read32(m_dag1.i[i])); m_dag1.i[i] += u32(mod); update_circular_buffer_dm(i); }
        else   { REG(dreg) = s32(m_bus->dm_read32(m_dag1.i[i] + u32(mod))); }
    }
}
void SHARC::sharcop_compute_dreg_to_dm_immmod() {
    int cond = op_get_cond(m_opcode); int u = (m_opcode >> 38) & 0x1;
    int dreg = (m_opcode >> 23) & 0xf; int i = (m_opcode >> 41) & 0x7;
    int mod = op_get_reladdr(m_opcode); int compute = op_get_compute(m_opcode);
    u32 parallel_dreg = UIREG(dreg);
    if (IF_CONDITION_CODE(cond)) {
        if (compute) COMPUTE(compute);
        if (u) { m_bus->dm_write32(m_dag1.i[i], parallel_dreg); m_dag1.i[i] += u32(mod); update_circular_buffer_dm(i); }
        else   { m_bus->dm_write32(m_dag1.i[i] + u32(mod), parallel_dreg); }
    }
}
void SHARC::sharcop_compute_pm_to_dreg_immmod() {
    int cond = op_get_cond(m_opcode); int u = (m_opcode >> 38) & 0x1;
    int dreg = (m_opcode >> 23) & 0xf; int i = (m_opcode >> 41) & 0x7;
    int mod = op_get_reladdr(m_opcode); int compute = op_get_compute(m_opcode);
    if (IF_CONDITION_CODE(cond)) {
        if (compute) COMPUTE(compute);
        if (u) { REG(dreg) = s32(m_bus->pm_read32(m_dag2.i[i])); m_dag2.i[i] += u32(mod); update_circular_buffer_pm(i); }
        else   { REG(dreg) = s32(m_bus->pm_read32(m_dag2.i[i] + u32(mod))); }
    }
}
void SHARC::sharcop_compute_dreg_to_pm_immmod() {
    int cond = op_get_cond(m_opcode); int u = (m_opcode >> 38) & 0x1;
    int dreg = (m_opcode >> 23) & 0xf; int i = (m_opcode >> 41) & 0x7;
    int mod = op_get_reladdr(m_opcode); int compute = op_get_compute(m_opcode);
    u32 parallel_dreg = UIREG(dreg);
    if (IF_CONDITION_CODE(cond)) {
        if (compute) COMPUTE(compute);
        if (u) { m_bus->pm_write32(m_dag2.i[i], parallel_dreg); m_dag2.i[i] += u32(mod); update_circular_buffer_pm(i); }
        else   { m_bus->pm_write32(m_dag2.i[i] + u32(mod), parallel_dreg); }
    }
}
void SHARC::sharcop_compute_ureg_to_ureg() {
    int src_ureg = op_get_ureg_src(m_opcode);
    int dst_ureg = op_get_ureg_dst(m_opcode);
    int cond = op_get_cond_ureg(m_opcode);
    int compute = op_get_compute(m_opcode);
    if (IF_CONDITION_CODE(cond)) {
        u32 parallel_ureg = GET_UREG(src_ureg);
        if (compute) COMPUTE(compute);
        SET_UREG(dst_ureg, parallel_ureg);
    }
}
void SHARC::sharcop_imm_shift_dreg_dmpm() {
    int i = (m_opcode >> 41) & 0x7; int m = (m_opcode >> 38) & 0x7;
    int g = (m_opcode >> 32) & 0x1; int d = (m_opcode >> 31) & 0x1;
    int dreg = (m_opcode >> 23) & 0xf; int cond = op_get_cond(m_opcode);
    int data = int(((m_opcode >> 8) & 0xff) | ((m_opcode >> 19) & 0xf00));
    int shiftop = (m_opcode >> 16) & 0x3f;
    int rn = (m_opcode >> 4) & 0xf; int rx = m_opcode & 0xf;
    if (IF_CONDITION_CODE(cond)) {
        u32 parallel_dreg = UIREG(dreg);
        SHIFT_OPERATION_IMM(shiftop, data, rn, rx);
        if (g) {
            if (d) { m_bus->pm_write32(m_dag2.i[i], parallel_dreg); } else { REG(dreg) = s32(m_bus->pm_read32(m_dag2.i[i])); }
            m_dag2.i[i] += m_dag2.m[m]; update_circular_buffer_pm(i);
        } else {
            if (d) { m_bus->dm_write32(m_dag1.i[i], parallel_dreg); } else { REG(dreg) = s32(m_bus->dm_read32(m_dag1.i[i])); }
            m_dag1.i[i] += m_dag1.m[m]; update_circular_buffer_dm(i);
        }
    }
}
void SHARC::sharcop_imm_shift() {
    int cond = op_get_cond(m_opcode);
    int data = int(((m_opcode >> 8) & 0xff) | ((m_opcode >> 19) & 0xf00));
    int shiftop = (m_opcode >> 16) & 0x3f;
    int rn = (m_opcode >> 4) & 0xf; int rx = m_opcode & 0xf;
    if (IF_CONDITION_CODE(cond)) SHIFT_OPERATION_IMM(shiftop, data, rn, rx);
}
void SHARC::sharcop_compute_modify() {
    int cond = op_get_cond(m_opcode); int compute = op_get_compute(m_opcode);
    int g = (m_opcode >> 38) & 0x1; int m = (m_opcode >> 27) & 0x7; int i = (m_opcode >> 30) & 0x7;
    if (IF_CONDITION_CODE(cond)) {
        if (compute) COMPUTE(compute);
        if (g) { m_dag2.i[i] += m_dag2.m[m]; update_circular_buffer_pm(i); }
        else   { m_dag1.i[i] += m_dag1.m[m]; update_circular_buffer_dm(i); }
    }
}

void SHARC::sharcop_direct_call() {
    int j = op_get_jump_j(m_opcode); int cond = op_get_cond(m_opcode);
    u32 address = u32(m_opcode & 0xffffff);
    if (IF_CONDITION_CODE(cond)) {
        PUSH_PC();
        if (j) { m_pcstk = m_nfaddr; CHANGE_PC_DELAYED(address); }
        else   { m_pcstk = m_daddr; CHANGE_PC(address); }
    }
}
void SHARC::sharcop_direct_jump() {
    int la = op_get_jump_la(m_opcode); int ci = op_get_jump_ci(m_opcode);
    int j = op_get_jump_j(m_opcode); int cond = op_get_cond(m_opcode);
    u32 address = u32(m_opcode & 0xffffff);
    if (IF_CONDITION_CODE(cond)) {
        if (ci) { if (m_active_irq_num >= 6 && m_active_irq_num <= 8) POP_STATUS_STACK(); m_interrupt_active = 0; m_irptl &= ~(1 << m_active_irq_num); }
        if (la) { POP_PC(); POP_LOOP(); }
        if (j) CHANGE_PC_DELAYED(address); else CHANGE_PC(address);
    }
}
void SHARC::sharcop_relative_call() {
    int j = op_get_jump_j(m_opcode); int cond = op_get_cond(m_opcode);
    u32 address = u32(m_opcode & 0xffffff);
    if (IF_CONDITION_CODE(cond)) {
        PUSH_PC();
        if (j) { m_pcstk = m_pc + 3; CHANGE_PC_DELAYED(m_pc + u32(sext(address, 24))); }
        else   { m_pcstk = m_pc + 1; CHANGE_PC(m_pc + u32(sext(address, 24))); }
    }
}
void SHARC::sharcop_relative_jump() {
    int la = op_get_jump_la(m_opcode); int ci = op_get_jump_ci(m_opcode);
    int j = op_get_jump_j(m_opcode); int cond = op_get_cond(m_opcode);
    u32 address = u32(m_opcode & 0xffffff);
    if (IF_CONDITION_CODE(cond)) {
        if (ci) { if (m_active_irq_num >= 6 && m_active_irq_num <= 8) POP_STATUS_STACK(); m_interrupt_active = 0; m_irptl &= ~(1 << m_active_irq_num); }
        if (la) { POP_PC(); POP_LOOP(); }
        if (j) CHANGE_PC_DELAYED(m_pc + u32(sext(address, 24))); else CHANGE_PC(m_pc + u32(sext(address, 24)));
    }
}
void SHARC::sharcop_indirect_jump() {
    int la = op_get_jump_la(m_opcode); int ci = op_get_jump_ci(m_opcode);
    int j = op_get_jump_j(m_opcode); int e = op_get_jump_e(m_opcode);
    int pmi = op_get_pmi(m_opcode); int pmm = op_get_pmm(m_opcode);
    int cond = op_get_cond(m_opcode); int compute = op_get_compute(m_opcode);
    if (ci) { if (m_active_irq_num >= 6 && m_active_irq_num <= 8) POP_STATUS_STACK(); m_interrupt_active = 0; m_irptl &= ~(1 << m_active_irq_num); }
    if (e) {
        if (IF_CONDITION_CODE(cond)) { if (la) { POP_PC(); POP_LOOP(); } if (j) CHANGE_PC_DELAYED(m_dag2.i[pmi]+m_dag2.m[pmm]); else CHANGE_PC(m_dag2.i[pmi]+m_dag2.m[pmm]); }
        else { if (compute) COMPUTE(compute); }
    } else {
        if (IF_CONDITION_CODE(cond)) { if (compute) COMPUTE(compute); if (la) { POP_PC(); POP_LOOP(); } if (j) CHANGE_PC_DELAYED(m_dag2.i[pmi]+m_dag2.m[pmm]); else CHANGE_PC(m_dag2.i[pmi]+m_dag2.m[pmm]); }
    }
}
void SHARC::sharcop_indirect_call() {
    int j = op_get_jump_j(m_opcode); int e = op_get_jump_e(m_opcode);
    int pmi = op_get_pmi(m_opcode); int pmm = op_get_pmm(m_opcode);
    int cond = op_get_cond(m_opcode); int compute = op_get_compute(m_opcode);
    if (e) {
        if (IF_CONDITION_CODE(cond)) { PUSH_PC(); if (j) { m_pcstk = m_nfaddr; CHANGE_PC_DELAYED(m_dag2.i[pmi]+m_dag2.m[pmm]); } else { m_pcstk = m_daddr; CHANGE_PC(m_dag2.i[pmi]+m_dag2.m[pmm]); } }
        else { if (compute) COMPUTE(compute); }
    } else {
        if (IF_CONDITION_CODE(cond)) { if (compute) COMPUTE(compute); PUSH_PC(); if (j) { m_pcstk = m_nfaddr; CHANGE_PC_DELAYED(m_dag2.i[pmi]+m_dag2.m[pmm]); } else { m_pcstk = m_daddr; CHANGE_PC(m_dag2.i[pmi]+m_dag2.m[pmm]); } }
    }
}

void SHARC::sharcop_relative_jump_compute() {
    int la = op_get_jump_la(m_opcode); int ci = op_get_jump_ci(m_opcode);
    int j = op_get_jump_j(m_opcode); int e = op_get_jump_e(m_opcode);
    int cond = op_get_cond(m_opcode); int compute = op_get_compute(m_opcode);
    if (ci) { if (m_active_irq_num >= 6 && m_active_irq_num <= 8) POP_STATUS_STACK(); m_interrupt_active = 0; m_irptl &= ~(1 << m_active_irq_num); }
    if (e) {
        if (IF_CONDITION_CODE(cond)) { if (la) { POP_PC(); POP_LOOP(); } if (j) CHANGE_PC_DELAYED(m_pc + u32(op_get_reladdr(m_opcode))); else CHANGE_PC(m_pc + u32(op_get_reladdr(m_opcode))); }
        else { if (compute) COMPUTE(compute); }
    } else {
        if (IF_CONDITION_CODE(cond)) { if (compute) COMPUTE(compute); if (la) { POP_PC(); POP_LOOP(); } if (j) CHANGE_PC_DELAYED(m_pc + u32(op_get_reladdr(m_opcode))); else CHANGE_PC(m_pc + u32(op_get_reladdr(m_opcode))); }
    }
}
void SHARC::sharcop_relative_call_compute() {
    int j = op_get_jump_j(m_opcode); int e = op_get_jump_e(m_opcode);
    int cond = op_get_cond(m_opcode); int compute = op_get_compute(m_opcode);
    if (e) {
        if (IF_CONDITION_CODE(cond)) { PUSH_PC(); if (j) { m_pcstk = m_nfaddr; CHANGE_PC_DELAYED(m_pc + u32(op_get_reladdr(m_opcode))); } else { m_pcstk = m_daddr; CHANGE_PC(m_pc + u32(op_get_reladdr(m_opcode))); } }
        else { if (compute) COMPUTE(compute); }
    } else {
        if (IF_CONDITION_CODE(cond)) { if (compute) COMPUTE(compute); PUSH_PC(); if (j) { m_pcstk = m_nfaddr; CHANGE_PC_DELAYED(m_pc + u32(op_get_reladdr(m_opcode))); } else { m_pcstk = m_daddr; CHANGE_PC(m_pc + u32(op_get_reladdr(m_opcode))); } }
    }
}
void SHARC::sharcop_indirect_jump_compute_dreg_dm() {
    int d = (m_opcode >> 44) & 0x1; int dmi = op_get_dmi(m_opcode); int dmm = op_get_dmm(m_opcode);
    int pmi = op_get_pmi(m_opcode); int pmm = op_get_pmm(m_opcode);
    int cond = op_get_cond(m_opcode); int dreg = (m_opcode >> 23) & 0xf;
    if (IF_CONDITION_CODE(cond)) { CHANGE_PC(m_dag2.i[pmi]+m_dag2.m[pmm]); }
    else {
        u32 compute = op_get_compute(m_opcode); u32 parallel_dreg = UIREG(dreg);
        if (compute) COMPUTE(compute);
        if (d) { m_bus->dm_write32(m_dag1.i[dmi], parallel_dreg); } else { REG(dreg) = s32(m_bus->dm_read32(m_dag1.i[dmi])); }
        m_dag1.i[dmi] += m_dag1.m[dmm]; update_circular_buffer_dm(dmi);
    }
}
void SHARC::sharcop_relative_jump_compute_dreg_dm() {
    int d = (m_opcode >> 44) & 0x1; int dmi = op_get_dmi(m_opcode); int dmm = op_get_dmm(m_opcode);
    int cond = op_get_cond(m_opcode); int dreg = (m_opcode >> 23) & 0xf;
    if (IF_CONDITION_CODE(cond)) { CHANGE_PC(m_pc + u32(op_get_reladdr(m_opcode))); }
    else {
        u32 compute = op_get_compute(m_opcode); u32 parallel_dreg = UIREG(dreg);
        if (compute) COMPUTE(compute);
        if (d) { m_bus->dm_write32(m_dag1.i[dmi], parallel_dreg); } else { REG(dreg) = s32(m_bus->dm_read32(m_dag1.i[dmi])); }
        m_dag1.i[dmi] += m_dag1.m[dmm]; update_circular_buffer_dm(dmi);
    }
}
void SHARC::sharcop_rts() {
    int cond = op_get_cond(m_opcode); int j = op_get_jump_j(m_opcode); int e = op_get_jump_e(m_opcode);
    int compute = op_get_compute(m_opcode);
    if (e) { if (IF_CONDITION_CODE(cond)) { if (j) CHANGE_PC_DELAYED(POP_PC()); else CHANGE_PC(POP_PC()); } else { if (compute) COMPUTE(compute); } }
    else   { if (IF_CONDITION_CODE(cond)) { if (compute) COMPUTE(compute); if (j) CHANGE_PC_DELAYED(POP_PC()); else CHANGE_PC(POP_PC()); } }
}
void SHARC::sharcop_rti() {
    int cond = op_get_cond(m_opcode); int j = op_get_jump_j(m_opcode); int e = op_get_jump_e(m_opcode);
    int compute = op_get_compute(m_opcode);
    m_irptl &= ~(1 << m_active_irq_num);
    if (e) { if (IF_CONDITION_CODE(cond)) { if (j) CHANGE_PC_DELAYED(POP_PC()); else CHANGE_PC(POP_PC()); } else { if (compute) COMPUTE(compute); } }
    else   { if (IF_CONDITION_CODE(cond)) { if (compute) COMPUTE(compute); if (j) CHANGE_PC_DELAYED(POP_PC()); else CHANGE_PC(POP_PC()); } }
    if (m_active_irq_num >= 6 && m_active_irq_num <= 8) POP_STATUS_STACK();
    m_interrupt_active = 0;
    check_interrupts();
}

void SHARC::sharcop_do_until_counter_imm() {
    u16 data = u16(m_opcode >> 24);
    int offset = sext(u32(m_opcode & 0xffffff), 24);
    u32 address = m_pc + u32(offset);
    int distance = std::abs(offset);
    int type = (distance == 1) ? 1 : (distance == 2) ? 2 : 3;
    m_lcntr = data;
    PUSH_PC(); PUSH_LOOP();
    m_pcstk = m_pc + 1;
    m_laddr.addr = address; m_laddr.code = 0xf; m_laddr.loop_type = u32(type);
}
void SHARC::sharcop_do_until_counter_ureg() {
    int ureg = (m_opcode >> 32) & 0xff;
    int offset = sext(u32(m_opcode & 0xffffff), 24);
    u32 address = m_pc + u32(offset);
    int distance = std::abs(offset);
    int type = (distance == 1) ? 1 : (distance == 2) ? 2 : 3;
    m_lcntr = GET_UREG(ureg);
    PUSH_PC(); PUSH_LOOP();
    m_pcstk = m_pc + 1;
    m_laddr.addr = address; m_laddr.code = 0xf; m_laddr.loop_type = u32(type);
}
void SHARC::sharcop_do_until() {
    int cond = op_get_cond(m_opcode);
    int offset = sext(u32(m_opcode & 0xffffff), 24);
    u32 address = m_pc + u32(offset);
    PUSH_PC(); PUSH_LOOP();
    m_pcstk = m_pc + 1;
    m_laddr.addr = address; m_laddr.code = u32(cond); m_laddr.loop_type = 0;
}
void SHARC::sharcop_dm_to_ureg_direct() {
    int ureg = (m_opcode >> 32) & 0xff;
    u32 address = u32(m_opcode);
    SET_UREG(ureg, m_bus->dm_read32(address));
}
void SHARC::sharcop_ureg_to_dm_direct() {
    int ureg = (m_opcode >> 32) & 0xff;
    u32 address = u32(m_opcode);
    m_bus->dm_write32(address, GET_UREG(ureg));
}
void SHARC::sharcop_pm_to_ureg_direct() {
    int ureg = (m_opcode >> 32) & 0xff;
    u32 address = u32(m_opcode);
    if (ureg == 0xdb) m_px = m_bus->pm_read48(address);
    else SET_UREG(ureg, m_bus->pm_read32(address));
}
void SHARC::sharcop_ureg_to_pm_direct() {
    int ureg = (m_opcode >> 32) & 0xff;
    u32 address = u32(m_opcode);
    if (ureg == 0xdb) m_bus->pm_write48(address, m_px);
    else m_bus->pm_write32(address, GET_UREG(ureg));
}
void SHARC::sharcop_dm_to_ureg_indirect() {
    int ureg = (m_opcode >> 32) & 0xff; u32 offset = u32(m_opcode); int i = (m_opcode >> 41) & 0x7;
    SET_UREG(ureg, m_bus->dm_read32(m_dag1.i[i] + offset));
}
void SHARC::sharcop_ureg_to_dm_indirect() {
    int ureg = (m_opcode >> 32) & 0xff; u32 offset = u32(m_opcode); int i = (m_opcode >> 41) & 0x7;
    m_bus->dm_write32(m_dag1.i[i] + offset, GET_UREG(ureg));
}
void SHARC::sharcop_pm_to_ureg_indirect() {
    int ureg = (m_opcode >> 32) & 0xff; u32 offset = u32(m_opcode & 0xffffff); int i = (m_opcode >> 41) & 0x7;
    if (ureg == 0xdb) m_px = m_bus->pm_read48(m_dag2.i[i] + offset);
    else SET_UREG(ureg, m_bus->pm_read32(m_dag2.i[i] + offset));
}
void SHARC::sharcop_ureg_to_pm_indirect() {
    int ureg = (m_opcode >> 32) & 0xff; u32 offset = u32(m_opcode); int i = (m_opcode >> 41) & 0x7;
    if (ureg == 0xdb) m_bus->pm_write48(m_dag2.i[i] + offset, m_px);
    else m_bus->pm_write32(m_dag2.i[i] + offset, GET_UREG(ureg));
}
void SHARC::sharcop_imm_to_dmpm() {
    int i = (m_opcode >> 41) & 0x7; int m = (m_opcode >> 38) & 0x7;
    int g = (m_opcode >> 37) & 0x1; u32 data = u32(m_opcode);
    if (g) { m_bus->pm_write32(m_dag2.i[i], data); m_dag2.i[i] += m_dag2.m[m]; update_circular_buffer_pm(i); }
    else   { m_bus->dm_write32(m_dag1.i[i], data); m_dag1.i[i] += m_dag1.m[m]; update_circular_buffer_dm(i); }
}
void SHARC::sharcop_imm_to_ureg() {
    int ureg = (m_opcode >> 32) & 0xff; u32 data = u32(m_opcode);
    SET_UREG(ureg, data);
}

void SHARC::sharcop_sysreg_bitop() {
    int bop = (m_opcode >> 37) & 0x7; int sreg = (m_opcode >> 32) & 0xf;
    u32 data = u32(m_opcode); u32 src = GET_UREG(0x70 | sreg);
    switch (bop) {
    case 0: src |= data; break;
    case 1: src &= ~data; break;
    case 2: src ^= data; break;
    case 4: if ((src & data) == data) m_astat |= BTF; else m_astat &= ~BTF; break;
    case 5: if (src == data) m_astat |= BTF; else m_astat &= ~BTF; break;
    default: break;
    }
    SET_UREG(0x70 | sreg, src);
}
void SHARC::sharcop_modify() {
    int g = (m_opcode >> 38) & 0x1; int i = (m_opcode >> 32) & 0x7;
    s32 data = s32(u32(m_opcode));
    if (g) { m_dag2.i[i] += u32(data); update_circular_buffer_pm(i); }
    else   { m_dag1.i[i] += u32(data); update_circular_buffer_dm(i); }
}
void SHARC::sharcop_bit_reverse() { /* unimplemented */ }
void SHARC::sharcop_push_pop_stacks() {
    if (m_opcode & 0x008000000000ULL) PUSH_LOOP();
    if (m_opcode & 0x004000000000ULL) POP_LOOP();
    if (m_opcode & 0x002000000000ULL) PUSH_STATUS_STACK();
    if (m_opcode & 0x001000000000ULL) POP_STATUS_STACK();
    if (m_opcode & 0x000800000000ULL) PUSH_PC();
    if (m_opcode & 0x000400000000ULL) POP_PC();
}
void SHARC::sharcop_nop() {}
void SHARC::sharcop_idle() {
    m_daddr = m_pc; m_faddr = m_pc + 1; m_nfaddr = m_pc + 2;
    m_idle = 1;
}
void SHARC::sharcop_unimplemented() { /* silently ignore for now */ }

// ============================================================================
// Diagnostics
// ============================================================================

std::string SHARC::state_string() const
{
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "PC=%06X ASTAT=%08X MODE1=%08X LCNTR=%08X",
        m_pc, m_astat, m_mode1, m_lcntr);
    return buf;
}

#undef REG
#undef FREG
#undef UIREG

}  // namespace sm2::cpu::sharc
