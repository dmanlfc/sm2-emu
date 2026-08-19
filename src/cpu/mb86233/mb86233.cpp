// SPDX-License-Identifier: BSD-3-Clause
//
// Ported from MAME's src/devices/cpu/mb86233/mb86233.cpp (BSD-3-Clause,
// copyright-holders Olivier Galibert), which builds on Elsemi's original reverse
// engineering of the part.
//
// MAME's own notes on the hardware, kept because they record findings that are
// not deducible from the code:
//
//   The 86232 has 512 32-bit words of triple-port memory (1 write, 2 read). The
//   86233 and 86234 have instead two normal (1 read, 1 write, non-simultaneous)
//   independent RAM banks, one of 256 words and one of 512.
//
//   The RAM banks are mapped at 0x000-0x0ff and 0x200-0x3ff, which is proven by
//   geometrizer code that clears the RAM at startup. Move and load instructions
//   sort of target a specific bank, but do it by adding 0x200 to the address on
//   one side or the other, which can then end up anywhere. In particular the
//   Model 1 coprocessor has its output FIFO at 0x400, which is sometimes hit by
//   having x1 at 0x200 and using the automatic 0x200 adder. External accesses to
//   0x100-0x1ff and 0x400 upwards appear to be routed externally.
//
//   The part can in theory work in either floating point (32-bit IEEE) or fixed
//   point (32/36/48-bit) modes. Every Sega program starts by selecting floating
//   point and stays there, so fixed point is not implemented.
//
//   An interrupt updates the rf0 register in the coprocessor programs. It is on
//   bit 1 of the mask, vector 0x004, and is probably periodic, maybe on vertical
//   blank. The coprocessor programs never initialise the stack pointer.
//   Interrupts are not implemented.
//
//   In the event of two writes to the same register in one instruction, priority
//   varies with the ALU operation: transfers take precedence over integer ops,
//   while floating point ops take precedence over transfers. This is probably
//   because a floating point op takes more than one cycle, so the register is
//   updated after the transfer.

#include "cpu/mb86233/mb86233.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace sm2::cpu::mb86233 {

MB86233::MB86233(Bus& bus) : m_bus(&bus)
{
    reset();
}

void MB86233::reset()
{
    m_pc  = 0;
    m_ppc = 0;
    m_st  = kZeroC | kZeroD | kZeroX0 | kZeroX1 | kZeroX2 | kZeroC0 | kZeroC1;
    m_sp  = 0;
    m_a   = 0;
    m_b   = 0;
    m_d   = 0;
    m_p   = 0;
    // The repeat counters come up at one, not zero: a value of one means "do not
    // repeat", so zero would mean an instruction runs 256 times.
    m_r    = 1;
    m_rpc  = 1;
    m_c0   = 1;
    m_c1   = 1;
    m_b0   = 0;
    m_b1   = 0;
    m_x0   = 0;
    m_x1   = 0;
    m_i0   = 0;
    m_i1   = 0;
    m_sft  = 0;
    m_vsm  = 0;
    m_vsmr = 7;
    m_mask = 0;
    m_m    = 1;

    m_alu_stmask = 0;
    m_alu_stset  = 0;
    m_alu_r1     = 0;
    m_alu_r2     = 0;
    std::fill(std::begin(m_pcs), std::end(m_pcs), u16{0});

    m_stall = false;

    m_gpio0 = m_gpio1 = m_gpio2 = m_gpio3 = false;
}

void MB86233::set_gpio(u32 index, bool state)
{
    switch (index) {
        case 0: m_gpio0 = state; break;
        case 1: m_gpio1 = state; break;
        case 2: m_gpio2 = state; break;
        case 3: m_gpio3 = state; break;
        default: break;
    }
}

