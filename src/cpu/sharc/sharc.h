// SPDX-License-Identifier: BSD-3-Clause
//
// Analog Devices ADSP-21062 "SHARC" DSP core — interpreter.
//
// Derived from MAME's src/devices/cpu/sharc/, which is BSD-3-Clause,
// copyright-holders Ville Linde.
//
// Changes from upstream: MAME device/address-space plumbing replaced by a Bus
// interface; dynamic recompiler removed; state kept in-class rather than in a
// cache-aligned heap block.
#pragma once

#include "core/types.h"

#include <cstdint>
#include <string>

namespace sm2::cpu::sharc {

// ============================================================================
// Bus — the three address spaces visible to the SHARC
// ============================================================================

class Bus {
public:
    virtual ~Bus() = default;

    /// Program memory: 48-bit instruction words.
    [[nodiscard]] virtual u64 pm_read48(u32 address) = 0;
    virtual void pm_write48(u32 address, u64 data) = 0;

    /// Program memory accessed as 32-bit data (upper 32 of the 48-bit word).
    [[nodiscard]] virtual u32 pm_read32(u32 address) = 0;
    virtual void pm_write32(u32 address, u32 data) = 0;

    /// Data memory: 32-bit words.
    [[nodiscard]] virtual u32 dm_read32(u32 address) = 0;
    virtual void dm_write32(u32 address, u32 data) = 0;

    /// External DMA write — called by the host to push data into the SHARC
    /// program memory via the DMA controller (packing mode 16/48).
    virtual void external_dma_write(u32 address, u64 data) { (void)address; (void)data; }
};

// ============================================================================
// SHARC — the CPU core
// ============================================================================

class SHARC {
public:
    explicit SHARC(Bus& bus);

    SHARC(const SHARC&)            = delete;
    SHARC& operator=(const SHARC&) = delete;

    void reset();

    /// Execute up to `cycles` instructions. Returns the number actually consumed.
    [[nodiscard]] s32 run(s32 cycles);

    /// External halt — the host holds this while uploading microcode.
    void set_halted(bool halted) { m_halted = halted; }
    [[nodiscard]] bool halted() const { return m_halted; }

    /// Write a value to an IOP register from the external host bus.
    void external_iop_write(u32 address, u32 data);

    /// Write 48-bit data via the external port DMA buffer (host packing path).
    void external_dma_write(u32 address, u64 data);

    /// Set FLAG input pins.
    void set_flag_input(int flag_num, int state);

    /// Signal write stall from external bus.
    void write_stall(bool state) { m_write_stalled = state; }

    // -- state accessors for debugger / diagnostics -------------------------

    [[nodiscard]] u32 pc() const { return m_pc; }
    [[nodiscard]] u32 daddr() const { return m_daddr; }
    [[nodiscard]] u32 faddr() const { return m_faddr; }
    [[nodiscard]] u32 astat() const { return m_astat; }
    [[nodiscard]] u32 mode1() const { return m_mode1; }
    [[nodiscard]] u32 mode2() const { return m_mode2; }
    [[nodiscard]] u32 irptl() const { return m_irptl; }
    [[nodiscard]] u32 imask() const { return m_imask; }
    [[nodiscard]] u32 stky() const { return m_stky; }
    [[nodiscard]] u32 lcntr() const { return m_lcntr; }
    [[nodiscard]] u32 curlcntr() const { return m_curlcntr; }
    [[nodiscard]] u32 reg(int index) const { return m_r[index & 0xf].r; }
    [[nodiscard]] float freg(int index) const { return m_r[index & 0xf].f; }
    [[nodiscard]] u64 instructions() const { return m_instruction_count; }
    [[nodiscard]] u32 dma_status() const { return m_dma_status; }

    void set_pc(u32 value);

    [[nodiscard]] std::string state_string() const;

