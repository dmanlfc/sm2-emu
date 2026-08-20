// SPDX-License-Identifier: BSD-3-Clause
//
// Zilog Z80 CPU core — interpreter.
//
// Derived from MAME's src/devices/cpu/z80/, which is BSD-3-Clause,
// copyright-holders Juergen Buchmueller, Andrei I. Holub.
//
// The instruction semantics, the deferred-flag model and the T-state costs all
// come from MAME's z80.lst (the microcode list z80make.py turns into z80.hxx)
// and from the helper members in z80.cpp. This port keeps MAME's identifier
// names and the z80.inc register macros so the two can still be diffed.
//
// What is deliberately different: MAME's generated code is a resumable state
// machine keyed on `m_ref` (prefix, opcode, step) so the CPU can be suspended
// part-way through a bus cycle, which matters for machines with contended
// memory. Nothing on a Model 2 board can do that, so here an instruction runs to
// completion and the cycle charges land at the same points, giving identical
// totals. The consequence is that a `run` slice can end a few T-states late;
// `run` carries the surplus rather than losing it.
//
// This core assumes a Zilog NMOS Z80, same as MAME. Known differences between
// Z80 variants that are therefore *not* selectable here:
//   - the LD A,I / LD A,R P/V reset glitch (NMOS only) is off, see kHasLdairQuirk
//   - OUT (C),0 writes 0 (NMOS); CMOS parts write 0xff
//   - SCF/CCF X/Y come from (F | A) & 0x28 via the Q shadow register

#include "cpu/z80/z80.h"

#include "core/log.h"

#include <cstdio>
#include <utility>

namespace sm2::cpu::z80 {

// ---------------------------------------------------------------------------
// Register aliases, from MAME's z80.inc.
//
// These exist so the instruction bodies below read the same as upstream's. They
// are #undef'd at the end of the file; nothing is included after this point.
// ---------------------------------------------------------------------------

#define PRVPC m_prvpc.w
#define PC    m_pc.w
#define SP    m_sp.w

#define Q  m_f.q
#define QT m_f.qtemp
#define I  m_i
#define R  m_r
#define R2 m_r2

#define AF m_af.w
#define A  m_af.b.h
#define F  m_af.b.l

#define BC m_bc.w
#define B  m_bc.b.h
#define C  m_bc.b.l

#define DE m_de.w
#define D  m_de.b.h
#define E  m_de.b.l

#define HL m_hl.w
#define H  m_hl.b.h
#define L  m_hl.b.l

#define WZ   m_wz.w
#define WZ_H m_wz.b.h
#define WZ_L m_wz.b.l

// 16-bit value handed between microcode steps, and its halves. TDAT8 is the
// D0..D7 staging byte: everything that reads or writes memory goes through it.
#define TDAT   m_shared_data.w
#define TDAT2  m_shared_data2.w
#define TDAT_H m_shared_data.b.h
#define TDAT_L m_shared_data.b.l
#define TDAT8  m_shared_data.b.l

// ============================================================================
// Construction / reset
// ============================================================================

Z80::Z80(Bus& bus) : m_bus(&bus)
{
    // MAME's device_start tail. set_f(0) first, because the deferred-flag
    // members are not all zero for F == 0: parity of zero is even, so pv_val
    // has to be seeded or F would come up reading 0x44 instead of 0x40. Then IX
    // and IY read 0xffff after a reset, and the zero flag comes up set.
    set_f(0);
    m_ix.w = 0xffff;
    m_iy.w = 0xffff;
    m_f.z_val = 0;
}

void Z80::reset()
{
    leave_halt();

    PC = 0;
    WZ = PC;
    I  = 0;
    R  = 0;
    R2 = 0;
    m_iff1 = false;
    m_iff2 = false;

    set_service_attention<SA_NMI_PENDING, 0>();
    set_service_attention<SA_AFTER_EI, 0>();
    set_service_attention<SA_AFTER_LDAIR, 0>();

    // Note what is *not* here, matching MAME: the register file, SP and the
    // interrupt mode survive a reset. Real silicon does force IM 0 and leaves
    // AF/SP at 0xffff, but MAME models neither and code that depends on it does
    // not exist on these boards.
    //
    // m_cycle_debt is not cleared either, and deliberately. It is not CPU state:
    // it records work already done and not yet reported to the caller. Dropping
    // it would hand the caller back cycles it has not been told about.
}

// ============================================================================
// Flag helpers (for eg. POP/PUSH AF, EX AF,AF')
// ============================================================================

u8 Z80::get_f() const
{
    u8 f = 0;
    f |= m_f.s();
    f |= m_f.z();
    f |= m_f.yx();
    f |= m_f.h();
    f |= m_f.pv();
    f |= m_f.n ? NF : 0;
    f |= m_f.c ? CF : 0;
    return f;
}

void Z80::set_f(u8 f)
{
    m_f.s_val  = f;
    m_f.z_val  = !(f & ZF);
    m_f.yx_val = f;
    m_f.h_val  = f;
    m_f.pv_val = !(f & PF);
    m_f.n      = (f & NF) != 0;
    m_f.c      = (f & CF) != 0;
}

void Z80::set_pc(u16 value)
{
    PC    = value;
    PRVPC = value;
    set_service_attention<SA_AFTER_EI, 0>();
    set_service_attention<SA_AFTER_LDAIR, 0>();
}

void Z80::set_af(u16 value)
{
    A = u8(value >> 8);
    set_f(u8(value));
}

// ============================================================================
// Halt state
// ============================================================================

void Z80::halt()
{
    if (!m_halt) {
        m_halt = 1;
        set_service_attention<SA_HALT, 1>();
    }
}

void Z80::leave_halt()
{
    if (m_halt) {
        m_halt = 0;
        set_service_attention<SA_HALT, 0>();
    }
}

// ============================================================================
// Bus access
// ============================================================================
// Each helper charges what the matching z80.lst macro charges. `m1_cycles`,
// `mreq_cycles` and `iorq_cycles` are 4/3/4 on a stock part.

u8 Z80::data_read(u16 addr)
{
    return m_bus->read8(addr);
}

void Z80::data_write(u16 addr, u8 value)
{
    m_bus->write8(addr, value);
}

u8 Z80::opcode_read()
{
    return m_bus->read8(PC);
}

u8 Z80::arg_read()
{
    return m_bus->read8(PC);
}

u8 Z80::rop()
{
    m_icount -= m_m1_cycles;
    TDAT8 = opcode_read();
    PC++;
    R++;
    // Roll the "did the last instruction write F" shadow over. SCF/CCF are the
    // only readers; see Flags::q.
    Q  = QT;
    QT = YXF;
    return TDAT8;
}

void Z80::arg()
{
    m_icount -= m_mreq_cycles;
    TDAT8 = arg_read();
    PC++;
}

u16 Z80::arg16()
{
    m_icount -= m_mreq_cycles;
    TDAT_L = arg_read();
    PC++;
    m_icount -= m_mreq_cycles;
    TDAT_H = arg_read();
    PC++;
    return TDAT;
}

void Z80::rm(u16 addr)
{
    m_icount -= m_mreq_cycles;
    TDAT8 = data_read(addr);
}

void Z80::rm_reg(u16 addr)
{
    rm(addr);
    nomreq_addr(addr, 1);
}

u16 Z80::rm16(u16 addr)
{
    m_icount -= m_mreq_cycles;
    TDAT_L = data_read(addr);
    m_icount -= m_mreq_cycles;
    TDAT_H = data_read(u16(addr + 1));
    return TDAT;
}

void Z80::wm(u16 addr)
{
    m_icount -= m_mreq_cycles;
    data_write(addr, TDAT8);
}

void Z80::wm16(u16 addr, u16 value)
{
    TDAT = value;
    m_icount -= m_mreq_cycles;
    data_write(addr, TDAT_L);
    m_icount -= m_mreq_cycles;
    data_write(u16(addr + 1), TDAT_H);
}

void Z80::wm_sp(u8 value)
{
    SP--;
    m_icount -= m_mreq_cycles;
    stack_write(SP, value);
}

void Z80::wm16_sp(u16 value)
{
    wm_sp(u8(value >> 8));
    wm_sp(u8(value));
}

u16 Z80::pop16()
{
    m_icount -= m_mreq_cycles;
    TDAT_L = stack_read(SP);
    SP++;
    m_icount -= m_mreq_cycles;
    TDAT_H = stack_read(SP);
    SP++;
    return TDAT;
}

void Z80::push16(u16 value)
{
    nomreq_ir(1);
    wm16_sp(value);
}

void Z80::in_port(u16 port)
{
    m_icount -= m_iorq_cycles;
    TDAT8 = m_bus->io_read8(port);
}

void Z80::out_port(u16 port)
{
    m_icount -= m_iorq_cycles;
    m_bus->io_write8(port, TDAT8);
}

void Z80::nomreq_addr(u16 addr, int count)
{
    // The address is what the real part drives during these cycles. MAME hands
    // it to a callback used for ULA contention modelling; nothing here wants it.
    (void)addr;
    m_icount -= count;
}

void Z80::nomreq_ir(int count)
{
    nomreq_addr(u16((I << 8) | (R2 & 0x80) | (R & 0x7f)), count);
}

// ============================================================================
// 8-bit arithmetic and logic
// ============================================================================

void Z80::inc(u8& r)
{
    ++r;
    {
        QT = 0;
        // keep C
        m_f.s_val = m_f.z_val = m_f.yx_val = r;
        m_f.pv_val = r != 0x80;
        m_f.h_val  = (r & 0x0f) == 0x00 ? HF : 0;
        m_f.n      = false;
    }
}

void Z80::dec(u8& r)
{
    --r;
    {
        QT = 0;
        // keep C
        m_f.s_val = m_f.z_val = m_f.yx_val = r;
        m_f.pv_val = r != 0x7f;
        m_f.h_val  = (r & 0x0f) == 0x0f ? HF : 0;
        m_f.n      = true;
    }
}

void Z80::rlca()
{
    A = u8((A << 1) | (A >> 7));
    {
        QT = 0;
        // keep SZP
        m_f.yx_val = A;
        m_f.h_val  = 0;
        m_f.n      = false;
        m_f.c      = (A & 0x01) != 0;
    }
}

void Z80::rrca()
{
    const u8 a0 = A;
    A = u8((a0 >> 1) | (a0 << 7));
    {
        QT = 0;
        // keep SZP
        m_f.yx_val = A;
        m_f.h_val  = 0;
        m_f.n      = false;
        m_f.c      = (a0 & 0x01) != 0;
    }
}

void Z80::rla()
{
    const u8 result = u8((A << 1) + m_f.c);
    {
        QT = 0;
        // keep SZP
        m_f.yx_val = result;
        m_f.h_val  = 0;
        m_f.n      = false;
        m_f.c      = (A & 0x80) != 0;
    }
    A = result;
}

void Z80::rra()
{
    const u8 result = u8((m_f.c << 7) | (A >> 1));
    {
        QT = 0;
        // keep SZP
        m_f.yx_val = result;
        m_f.h_val  = 0;
        m_f.n      = false;
        m_f.c      = (A & 0x01) != 0;
    }
    A = result;
}

void Z80::add_a(u8 value)
{
    const u16 result = u16(A + value);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.yx_val = u8(result);
        m_f.c      = (result & 0x100) != 0;
        m_f.h_val  = u8((A & 0x0f) + (value & 0x0f));
        m_f.pv_val = !((A ^ result) & (value ^ result) & 0x80);
        m_f.n      = false;
    }
    A = u8(result);
}

