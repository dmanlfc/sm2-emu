// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// NEC V60 (uPD70615) CPU core.
//
// Derived from MAME's src/devices/cpu/v60/v60.cpp and its .hxx opcode files,
// which are BSD-3-Clause. Copyright (c) Farfetch'd, R. Belmont.
//
// The .hxx files are upstream's, with these mechanical substitutions:
//
//   MAME                              here
//   --------------------------------  ----------------------------------------
//   fatalerror(...)                   fatal(...)      throws Fault
//   logerror(...)                     SM2_DEBUG(...)
//   m_program / m_io address_space    Space over a cpu::Bus (v60.h)
//
// device_start, device_reset, execute_run and execute_set_input are replaced
// by the constructor, reset(), run() and set_irq_line(). MAME's save-state and
// debugger-state registration is dropped.

#include "cpu/v60/v60.h"

#include "core/archive.h"
#include "core/log.h"
#include "cpu/mame_compat.h"

#include <cassert>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace sm2::cpu::v60 {

namespace {

inline float u2f(u32 v)
{
    float f;
    std::memcpy(&f, &v, sizeof f);
    return f;
}

inline u32 f2u(float f)
{
    u32 v;
    std::memcpy(&v, &f, sizeof v);
    return v;
}

inline s32 div_64x32_rem(s64 a, s32 b, s32& remainder)
{
    if (b == 0 || (b == -1 && a == INT64_MIN)) {
        remainder = 0;
        return 0;
    }
    const s64 quotient = a / b;
    remainder          = static_cast<s32>(a % b);
    return static_cast<s32>(quotient);
}

inline u32 divu_64x32_rem(u64 a, u32 b, u32& remainder)
{
    if (b == 0) {
        remainder = 0;
        return 0;
    }
    const u64 quotient = a / b;
    remainder          = static_cast<u32>(a % b);
    return static_cast<u32>(quotient);
}

inline u32 swapendian_int32(u32 v)
{
    return ((v & 0xff) << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
}

template <int N, typename T, typename... B>
constexpr T bitswap(T value, B... bits)
{
    static_assert(sizeof...(B) == N);
    T   result = 0;
    int shift  = N - 1;
    ((result |= static_cast<T>(((value >> bits) & 1) << shift--)), ...);
    return result;
}

}  // namespace

constexpr int CLEAR_LINE  = kClearLine;
constexpr int ASSERT_LINE = kAssertLine;

V60::V60(Bus& program, Bus* io)
{
    m_program_space.bind(&program, 0xffffff);
    m_io_space.bind(io != nullptr ? io : &program, 0xffffff);
    std::memset(m_reg, 0, sizeof m_reg);
    m_reg[45] = 0x00006000;  // PIR
    m_start_pc = 0xfffffff0;
    m_irq_line = CLEAR_LINE;
    m_nmi_line = CLEAR_LINE;
    m_stall_io = 0;
    m_PPC = 0;
    m_icount = 0;
    m_op1 = m_op2 = 0;
    m_flag1 = m_flag2 = 0;
    m_instflags = 0;
    m_lenop1 = m_lenop2 = 0;
    m_subop = 0;
    m_bamoffset1 = m_bamoffset2 = 0;
    m_amflag = 0;
    m_amout = 0;
    m_bamoffset = 0;
    m_amlength1 = m_amlength2 = 0;
    m_modadd = 0;
    m_modm = 0;
    m_modval = m_modval2 = 0;
    m_modwritevalb = 0;
    m_modwritevalh = 0;
    m_modwritevalw = 0;
    m_moddim = 0;
    m_flags = {};
}

void V60::fatal(const char* format, ...)
{
    char    text[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    throw Fault(text);
}

void V60::bind_fetch_window(unsigned index, u32 start, const u8* base, usize bytes)
{
    // The last 7 bytes stay on the bus, so a 32-bit read never crosses the end.
    m_fetch[index & 3] = FetchWindow{start, base != nullptr && bytes > 8 ? static_cast<u32>(bytes - 7) : 0, base};
}

u8 V60::OpRead8(u32 a)
{
    a &= 0xffffff;
    for (const FetchWindow& w : m_fetch) {
        if (a - w.start < w.bytes) return w.base[a - w.start];
    }
    return m_program->read_byte(a);
}

u16 V60::OpRead16(u32 a)
{
    a &= 0xffffff;
    for (const FetchWindow& w : m_fetch) {
        if (a - w.start < w.bytes) {
            const u8* p = w.base + (a - w.start);
            return static_cast<u16>(p[0] | (p[1] << 8));
        }
    }
    return m_program->read_word_unaligned(a);
}

u32 V60::OpRead32(u32 a)
{
    a &= 0xffffff;
    for (const FetchWindow& w : m_fetch) {
        if (a - w.start < w.bytes) {
            const u8* p = w.base + (a - w.start);
            return static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) | (static_cast<u32>(p[2]) << 16) |
                   (static_cast<u32>(p[3]) << 24);
        }
    }
    return m_program->read_dword_unaligned(a);
}

u8 V60::m_irq_cycle()
{
    return m_irq_cycle_cb != nullptr ? m_irq_cycle_cb(m_irq_cycle_ctx) : 0;
}

// macros stolen from MAME for flags calc
// note that these types are in x86 naming:
// byte = 8 bit, word = 16 bit, long = 32 bit

// parameter x = result, y = source 1, z = source 2

#define SetOFL_Add(x, y,z)  (_OV = (((x) ^ (y)) & ((x) ^ (z)) & 0x80000000) ? 1: 0)
#define SetOFW_Add(x, y,z)  (_OV = (((x) ^ (y)) & ((x) ^ (z)) & 0x8000) ? 1 : 0)
#define SetOFB_Add(x, y,z)  (_OV = (((x) ^ (y)) & ((x) ^ (z)) & 0x80) ? 1 : 0)

#define SetOFL_Sub(x, y,z)  (_OV = (((z) ^ (y)) & ((z) ^ (x)) & 0x80000000) ? 1 : 0)
#define SetOFW_Sub(x, y,z)  (_OV = (((z) ^ (y)) & ((z) ^ (x)) & 0x8000) ? 1 : 0)
#define SetOFB_Sub(x, y,z)  (_OV = (((z) ^ (y)) & ((z) ^ (x)) & 0x80) ? 1 : 0)

#define SetCFB(x)           {_CY = ((x) & 0x100) ? 1 : 0; }
#define SetCFW(x)           {_CY = ((x) & 0x10000) ? 1 : 0; }
#define SetCFL(x)           {_CY = ((x) & (((uint64_t)1) << 32)) ? 1 : 0; }

#define SetSF(x)            (_S = (x))
#define SetZF(x)            (_Z = (x))

#define SetSZPF_Byte(x)     {_Z = ((uint8_t)(x) == 0);  _S = ((x)&0x80) ? 1 : 0; }
#define SetSZPF_Word(x)     {_Z = ((uint16_t)(x) == 0);  _S = ((x)&0x8000) ? 1 : 0; }
#define SetSZPF_Long(x)     {_Z = ((uint32_t)(x) == 0);  _S = ((x)&0x80000000) ? 1 : 0; }

#define ORB(dst, src)       { (dst) |= (src); _OV = 0; SetSZPF_Byte(dst); }
#define ORW(dst, src)       { (dst) |= (src); _OV = 0; SetSZPF_Word(dst); }
#define ORL(dst, src)       { (dst) |= (src); _OV = 0; SetSZPF_Long(dst); }

#define ANDB(dst, src)      { (dst) &= (src); _OV = 0; SetSZPF_Byte(dst); }
#define ANDW(dst, src)      { (dst) &= (src); _OV = 0; SetSZPF_Word(dst); }
#define ANDL(dst, src)      { (dst) &= (src); _OV = 0; SetSZPF_Long(dst); }

#define XORB(dst, src)      { (dst) ^= (src); _OV = 0; SetSZPF_Byte(dst); }
#define XORW(dst, src)      { (dst) ^= (src); _OV = 0; SetSZPF_Word(dst); }
#define XORL(dst, src)      { (dst) ^= (src); _OV = 0; SetSZPF_Long(dst); }

#define SUBB(dst, src, c)   { unsigned res = (dst) - (src) - (c); SetCFB(res); SetOFB_Sub(res, src, dst); SetSZPF_Byte(res); dst = (uint8_t)res; }
#define SUBW(dst, src, c)   { unsigned res = (dst) - (src) - (c); SetCFW(res); SetOFW_Sub(res, src, dst); SetSZPF_Word(res); dst = (uint16_t)res; }
#define SUBL(dst, src, c)   { uint64_t res = (uint64_t)(dst) - (int64_t)(src) - (c); SetCFL(res); SetOFL_Sub(res, src, dst); SetSZPF_Long(res); dst = (uint32_t)res; }

#define ADDB(dst, src, c)   { unsigned res = (dst) + (src) + (c); SetCFB(res); SetOFB_Add(res, src, dst); SetSZPF_Byte(res); dst = (uint8_t)res; }
#define ADDW(dst, src, c)   { unsigned res = (dst) + (src) + (c); SetCFW(res); SetOFW_Add(res, src, dst); SetSZPF_Word(res); dst = (uint16_t)res; }
#define ADDL(dst, src, c)   { uint64_t res = (uint64_t)(dst) + (uint64_t)(src) + (c); SetCFL(res); SetOFL_Add(res, src, dst); SetSZPF_Long(res); dst = (uint32_t)res; }

#define SETREG8(a, b)       (a) = ((a) & ~0xff) | ((b) & 0xff)
#define SETREG16(a, b)      (a) = ((a) & ~0xffff) | ((b) & 0xffff)


/*
 * Prevent warnings on NetBSD.  All identifiers beginning with an underscore
 * followed by an uppercase letter are reserved by the C standard (ISO / IEC
 * 9899:1999, 7.1.3) to be used by the implementation.  It'd be best to rename
 * all such instances, but this is less intrusive and error-prone.
 */
#undef _S

#define _CY     m_flags.CY
#define _OV     m_flags.OV
#define _S      m_flags.S
#define _Z      m_flags.Z


// Defines of all v60 register...
#define R0      m_reg[0]
#define R1      m_reg[1]
#define R2      m_reg[2]
#define R3      m_reg[3]
#define R4      m_reg[4]
#define R5      m_reg[5]
#define R6      m_reg[6]
#define R7      m_reg[7]
#define R8      m_reg[8]
#define R9      m_reg[9]
#define R10     m_reg[10]
#define R11     m_reg[11]
#define R12     m_reg[12]
#define R13     m_reg[13]
#define R14     m_reg[14]
#define R15     m_reg[15]
#define R16     m_reg[16]
#define R17     m_reg[17]
#define R18     m_reg[18]
#define R19     m_reg[19]
#define R20     m_reg[20]
#define R21     m_reg[21]
#define R22     m_reg[22]
#define R23     m_reg[23]
#define R24     m_reg[24]
#define R25     m_reg[25]
#define R26     m_reg[26]
#define R27     m_reg[27]
#define R28     m_reg[28]
#define AP      m_reg[29]
#define FP      m_reg[30]
#define SP      m_reg[31]

#define PC      m_reg[32]
#define PSW     m_reg[33]

// Privileged registers
#define ISP     m_reg[36]
#define L0SP    m_reg[37]
#define L1SP    m_reg[38]
#define L2SP    m_reg[39]
#define L3SP    m_reg[40]
#define SBR     m_reg[41]
#define TR      m_reg[42]
#define SYCW    m_reg[43]
#define TKCW    m_reg[44]
#define PIR     m_reg[45]
//10-14 reserved
#define PSW2    m_reg[51]
#define ATBR0   m_reg[52]
#define ATLR0   m_reg[53]
#define ATBR1   m_reg[54]
#define ATLR1   m_reg[55]
#define ATBR2   m_reg[56]
#define ATLR2   m_reg[57]
#define ATBR3   m_reg[58]
#define ATLR3   m_reg[59]
#define TRMODE  m_reg[60]
#define ADTR0   m_reg[61]
#define ADTR1   m_reg[62]
#define ADTMR0  m_reg[63]
#define ADTMR1  m_reg[64]
//29-31 reserved

// Defines...
#define NORMALIZEFLAGS() \
{ \
	_S    = _S  ? 1 : 0; \
	_OV   = _OV ? 1 : 0; \
	_Z    = _Z  ? 1 : 0; \
	_CY   = _CY ? 1 : 0; \
}


void v60_device::v60SaveStack()
{
	if (PSW & 0x10000000)
		ISP = SP;
	else
		m_reg[37 + ((PSW >> 24) & 3)] = SP;
}

void v60_device::v60ReloadStack()
{
	if (PSW & 0x10000000)
		SP = ISP;
	else
		SP = m_reg[37 + ((PSW >> 24) & 3)];
}

uint32_t v60_device::v60ReadPSW()
{
	PSW &= 0xfffffff0;
	PSW |= (_Z?1:0) | (_S?2:0) | (_OV?4:0) | (_CY?8:0);
	return PSW;
}

void v60_device::v60WritePSW(uint32_t newval)
{
	/* determine if we need to save / restore the stacks */
	int updateStack = 0;

	/* if the interrupt state is changing, we definitely need to update */
	if ((newval ^ PSW) & 0x10000000)
		updateStack = 1;

	/* if we are not in interrupt mode and the level is changing, we also must update */
	else if (!(PSW & 0x10000000) && ((newval ^ PSW) & 0x03000000))
		updateStack = 1;

	/* save the previous stack value */
	if (updateStack)
		v60SaveStack();

	/* set the new value and update the flags */
	PSW = newval;
	_Z =  (uint8_t)(PSW & 1);
	_S =  (uint8_t)(PSW & 2);
	_OV = (uint8_t)(PSW & 4);
	_CY = (uint8_t)(PSW & 8);

	/* fetch the new stack value */
	if (updateStack)
		v60ReloadStack();
}


uint32_t v60_device::v60_update_psw_for_exception(int is_interrupt, int target_level)
{
	uint32_t oldPSW = v60ReadPSW();
	uint32_t newPSW = oldPSW;

	// Change to interrupt context
	newPSW &= ~(3 << 24);  // PSW.EL = 0
	newPSW |= target_level << 24; // set target level
	newPSW &= ~(1 << 18);  // PSW.IE = 0
	newPSW &= ~(1 << 16);  // PSW.TE = 0
	newPSW &= ~(1 << 27);  // PSW.TP = 0
	newPSW &= ~(1 << 17);  // PSW.AE = 0
	newPSW &= ~(1 << 29);  // PSW.EM = 0
	if (is_interrupt)
		newPSW |=  (1 << 28);// PSW.IS = 1
	newPSW |=  (1 << 31);  // PSW.ASA = 1
	v60WritePSW(newPSW);

	return oldPSW;
}


#define GETINTVECT(nint)                    m_program->read_dword_unaligned((SBR & ~0xfff) + (nint) * 4)
#define EXCEPTION_CODE_AND_SIZE(code, size) (((code) << 16) | (size))


// Addressing mode decoding functions
#include "am.hxx"

// Opcode functions
#include "op12.hxx"
#include "op2.hxx"
#include "op3.hxx"
#include "op4.hxx"
#include "op5.hxx"
#include "op6.hxx"
#include "op7a.hxx"

uint32_t v60_device::opUNHANDLED()
{
	fatal("Unhandled OpCode found : %02x at %08x\n", OpRead16(PC), PC);
}

// Opcode jump table
#include "optable.hxx"

void V60::reset()
{
	PSW   = 0x10000000;
	PC    = m_start_pc;
	SBR   = 0x00000000;
	SYCW  = 0x00000070;
	TKCW  = 0x0000e000;
	PSW2  = 0x0000f002;

	_CY   = 0;
	_OV   = 0;
	_S    = 0;
	_Z    = 0;

	m_halted = false;
	m_faulted = false;
	m_fault_message.clear();
}

u32 V60::psw()
{
	return v60ReadPSW();
}

void v60_device::v60_do_irq(int vector)
{
	uint32_t oldPSW = v60_update_psw_for_exception(1, 0);

	// Push PC and PSW onto the stack
	SP-=4;
	m_program->write_dword_unaligned(SP, oldPSW);
	SP-=4;
	m_program->write_dword_unaligned(SP, PC);

	// Jump to vector for user interrupt
	PC = GETINTVECT(vector);
}

void v60_device::v60_try_irq()
{
	if(m_irq_line == CLEAR_LINE)
		return;
	if((PSW & (1 << 18)) != 0) {
		int vector;
		if(m_irq_line != ASSERT_LINE)
			m_irq_line = CLEAR_LINE;

		vector = m_irq_cycle();

		v60_do_irq(vector + 0x40);
	}
}

void V60::set_nmi_line(int state)
{
	switch(state) {
	case ASSERT_LINE:
		if(m_nmi_line == CLEAR_LINE) {
			m_nmi_line = ASSERT_LINE;
			v60_do_irq(2);
		}
		break;
	case CLEAR_LINE:
		m_nmi_line = CLEAR_LINE;
		break;
	}
}

// MAME applies a line change through the scheduler, between instructions. A
// device here changes it from inside one (a register write), so the line is
// latched and run() takes the interrupt at the next instruction boundary.
void V60::set_irq_line(int state)
{
	m_irq_line = state;
}

// Actual cycles / instruction is unknown

s32 V60::run(s32 cycles)
{
	if (m_faulted || m_halted)
		return cycles;

	m_icount = cycles;

	try {
		if (m_irq_line != CLEAR_LINE)
			v60_try_irq();

		while (m_icount > 0 && !m_halted)
		{
			uint32_t inc;
			m_PPC = PC;
			if (m_trace_hook != nullptr)
				m_trace_hook(m_trace_context, PC);
			m_icount -= 8;  /* fix me -- this is just an average */
			inc = (this->*s_OpCodeTable[OpRead8(PC)])();
			PC += inc;
			++m_instruction_count;
			if (m_irq_line != CLEAR_LINE)
				v60_try_irq();
		}
	} catch (const Fault& fault) {
		m_faulted = true;
		m_fault_message = fault.what();
		SM2_ERROR("V60: %s (PPC=%06x)", m_fault_message.c_str(), m_PPC);
		return cycles;
	}

	return cycles - m_icount;
}

std::string V60::state_string()
{
	char text[512];
	std::snprintf(text, sizeof text,
		"PC=%06x PPC=%06x PSW=%08x SP=%08x FP=%08x AP=%08x R0=%08x R1=%08x R2=%08x R3=%08x R4=%08x R5=%08x R6=%08x R7=%08x",
		PC, m_PPC, v60ReadPSW(), SP, FP, AP, R0, R1, R2, R3, R4, R5, R6, R7);
	return text;
}

void V60::serialize(Archive& ar)
{
	ar.bytes(m_reg, 68);
	ar.raw(m_irq_line);
	ar.raw(m_nmi_line);
	ar.raw(m_PPC);
	ar.raw(m_flags.CY);
	ar.raw(m_flags.OV);
	ar.raw(m_flags.S);
	ar.raw(m_flags.Z);
	ar.raw(m_stall_io);
	ar.raw(m_halted);
	ar.raw(m_instruction_count);
}

}  // namespace sm2::cpu::v60