    using TraceHook = void (*)(void* context, u32 pc, u64 opcode);
    void set_trace_hook(TraceHook hook, void* context)
    {
        m_trace_hook    = hook;
        m_trace_context = context;
    }

private:
    // -- ASTAT flags --------------------------------------------------------
    static constexpr u32 AZ  = 0x00000001;
    static constexpr u32 AV  = 0x00000002;
    static constexpr u32 AN  = 0x00000004;
    static constexpr u32 AC  = 0x00000008;
    static constexpr u32 AS  = 0x00000010;
    static constexpr u32 AI  = 0x00000020;
    static constexpr u32 MN  = 0x00000040;
    static constexpr u32 MV  = 0x00000080;
    static constexpr u32 MU  = 0x00000100;
    static constexpr u32 MI  = 0x00000200;
    static constexpr u32 AF  = 0x00000400;
    static constexpr u32 SV  = 0x00000800;
    static constexpr u32 SZ  = 0x00001000;
    static constexpr u32 SS  = 0x00002000;
    static constexpr u32 BTF = 0x00040000;
    static constexpr u32 FLG0 = 0x00080000;
    static constexpr u32 FLG1 = 0x00100000;
    static constexpr u32 FLG2 = 0x00200000;
    static constexpr u32 FLG3 = 0x00400000;

    // -- STKY flags ---------------------------------------------------------
    static constexpr u32 AUS  = 0x00000001;
    static constexpr u32 AVS  = 0x00000002;
    static constexpr u32 AOS  = 0x00000004;
    static constexpr u32 AIS  = 0x00000020;
    static constexpr u32 MOS  = 0x00000040;
    static constexpr u32 MVS  = 0x00000080;
    static constexpr u32 MUS  = 0x00000100;
    static constexpr u32 MIS  = 0x00000200;
    static constexpr u32 PCFL = 0x00200000;
    static constexpr u32 PCEM = 0x00400000;
    static constexpr u32 SSOV = 0x00800000;
    static constexpr u32 SSEM = 0x01000000;
    static constexpr u32 LSOV = 0x02000000;
    static constexpr u32 LSEM = 0x04000000;

    // -- MODE1 flags --------------------------------------------------------
    static constexpr u32 MODE1_BR8      = 0x00000001;
    static constexpr u32 MODE1_BR0      = 0x00000002;
    static constexpr u32 MODE1_SRCU     = 0x00000004;
    static constexpr u32 MODE1_SRD1H    = 0x00000008;
    static constexpr u32 MODE1_SRD1L    = 0x00000010;
    static constexpr u32 MODE1_SRD2H    = 0x00000020;
    static constexpr u32 MODE1_SRD2L    = 0x00000040;
    static constexpr u32 MODE1_SRRFH    = 0x00000080;
    static constexpr u32 MODE1_SRRFL    = 0x00000400;
    static constexpr u32 MODE1_NESTM    = 0x00000800;
    static constexpr u32 MODE1_IRPTEN   = 0x00001000;
    static constexpr u32 MODE1_ALUSAT   = 0x00002000;
    static constexpr u32 MODE1_SSE      = 0x00004000;
    static constexpr u32 MODE1_TRUNCATE = 0x00008000;
    static constexpr u32 MODE1_RND32    = 0x00010000;
    static constexpr u32 MODE1_CSEL     = 0x00060000;

    // -- Register union -----------------------------------------------------
    union REG_UNION {
        s32 r;
        float f;
    };

    // -- DAG (Data Address Generator) structure -----------------------------
    struct DAG {
        u32 i[8]{};
        u32 m[8]{};
        u32 b[8]{};
        u32 l[8]{};
    };

    // -- Loop address structure ---------------------------------------------
    struct LADDR {
        u32 addr = 0;
        u32 code = 0;
        u32 loop_type = 0;

        [[nodiscard]] u32 pack() const
        {
            return (loop_type << 30) | (code << 24) | addr;
        }
        void unpack(u32 in)
        {
            addr      = in & 0x00ffffff;
            code      = (in >> 24) & 0x1f;
            loop_type = (in >> 30) & 0x3;
        }
    };

    // -- DMA register set ---------------------------------------------------
    struct DMA_REGS {
        u32 control      = 0;
        u32 int_index    = 0;
        u32 int_modifier = 0;
        u32 int_count    = 0;
        u32 chain_ptr    = 0;
        u32 gen_purpose  = 0;
        u32 ext_index    = 0;
        u32 ext_modifier = 0;
        u32 ext_count    = 0;
    };