void Z80::adc_a(u8 value)
{
    const int c      = m_f.c;
    const u16 result = u16(A + value + c);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.yx_val = u8(result);
        m_f.c      = (result & 0x100) != 0;
        m_f.h_val  = u8((A & 0x0f) + (value & 0x0f) + c);
        m_f.pv_val = !((A ^ result) & (value ^ result) & 0x80);
        m_f.n      = false;
    }
    A = u8(result);
}

void Z80::sub_a(u8 value)
{
    const u16 result = u16(A - value);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.yx_val = u8(result);
        m_f.c      = (result & 0x100) != 0;
        m_f.h_val  = u8((A & 0x0f) - (value & 0x0f));
        m_f.pv_val = !((A ^ value) & (A ^ result) & 0x80);
        m_f.n      = true;
    }
    A = u8(result);
}

void Z80::sbc_a(u8 value)
{
    const int c      = m_f.c;
    const u16 result = u16(A - value - c);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.yx_val = u8(result);
        m_f.c      = (result & 0x100) != 0;
        m_f.h_val  = u8((A & 0x0f) - (value & 0x0f) - c);
        m_f.pv_val = !((A ^ value) & (A ^ result) & 0x80);
        m_f.n      = true;
    }
    A = u8(result);
}

void Z80::neg()
{
    const u8 value = A;
    A = 0;
    sub_a(value);
}

void Z80::daa()
{
    u8 adjusted = A;
    if (m_f.n) {
        if (m_f.h() || ((A & 0xf) > 9)) adjusted -= 6;
        if (m_f.c || (A > 0x99)) adjusted -= 0x60;
    } else {
        if (m_f.h() || ((A & 0xf) > 9)) adjusted += 6;
        if (m_f.c || (A > 0x99)) adjusted += 0x60;
    }
    {
        QT = 0;
        // keep N
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = adjusted;
        m_f.h_val = u8(A ^ adjusted);
        m_f.c     = m_f.c || A > 0x99;
    }
    A = adjusted;
}

void Z80::and_a(u8 value)
{
    A &= value;
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = A;
        m_f.n     = false;
        m_f.c     = false;
        m_f.h_val = HF;
    }
}

void Z80::or_a(u8 value)
{
    A |= value;
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = A;
        m_f.h_val = 0;
        m_f.n     = false;
        m_f.c     = false;
    }
}

void Z80::xor_a(u8 value)
{
    A ^= value;
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = A;
        m_f.h_val = 0;
        m_f.n     = false;
        m_f.c     = false;
    }
}

void Z80::cp(u8 value)
{
    const u16 result = u16(A - value);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = u8(result);
        // CP leaks the *operand's* bits 5 and 3, not the result's.
        m_f.yx_val = value;
        m_f.c      = (result & 0x100) != 0;
        m_f.h_val  = u8((A & 0x0f) - (value & 0x0f));
        m_f.pv_val = !((A ^ value) & (A ^ result) & 0x80);
        m_f.n      = true;
    }
}

void Z80::exx()
{
    using std::swap;
    swap(m_bc, m_bc2);
    swap(m_de, m_de2);
    swap(m_hl, m_hl2);
}

// ============================================================================
// Rotates, shifts and bit operations
// ============================================================================

u8 Z80::rlc(u8 value)
{
    const u8 result = u8((value << 1) | (value >> 7));
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = result;
        m_f.h_val = 0;
        m_f.n     = false;
        m_f.c     = (value & 0x80) != 0;
    }
    return result;
}

u8 Z80::rrc(u8 value)
{
    const u8 result = u8((value >> 1) | (value << 7));
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = result;
        m_f.h_val = 0;
        m_f.n     = false;
        m_f.c     = (value & 0x01) != 0;
    }
    return result;
}

u8 Z80::rl(u8 value)
{
    const u8 result = u8((value << 1) + m_f.c);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = result;
        m_f.h_val = 0;
        m_f.n     = false;
        m_f.c     = (value & 0x80) != 0;
    }
    return result;
}

u8 Z80::rr(u8 value)
{
    const u8 result = u8((value >> 1) | (m_f.c << 7));
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = result;
        m_f.h_val = 0;
        m_f.n     = false;
        m_f.c     = (value & 0x01) != 0;
    }
    return result;
}

u8 Z80::sla(u8 value)
{
    const u8 result = u8(value << 1);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = result;
        m_f.h_val = 0;
        m_f.n     = false;
        m_f.c     = (value & 0x80) != 0;
    }
    return result;
}

u8 Z80::sra(u8 value)
{
    const u8 result = u8((value >> 1) | (value & 0x80));
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = result;
        m_f.h_val = 0;
        m_f.n     = false;
        m_f.c     = (value & 0x01) != 0;
    }
    return result;
}

/// The undocumented CB 30..37 group. Zilog never named it; SLL/SLIA/SL1 all
/// refer to this. Shifts left and sets bit 0.
u8 Z80::sll(u8 value)
{
    const u8 result = u8((value << 1) | 0x01);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = result;
        m_f.h_val = 0;
        m_f.n     = false;
        m_f.c     = (value & 0x80) != 0;
    }
    return result;
}

u8 Z80::srl(u8 value)
{
    const u8 result = u8(value >> 1);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = m_f.pv_val = m_f.yx_val = result;
        m_f.h_val = 0;
        m_f.n     = false;
        m_f.c     = (value & 0x01) != 0;
    }
    return result;
}

u8 Z80::shift_op(int operation, u8 value)
{
    switch (operation) {
    case 0: return rlc(value);
    case 1: return rrc(value);
    case 2: return rl(value);
    case 3: return rr(value);
    case 4: return sla(value);
    case 5: return sra(value);
    case 6: return sll(value);
    default: return srl(value);
    }
}

void Z80::bit(int bit_index, u8 value)
{
    QT = 0;
    m_f.s_val = m_f.z_val = m_f.pv_val = u8(value & (1 << bit_index));
    m_f.h_val = HF;
    m_f.n     = false;
    m_f.yx_val = value;
}

/// BIT b,(HL) takes its X/Y flags from the internal address latch rather than
/// from the operand. MAME models that latch as WZ.
void Z80::bit_hl(int bit_index, u8 value)
{
    QT = 0;
    m_f.s_val = m_f.z_val = m_f.pv_val = u8(value & (1 << bit_index));
    m_f.h_val = HF;
    m_f.n     = false;
    m_f.yx_val = WZ_H;
}