s32 MB86233::run(s32 cycles)
{
    if (m_halted || cycles <= 0) {
        // Consume the whole budget: a halted part is not going to make progress
        // and the caller should not spin trying.
        return cycles;
    }

    const s32 before = cycles;
    m_icount         = cycles;
    execute_run();
    const s32 used = before - m_icount;
    m_instructions += static_cast<u64>(used > 0 ? used : 0);
    return used;
}

std::string MB86233::state_string() const
{
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "PC=%04x PPC=%04x ST=%08x SP=%04x A=%08x B=%08x D=%08x P=%08x "
                  "R=%02x X0=%04x X1=%04x I0=%04x I1=%04x",
                  m_pc, m_ppc, m_st, m_sp, m_a, m_b, m_d, m_p, m_r, m_x0, m_x1, m_i0,
                  m_i1);
    return buffer;
}

u32 MB86233::set_exp(u32 val, u32 exp)
{
	return (val & 0x807fffff) | ((exp & 0xff) << 23);
}

u32 MB86233::set_mant(u32 val, u32 mant)
{
	return (val & 0x07f800000) | ((mant & 0x00800000) << 8) | (mant & 0x007fffff);
}

u32 MB86233::get_exp(u32 val)
{
	return (val >> 23) & 0xff;
}

u32 MB86233::get_mant(u32 val)
{
	return val & 0x80000000 ? val | 0x7f800000 : val & 0x807fffff;
}

void MB86233::pcs_push()
{
	for(unsigned int i=3; i; i--)
		m_pcs[i] = m_pcs[i-1];
	m_pcs[0] = m_pc;
}

void MB86233::pcs_pop()
{
	m_pc = m_pcs[0];
	for(unsigned int i=0; i != 3; i++)
		m_pcs[i] = m_pcs[i+1];
}

void MB86233::stset_set_sz_int(u32 val)
{
	m_alu_stset = val ? (val & 0x80000000 ? F_SGD : 0) : F_ZRD;
}

void MB86233::stset_set_sz_fp(u32 val)
{
	m_alu_stset = (val & 0x7fffffff) ? (val & 0x80000000 ? F_SGD : 0) : F_ZRD;
}