    struct DMA_OP {
        u32 src = 0;
        u32 dst = 0;
        u32 chain_ptr = 0;
        s32 src_modifier = 0;
        s32 dst_modifier = 0;
        s32 src_count = 0;
        s32 dst_count = 0;
        s32 pmode = 0;
        s32 chained_direction = 0;
        bool active = false;
        bool chained = false;
    };

    // -- Status stack entry -------------------------------------------------
    struct StatusEntry {
        u32 mode1 = 0;
        u32 astat = 0;
    };

    // -- Opcode dispatch table type -----------------------------------------
    using OpcodeFunc = void (SHARC::*)();

    // -- Internal state -----------------------------------------------------
    Bus* m_bus = nullptr;

    REG_UNION m_r[16]{};
    REG_UNION m_reg_alt[16]{};

    u32 m_pc      = 0;
    u32 m_daddr   = 0;
    u32 m_faddr   = 0;
    u32 m_nfaddr  = 0;

    u64 m_mrf = 0;
    u64 m_mrb = 0;

    u32 m_pcstack[30]{};
    u32 m_lcstack[6]{};
    u32 m_lastack[6]{};
    u32 m_lstkp = 0;

    u32     m_pcstk    = 0;
    u32     m_pcstkp   = 0;
    LADDR   m_laddr{};
    u32     m_curlcntr = 0;
    u32     m_lcntr    = 0;

    DAG m_dag1{};
    DAG m_dag2{};
    DAG m_dag1_alt{};
    DAG m_dag2_alt{};

    DMA_REGS m_dma[12]{};
    DMA_OP   m_dma_op[12]{};
    u32      m_dma_status = 0;

    u32 m_mode1 = 0;
    u32 m_mode2 = 0;
    u32 m_astat = 0;
    u32 m_stky  = 0;
    u32 m_irptl = 0;
    u32 m_imask = 0;
    u32 m_imaskp = 0;
    u32 m_ustat1 = 0;
    u32 m_ustat2 = 0;

    u32 m_flag[4]{};

    u32 m_syscon  = 0;
    u32 m_sysstat = 0;

    StatusEntry m_status_stack[5]{};
    s32 m_status_stkp = 0;

    u64 m_px = 0;

    s32 m_icount = 0;
    u64 m_opcode = 0;

    s32 m_idle = 0;
    s32 m_irq_pending = 0;
    s32 m_active_irq_num = 0;
    s32 m_interrupt_active = 0;

    u32 m_delay_slot1 = 0;
    u32 m_delay_slot2 = 0;

    s32 m_systemreg_latency_cycles = 0;
    s32 m_systemreg_latency_reg = -1;
    u32 m_systemreg_latency_data = 0;
    u32 m_systemreg_previous_data = 0;

    u32 m_astat_old = 0;
    u32 m_astat_old_old = 0;
    u32 m_astat_old_old_old = 0;

    u8  m_extdma_shift = 0;
    u32 m_iop_write_num = 0;
    u32 m_iop_data = 0;

    bool m_halted = false;
    bool m_write_stalled = false;

    u64 m_instruction_count = 0;

    TraceHook m_trace_hook    = nullptr;
    void*     m_trace_context = nullptr;

    OpcodeFunc m_sharc_op[512]{};

    // -- Lookup tables (static) ---------------------------------------------
    static const u32 s_recips_mantissa_lookup[128];
    static const u32 s_rsqrts_mantissa_lookup[128];

    // -- Opcode table -------------------------------------------------------
    struct OpTableEntry {
        u32 op_mask;
        u32 op_bits;
        OpcodeFunc handler;
    };
    static const OpTableEntry s_opcode_table[];
    static const int s_num_ops;

    void build_opcode_table();

    // -- PC / flow control --------------------------------------------------
    void CHANGE_PC(u32 newpc);
    void CHANGE_PC_DELAYED(u32 newpc);

    // -- Stack operations ---------------------------------------------------
    void PUSH_PC();
    u32  POP_PC();
    u32  TOP_PC();
    void PUSH_LOOP();
    void POP_LOOP();
    void PUSH_STATUS_STACK();
    void POP_STATUS_STACK();