void Z80::bit_xy(int bit_index, u8 value)
{
    QT = 0;
    m_f.s_val = m_f.z_val = m_f.pv_val = u8(value & (1 << bit_index));
    m_f.h_val = HF;
    m_f.n     = false;
    m_f.yx_val = u8(m_ea >> 8);
}

u8 Z80::res(int bit_index, u8 value)
{
    return u8(value & ~(1 << bit_index));
}

u8 Z80::set(int bit_index, u8 value)
{
    return u8(value | (1 << bit_index));
}

/// Flags left behind when a repeating I/O block op is interrupted part-way,
/// i.e. when INIR/OTIR/INDR/OTDR loops rather than falls through.
void Z80::block_io_interrupted_flags()
{
    m_f.yx_val = u8(PC >> 8);

    const u8 pv_old = m_f.pv();
    if (m_f.c) {
        m_f.h_val = 0;
        if (TDAT8 & 0x80) {
            m_f.pv_val = u8((B - 1) & 0x07);
            if ((B & 0x0f) == 0x00) m_f.h_val = HF;
        } else {
            m_f.pv_val = u8((B + 1) & 0x07);
            if ((B & 0x0f) == 0x0f) m_f.h_val = HF;
        }
    } else {
        m_f.pv_val = u8(B & 0x07);
    }
    m_f.pv_val = u8((pv_old ^ m_f.pv()) & PF);
}

void Z80::ei()
{
    m_iff1 = m_iff2 = true;
    set_service_attention<SA_AFTER_EI, 1>();
}

// ============================================================================
// 16-bit arithmetic
// ============================================================================

void Z80::add16(Pair16& dst, u16 src)
{
    nomreq_ir(7);
    const u32 result = u32(dst.w) + src;
    WZ = u16(dst.w + 1);
    {
        QT = 0;
        // keep SZV
        m_f.yx_val = u8(result >> 8);
        m_f.h_val  = u8((dst.w ^ result ^ src) >> 8);
        m_f.n      = false;
        m_f.c      = (result & 0x10000) != 0;
    }
    dst.w = u16(result);
}

void Z80::adc_hl(u16 src)
{
    nomreq_ir(7);
    const u32 result = u32(HL) + src + m_f.c;
    WZ = u16(HL + 1);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = u8(((result & 0x8000) >> 8) | (u16(result) != 0));
        m_f.yx_val = u8(result >> 8);
        m_f.h_val  = u8((HL ^ result ^ src) >> 8);
        m_f.pv_val = !((src ^ HL ^ 0x8000) & (src ^ result) & 0x8000);
        m_f.n      = false;
        m_f.c      = (result & 0x10000) != 0;
    }
    HL = u16(result);
}

void Z80::sbc_hl(u16 src)
{
    nomreq_ir(7);
    const u32 result = u32(HL) - src - m_f.c;
    WZ = u16(HL + 1);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = u8(((result & 0x8000) >> 8) | (u16(result) != 0));
        m_f.yx_val = u8(result >> 8);
        m_f.h_val  = u8((HL ^ result ^ src) >> 8);
        m_f.pv_val = !((src ^ HL) & (HL ^ result) & 0x8000);
        m_f.n      = true;
        m_f.c      = (result & 0x10000) != 0;
    }
    HL = u16(result);
}

void Z80::ex_sp(Pair16& dst)
{
    const u16 value = pop16();
    nomreq_addr(u16(SP - 1), 1);
    wm16_sp(dst.w);
    dst.w = value;
    nomreq_addr(SP, 2);
    WZ = value;
}

void Z80::eax(Pair16& xy)
{
    arg();
    m_ea = u16(xy.w + s8(TDAT8));
    WZ   = m_ea;
}

// ============================================================================
// Flow control
// ============================================================================

void Z80::jp()
{
    PC = arg16();
    WZ = PC;
}

void Z80::jp_cond(bool condition)
{
    if (condition) {
        PC = arg16();
        WZ = PC;
    } else {
        // Not taken still reads both operand bytes, so PC advances by two.
        WZ = arg16();
    }
}

void Z80::jr()
{
    arg();
    nomreq_addr(u16(PC - 1), 5);
    PC = u16(PC + s8(TDAT8));
    WZ = PC;
}

void Z80::jr_cond(bool condition)
{
    if (condition) {
        jr();
    } else {
        arg();
    }
}

void Z80::arg16_call()
{
    m_ea = arg16();
    nomreq_addr(u16(PC - 1), 1);
    WZ = m_ea;
    wm16_sp(PC);
    PC = m_ea;
}

void Z80::call_cond(bool condition)
{
    if (condition) {
        arg16_call();
    } else {
        WZ = arg16();
    }
}

void Z80::ret_cond(bool condition)
{
    nomreq_ir(1);
    if (condition) {
        PC = pop16();
        WZ = PC;
    }
}

void Z80::retn()
{
    PC = pop16();
    WZ = PC;
    m_iff1 = m_iff2;
}

void Z80::reti()
{
    PC = pop16();
    WZ = PC;
    m_iff1 = m_iff2;
    // MAME also walks the daisy chain here so the interrupting peripheral can
    // clear its in-service latch. There is no daisy chain in this port; a board
    // that needs one can watch for the opcode through the trace hook.
}

void Z80::rst(u16 address)
{
    push16(PC);
    PC = address;
    WZ = PC;
}

// ============================================================================
// I and R register transfers
// ============================================================================

void Z80::ld_r_a()
{
    nomreq_ir(1);
    R  = A;
    R2 = A & 0x80;  // keep bit 7 of r
}

void Z80::ld_a_r()
{
    nomreq_ir(1);
    A = u8((R & 0x7f) | R2);
    {
        QT = 0;
        // keep C
        m_f.s_val = m_f.z_val = A;
        m_f.yx_val = A;
        m_f.h_val  = 0;
        m_f.n      = false;
        m_f.pv_val = !m_iff2;
    }
    if constexpr (kHasLdairQuirk) {
        set_service_attention<SA_AFTER_LDAIR, 1>();
    }
}

void Z80::ld_i_a()
{
    nomreq_ir(1);
    I = A;
}

void Z80::ld_a_i()
{
    nomreq_ir(1);
    A = I;
    {
        QT = 0;
        // keep C
        m_f.s_val = m_f.z_val = A;
        m_f.yx_val = A;
        m_f.h_val  = 0;
        m_f.n      = false;
        m_f.pv_val = !m_iff2;
    }
    if constexpr (kHasLdairQuirk) {
        set_service_attention<SA_AFTER_LDAIR, 1>();
    }
}

void Z80::rrd()
{
    rm(HL);
    WZ = u16(HL + 1);
    nomreq_addr(HL, 4);
    TDAT_H = TDAT8;
    TDAT8  = u8((TDAT8 >> 4) | (A << 4));
    wm(HL);
    A = u8((A & 0xf0) | (TDAT_H & 0x0f));
    {
        QT = 0;
        // keep C
        m_f.s_val = m_f.z_val = m_f.pv_val = A;
        m_f.yx_val = A;
        m_f.h_val  = 0;
        m_f.n      = false;
    }
}

void Z80::rld()
{
    rm(HL);
    WZ = u16(HL + 1);
    nomreq_addr(HL, 4);
    TDAT_H = TDAT8;
    TDAT8  = u8((TDAT8 << 4) | (A & 0x0f));
    wm(HL);
    A = u8((A & 0xf0) | (TDAT_H >> 4));
    {
        QT = 0;
        // keep C
        m_f.s_val = m_f.z_val = m_f.pv_val = A;
        m_f.yx_val = A;
        m_f.h_val  = 0;
        m_f.n      = false;
    }
}

// ============================================================================
// Block transfer, compare and I/O
// ============================================================================

void Z80::ldi()
{
    rm(HL);
    wm(DE);
    nomreq_addr(DE, 2);
    HL++;
    DE++;
    BC--;
    {
        QT = 0;
        // keep SZC
        m_f.yx_val = u8(((A + TDAT8) << 4) | ((A + TDAT8) & 0x0f));
        m_f.h_val  = 0;
        m_f.n      = false;
        m_f.pv_val = !BC;
    }
}

void Z80::cpi()
{
    rm(HL);
    nomreq_addr(HL, 5);
    WZ++;
    HL++;
    BC--;
    u8 result = u8(A - TDAT8);
    {
        QT = 0;
        // keep C
        m_f.s_val = m_f.z_val = result;
        m_f.h_val = u8(A ^ TDAT8 ^ result);
        if (m_f.h()) result -= 1;
        m_f.yx_val = u8((result << 4) | (result & 0x0f));
        m_f.pv_val = !BC;
        m_f.n      = true;
    }
}

