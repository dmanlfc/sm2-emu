// SPDX-License-Identifier: BSD-3-Clause
//
// Intel 80960KB CPU core.
//
// Derived from MAME's src/devices/cpu/i960/i960.h, which is BSD-3-Clause.
// Copyright (c) Farfetch'd, R. Belmont.
//
// Changes from upstream: MAME's device, memory and state interfaces are replaced
// by a plain class over cpu::Bus, and an unhandled condition throws Fault rather
// than calling fatalerror. The instruction implementations are otherwise kept as
// close to upstream as possible so that fixes there remain diffable.

#pragma once

#include "core/types.h"
#include "cpu/bus.h"

#include <exception>
#include <string>

namespace sm2::cpu::i960 {

/// Register file indices, also used by the debugger and state export.
enum {
    I960_PFP = 0,
    I960_SP  = 1,
    I960_RIP = 2,
    I960_FP  = 31,

    I960_R0 = 0,   I960_R1 = 1,   I960_R2 = 2,   I960_R3 = 3,
    I960_R4 = 4,   I960_R5 = 5,   I960_R6 = 6,   I960_R7 = 7,
    I960_R8 = 8,   I960_R9 = 9,   I960_R10 = 10, I960_R11 = 11,
    I960_R12 = 12, I960_R13 = 13, I960_R14 = 14, I960_R15 = 15,

    I960_G0 = 16,  I960_G1 = 17,  I960_G2 = 18,  I960_G3 = 19,
    I960_G4 = 20,  I960_G5 = 21,  I960_G6 = 22,  I960_G7 = 23,
    I960_G8 = 24,  I960_G9 = 25,  I960_G10 = 26, I960_G11 = 27,
    I960_G12 = 28, I960_G13 = 29, I960_G14 = 30, I960_G15 = 31,
};

/// The four external interrupt lines.
///
/// The core supports these in "normal" mode only. The real i960's interrupt
/// support is more complete, but Sega and Namco both took the cheap option and
/// wired only these.
enum {
    I960_IRQ0 = 0,
    I960_IRQ1 = 1,
    I960_IRQ2 = 2,
    I960_IRQ3 = 3,
};

enum LineState {
    kClearLine  = 0,
    kAssertLine = 1,
};

/// Thrown when the core reaches a condition upstream MAME treats as fatal:
/// an unimplemented opcode, an unsupported addressing mode, a write to a
/// literal operand.
///
/// Upstream calls fatalerror, which aborts. Throwing instead lets the machine
/// report what happened, with the address, and stop cleanly rather than killing
/// the process. The throw sites are all genuinely unreachable in working code,
/// so this never costs anything on the hot path.
class Fault : public std::exception {
public:
    explicit Fault(std::string message) : m_message(std::move(message)) {}
    [[nodiscard]] const char* what() const noexcept override { return m_message.c_str(); }

private:
    std::string m_message;
};

class I960 {
public:
    /// Flag a bus region must report for burst accesses. See cpu::kBusFlagBurst.
    static constexpr u16 BURST = kBusFlagBurst;

    enum { I960_RCACHE_SIZE = 4 };

    explicit I960(Bus& bus);

    /// Read the initial memory image and enter the reset state.
    ///
    /// Takes SAT from address 0, PRCB from 4 and the initial instruction pointer
    /// from 12, then the initial frame pointer from PRCB+24.
    void reset();

    /// Execute until `cycles` have been consumed. Returns the number actually
    /// used, which can exceed the request because instructions are not
    /// interruptible.
    s32 run(s32 cycles);

    /// Change the state of an external interrupt line.
    void set_irq_line(int line, int state);

    /// Stall the current instruction and re-execute it.
    ///
    /// Called from a bus handler when the geometry coprocessor's output FIFO is
    /// empty. Rewinding the instruction pointer to the previous instruction means
    /// the access is retried once the coprocessor has produced data, which is how
    /// the real hardware's bus stall behaves.
    void stall()
    {
        m_stalled = true;
        m_IP = m_PIP;
    }

    /// Stop and resume execution, for the HALT line the copro FIFO drives.
    void set_halted(bool halted) { m_halted = halted; }
    [[nodiscard]] bool halted() const { return m_halted; }

    /// True once a Fault has been caught. The core will not execute further.
    [[nodiscard]] bool faulted() const { return m_faulted; }
    [[nodiscard]] const std::string& fault_message() const { return m_fault_message; }

    // -- state, for debugging and the memory-access tracer -----------------