    // -- Condition codes ----------------------------------------------------
    int IF_CONDITION_CODE(int cond);
    int DO_CONDITION_CODE(int cond);

    // -- Register access (UREG) --------------------------------------------
    u32  GET_UREG(int ureg);
    void SET_UREG(int ureg, u32 data);

    // -- Compute dispatch ---------------------------------------------------
    void COMPUTE(u32 opcode);
    void SHIFT_OPERATION_IMM(int shiftop, int data, int rn, int rx);

    // -- Circular buffer update ---------------------------------------------
    void update_circular_buffer_pm(int i);
    void update_circular_buffer_dm(int i);

    // -- DMA ----------------------------------------------------------------
    void schedule_dma_op(int channel, u32 src, u32 dst, s32 src_modifier,
                         s32 dst_modifier, s32 src_count, s32 dst_count, int pmode);
    void schedule_chained_dma_op(int channel, u32 dma_chain_ptr, int chained_direction);
    void dma_op(int channel);
    void dma_run_cycle(int channel);
    void sharc_dma_exec(int channel);

    // -- IOP write handling -------------------------------------------------
    void iop_write(u32 offset, u32 data);

    // -- System register latency --------------------------------------------
    void add_systemreg_write_latency_effect(int sysreg, u32 data, u32 prev_data);
    void systemreg_write_latency_effect();

    // -- Interrupts ---------------------------------------------------------
    void check_interrupts();

    // -- Opcode handlers ----------------------------------------------------
    void sharcop_compute_dreg_dm_dreg_pm();
    void sharcop_compute();
    void sharcop_compute_ureg_dmpm_premod();
    void sharcop_compute_ureg_dmpm_postmod();
    void sharcop_compute_dm_to_dreg_immmod();
    void sharcop_compute_dreg_to_dm_immmod();
    void sharcop_compute_pm_to_dreg_immmod();
    void sharcop_compute_dreg_to_pm_immmod();
    void sharcop_compute_ureg_to_ureg();
    void sharcop_imm_shift_dreg_dmpm();
    void sharcop_imm_shift();
    void sharcop_compute_modify();
    void sharcop_direct_call();
    void sharcop_direct_jump();
    void sharcop_relative_call();
    void sharcop_relative_jump();
    void sharcop_indirect_jump();
    void sharcop_indirect_call();
    void sharcop_relative_jump_compute();
    void sharcop_relative_call_compute();
    void sharcop_indirect_jump_compute_dreg_dm();
    void sharcop_relative_jump_compute_dreg_dm();
    void sharcop_rts();
    void sharcop_rti();
    void sharcop_do_until_counter_imm();
    void sharcop_do_until_counter_ureg();
    void sharcop_do_until();
    void sharcop_dm_to_ureg_direct();
    void sharcop_ureg_to_dm_direct();
    void sharcop_pm_to_ureg_direct();
    void sharcop_ureg_to_pm_direct();
    void sharcop_dm_to_ureg_indirect();
    void sharcop_ureg_to_dm_indirect();
    void sharcop_pm_to_ureg_indirect();
    void sharcop_ureg_to_pm_indirect();
    void sharcop_imm_to_dmpm();
    void sharcop_imm_to_ureg();
    void sharcop_sysreg_bitop();
    void sharcop_modify();
    void sharcop_bit_reverse();
    void sharcop_push_pop_stacks();
    void sharcop_nop();
    void sharcop_idle();
    void sharcop_unimplemented();

    // -- ALU float helpers --------------------------------------------------
    REG_UNION FADD(int fx, int fy);
    REG_UNION FSUB(int fx, int fy);
    REG_UNION FAVG(int fx, int fy);
    REG_UNION FABS(int fx);
    REG_UNION FMIN(int fx, int fy);
    REG_UNION FMAX(int fx, int fy);
    std::pair<REG_UNION, REG_UNION> FADD_FSUB(int fx, int fy);
    u32 SCALB(REG_UNION fx, int ry);
    REG_UNION FMUL(int fx, int fy);