void MB86233::alu_pre(u32 alu)
{
	switch(alu) {
	case 0x00: break; // no alu

	case 0x01: {
		// andd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d & m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x02: {
		// orad
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d | m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x03: {
		// eord
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d ^ m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x04: {
		// notd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = ~m_d;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x05: {
		// fcpd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		u32 r = f2u(u2f(m_d) - u2f(m_a));
		stset_set_sz_fp(r);
		break;
	}

	case 0x06: {
		// fadd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x07: {
		// fsbd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) - u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x08: {
		// fml
		m_alu_stmask = 0;
		m_alu_r1 = f2u(u2f(m_a) * u2f(m_b));
		m_alu_stset = 0;
		break;
	}

	case 0x09: {
		// fmsd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_p));
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0a: {
		// fmrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) - u2f(m_p));
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0b: {
		// fabd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d & 0x7fffffff;
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0c: {
		// fsmd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) + u2f(m_p));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0d: {
		// fspd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_p;
		m_alu_r2 = f2u(u2f(m_a) * u2f(m_b));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x0e: {
		// cxfd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(s32(m_d));
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x0f: {
		// cfxd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		switch((m_m >> 1) & 3) {
		case 0: m_alu_r1 = s32(roundf(u2f(m_d))); break;
		case 1: m_alu_r1 = s32(ceilf(u2f(m_d))); break;
		case 2: m_alu_r1 = s32(floorf(u2f(m_d))); break;
		case 3: m_alu_r1 = s32(u2f(m_d)); break;
		}
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x10: {
		// fdvd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_d) / u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x11: {
		// fned
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d ? m_d ^ 0x80000000 : 0;
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x13: {
		// d = b + a
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_b) + u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x14: {
		// d = b - a
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = f2u(u2f(m_b) - u2f(m_a));
		stset_set_sz_fp(m_alu_r1);
		break;
	}

	case 0x16: {
		// lsrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d >> m_sft;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x17: {
		// lsld
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d << m_sft;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x18: {
		// asrd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = s32(m_d) >> m_sft;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x19: {
		// asld
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = s32(m_d) << m_sft;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x1a: {
		// addd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d + m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	case 0x1b: {
		// subd
		m_alu_stmask = F_ZRD|F_SGD|F_CPD|F_OVD|F_DVZD;
		m_alu_r1 = m_d - m_a;
		stset_set_sz_int(m_alu_r1);
		break;
	}

	default:
		SM2_WARN("unhandled alu pre %02x\n", alu);
		break;
	}
}

void MB86233::alu_update_st()
{
	m_st = (m_st & ~m_alu_stmask) | m_alu_stset;
}

void MB86233::alu_post_1(u32 alu)
{
	// integer alu post ops
	switch(alu) {
	case 0x01: case 0x02: case 0x03: case 0x04:
	case 0x0e: case 0x0f: case 0x16: case 0x17:
	case 0x18: case 0x19: case 0x1a: case 0x1b:
		// d update
		m_d = m_alu_r1;
		alu_update_st();
		break;

	default:
		break;
	}
}

void MB86233::alu_post_2(u32 alu)
{
	// floating point alu post ops
	// assume each one takes 2 cycles
	switch (alu) {
	case 0x05:
		// flags only
		alu_update_st();
		m_icount--;
		break;

	case 0x06: case 0x07: case 0x0b: case 0x0c:
	case 0x10: case 0x11: case 0x13: case 0x14:
		// d update
		m_d = m_alu_r1;
		alu_update_st();
		m_icount--;
		break;

	case 0x08:
		// p update
		m_p = m_alu_r1;
		m_icount--;
		break;

	case 0x09: case 0x0a: case 0x0d:
		// d, p update
		m_d = m_alu_r1;
		m_p = m_alu_r2;
		alu_update_st();
		m_icount--;
		break;

	default:
		break;
	}
}

u16 MB86233::ea_pre_0(u32 r)
{
	switch(r & 0x180) {
	case 0x000: return r & 0x7f;
	case 0x080: case 0x100: return (r & 0x7f) + m_b0 + m_x0;
	case 0x180: {
		switch(r & 0x60) {
		case 0x00: return m_b0 + m_x0;
		case 0x20: return m_x0;
		case 0x40: return m_b0 + (m_x0 & m_vsmr);
		case 0x60: return m_x0 & m_vsmr;
		}
	}
	}
	return 0;
}

void MB86233::ea_post_0(u32 r)
{
	if(!(r & 0x100))
		return;
	if(!(r & 0x080))
		m_x0 += m_i0;
	else
		m_x0 += sign_extend(r, 5);
}

u16 MB86233::ea_pre_1(u32 r)
{
	switch(r & 0x180) {
	case 0x000: return r & 0x7f;
	case 0x080: case 0x100: return (r & 0x7f) + m_b1 + m_x1;
	case 0x180: {
		switch(r & 0x60) {
		case 0x00: return m_b1 + m_x1;
		case 0x20: return m_x1;
		case 0x40: return m_b1 + (m_x1 & m_vsmr);
		case 0x60: return m_x1 & m_vsmr;
		}
	}
	}
	return 0;
}

void MB86233::ea_post_1(u32 r)
{
	if(!(r & 0x100))
		return;
	if(!(r & 0x080))
		m_x1 += m_i1;
	else
		m_x1 += sign_extend(r, 5);
}

u32 MB86233::read_reg(u32 r)
{
	r &= 0x3f;
	if(r >= 0x20 && r < 0x30)
		return m_bus->read_rf(r & 0x1f);
	switch(r) {
	case 0x00: return m_b0;
	case 0x01: return m_b1;
	case 0x02: return m_x0;
	case 0x03: return m_x1;

	case 0x0c: return m_c0;
	case 0x0d: return m_c1;

	case 0x10: return m_a;
	case 0x11: return get_exp(m_a);
	case 0x12: return get_mant(m_a);
	case 0x13: return m_b;
	case 0x14: return get_exp(m_b);
	case 0x15: return get_mant(m_b);
	case 0x19: return m_d;
		/* c */
	case 0x1a: return get_exp(m_d);
	case 0x1b: return get_mant(m_d);
	case 0x1c: return m_p;
	case 0x1d: return get_exp(m_p);
	case 0x1e: return get_mant(m_p);
	case 0x1f: return m_sft;

	case 0x34: return m_rpc;

	default:
		SM2_WARN("unimplemented read_reg(%02x) (%x)\n", r, m_ppc);
		return 0;
	}
}

void MB86233::write_reg(u32 r, u32 v)
{
	r &= 0x3f;
	if(r >= 0x20 && r < 0x30) {
		m_bus->write_rf(r & 0x1f, v);
		return;
	}
	switch(r) {
	case 0x00: m_b0 = v; break;
	case 0x01: m_b1 = v; break;
	case 0x02: m_x0 = v; break;
	case 0x03: m_x1 = v; break;

	case 0x05: m_i0 = v; break;
	case 0x06: m_i1 = v; break;

	case 0x08: m_sp = v; break;

	case 0x0a: m_vsm = v & 7; m_vsmr = (8 << m_vsm) - 1; break;

	case 0x0c:
		m_c0 = v;
		if(m_c0 == 1)
			m_st |= F_ZC0;
		else
			m_st &= ~F_ZC0;
		break;

	case 0x0d:
		m_c1 = v;
		if(m_c1 == 1)
			m_st |= F_ZC1;
		else
			m_st &= ~F_ZC1;
		break;

	case 0x0f: break;

	case 0x10: m_a = v; break;
	case 0x11: m_a = set_exp(m_a, v); break;
	case 0x12: m_a = set_mant(m_a, v); break;
	case 0x13: m_b = v; break;
	case 0x14: m_b = set_exp(m_b, v); break;
	case 0x15: m_b = set_mant(m_b, v); break;
		/* c */
	case 0x19: m_d = v; break;
	case 0x1a: m_d = set_exp(m_d, v); break;
	case 0x1b: m_d = set_mant(m_d, v); break;
	case 0x1c: m_p = v; break;
	case 0x1d: m_p = set_exp(m_p, v); break;
	case 0x1e: m_p = set_mant(m_p, v); break;
	case 0x1f: m_sft = v; break;

	case 0x34: m_rpc = v; break;
	case 0x3c: m_mask = v; break;

	default:
		SM2_WARN("unimplemented write_reg(%02x, %08x) (%x)\n", r, v, m_ppc);
		break;
	}
}

void MB86233::write_mem_internal_1(u32 r, u32 v, bool bank)
{
	u16 ea = ea_pre_1(r);
	if(bank)
		ea += 0x200;
	m_bus->write_data(ea, v);
	ea_post_1(r);
}

void MB86233::write_mem_io_1(u32 r, u32 v)
{
	u16 ea = ea_pre_1(r);
	m_bus->write_io(ea, v);
	ea_post_1(r);
}

void MB86233::execute_run()
{
	while(m_icount > 0) {
		// Halt can be asserted part-way through a slice by a fifo callback, so it
		// is checked here rather than only on entry.
		if(m_halted)
			break;
		m_ppc = m_pc;
		if(m_trace_hook)
			m_trace_hook(m_trace_context, m_ppc);
		u32 opcode = m_bus->fetch(m_pc++);

		switch((opcode >> 26) & 0x3f) {
		case 0x00: {
			// lab
			u32 r1 = opcode & 0x1ff;
			u32 r2 = (opcode >> 9) & 0x1ff;
			u32 alu = (opcode >> 21) & 0x1f;
			u32 op = (opcode >> 18) & 0x7;

			alu_pre(alu);

			switch(op) {
			case 0: case 1: {
				// lab mem, mem (e)

				u32 ea1 = ea_pre_0(r1);
				u32 v1 = m_bus->read_data(ea1);
				if(m_stall) goto do_stall;

				u32 ea2 = ea_pre_1(r2);
				u32 v2 = m_bus->read_io(ea2);
				if(m_stall) goto do_stall;

				ea_post_0(r1);
				ea_post_1(r2);

				m_a = v1;
				m_b = v2;
				break;
			}

			case 3: {
				// lab mem, mem + 0x200

				u32 ea1 = ea_pre_0(r1);
				u32 v1 = m_bus->read_data(ea1);
				if(m_stall) goto do_stall;

				u32 ea2 = ea_pre_1(r2) + 0x200;
				u32 v2 = m_bus->read_data(ea2);
				if(m_stall) goto do_stall;

				ea_post_0(r1);
				ea_post_1(r2);

				m_a = v1;
				m_b = v2;
				break;
			}

			case 4: {
				// lab mem + 0x200, mem

				u32 ea1 = ea_pre_0(r1) + 0x200;
				u32 v1 = m_bus->read_data(ea1);
				if(m_stall) goto do_stall;

				u32 ea2 = ea_pre_1(r2);
				u32 v2 = m_bus->read_data(ea2);
				if(m_stall) goto do_stall;

				ea_post_0(r1);
				ea_post_1(r2);

				m_a = v1;
				m_b = v2;
				break;
			}

			default:
				SM2_WARN("unhandled lab subop %x\n", op);
				SM2_WARN("%x\n", m_ppc);
				break;

			}

			alu_post_1(alu);
			alu_post_2(alu);
			break;
		}


		case 0x07: {
			// ld / mov
			u32 r1 = opcode & 0x1ff;
			u32 r2 = (opcode >> 9) & 0x1ff;
			u32 alu = (opcode >> 21) & 0x1f;
			u32 op = (opcode >> 18) & 0x7;

			alu_pre(alu);

			switch(op) {
			case 0: {
				// mov mem, mem (e)
				u32 ea = ea_pre_0(r1);
				u32 v = m_bus->read_data(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_io_1(r2, v);
				break;
			}

			case 1: {
				// mov mem, mem (e)
				u32 ea = ea_pre_0(r1);
				u32 v = m_bus->read_data(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_io_1(r2, v);
				break;
			}

			case 2: {
				// mov mem (e), mem
				u32 ea = ea_pre_0(r1);
				u32 v = m_bus->read_io(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_internal_1(r2, v, false);
				break;
			}

			case 3: {
				// mov mem, mem + 0x200
				u32 ea = ea_pre_0(r1);
				u32 v = m_bus->read_data(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_internal_1(r2, v, true);
				break;
			}

			case 4: {
				// mov mem + 0x200, mem
				u32 ea = ea_pre_0(r1) + 0x200;
				u32 v = m_bus->read_data(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_internal_1(r2, v, false);
				break;
			}

			case 5: {
				// mov mem (o), mem
				u32 ea = ea_pre_0(r1);
				u32 v = m_bus->read_program(ea);
				if(m_stall) goto do_stall;
				ea_post_0(r1);
				alu_post_1(alu);
				write_mem_internal_1(r2, v, false);
				break;
			}

			case 7: {
				switch(r2 >> 6) {
				case 0: {
					// mov reg, mem
					u32 v = read_reg(r2);
					if(m_stall) goto do_stall;
					alu_post_1(alu);
					write_mem_internal_1(r1, v, false);
					break;
				}

				case 1: {
					// mov reg, mem (e)
					u32 v = read_reg(r2);
					if(m_stall) goto do_stall;
					alu_post_1(alu);
					write_mem_io_1(r1, v);
					break;
				}

				case 2: {
					// mov mem + 0x200, reg
					u32 ea = ea_pre_1(r1) + 0x200;
					u32 v = m_bus->read_data(ea);
					if(m_stall) goto do_stall;
					ea_post_1(r1);
					alu_post_1(alu);
					write_reg(r2, v);
					break;
				}

				case 3: {
					// mov mem, reg
					u32 ea = ea_pre_1(r1);
					u32 v = m_bus->read_data(ea);
					if(m_stall) goto do_stall;
					ea_post_1(r1);
					alu_post_1(alu);
					write_reg(r2, v);
					break;
				}

				case 4: {
					// mov mem (e), reg
					u32 ea = ea_pre_1(r1);
					u32 v = m_bus->read_io(ea);
					if(m_stall) goto do_stall;
					ea_post_1(r1);
					alu_post_1(alu);
					write_reg(r2, v);
					break;
				}

				case 5: {
					// mov mem (o), reg
					u32 ea = ea_pre_0(r1);
					u32 v = m_bus->read_program(ea);
					if(m_stall) goto do_stall;
					ea_post_0(r1);
					alu_post_1(alu);
					write_reg(r2, v);
					break;
				}

				case 6: {
					// mov reg, reg
					u32 v = read_reg(r1);
					if(m_stall) goto do_stall;
					alu_post_1(alu);
					write_reg(r2, v);
					break;
				}

				default:
					alu_post_1(alu);
					SM2_WARN("unhandled ld/mov subop 7/%x (%x)\n", r2 >> 6, m_ppc);
					break;
				}
				break;
			}

			default:
				alu_post_1(alu);
				SM2_WARN("unhandled ld/mov subop %x (%x)\n", op, m_ppc);
				break;
			}

			// For floating point ops, registers are updated after transfer
			alu_post_2(alu);
			break;
		}

		case 0x0d: {
			// stm/clm
			u32 sub2 = (opcode >> 17) & 7;

			// Theorically has restricted alu too

			switch(sub2) {
			case 5:
				// stmh
				// bit 0 = floating point
				// bit 1-2 = rounding mode
				m_m = opcode;
				break;

			default:
				SM2_WARN("unimplemented opcode 0d/%x (%x)\n", sub2, m_ppc);
				break;
			}
			break;
		}

		case 0x0e: {
			// lipl / lia / lib / lid
			switch((opcode >> 24) & 0x3) {
			case 0:
				m_p = (m_p & 0xffffff000000) | (opcode & 0xffffff);
				break;
			case 1:
				m_a = sign_extend(opcode, 24);
				break;
			case 2:
				m_b = sign_extend(opcode, 24);
				break;
			case 3:
				m_d = sign_extend(opcode, 24);
				break;
			}
			break;
		}

		case 0x0f: {
			// rep/clr0/clr1/set
			u32 alu = (opcode >> 20) & 0x1f;
			u32 sub2 = (opcode >> 17) & 7;

			alu_pre(alu);

			switch(sub2) {
			case 0:
				// clr0
				if(opcode & 0x0004) m_a = 0;
				if(opcode & 0x0008) m_b = 0;
				if(opcode & 0x0010) m_d = 0;
				break;

			case 1:
				// clr1 - flags mapping unknown
				break;

			case 2: {
				// rep
				u8 r = opcode & 0x8000 ? read_reg(opcode) : opcode;
				if(m_stall) goto do_stall;
				m_r = r;
				goto rep_start;
			}

			case 3:
				// set - flags mapping unknown
				// 0800 = enable interrupt flag
				break;

			default:
				SM2_WARN("unimplemented opcode 0f/%x (%x)\n", sub2, m_ppc);
				break;
			}

			alu_post_1(alu);
			break;
		}

		case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
		case 0x18: case 0x19: case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f: {
			// ldi
			write_reg(opcode >> 24, sign_extend(opcode, 24));
			break;
		}

		case 0x2f: case 0x3f: {
			// Conditional branch of every kind
			u32 cond = ( opcode >> 20 ) & 0x1f;
			u32 subtype = ( opcode >> 17 ) & 7;
			u32 data = opcode & 0xffff;
			bool invert = opcode & 0x40000000;

			bool cond_passed = false;

			switch(cond) {
			case 0x00: // zrd - d zero
				cond_passed = m_st & F_ZRD;
				break;

			case 0x01: // ged - d >= 0
				cond_passed = !(m_st & F_SGD);
				break;

			case 0x02: // led - d <= 0
				cond_passed = m_st & (F_ZRD | F_SGD);
				break;

			case 0x0a: // gpio0
				cond_passed = m_gpio0;
				break;

			case 0x0b: // gpio1
				cond_passed = m_gpio1;
				break;

			case 0x0c: // gpio2
				cond_passed = m_gpio2;
				break;

			case 0x10: // zc0 - c0 == 1
				cond_passed = !(m_st & F_ZC0);
				break;

			case 0x11: // zc1 - c1 == 1
				cond_passed = !(m_st & F_ZC1);
				break;

			case 0x12: // gpio3
				cond_passed = m_gpio3;
				break;

			case 0x16: // alw - always
				cond_passed = true;
				break;

			default:
				SM2_WARN("unimplemented condition %x (%x)\n", cond, m_ppc);
				break;
			}
			if(invert)
				cond_passed = !cond_passed;

			if(cond_passed) {
				switch(subtype) {
				case 0: // brif #adr
					m_pc = data;
					break;

				case 1: // brul
					if(opcode & 0x4000) {
						// brul reg
						u32 v = read_reg(opcode);
						if(m_stall) goto do_stall;
						m_pc = v;
					} else {
						// brul adr
						u32 ea = ea_pre_0(opcode);
						u32 v = m_bus->read_data(ea);
						if(m_stall) goto do_stall;
						ea_post_0(opcode);
						m_pc = v;
					}
					break;

				case 2: // bsif #adr
					pcs_push();
					m_pc = data;
					break;

				case 3: // bsul
					if(opcode & 0x4000) {
						// bsul reg
						u32 v = read_reg(opcode);
						if(m_stall) goto do_stall;
						pcs_push();
						m_pc = v;
					} else {
						// bsul adr
						u32 ea = ea_pre_0(opcode);
						u32 v = m_bus->read_data(ea);
						if(m_stall) goto do_stall;
						ea_post_0(opcode);
						pcs_push();
						m_pc = v;
					}
					break;

				case 5: // rtif #adr
					pcs_pop();
					break;

				case 6: { // ldif adr, rn
					u32 ea = ea_pre_0(opcode);
					u32 v = m_bus->read_data(ea);
					if(m_stall) goto do_stall;
					ea_post_0(opcode);
					write_reg(opcode >> 9, v);
					break;
				}

				default:
					SM2_WARN("unimplemented branch subtype %x (%x)\n", subtype, m_ppc);
					break;
				}
			}

			if(subtype < 2)
				switch(cond) {
				case 0x10:
					if(m_c0 != 1) {
						m_c0 --;
						if(m_c0 == 1)
							m_st |= F_ZC0;
					}
					break;

				case 0x11:
					if(m_c1 != 1) {
						m_c1 --;
						if(m_c1 == 1)
							m_st |= F_ZC1;
					}
				break;
				}

			break;
		}

		default:
			SM2_WARN("unimplemented opcode type %02x (%x)\n", (opcode >> 26) & 0x3f, m_ppc);
			break;
		}

		if(m_r != 1) {
			m_pc = m_ppc;
			m_r --;
		}

	rep_start:
		if(0) {
		do_stall:
			m_pc = m_ppc;
			m_stall = false;
		}
		m_icount--;
	}
}

}  // namespace sm2::cpu::mb86233
