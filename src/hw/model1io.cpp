// SPDX-License-Identifier: BSD-3-Clause
//
// Derived from MAME's src/mame/sega/model1io.cpp, BSD-3-Clause.

#include "hw/model1io.h"

#include "core/log.h"

#include <algorithm>

namespace sm2::hw {
namespace {

// ---------------------------------------------------------------------------
// Z80 memory map, from MAME's model1io_device::mem_map
// ---------------------------------------------------------------------------

constexpr u16 kRomBase     = 0x0000;
constexpr u16 kRamBase     = 0x4000;
constexpr u16 kExpanderBase = 0x8000;
constexpr u16 kExpanderSize = 0x0010;
constexpr u16 kAdcBase     = 0xc000;
constexpr u16 kAdcSize     = 0x0004;

/// What an undriven Z80 bus reads back.
constexpr u8 kFloatingBus = 0xff;

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Model1io::Model1io() : m_cpu(*this)
{
    m_ram.assign(kRamSize, 0);

    // MAME's device_add_mconfig. The expander's serial side goes to the dual-port
    // RAM; its ports carry the panel, the lamps and the EEPROM.
    m_io.set_serial_read([this](u32 address) { return io_read(address); });
    m_io.set_serial_write([this](u32 address, u8 value) { io_write(address, value); });
    m_io.set_output(0, [this](u8 value) { io_pa_write(value); });
    m_io.set_input(1, [this] { return io_pb_read(); });
    m_io.set_input(2, [this] { return io_pc_read(); });
    m_io.set_input(3, [this] { return io_pd_read(); });
    m_io.set_input(4, [this] { return io_pe_read(); });
    m_io.set_output(4, [this](u8 value) { io_pe_write(value); });
    m_io.set_output(5, [this](u8 value) { io_pf_write(value); });
    m_io.set_input(6, [this] { return io_pg_read(); });

    for (u32 channel = 0; channel < Msm6253::kChannelCount; ++channel) {
        m_adc.set_input(channel, [this, channel] { return analog_read(channel); });
    }
}

Model1io::~Model1io() = default;

void Model1io::attach(std::span<const u8> firmware)
{
    m_firmware = firmware;
    if (firmware.empty()) {
        SM2_WARN("model1io: no firmware; the I/O board will not run and the "
                 "program will see no inputs");
    } else if (firmware.size() < kRomWindow) {
        SM2_WARN("model1io: firmware is %zu byte(s), less than the 16 KB window",
                 firmware.size());
    }
}

void Model1io::reset()
{
    std::fill(m_ram.begin(), m_ram.end(), u8{0});
    m_io.reset();
    m_adc.reset();
    m_secondary_controls = false;
    m_cycle_debt         = 0;
    m_counters           = Counters{};
    m_cpu.reset();
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

void Model1io::run(u32 host_cycles)
{
    if (!present()) {
        return;
    }

    // 4 MHz against the main board's 25 MHz. The debt accumulates in 25ths of a
    // host cycle so the ratio is exact across slices; the machine interleaves in
    // slices of about 128 host cycles, which is only 20 Z80 cycles, so rounding
    // per call would throw away a fifth of the board's time and the firmware
    // would run measurably slow.
    m_cycle_debt += host_cycles * 4;
    const s32 cycles = static_cast<s32>(m_cycle_debt / 25);
    m_cycle_debt %= 25;

    if (cycles > 0) {
        static_cast<void>(m_cpu.run(cycles));
    }
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

void Model1io::set_dual_port(DualPortRead read_handler, DualPortWrite write_handler)
{
    m_dual_port_read  = std::move(read_handler);
    m_dual_port_write = std::move(write_handler);
}

void Model1io::set_input(u32 index, InputHandler handler)
{
    if (index < kInputCount) {
        m_input[index] = std::move(handler);
    }
}

void Model1io::set_analog(u32 channel, InputHandler handler)
{
    if (channel < kAnalogCount) {
        m_analog[channel] = std::move(handler);
    }
}

void Model1io::set_output(OutputHandler handler)
{
    m_output = std::move(handler);
}

void Model1io::set_drive(InputHandler read_handler, OutputHandler write_handler)
{
    m_drive_read  = std::move(read_handler);
    m_drive_write = std::move(write_handler);
}

// ---------------------------------------------------------------------------
// Expander callbacks
// ---------------------------------------------------------------------------

u8 Model1io::io_read(u32 address)
{
    ++m_counters.dual_port_reads;
    return m_dual_port_read ? m_dual_port_read(address) : kFloatingBus;
}

void Model1io::io_write(u32 address, u8 value)
{
    ++m_counters.dual_port_writes;
    if (m_dual_port_write) {
        m_dual_port_write(address, value);
    }
}

void Model1io::io_pa_write(u8 value)
{
    // 7-------  eeprom clk
    // -6------  eeprom cs
    // --5-----  eeprom di
    // ---4----  eeprom pe
    // ----32--  not used
    // ------1-  led2
    // -------0  control switch (0 = first set of controls, 1 = second)
    //
    // MAME writes clk before di and cs; the order is set here so the data and
    // chip-select lines are settled before the clock edge that samples them,
    // because this EEPROM latches DI on the rising edge. The firmware holds DI
    // steady across the clock pulse, so both orders give the same result -- but
    // only one of them is robust to firmware that does not.
    m_eeprom.set_di(bit(value, 5) != 0);
    m_eeprom.set_cs(bit(value, 6) != 0);
    m_eeprom.set_clk(bit(value, 7) != 0);

    m_secondary_controls = bit(value, 0) != 0;
}

u8 Model1io::io_pb_read()
{
    // In secondary mode the three input ports read the board's own dipswitches
    // instead of the cabinet's panel. Those are unbound here (see the header), so
    // they read as all ones, which is their idle position.
    if (m_secondary_controls) {
        return 0xff;
    }
    return m_input[0] ? m_input[0]() : 0xff;
}

u8 Model1io::io_pc_read()
{
    if (m_secondary_controls) {
        return 0xff;
    }
    return m_input[1] ? m_input[1]() : 0xff;
}

u8 Model1io::io_pd_read()
{
    if (m_secondary_controls) {
        return 0xff;
    }
    return m_input[2] ? m_input[2]() : 0xff;
}

u8 Model1io::io_pe_read()
{
    return m_drive_read ? m_drive_read() : 0xff;
}

void Model1io::io_pe_write(u8 value)
{
    if (m_drive_write) {
        m_drive_write(value);
    }
}

void Model1io::io_pf_write(u8 value)
{
    ++m_counters.output_writes;
    if (m_output) {
        m_output(value);
    }
}

u8 Model1io::io_pg_read()
{
    // 7-------  eeprom do
    // -6------  eeprom nc
    // --54----  not used
    // ----3210  button board 0 to 3 (SW4 to SW7), active low
    //
    // The four on-board buttons read as released. The 0x70 is MAME's, and it is
    // what leaves bit 6 -- the EEPROM's unconnected second data pin -- high.
    u8 data = 0x70 | 0x0f;
    if (m_eeprom.data_out()) {
        data |= 0x80;
    }
    return data;
}

u8 Model1io::analog_read(u32 channel)
{
    ++m_counters.analog_samples;
    const u32           selected = m_secondary_controls ? channel + 4 : channel;
    const InputHandler& handler  = m_analog[selected];
    return handler ? handler() : 0xff;
}

// ---------------------------------------------------------------------------
// Z80 bus
// ---------------------------------------------------------------------------

u8 Model1io::read8(u16 address)
{
    if (address < kRomBase + kRomWindow) {
        const u32 offset = address - kRomBase;
        return offset < m_firmware.size() ? m_firmware[offset] : kFloatingBus;
    }
    if (address >= kRamBase && address < kRamBase + kRamSize) {
        return m_ram[address - kRamBase];
    }
    if (address >= kExpanderBase && address < kExpanderBase + kExpanderSize) {
        return m_io.read(address - kExpanderBase);
    }
    if (address >= kAdcBase && address < kAdcBase + kAdcSize) {
        return m_adc.d0_read();
    }

    ++m_counters.unmapped_reads;
    SM2_TRACE("model1io: unmapped read at %04x (pc %04x)", address, m_cpu.pc());
    return kFloatingBus;
}

void Model1io::write8(u16 address, u8 value)
{
    if (address >= kRamBase && address < kRamBase + kRamSize) {
        m_ram[address - kRamBase] = value;
        return;
    }
    if (address >= kExpanderBase && address < kExpanderBase + kExpanderSize) {
        m_io.write(address - kExpanderBase, value);
        return;
    }
    if (address >= kAdcBase && address < kAdcBase + kAdcSize) {
        // A write starts a conversion; the low two bits of the address pick the
        // channel and the data is ignored.
        m_adc.address_write(address - kAdcBase);
        return;
    }
    if (address < kRomBase + kRomWindow) {
        return;  // writes to the firmware EPROM are discarded
    }

    ++m_counters.unmapped_writes;
    SM2_TRACE("model1io: unmapped write at %04x = %02x (pc %04x)", address, value,
              m_cpu.pc());
}

u8 Model1io::io_read8(u16 port)
{
    // The board decodes nothing in the Z80's I/O space: MAME's model1io_device
    // configures no AS_IO map at all. Counted rather than ignored, because an IN
    // instruction here would mean the firmware is not the one we think it is.
    ++m_counters.io_port_reads;
    SM2_TRACE("model1io: I/O read from port %04x (pc %04x)", port, m_cpu.pc());
    return kFloatingBus;
}

void Model1io::io_write8(u16 port, u8 value)
{
    ++m_counters.io_port_writes;
    SM2_TRACE("model1io: I/O write to port %04x = %02x (pc %04x)", port, value,
              m_cpu.pc());
}

}  // namespace sm2::hw