void Z80::ini()
{
    nomreq_ir(1);
    in_port(BC);
    WZ = u16(BC + 1);
    B--;
    wm(HL);
    HL++;
    const u16 t = u16(((C + 1) & 0xff) + TDAT8);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = B;
        m_f.yx_val = B;
        m_f.h_val  = u8(t >> 4);
        m_f.pv_val = u8((t & 0x07) ^ B);
        m_f.n      = (TDAT8 & SF) != 0;
        m_f.c      = (t & 0x100) != 0;
    }
}

void Z80::outi()
{
    nomreq_ir(1);
    rm(HL);
    B--;
    WZ = u16(BC + 1);
    out_port(BC);
    HL++;
    const u16 t = u16(L + TDAT8);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = B;
        m_f.yx_val = B;
        m_f.h_val  = u8(t >> 4);
        m_f.pv_val = u8((t & 0x07) ^ B);
        m_f.n      = (TDAT8 & SF) != 0;
        m_f.c      = (t & 0x100) != 0;
    }
}

void Z80::ldd()
{
    rm(HL);
    wm(DE);
    nomreq_addr(DE, 2);
    HL--;
    DE--;
    BC--;
    {
        QT = 0;
        // keep SZC
        m_f.yx_val = u8(((A + TDAT8) << 4) | ((A + TDAT8) & 0x0f));
        m_f.h_val  = 0;
        m_f.n      = false;
        m_f.pv_val = !BC;
    }
}

void Z80::cpd()
{
    rm(HL);
    nomreq_addr(HL, 5);
    WZ--;
    HL--;
    BC--;
    u8 result = u8(A - TDAT8);
    {
        QT = 0;
        // keep C
        m_f.s_val = m_f.z_val = result;
        m_f.h_val = u8(A ^ TDAT8 ^ result);
        if (m_f.h()) result -= 1;
        m_f.yx_val = u8((result << 4) | (result & 0x0f));
        m_f.pv_val = !BC;
        m_f.n      = true;
    }
}

void Z80::ind()
{
    nomreq_ir(1);
    in_port(BC);
    WZ = u16(BC - 1);
    B--;
    wm(HL);
    HL--;
    const u16 t = u16(((C - 1) & 0xff) + TDAT8);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = B;
        m_f.yx_val = B;
        m_f.h_val  = u8(t >> 4);
        m_f.pv_val = u8((t & 0x07) ^ B);
        m_f.n      = (TDAT8 & SF) != 0;
        m_f.c      = (t & 0x100) != 0;
    }
}

void Z80::outd()
{
    nomreq_ir(1);
    rm(HL);
    B--;
    WZ = u16(BC - 1);
    out_port(BC);
    HL--;
    const u16 t = u16(L + TDAT8);
    {
        QT = 0;
        m_f.s_val = m_f.z_val = B;
        m_f.yx_val = B;
        m_f.h_val  = u8(t >> 4);
        m_f.pv_val = u8((t & 0x07) ^ B);
        m_f.n      = (TDAT8 & SF) != 0;
        m_f.c      = (t & 0x100) != 0;
    }
}

// ============================================================================
// Undocumented-opcode diagnostics
// ============================================================================
// MAME logs these through a maskable channel that is on by default. A DD or FD
// prefix in front of an opcode that has no index-register form is common enough
// in real code that warning about it would be noise, so it lands on the
// development channel instead.

void Z80::illegal_1(u8 prefix, u8 op)
{
    SM2_DEBUG("z80: ill. opcode $%02x $%02x ($%04x)", prefix, op, u16(PC - 2));
}

void Z80::illegal_2(u8 op)
{
    SM2_DEBUG("z80: ill. opcode $ed $%02x ($%04x)", op, u16(PC - 2));
}

// ============================================================================
// Register-field decoding
// ============================================================================

u8* Z80::reg8_ptr(int index)
{
    switch (index) {
    case 0: return &B;
    case 1: return &C;
    case 2: return &D;
    case 3: return &E;
    case 4: return &H;
    case 5: return &L;
    // Field value 6 means "the memory operand", which every caller has already
    // staged in TDAT8. Returning it keeps the CB and DDCB bodies uniform.
    case 6: return &TDAT8;
    default: return &A;
    }
}

// ============================================================================
// Main opcode table (no prefix)
// ============================================================================
// The four prefix opcodes (CB, DD, ED, FD) are consumed by execute_one() and
// never reach here.