    [[nodiscard]] u32 ip() const  { return m_IP; }
    [[nodiscard]] u32 pip() const { return m_PIP; }
    [[nodiscard]] u32 sat() const { return m_SAT; }
    [[nodiscard]] u32 prcb() const { return m_PRCB; }
    [[nodiscard]] u32 pc() const  { return m_PC; }
    [[nodiscard]] u32 ac() const  { return m_AC; }
    [[nodiscard]] u32 icr() const { return m_ICR; }
    [[nodiscard]] u32 reg(int index) const { return m_r[index & 0x1f]; }

    void set_ip(u32 value) { m_IP = value; }

    /// One-line register dump, for logging a fault or a boot trace.
    [[nodiscard]] std::string state_string() const;

    /// Total instructions retired since reset.
    [[nodiscard]] u64 instructions() const { return m_instruction_count; }

    /// Called before each instruction when set. Used by the boot tracer; leave
    /// unset for full speed.
    using TraceHook = void (*)(void* context, u32 ip, u32 opcode);
    void set_trace_hook(TraceHook hook, void* context)
    {
        m_trace_hook    = hook;
        m_trace_context = context;
    }

private:
    [[noreturn]] void fatal(const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;

    void burst_stall_save(u32 t1, u32 t2, int index, int size, bool iswriteop);

    struct {
        u32  t1 = 0, t2 = 0;
        int  index = 0, size = 0;
        bool burst_mode = false;
        bool iswriteop  = false;
    } m_stall_state;

    bool        m_stalled = false;
    bool        m_halted  = false;
    bool        m_faulted = false;
    std::string m_fault_message;

    Bus* m_bus = nullptr;

    u32 m_r[0x20]{};
    u32 m_rcache[I960_RCACHE_SIZE][0x10]{};
    u32 m_rcache_frame_addr[I960_RCACHE_SIZE]{};

    // How deep in the stack we are. 0 to I960_RCACHE_SIZE-1 means in-cache;
    // I960_RCACHE_SIZE or greater means out of cache and frames must be saved
    // to memory.
    s32 m_rcache_pos = 0;

    double m_fp[4]{};

    u32 m_SAT  = 0;
    u32 m_PRCB = 0;
    u32 m_PC   = 0;
    u32 m_AC   = 0;
    u32 m_IP   = 0;
    u32 m_PIP  = 0;
    u32 m_ICR  = 0;

    int m_immediate_irq    = 0;
    int m_immediate_vector = 0;
    int m_immediate_pri    = 0;

    s8 m_irq_line_state[4]{};

    s32 m_icount = 0;
    u64 m_instruction_count = 0;

    TraceHook m_trace_hook    = nullptr;
    void*     m_trace_context = nullptr;

    // -- memory helpers ---------------------------------------------------

    u32  i960_read_dword_unaligned(u32 address);
    std::pair<u32, u16> i960_read_dword_unaligned_flags(u32 address);
    u16  i960_read_word_unaligned(u32 address);
    void i960_write_dword_unaligned(u32 address, u32 data);
    u16  i960_write_dword_unaligned_flags(u32 address, u32 data);
    void i960_write_word_unaligned(u32 address, u16 data);

    // -- decode helpers ---------------------------------------------------

    void   send_iac(u32 adr);
    u32    get_ea(u32 opcode);
    u32    get_1_ri(u32 opcode);
    u32    get_2_ri(u32 opcode);
    u64    get_2_ri64(u32 opcode);
    void   set_ri(u32 opcode, u32 val);
    void   set_ri2(u32 opcode, u32 val, u32 val2);
    void   set_ri64(u32 opcode, u64 val);
    double get_1_rif(u32 opcode);
    double get_2_rif(u32 opcode);
    void   set_rif(u32 opcode, double val);
    double get_1_rifl(u32 opcode);
    double get_2_rifl(u32 opcode);
    void   set_rifl(u32 opcode, double val);
    u32    get_1_ci(u32 opcode);
    u32    get_2_ci(u32 opcode);
    u32    get_disp(u32 opcode);
    u32    get_disp_s(u32 opcode);
    void   cmp_s(s32 v1, s32 v2);
    void   cmp_u(u32 v1, u32 v2);
    void   concmp_s(s32 v1, s32 v2);
    void   concmp_u(u32 v1, u32 v2);
    void   cmp_d(double v1, double v2);
    void   bxx(u32 opcode, int mask);
    void   bxx_s(u32 opcode, int mask);
    void   fxx(u32 opcode, int mask);
    void   test(u32 opcode, int mask);
    double round_to_int(double val);
    void   execute_op(u32 opcode);
    void   execute_burst_stall_op(u32 opcode);
    void   take_interrupt(int vector, int lvl);
    void   check_immediate_irqs();
    void   check_pending_irqs();
    void   do_call(u32 adr, int type, u32 stack);
    void   do_ret_0();
    void   do_ret();
};

}  // namespace sm2::cpu::i960
