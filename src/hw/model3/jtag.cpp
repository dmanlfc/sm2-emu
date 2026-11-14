// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Model 3's JTAG test access port.
//
// Derived from Supermodel's Src/Model3/JTAG.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2003-2024 The Supermodel Team.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// Which JTAG standard the device follows is unclear (and probably does not
// matter), so IEEE 1149.1-1990 is assumed. All the video board ASICs and the
// 3D-RAM chips are JTAG-compatible, with their JTAG lines daisy-chained.
// Usually only one device is accessed at a time (the others are given the
// BYPASS instruction), but reading ID codes and boundary scan tests access most
// or all of the devices at once.
// 3D-RAM chips are only used for boundary scan tests (currently unemulated).

#include "hw/model3/jtag.h"

#include "core/archive.h"

namespace sm2::hw::model3 {

// -- devices -------------------------------------------------------------------------

bool Jtag::Device::shift(bool tdi)
{
    const bool tdo = (shift_reg & 1) != 0;
    shift_reg >>= 1;
    const u64 top = u64{1} << (shift_size - 1);
    shift_reg = tdi ? (shift_reg | top) : (shift_reg & ~top);
    return tdo;
}

void Jtag::Device::capture_dr()
{
    if (is_asic) {
        switch (instruction) {
            case 3:  // IDCODE
                shift_size = 32;
                shift_reg  = id_code;
                return;
            case 8:  // MODEWORD
                shift_size = 32;
                shift_reg  = modeword;
                return;
            default: break;
        }
    }
    shift_size = 1;  // BYPASS
    shift_reg  = 0;
}

void Jtag::Device::capture_ir()
{
    if (is_asic) {
        shift_size = 5;
        shift_reg  = 1;
    } else {
        shift_size = 4;
        shift_reg  = 0b1001;
    }
}

void Jtag::Device::update_ir()
{
    instruction = static_cast<u8>(shift_reg);
}

void Jtag::Device::reset()
{
    if (is_asic) {
        shift_size  = 32;
        shift_reg   = id_code;
        instruction = 3;  // IDCODE
    } else {
        shift_size  = 1;
        shift_reg   = 0;
        instruction = 15;  // BYPASS
    }
}

void Jtag::update_dr(Device& device)
{
    if (device.is_asic && device.instruction == 8) {  // MODEWORD
        device.modeword = static_cast<u32>(device.shift_reg);
        if (m_sink != nullptr && device.name != Asic::Dummy) {
            m_sink->write_jtag_modeword(device.name, device.modeword);
        }
    }
}

// -- chain ---------------------------------------------------------------------------

Jtag::Jtag(JtagModewordSink* sink)
    : m_sink(sink)
{
    // Order of JTAG devices on step 2.x video boards; step 1.x uses the first
    // ten. Only the first ASIC of each type forwards its modeword.
    const Asic asics[] = {
        Asic::Jupiter,  // 0
        Asic::Mercury,  // 1
        Asic::Venus,    // 2
        Asic::Earth,    // 3 earth0
    };
    for (usize i = 0; i < m_chain.size(); ++i) m_chain[i].is_asic = false;
    for (usize i = 0; i < 4; ++i) {
        m_chain[i].is_asic = true;
        m_chain[i].name    = asics[i];
    }
    m_chain[5].is_asic  = true;  // mars00
    m_chain[5].name     = Asic::Mars;
    m_chain[6].is_asic  = true;  // mars01
    m_chain[10].is_asic = true;  // earth1
    m_chain[12].is_asic = true;  // mars10
    m_chain[13].is_asic = true;  // mars11
    for (auto& d : m_chain) d.reset();
}

void Jtag::set_stepping(int stepping)
{
    auto set = [this](usize index, u32 id) { m_chain[index].id_code = id; };
    switch (stepping) {
        case 0x21:
            set(1, 0x416c3057); set(2, 0x316c4057); set(3, 0x516c5057); set(10, 0x516c5057);
            set(5, 0x316c6057); set(6, 0x316c6057); set(12, 0x316c6057); set(13, 0x316c6057);
            set(0, 0x516c7057);
            m_num_devices = 17;
            break;
        case 0x20:
            set(1, 0x416c3057); set(2, 0x316c4057); set(3, 0x416c5057); set(10, 0x416c5057);
            set(5, 0x316c6057); set(6, 0x316c6057); set(12, 0x316c6057); set(13, 0x316c6057);
            set(0, 0x416c7057);
            m_num_devices = 17;
            break;
        case 0x15:
            set(1, 0x316c3057); set(2, 0x216c4057); set(3, 0x316c5057);
            set(5, 0x216c6057); set(6, 0x216c6057); set(0, 0x316c7057);
            m_num_devices = 10;
            break;
        case 0x10:
        default:
            set(1, 0x216c3057); set(2, 0x116c4057); set(3, 0x216c5057);
            set(5, 0x116c6057); set(6, 0x116c6057); set(0, 0x116c7057);
            m_num_devices = 10;
            break;
    }
}

namespace {

// Finite state machine. Each state has two possible next states (tms = 0, 1).
constexpr u8 kFsm[16][2] = {
    {1, 0},    // 0  Test-Logic/Reset
    {1, 2},    // 1  Run-Test/Idle
    {3, 9},    // 2  Select-DR-Scan
    {4, 5},    // 3  Capture-DR
    {4, 5},    // 4  Shift-DR
    {6, 8},    // 5  Exit1-DR
    {6, 7},    // 6  Pause-DR
    {4, 8},    // 7  Exit2-DR
    {1, 2},    // 8  Update-DR
    {10, 0},   // 9  Select-IR-Scan
    {11, 12},  // 10 Capture-IR
    {11, 12},  // 11 Shift-IR
    {13, 15},  // 12 Exit1-IR
    {13, 14},  // 13 Pause-IR
    {11, 15},  // 14 Exit2-IR
    {1, 2},    // 15 Update-IR
};

}  // namespace

void Jtag::write(bool tck, bool tms, bool tdi, bool trst)
{
    if (!trst) {
        for (u32 i = 0; i < m_num_devices; ++i) m_chain[i].reset();
        return;
    }
    if (m_last_tck == tck) return;
    m_last_tck = tck;

    if (tck) {
        switch (m_state) {
            case State::TestLogicReset:
                for (u32 i = 0; i < m_num_devices; ++i) m_chain[i].reset();
                break;
            case State::CaptureDR:
                for (u32 i = 0; i < m_num_devices; ++i) m_chain[i].capture_dr();
                break;
            case State::CaptureIR:
                for (u32 i = 0; i < m_num_devices; ++i) m_chain[i].capture_ir();
                break;
            case State::ShiftDR:
            case State::ShiftIR:
                for (u32 i = 0; i < m_num_devices; ++i) tdi = m_chain[i].shift(tdi);
                break;
            default: break;
        }
        m_state = static_cast<State>(kFsm[static_cast<int>(m_state)][tms ? 1 : 0]);
    } else {
        switch (m_state) {
            case State::ShiftDR:
            case State::ShiftIR:
                m_tdo = (m_chain[m_num_devices - 1].shift_reg & 1) != 0;
                break;
            case State::UpdateDR:
                for (u32 i = 0; i < m_num_devices; ++i) update_dr(m_chain[i]);
                break;
            case State::UpdateIR:
                for (u32 i = 0; i < m_num_devices; ++i) m_chain[i].update_ir();
                break;
            default: break;
        }
    }
}

void Jtag::reset()
{
    m_state = State::TestLogicReset;
    for (auto& d : m_chain) d.reset();
}

void Jtag::serialize(Archive& ar)
{
    ar.raw(m_state);
    ar.raw(m_last_tck);
    ar.raw(m_tdo);
    ar.raw(m_num_devices);
    for (auto& d : m_chain) {
        ar.raw(d.shift_reg);
        ar.raw(d.shift_size);
        ar.raw(d.instruction);
        ar.raw(d.id_code);
        ar.raw(d.modeword);
        if (ar.loading() && (d.shift_size == 0 || d.shift_size > 32)) ar.mark_failed();
    }
    if (ar.loading() && (m_num_devices == 0 || m_num_devices > m_chain.size())) ar.mark_failed();
}

}  // namespace sm2::hw::model3