void Z80::op_main(u8 op)
{
    switch (op) {
    case 0x00: break;  // NOP
    case 0x01: BC = arg16(); break;                                 // LD   BC,w
    case 0x02:                                                      // LD   (BC),A
        TDAT8 = A;
        wm(BC);
        WZ_L = u8((BC + 1) & 0xff);
        WZ_H = A;
        break;
    case 0x03: nomreq_ir(2); BC++; break;                           // INC  BC
    case 0x04: inc(B); break;                                       // INC  B
    case 0x05: dec(B); break;                                       // DEC  B
    case 0x06: arg(); B = TDAT8; break;                             // LD   B,n
    case 0x07: rlca(); break;                                       // RLCA
    case 0x08: {                                                    // EX   AF,AF'
        using std::swap;
        F = get_f();
        swap(m_af, m_af2);
        set_f(F);
        break;
    }
    case 0x09: add16(m_hl, BC); break;                              // ADD  HL,BC
    case 0x0a: rm(BC); A = TDAT8; WZ = u16(BC + 1); break;          // LD   A,(BC)
    case 0x0b: nomreq_ir(2); BC--; break;                           // DEC  BC
    case 0x0c: inc(C); break;                                       // INC  C
    case 0x0d: dec(C); break;                                       // DEC  C
    case 0x0e: arg(); C = TDAT8; break;                             // LD   C,n
    case 0x0f: rrca(); break;                                       // RRCA

    case 0x10: nomreq_ir(1); jr_cond(--B != 0); break;              // DJNZ o
    case 0x11: DE = arg16(); break;                                 // LD   DE,w
    case 0x12:                                                      // LD   (DE),A
        TDAT8 = A;
        wm(DE);
        WZ_L = u8((DE + 1) & 0xff);
        WZ_H = A;
        break;
    case 0x13: nomreq_ir(2); DE++; break;                           // INC  DE
    case 0x14: inc(D); break;                                       // INC  D
    case 0x15: dec(D); break;                                       // DEC  D
    case 0x16: arg(); D = TDAT8; break;                             // LD   D,n
    case 0x17: rla(); break;                                        // RLA
    case 0x18: jr(); break;                                         // JR   o
    case 0x19: add16(m_hl, DE); break;                              // ADD  HL,DE
    case 0x1a: rm(DE); A = TDAT8; WZ = u16(DE + 1); break;          // LD   A,(DE)
    case 0x1b: nomreq_ir(2); DE--; break;                           // DEC  DE
    case 0x1c: inc(E); break;                                       // INC  E
    case 0x1d: dec(E); break;                                       // DEC  E
    case 0x1e: arg(); E = TDAT8; break;                             // LD   E,n
    case 0x1f: rra(); break;                                        // RRA

    case 0x20: jr_cond(!m_f.z()); break;                            // JR   NZ,o
    case 0x21: HL = arg16(); break;                                 // LD   HL,w
    case 0x22: {                                                    // LD   (w),HL
        const u16 ea = arg16();
        wm16(ea, HL);
        WZ = u16(ea + 1);
        break;
    }
    case 0x23: nomreq_ir(2); HL++; break;                           // INC  HL
    case 0x24: inc(H); break;                                       // INC  H
    case 0x25: dec(H); break;                                       // DEC  H
    case 0x26: arg(); H = TDAT8; break;                             // LD   H,n
    case 0x27: daa(); break;                                        // DAA
    case 0x28: jr_cond(m_f.z()); break;                             // JR   Z,o
    case 0x29: add16(m_hl, HL); break;                              // ADD  HL,HL
    case 0x2a: {                                                    // LD   HL,(w)
        const u16 ea = arg16();
        HL = rm16(ea);
        WZ = u16(ea + 1);
        break;
    }
    case 0x2b: nomreq_ir(2); HL--; break;                           // DEC  HL
    case 0x2c: inc(L); break;                                       // INC  L
    case 0x2d: dec(L); break;                                       // DEC  L
    case 0x2e: arg(); L = TDAT8; break;                             // LD   L,n
    case 0x2f:                                                      // CPL
        A ^= 0xff;
        {
            // keep SZPC. Note MAME does not clear QT here, so an SCF or CCF
            // immediately after a CPL still sees the previous instruction's
            // X/Y bits. Kept as upstream has it.
            m_f.yx_val = A;
            m_f.h_val  = HF;
            m_f.n      = true;
        }
        break;

    case 0x30: jr_cond(!m_f.c); break;                              // JR   NC,o
    case 0x31: SP = arg16(); break;                                 // LD   SP,w
    case 0x32: {                                                    // LD   (w),A
        const u16 ea = arg16();
        TDAT8 = A;
        wm(ea);
        WZ_L = u8((ea + 1) & 0xff);
        WZ_H = A;
        break;
    }
    case 0x33: nomreq_ir(2); SP++; break;                           // INC  SP
    case 0x34: rm_reg(HL); inc(TDAT8); wm(HL); break;               // INC  (HL)
    case 0x35: rm_reg(HL); dec(TDAT8); wm(HL); break;               // DEC  (HL)
    case 0x36: arg(); wm(HL); break;                                // LD   (HL),n
    case 0x37: {                                                    // SCF
        QT = 0;
        // keep SZP
        m_f.yx_val = u8((m_f.yx_val & Q) | A);
        m_f.h_val  = 0;
        m_f.n      = false;
        m_f.c      = true;
        break;
    }
    case 0x38: jr_cond(m_f.c); break;                               // JR   C,o
    case 0x39: add16(m_hl, SP); break;                              // ADD  HL,SP
    case 0x3a: {                                                    // LD   A,(w)
        const u16 ea = arg16();
        rm(ea);
        A  = TDAT8;
        WZ = u16(ea + 1);
        break;
    }
    case 0x3b: nomreq_ir(2); SP--; break;                           // DEC  SP
    case 0x3c: inc(A); break;                                       // INC  A
    case 0x3d: dec(A); break;                                       // DEC  A
    case 0x3e: arg(); A = TDAT8; break;                             // LD   A,n
    case 0x3f: {                                                    // CCF
        QT = 0;
        // keep SZP
        m_f.yx_val = u8((m_f.yx_val & Q) | A);
        m_f.h_val  = u8(m_f.c << 4);
        m_f.n      = false;
        m_f.c      = !m_f.c;
        break;
    }

    case 0x40: break;                                               // LD   B,B
    case 0x41: B = C; break;
    case 0x42: B = D; break;
    case 0x43: B = E; break;
    case 0x44: B = H; break;
    case 0x45: B = L; break;
    case 0x46: rm(HL); B = TDAT8; break;                            // LD   B,(HL)
    case 0x47: B = A; break;
    case 0x48: C = B; break;
    case 0x49: break;                                               // LD   C,C
    case 0x4a: C = D; break;
    case 0x4b: C = E; break;
    case 0x4c: C = H; break;
    case 0x4d: C = L; break;
    case 0x4e: rm(HL); C = TDAT8; break;                            // LD   C,(HL)
    case 0x4f: C = A; break;

    case 0x50: D = B; break;
    case 0x51: D = C; break;
    case 0x52: break;                                               // LD   D,D
    case 0x53: D = E; break;
    case 0x54: D = H; break;
    case 0x55: D = L; break;
    case 0x56: rm(HL); D = TDAT8; break;                            // LD   D,(HL)
    case 0x57: D = A; break;
    case 0x58: E = B; break;
    case 0x59: E = C; break;
    case 0x5a: E = D; break;
    case 0x5b: break;                                               // LD   E,E
    case 0x5c: E = H; break;
    case 0x5d: E = L; break;
    case 0x5e: rm(HL); E = TDAT8; break;                            // LD   E,(HL)
    case 0x5f: E = A; break;

    case 0x60: H = B; break;
    case 0x61: H = C; break;
    case 0x62: H = D; break;
    case 0x63: H = E; break;
    case 0x64: break;                                               // LD   H,H
    case 0x65: H = L; break;
    case 0x66: rm(HL); H = TDAT8; break;                            // LD   H,(HL)
    case 0x67: H = A; break;
    case 0x68: L = B; break;
    case 0x69: L = C; break;
    case 0x6a: L = D; break;
    case 0x6b: L = E; break;
    case 0x6c: L = H; break;
    case 0x6d: break;                                               // LD   L,L
    case 0x6e: rm(HL); L = TDAT8; break;                            // LD   L,(HL)
    case 0x6f: L = A; break;

    case 0x70: TDAT8 = B; wm(HL); break;                            // LD   (HL),B
    case 0x71: TDAT8 = C; wm(HL); break;
    case 0x72: TDAT8 = D; wm(HL); break;
    case 0x73: TDAT8 = E; wm(HL); break;
    case 0x74: TDAT8 = H; wm(HL); break;
    case 0x75: TDAT8 = L; wm(HL); break;
    case 0x76: halt(); break;                                       // HALT
    case 0x77: TDAT8 = A; wm(HL); break;                            // LD   (HL),A
    case 0x78: A = B; break;
    case 0x79: A = C; break;
    case 0x7a: A = D; break;
    case 0x7b: A = E; break;
    case 0x7c: A = H; break;
    case 0x7d: A = L; break;
    case 0x7e: rm(HL); A = TDAT8; break;                            // LD   A,(HL)
    case 0x7f: break;                                               // LD   A,A

    case 0x80: add_a(B); break;
    case 0x81: add_a(C); break;
    case 0x82: add_a(D); break;
    case 0x83: add_a(E); break;
    case 0x84: add_a(H); break;
    case 0x85: add_a(L); break;
    case 0x86: rm(HL); add_a(TDAT8); break;                         // ADD  A,(HL)
    case 0x87: add_a(A); break;
    case 0x88: adc_a(B); break;
    case 0x89: adc_a(C); break;
    case 0x8a: adc_a(D); break;
    case 0x8b: adc_a(E); break;
    case 0x8c: adc_a(H); break;
    case 0x8d: adc_a(L); break;
    case 0x8e: rm(HL); adc_a(TDAT8); break;                         // ADC  A,(HL)
    case 0x8f: adc_a(A); break;

    case 0x90: sub_a(B); break;
    case 0x91: sub_a(C); break;
    case 0x92: sub_a(D); break;
    case 0x93: sub_a(E); break;
    case 0x94: sub_a(H); break;
    case 0x95: sub_a(L); break;
    case 0x96: rm(HL); sub_a(TDAT8); break;                         // SUB  (HL)
    case 0x97: sub_a(A); break;
    case 0x98: sbc_a(B); break;
    case 0x99: sbc_a(C); break;
    case 0x9a: sbc_a(D); break;
    case 0x9b: sbc_a(E); break;
    case 0x9c: sbc_a(H); break;
    case 0x9d: sbc_a(L); break;
    case 0x9e: rm(HL); sbc_a(TDAT8); break;                         // SBC  A,(HL)
    case 0x9f: sbc_a(A); break;

    case 0xa0: and_a(B); break;
    case 0xa1: and_a(C); break;
    case 0xa2: and_a(D); break;
    case 0xa3: and_a(E); break;
    case 0xa4: and_a(H); break;
    case 0xa5: and_a(L); break;
    case 0xa6: rm(HL); and_a(TDAT8); break;                         // AND  (HL)
    case 0xa7: and_a(A); break;
    case 0xa8: xor_a(B); break;
    case 0xa9: xor_a(C); break;
    case 0xaa: xor_a(D); break;
    case 0xab: xor_a(E); break;
    case 0xac: xor_a(H); break;
    case 0xad: xor_a(L); break;
    case 0xae: rm(HL); xor_a(TDAT8); break;                         // XOR  (HL)
    case 0xaf: xor_a(A); break;

    case 0xb0: or_a(B); break;
    case 0xb1: or_a(C); break;
    case 0xb2: or_a(D); break;
    case 0xb3: or_a(E); break;
    case 0xb4: or_a(H); break;
    case 0xb5: or_a(L); break;
    case 0xb6: rm(HL); or_a(TDAT8); break;                          // OR   (HL)
    case 0xb7: or_a(A); break;
    case 0xb8: cp(B); break;
    case 0xb9: cp(C); break;
    case 0xba: cp(D); break;
    case 0xbb: cp(E); break;
    case 0xbc: cp(H); break;
    case 0xbd: cp(L); break;
    case 0xbe: rm(HL); cp(TDAT8); break;                            // CP   (HL)
    case 0xbf: cp(A); break;

    case 0xc0: ret_cond(!m_f.z()); break;                           // RET  NZ
    case 0xc1: BC = pop16(); break;                                 // POP  BC
    case 0xc2: jp_cond(!m_f.z()); break;                            // JP   NZ,a
    case 0xc3: jp(); break;                                         // JP   a
    case 0xc4: call_cond(!m_f.z()); break;                          // CALL NZ,a
    case 0xc5: push16(BC); break;                                   // PUSH BC
    case 0xc6: arg(); add_a(TDAT8); break;                          // ADD  A,n
    case 0xc7: rst(0x00); break;                                    // RST  0
    case 0xc8: ret_cond(m_f.z()); break;                            // RET  Z
    case 0xc9: PC = pop16(); WZ = PC; break;                        // RET
    case 0xca: jp_cond(m_f.z()); break;                             // JP   Z,a
    case 0xcc: call_cond(m_f.z()); break;                           // CALL Z,a
    case 0xcd: arg16_call(); break;                                 // CALL a
    case 0xce: arg(); adc_a(TDAT8); break;                          // ADC  A,n
    case 0xcf: rst(0x08); break;                                    // RST  1

    case 0xd0: ret_cond(!m_f.c); break;                             // RET  NC
    case 0xd1: DE = pop16(); break;                                 // POP  DE
    case 0xd2: jp_cond(!m_f.c); break;                              // JP   NC,a
    case 0xd3:                                                      // OUT  (n),A
        arg();
        TDAT2 = u16(TDAT8 | (A << 8));
        TDAT8 = A;
        out_port(TDAT2);
        WZ_L = u8(((TDAT2 & 0xff) + 1) & 0xff);
        WZ_H = A;
        break;
    case 0xd4: call_cond(!m_f.c); break;                            // CALL NC,a
    case 0xd5: push16(DE); break;                                   // PUSH DE
    case 0xd6: arg(); sub_a(TDAT8); break;                          // SUB  n
    case 0xd7: rst(0x10); break;                                    // RST  2
    case 0xd8: ret_cond(m_f.c); break;                              // RET  C
    case 0xd9: exx(); break;                                        // EXX
    case 0xda: jp_cond(m_f.c); break;                               // JP   C,a
    case 0xdb:                                                      // IN   A,(n)
        arg();
        TDAT2 = u16(TDAT8 | (A << 8));
        in_port(TDAT2);
        A  = TDAT8;
        WZ = u16(TDAT2 + 1);
        break;
    case 0xdc: call_cond(m_f.c); break;                             // CALL C,a
    case 0xde: arg(); sbc_a(TDAT8); break;                          // SBC  A,n
    case 0xdf: rst(0x18); break;                                    // RST  3

    case 0xe0: ret_cond(!m_f.pv()); break;                          // RET  PO
    case 0xe1: HL = pop16(); break;                                 // POP  HL
    case 0xe2: jp_cond(!m_f.pv()); break;                           // JP   PO,a
    case 0xe3: ex_sp(m_hl); break;                                  // EX   HL,(SP)
    case 0xe4: call_cond(!m_f.pv()); break;                         // CALL PO,a
    case 0xe5: push16(HL); break;                                   // PUSH HL
    case 0xe6: arg(); and_a(TDAT8); break;                          // AND  n
    case 0xe7: rst(0x20); break;                                    // RST  4
    case 0xe8: ret_cond(m_f.pv()); break;                           // RET  PE
    case 0xe9: PC = HL; break;                                      // JP   (HL)
    case 0xea: jp_cond(m_f.pv()); break;                            // JP   PE,a
    case 0xeb: {                                                    // EX   DE,HL
        using std::swap;
        swap(DE, HL);
        break;
    }
    case 0xec: call_cond(m_f.pv()); break;                          // CALL PE,a
    case 0xee: arg(); xor_a(TDAT8); break;                          // XOR  n
    case 0xef: rst(0x28); break;                                    // RST  5

    case 0xf0: ret_cond(!m_f.s()); break;                           // RET  P
    case 0xf1:                                                      // POP  AF
        (void)pop16();
        A = TDAT_H;
        set_f(TDAT_L);
        break;
    case 0xf2: jp_cond(!m_f.s()); break;                            // JP   P,a
    case 0xf3: m_iff1 = m_iff2 = false; break;                      // DI
    case 0xf4: call_cond(!m_f.s()); break;                          // CALL P,a
    case 0xf5:                                                      // PUSH AF
        nomreq_ir(1);
        wm_sp(A);
        wm_sp(get_f());
        break;
    case 0xf6: arg(); or_a(TDAT8); break;                           // OR   n
    case 0xf7: rst(0x30); break;                                    // RST  6
    case 0xf8: ret_cond(m_f.s()); break;                            // RET  M
    case 0xf9: nomreq_ir(2); SP = HL; break;                        // LD   SP,HL
    case 0xfa: jp_cond(m_f.s()); break;                             // JP   M,a
    case 0xfb: ei(); break;                                         // EI
    case 0xfc: call_cond(m_f.s()); break;                           // CALL M,a
    case 0xfe: arg(); cp(TDAT8); break;                             // CP   n
    case 0xff: rst(0x38); break;                                    // RST  7

    default: break;  // 0xcb / 0xdd / 0xed / 0xfd, handled by execute_one()
    }
}