    // -- ALU fixed-point operations -----------------------------------------
    void compute_add(int rn, int rx, int ry);
    void compute_sub(int rn, int rx, int ry);
    void compute_add_ci(int rn, int rx, int ry);
    void compute_sub_ci(int rn, int rx, int ry);
    void compute_comp(int rx, int ry);
    void compute_add_ci(int rn, int rx);
    void compute_sub_ci(int rn, int rx);
    void compute_inc(int rn, int rx);
    void compute_dec(int rn, int rx);
    void compute_neg(int rn, int rx);
    void compute_abs(int rn, int rx);
    void compute_pass(int rn, int rx);
    void compute_and(int rn, int rx, int ry);
    void compute_or(int rn, int rx, int ry);
    void compute_xor(int rn, int rx, int ry);
    void compute_not(int rn, int rx);
    void compute_min(int rn, int rx, int ry);
    void compute_max(int rn, int rx, int ry);
    void compute_clip(int rn, int rx, int ry);

    // -- ALU floating-point operations --------------------------------------
    void compute_fadd(int fn, int fx, int fy);
    void compute_fsub(int fn, int fx, int fy);
    void compute_fadd_abs(int fn, int fx, int fy);
    void compute_fsub_abs(int fn, int fx, int fy);
    void compute_favg(int fn, int fx, int fy);
    void compute_fcomp(int fx, int fy);
    void compute_fneg(int fn, int fx);
    void compute_fabs(int fn, int fx);
    void compute_fpass(int fn, int fx);
    void compute_scalb(int fn, int fx, int ry);
    void compute_logb(int rn, int fx);
    void compute_fix(int rn, int fx);
    void compute_float(int fn, int rx);
    void compute_fix_scaled(int rn, int fx, int ry);
    void compute_float_scaled(int fn, int rx, int ry);
    void compute_recips(int fn, int fx);
    void compute_rsqrts(int fn, int fx);
    void compute_fcopysign(int fn, int fx, int fy);
    void compute_fmin(int fn, int fx, int fy);
    void compute_fmax(int fn, int fx, int fy);
    void compute_fclip(int fn, int fx, int fy);

    // -- Multiplier operations ----------------------------------------------
    void compute_mul_uuin(int rn, int rx, int ry);
    void compute_mul_ssin(int rn, int rx, int ry);
    u32  compute_mrf_plus_mul_ssin(int rx, int ry);
    u32  compute_mrb_plus_mul_ssin(int rx, int ry);
    void compute_fmul(int fn, int fx, int fy);

    // -- Dual add/subtract --------------------------------------------------
    void compute_dual_add_sub(int ra, int rs, int rx, int ry);
    void compute_dual_fadd_fsub(int fa, int fs, int fx, int fy);

    // -- Multi-function (multiply + ALU) ------------------------------------
    void compute_mul_ssfr_add(int rm, int rxm, int rym, int ra, int rxa, int rya);
    void compute_mul_ssfr_sub(int rm, int rxm, int rym, int ra, int rxa, int rya);
    void compute_fmul_fadd(int fm, int fxm, int fym, int fa, int fxa, int fya);
    void compute_fmul_fsub(int fm, int fxm, int fym, int fa, int fxa, int fya);
    void compute_fmul_float_scaled(int fm, int fxm, int fym, int fa, int rxa, int rya);
    void compute_fmul_fix_scaled(int fm, int fxm, int fym, int ra, int fxa, int rya);
    void compute_fmul_favg(int fm, int fxm, int fym, int fa, int fxa, int fya);
    void compute_fmul_fabs(int fm, int fxm, int fym, int fa, int fxa);
    void compute_fmul_fmax(int fm, int fxm, int fym, int fa, int fxa, int fya);
    void compute_fmul_fmin(int fm, int fxm, int fym, int fa, int fxa, int fya);
    void compute_fmul_dual_fadd_fsub(int fm, int fxm, int fym, int fa, int fs, int fxa, int fya);
    void compute_multi_mr_to_reg(int ai, int rk);
    void compute_multi_reg_to_mr(int ai, int rk);
};

}  // namespace sm2::cpu::sharc