// ============================================================================
// CB prefix — rotate, shift and bit operations
// ============================================================================

void Z80::op_cb(u8 op)
{
    const int  group = op >> 6;
    const int  y     = (op >> 3) & 7;  // shift selector, or bit index
    const bool mem   = (op & 7) == 6;
    u8* const  r     = reg8_ptr(op & 7);

    if (mem) rm_reg(HL);

    switch (group) {
    case 0: *r = shift_op(y, *r); break;
    case 1: mem ? bit_hl(y, *r) : bit(y, *r); return;  // BIT never writes back
    case 2: *r = res(y, *r); break;
    default: *r = set(y, *r); break;
    }

    if (mem) wm(HL);
}

// ============================================================================
// DD/FD CB prefix — the (IX+o) / (IY+o) bit group
// ============================================================================
// The register field is not encodable in the documented forms: assemblers only
// emit 6 there. Every other value additionally copies the result into that
// register, and the BIT forms ignore the field entirely.

void Z80::op_xycb(u8 op)
{
    const int group = op >> 6;
    const int y     = (op >> 3) & 7;
    const int reg   = op & 7;

    rm_reg(m_ea);

    switch (group) {
    case 0: TDAT8 = shift_op(y, TDAT8); break;
    case 1: bit_xy(y, TDAT8); return;  // BIT never writes back
    case 2: TDAT8 = res(y, TDAT8); break;
    default: TDAT8 = set(y, TDAT8); break;
    }

    if (reg != 6) *reg8_ptr(reg) = TDAT8;
    wm(m_ea);
}

// ============================================================================
// ED prefix
// ============================================================================

void Z80::op_ed(u8 op)
{
    switch (op) {
    // -- IN r,(C) --------------------------------------------------------
    // The 0x70 form has no destination register: it sets the flags and drops
    // the byte. Assemblers spell it IN (C) or IN F,(C).
    case 0x40: case 0x48: case 0x50: case 0x58:
    case 0x60: case 0x68: case 0x70: case 0x78: {
        in_port(BC);
        {
            QT = 0;
            // keep C
            m_f.s_val = m_f.z_val = m_f.pv_val = TDAT8;
            m_f.yx_val = TDAT8;
            m_f.h_val  = 0;
            m_f.n      = false;
        }
        const int reg = (op >> 3) & 7;
        if (reg != 6) *reg8_ptr(reg) = TDAT8;
        WZ = u16(BC + 1);
        break;
    }

    // -- OUT (C),r -------------------------------------------------------
    // The 0x71 form writes 0 on an NMOS part, which is what this core is.
    case 0x41: case 0x49: case 0x51: case 0x59:
    case 0x61: case 0x69: case 0x71: case 0x79: {
        const int reg = (op >> 3) & 7;
        TDAT8 = (reg == 6) ? 0 : *reg8_ptr(reg);
        out_port(BC);
        WZ = u16(BC + 1);
        break;
    }

    case 0x42: sbc_hl(BC); break;                                   // SBC  HL,BC
    case 0x52: sbc_hl(DE); break;                                   // SBC  HL,DE
    case 0x62: sbc_hl(HL); break;                                   // SBC  HL,HL
    case 0x72: sbc_hl(SP); break;                                   // SBC  HL,SP

    case 0x4a: adc_hl(BC); break;                                   // ADC  HL,BC
    case 0x5a: adc_hl(DE); break;                                   // ADC  HL,DE
    case 0x6a: adc_hl(HL); break;                                   // ADC  HL,HL
    case 0x7a: adc_hl(SP); break;                                   // ADC  HL,SP

    // -- LD (w),dd / LD dd,(w) -------------------------------------------
    case 0x43: case 0x53: case 0x63: case 0x73: {                   // LD   (w),dd
        const u16 ea = arg16();
        u16 value = 0;
        switch ((op >> 4) & 3) {
        case 0: value = BC; break;
        case 1: value = DE; break;
        case 2: value = HL; break;
        default: value = SP; break;
        }
        wm16(ea, value);
        WZ = u16(ea + 1);
        break;
    }
    case 0x4b: case 0x5b: case 0x6b: case 0x7b: {                   // LD   dd,(w)
        const u16 ea    = arg16();
        const u16 value = rm16(ea);
        switch ((op >> 4) & 3) {
        case 0: BC = value; break;
        case 1: DE = value; break;
        case 2: HL = value; break;
        default: SP = value; break;
        }
        WZ = u16(ea + 1);
        break;
    }

    case 0x44: case 0x4c: case 0x54: case 0x5c:                     // NEG
    case 0x64: case 0x6c: case 0x74: case 0x7c:
        neg();
        break;

    case 0x45: case 0x55: case 0x65: case 0x75: retn(); break;      // RETN
    case 0x4d: case 0x5d: case 0x6d: case 0x7d: reti(); break;      // RETI

    case 0x46: case 0x4e: case 0x66: case 0x6e: m_im = 0; break;    // IM   0
    case 0x56: case 0x76: m_im = 1; break;                          // IM   1
    case 0x5e: case 0x7e: m_im = 2; break;                          // IM   2

    case 0x47: ld_i_a(); break;                                     // LD   i,A
    case 0x4f: ld_r_a(); break;                                     // LD   r,A
    case 0x57: ld_a_i(); break;                                     // LD   A,i
    case 0x5f: ld_a_r(); break;                                     // LD   A,r

    case 0x67: rrd(); break;                                        // RRD  (HL)
    case 0x6f: rld(); break;                                        // RLD  (HL)

    case 0xa0: ldi(); break;                                        // LDI
    case 0xa1: cpi(); break;                                        // CPI
    case 0xa2: ini(); break;                                        // INI
    case 0xa3: outi(); break;                                       // OUTI
    case 0xa8: ldd(); break;                                        // LDD
    case 0xa9: cpd(); break;                                        // CPD
    case 0xaa: ind(); break;                                        // IND
    case 0xab: outd(); break;                                       // OUTD

    case 0xb0:                                                      // LDIR
        ldi();
        if (BC != 0) {
            nomreq_addr(DE, 5);
            PC = u16(PC - 2);
            WZ = u16(PC + 1);
            m_f.yx_val = u8(PC >> 8);
        }
        break;
    case 0xb1:                                                      // CPIR
        cpi();
        if (BC != 0 && !m_f.z()) {
            nomreq_addr(HL, 5);
            PC = u16(PC - 2);
            WZ = u16(PC + 1);
            m_f.yx_val = u8(PC >> 8);
        }
        break;
    case 0xb2:                                                      // INIR
        ini();
        if (B != 0) {
            nomreq_addr(HL, 5);
            PC = u16(PC - 2);
            block_io_interrupted_flags();
        }
        break;
    case 0xb3:                                                      // OTIR
        outi();
        if (B != 0) {
            nomreq_addr(BC, 5);
            PC = u16(PC - 2);
            block_io_interrupted_flags();
        }
        break;
    case 0xb8:                                                      // LDDR
        ldd();
        if (BC != 0) {
            nomreq_addr(DE, 5);
            PC = u16(PC - 2);
            WZ = u16(PC + 1);
            m_f.yx_val = u8(PC >> 8);
        }
        break;
    case 0xb9:                                                      // CPDR
        cpd();
        if (BC != 0 && !m_f.z()) {
            nomreq_addr(HL, 5);
            PC = u16(PC - 2);
            WZ = u16(PC + 1);
            m_f.yx_val = u8(PC >> 8);
        }
        break;
    case 0xba:                                                      // INDR
        ind();
        if (B != 0) {
            nomreq_addr(HL, 5);
            PC = u16(PC - 2);
            block_io_interrupted_flags();
        }
        break;
    case 0xbb:                                                      // OTDR
        outd();
        if (B != 0) {
            nomreq_addr(BC, 5);
            PC = u16(PC - 2);
            block_io_interrupted_flags();
        }
        break;

    // Everything else in the ED page is a two-byte no-op.
    default: illegal_2(op); break;
    }
}

// ============================================================================
// DD / FD prefix — the index-register opcodes
// ============================================================================
// `xy` is IX for DD and IY for FD. Returning false means the prefix has no
// effect on this opcode and the caller must run it from the main table, which
// is what the hardware does: the prefix costs its four T-states and is dropped.

bool Z80::op_index(u8 prefix, u8 op, Pair16& xy)
{
    u8& xh = xy.b.h;  // HX / HY
    u8& xl = xy.b.l;  // LX / LY

    switch (op) {
    case 0x09: add16(xy, BC); break;                                // ADD  IX,BC
    case 0x19: add16(xy, DE); break;                                // ADD  IX,DE
    case 0x29: add16(xy, xy.w); break;                              // ADD  IX,IX
    case 0x39: add16(xy, SP); break;                                // ADD  IX,SP

    case 0x21: xy.w = arg16(); break;                               // LD   IX,w
    case 0x22: {                                                    // LD   (w),IX
        const u16 ea = arg16();
        wm16(ea, xy.w);
        WZ = u16(ea + 1);
        break;
    }
    case 0x2a: {                                                    // LD   IX,(w)
        const u16 ea = arg16();
        xy.w = rm16(ea);
        WZ   = u16(ea + 1);
        break;
    }
    case 0x23: nomreq_ir(2); xy.w++; break;                         // INC  IX
    case 0x2b: nomreq_ir(2); xy.w--; break;                         // DEC  IX

    case 0x24: inc(xh); break;                                      // INC  HX
    case 0x25: dec(xh); break;                                      // DEC  HX
    case 0x26: arg(); xh = TDAT8; break;                            // LD   HX,n
    case 0x2c: inc(xl); break;                                      // INC  LX
    case 0x2d: dec(xl); break;                                      // DEC  LX
    case 0x2e: arg(); xl = TDAT8; break;                            // LD   LX,n

    case 0x34:                                                      // INC  (IX+o)
        eax(xy);
        nomreq_addr(u16(PC - 1), 5);
        rm_reg(m_ea);
        inc(TDAT8);
        wm(m_ea);
        break;
    case 0x35:                                                      // DEC  (IX+o)
        eax(xy);
        nomreq_addr(u16(PC - 1), 5);
        rm_reg(m_ea);
        dec(TDAT8);
        wm(m_ea);
        break;
    case 0x36:                                                      // LD   (IX+o),n
        eax(xy);
        arg();
        nomreq_addr(u16(PC - 1), 2);
        wm(m_ea);
        break;

    case 0x44: B = xh; break;                                       // LD   B,HX
    case 0x45: B = xl; break;                                       // LD   B,LX
    case 0x4c: C = xh; break;
    case 0x4d: C = xl; break;
    case 0x54: D = xh; break;
    case 0x55: D = xl; break;
    case 0x5c: E = xh; break;
    case 0x5d: E = xl; break;
    case 0x7c: A = xh; break;
    case 0x7d: A = xl; break;

    case 0x60: xh = B; break;                                       // LD   HX,B
    case 0x61: xh = C; break;
    case 0x62: xh = D; break;
    case 0x63: xh = E; break;
    case 0x64: break;                                               // LD   HX,HX
    case 0x65: xh = xl; break;                                      // LD   HX,LX
    case 0x67: xh = A; break;
    case 0x68: xl = B; break;                                       // LD   LX,B
    case 0x69: xl = C; break;
    case 0x6a: xl = D; break;
    case 0x6b: xl = E; break;
    case 0x6c: xl = xh; break;                                      // LD   LX,HX
    case 0x6d: break;                                               // LD   LX,LX
    case 0x6f: xl = A; break;

    // -- LD r,(IX+o). Note the destination is the real H and L, not HX/LX.
    case 0x46: case 0x4e: case 0x56: case 0x5e:
    case 0x66: case 0x6e: case 0x7e: {
        eax(xy);
        nomreq_addr(u16(PC - 1), 5);
        rm(m_ea);
        *reg8_ptr((op >> 3) & 7) = TDAT8;
        break;
    }

    // -- LD (IX+o),r. Likewise the source is the real H and L.
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: case 0x77: {
        eax(xy);
        nomreq_addr(u16(PC - 1), 5);
        TDAT8 = *reg8_ptr(op & 7);
        wm(m_ea);
        break;
    }

    case 0x84: add_a(xh); break;                                    // ADD  A,HX
    case 0x85: add_a(xl); break;
    case 0x8c: adc_a(xh); break;
    case 0x8d: adc_a(xl); break;
    case 0x94: sub_a(xh); break;
    case 0x95: sub_a(xl); break;
    case 0x9c: sbc_a(xh); break;
    case 0x9d: sbc_a(xl); break;
    case 0xa4: and_a(xh); break;
    case 0xa5: and_a(xl); break;
    case 0xac: xor_a(xh); break;
    case 0xad: xor_a(xl); break;
    case 0xb4: or_a(xh); break;
    case 0xb5: or_a(xl); break;
    case 0xbc: cp(xh); break;
    case 0xbd: cp(xl); break;

    // -- <alu> A,(IX+o)
    case 0x86: case 0x8e: case 0x96: case 0x9e:
    case 0xa6: case 0xae: case 0xb6: case 0xbe: {
        eax(xy);
        nomreq_addr(u16(PC - 1), 5);
        rm(m_ea);
        switch ((op >> 3) & 7) {
        case 0: add_a(TDAT8); break;
        case 1: adc_a(TDAT8); break;
        case 2: sub_a(TDAT8); break;
        case 3: sbc_a(TDAT8); break;
        case 4: and_a(TDAT8); break;
        case 5: xor_a(TDAT8); break;
        case 6: or_a(TDAT8); break;
        default: cp(TDAT8); break;
        }
        break;
    }

    case 0xcb: {                                                    // DD CB xx
        eax(xy);
        arg();
        nomreq_addr(u16(PC - 1), 2);
        op_xycb(TDAT8);
        break;
    }

    case 0xe1: xy.w = pop16(); break;                               // POP  IX
    case 0xe3: ex_sp(xy); break;                                    // EX   (SP),IX
    case 0xe5: push16(xy.w); break;                                 // PUSH IX
    case 0xe9: PC = xy.w; break;                                    // JP   (IX)
    case 0xf9: nomreq_ir(2); SP = xy.w; break;                      // LD   SP,IX

    default:
        illegal_1(prefix, op);
        return false;
    }

    return true;
}

// ============================================================================
// Interrupts
// ============================================================================

void Z80::set_irq_line(bool asserted)
{
    // INT is level-sensitive; MAME consults the daisy chain here to see whether
    // any device on it is still asserting. Without a chain the line is the line.
    m_irq_state = asserted;
    if (asserted) {
        set_service_attention<SA_IRQ_ON, 1>();
    } else {
        set_service_attention<SA_IRQ_ON, 0>();
    }
}

void Z80::set_nmi_line(bool asserted)
{
    // Latch on the rising edge. MAME additionally ignores an edge that arrives
    // while its global cycle count is still zero, to stop the line being
    // released as part of power-on from latching a spurious NMI. That is a
    // side effect of MAME's device start ordering; here the host calls reset()
    // explicitly and reset() drops any pending NMI, so the guard is unnecessary
    // and would only make an NMI asserted before the first run() disappear.
    if (!m_nmi_state && asserted) {
        set_service_attention<SA_NMI_PENDING, 1>();
    }
    m_nmi_state = asserted;
}

void Z80::check_interrupts()
{
    if (get_service_attention<SA_NMI_PENDING>()) {
        take_nmi();
    } else if (m_irq_state && m_iff1 && !get_service_attention<SA_AFTER_EI>()) {
        take_interrupt();
    }
}

void Z80::take_nmi()
{
    leave_halt();
    if constexpr (kHasLdairQuirk) {
        // reset parity flag after LD A,I or LD A,R
        if (get_service_attention<SA_AFTER_LDAIR>()) m_f.pv_val = !0;
    }
    m_iff1 = false;
    R++;
    m_icount -= 5;
    wm16_sp(PC);
    PC = 0x0066;
    WZ = PC;
    set_service_attention<SA_NMI_PENDING, 0>();
}

void Z80::take_interrupt()
{
    leave_halt();
    // Both flip-flops clear, so the handler runs with interrupts disabled until
    // it says otherwise. IFF2 keeps the pre-NMI copy for RETN, not this.
    m_iff1 = m_iff2 = false;
    R++;

    // MAME takes the vector from the daisy chain when one is present; here the
    // bus answers the acknowledge cycle directly.
    m_tmp_irq_vector = m_bus->interrupt_vector();

    // 'interrupt latency' cycles
    m_icount -= 2;

    if (m_im == 2) {
        // Zilog's datasheet claims the low bit of an IM 2 vector must be zero.
        // Experiments say otherwise: all eight bits are used and an odd vector,
        // 0xff included, works normally.
        m_icount -= 5;  // CALL opcode timing
        wm16_sp(PC);
        m_tmp_irq_vector = (m_tmp_irq_vector & 0xff) | (u32(I) << 8);
        PC = rm16(u16(m_tmp_irq_vector));
    } else if (m_im == 1) {
        m_icount -= 5;  // RST $38
        wm16_sp(PC);
        PC = 0x0038;
    } else {
        // Mode 0 executes whatever the acknowledging device drove onto the data
        // bus. MAME accepts a multi-byte CALL nnnn or JP nnnn there by letting
        // the vector carry 24 bits; this Bus supplies one byte, so only the
        // single-byte forms can occur. That is the whole of what any Model 2
        // board needs, but it is a real restriction: a device wanting a mode 0
        // CALL cannot express it.
        if (m_tmp_irq_vector != 0x00) {
            if ((m_tmp_irq_vector & 0xc7) == 0xc7) {
                m_icount -= 5;  // RST $xx cycles
                wm16_sp(PC);
                PC = u16(m_tmp_irq_vector & 0x0038);
            } else if (m_tmp_irq_vector == 0xfb) {
                m_icount -= 4;  // EI cycles
                ei();
            } else {
                SM2_WARN("z80: unexpected opcode $%02x acknowledged in interrupt mode 0",
                         u8(m_tmp_irq_vector));
            }
        }
    }
    WZ = PC;

    if constexpr (kHasLdairQuirk) {
        // reset parity flag after LD A,I or LD A,R
        if (get_service_attention<SA_AFTER_LDAIR>()) m_f.pv_val = !0;
    }
}

// ============================================================================
// Execution
// ============================================================================

void Z80::execute_one()
{
    if (m_service_attention) {
        check_interrupts();
        set_service_attention<SA_AFTER_EI, 0>();
        if constexpr (kHasLdairQuirk) {
            set_service_attention<SA_AFTER_LDAIR, 0>();
        }
        if (m_halt) {
            // Still halted: the part keeps running M1 cycles without advancing.
            // PC already points past the HALT, which is where an interrupt has
            // to return to, so the fetch is undone rather than the increment
            // suppressed. R keeps counting, as refresh does on real silicon.
            rop();
            PC--;
            return;
        }
    }

    PRVPC = PC;
    u8 op = rop();
    if (m_trace_hook != nullptr) {
        m_trace_hook(m_trace_context, PRVPC, op);
    }

    // Prefixes are iterated rather than recursed: a run of DD/FD bytes is legal
    // and each one only costs four T-states, so the chain can be arbitrarily
    // long. MAME's generated code does the same thing with a goto.
    for (;;) {
        if (op == 0xcb) {
            op_cb(rop());
            break;
        }
        if (op == 0xed) {
            op_ed(rop());
            break;
        }
        if (op == 0xdd) {
            op = rop();
            if (op_index(0xdd, op, m_ix)) break;
            continue;  // not an IX opcode: run it unprefixed
        }
        if (op == 0xfd) {
            op = rop();
            if (op_index(0xfd, op, m_iy)) break;
            continue;  // not an IY opcode: run it unprefixed
        }
        op_main(op);
        break;
    }

    ++m_instruction_count;
}

s32 Z80::run(s32 cycles)
{
    if (cycles <= 0) return 0;

    // Cycles borrowed by the previous slice's last instruction come off the top,
    // which is what keeps a long sequence of slices totalling correctly. The
    // allowance can go non-positive, in which case nothing runs this time and
    // the debt is simply paid down.
    const s32 allowance = cycles - m_cycle_debt;
    m_icount = allowance;

    if (m_wait_state) {
        // WAIT held: the CPU is stopped with the bus cycle stretched. It retires
        // nothing and the slice is gone.
        if (m_icount > 0) m_icount = 0;
    } else {
        while (m_icount > 0) {
            execute_one();
        }
    }

    m_total_cycles += u64(allowance - m_icount);

    const s32 consumed = cycles - m_icount;  // outstanding debt plus work done
    if (consumed > cycles) {
        m_cycle_debt = consumed - cycles;
        return cycles;
    }
    m_cycle_debt = 0;
    return consumed;
}

// ============================================================================
// Diagnostics
// ============================================================================

std::string Z80::state_string() const
{
    const u8 f = get_f();
    char flags[9];
    flags[0] = (f & SF) ? 'S' : '.';
    flags[1] = (f & ZF) ? 'Z' : '.';
    flags[2] = (f & 0x20) ? 'Y' : '.';
    flags[3] = (f & HF) ? 'H' : '.';
    flags[4] = (f & 0x08) ? 'X' : '.';
    flags[5] = (f & PF) ? 'P' : '.';
    flags[6] = (f & NF) ? 'N' : '.';
    flags[7] = (f & CF) ? 'C' : '.';
    flags[8] = '\0';

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "PC=%04X SP=%04X AF=%04X BC=%04X DE=%04X HL=%04X IX=%04X IY=%04X "
                  "I=%02X R=%02X IM%u IFF%u%u %s%s",
                  PC, SP, unsigned((A << 8) | f), BC, DE, HL, m_ix.w, m_iy.w,
                  I, unsigned(r()), unsigned(m_im), unsigned(m_iff1), unsigned(m_iff2),
                  flags, m_halt ? " HALT" : "");
    return buf;
}

#undef PRVPC
#undef PC
#undef SP
#undef Q
#undef QT
#undef I
#undef R
#undef R2
#undef AF
#undef A
#undef F
#undef BC
#undef B
#undef C
#undef DE
#undef D
#undef E
#undef HL
#undef H
#undef L
#undef WZ
#undef WZ_H
#undef WZ_L
#undef TDAT
#undef TDAT2
#undef TDAT_H
#undef TDAT_L
#undef TDAT8

}  // namespace sm2::cpu::z80
