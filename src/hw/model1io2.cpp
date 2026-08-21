// SPDX-License-Identifier: BSD-3-Clause
//
// See model1io2.h.
//
// Derived from MAME's src/mame/sega/model1io2.cpp, BSD-3-Clause.

#include "hw/model1io2.h"

#include "core/log.h"

#include <algorithm>

namespace sm2::hw {
namespace {

// ---------------------------------------------------------------------------
// Z80 memory map, from MAME's model1io2_device::mem_map
// ---------------------------------------------------------------------------

constexpr u16 kRomBase      = 0x0000;
constexpr u16 kExpanderBase = 0x8000;
constexpr u16 kExpanderSize = 0x0010;
constexpr u16 kBoardPort    = 0x8040;  ///< Board buttons, jumpers, EEPROM data out
constexpr u16 kDsw1Port     = 0x8080;
constexpr u16 kFpgaBase     = 0x8100;
constexpr u16 kFpgaSize     = 0x0010;
constexpr u16 kAdcBase      = 0x8200;  ///< Mirrored at 0x8204
constexpr u16 kAdcSize      = 0x0004;
constexpr u16 kRamBase      = 0xe000;
constexpr u32 kRamSize      = 0x2000;

/// The TMPZ84C015's internal I/O map, from MAME's internal_io_map. Every entry is
/// mirrored across the high address byte, so only the low byte is decoded.
constexpr u8 kCtcBase   = 0x10;
constexpr u8 kSioBase   = 0x18;
constexpr u8 kPioBase   = 0x1c;
constexpr u8 kWdtmrPort = 0xf0;
constexpr u8 kWdtcrPort = 0xf1;
constexpr u8 kIrqPrioPort = 0xf4;

/// What an undriven Z80 bus reads back.
constexpr u8 kFloatingBus = 0xff;

/// How many words the firmware has to push at the FPGA before it stops reporting
/// busy. MAME's threshold, and the firmware's bitstream is longer than this.
constexpr u32 kFpgaReadyAfter = 0x1400;

/// The off-screen test's border, as a fraction of an axis's declared range.
constexpr float kBorderFraction = 0.05F;

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Model1io2::Model1io2() : m_cpu(*this)
{
    m_ram.assign(kRamSize, 0);

    // MAME's device_add_mconfig. The expander's serial side reaches the dual-port
    // RAM; its ports carry the panel, the lamps, the EEPROM and the drive board.
    m_io.set_serial_read([this](u32 address) { return io_read(address); });
    m_io.set_serial_write([this](u32 address, u8 value) { io_write(address, value); });
    m_io.set_input(0, [this] { return io_pa_read(); });
    m_io.set_input(1, [this] { return io_pb_read(); });
    m_io.set_input(2, [this] { return io_pc_read(); });
    m_io.set_output(3, [this](u8 value) { io_pd_write(value); });
    m_io.set_input(4, [this] { return io_pe_read(); });
    m_io.set_output(4, [this](u8 value) { io_pe_write(value); });
    m_io.set_output(5, [this](u8 value) { io_pf_write(value); });
    m_io.set_output(6, [this](u8 value) { io_pg_write(value); });

    for (u32 channel = 0; channel < Msm6253::kChannelCount; ++channel) {
        m_adc.set_input(channel, [this, channel] { return analog_read(channel); });
    }

    // The CTC's interrupt is what paces the firmware. Channels 2 and 3 clock the
    // SIO's baud generator, which nothing here consumes.
    m_ctc.set_interrupt_handler([this](bool asserted) {
        if (asserted) ++m_counters.interrupts;
        m_cpu.set_irq_line(asserted);
    });

    m_pio.set_input(0, [this] { return m_dsw2; });
    m_pio.set_input(1, [this] { return m_dsw3; });

    m_lightgun_min.fill(0);
    m_lightgun_max.fill(0x3ff);
}

Model1io2::~Model1io2() = default;

void Model1io2::attach(std::span<const u8> firmware)
{
    m_firmware = firmware;
    if (firmware.empty()) {
        SM2_WARN("model1io2: no firmware; the I/O board will not run and the "
                 "program will see no inputs");
    } else if (firmware.size() < kRomWindow) {
        SM2_WARN("model1io2: firmware is %zu byte(s), less than the 32 KB window",
                 firmware.size());
    }
}

void Model1io2::reset()
{
    std::fill(m_ram.begin(), m_ram.end(), u8{0});
    m_io.reset();
    m_adc.reset();
    m_ctc.reset();
    m_pio.reset();
    m_secondary_controls = false;
    m_fpga_counter       = 0;
    m_lcd_data           = 0;
    m_sio_pointer.fill(0);
    m_cycle_debt = 0;
    m_counters   = Counters{};
    m_cpu.reset();
}

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------

void Model1io2::run(u32 host_cycles)
{
    if (!present()) {
        return;
    }

    // 9.8304 MHz against the main board's 25 MHz. The debt is kept in units of
    // 25 000 000 so the ratio is exact across slices; at the interleave the
    // machine uses, rounding per call would lose a large part of the board's time.
    constexpr u64 kHostClock = 25'000'000;
    m_cycle_debt += static_cast<u64>(host_cycles) * kCpuClock;
    const s32 cycles = static_cast<s32>(m_cycle_debt / kHostClock);
    m_cycle_debt %= kHostClock;

    if (cycles > 0) {
        static_cast<void>(m_cpu.run(cycles));
        m_ctc.run(static_cast<u32>(cycles));
    }
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

void Model1io2::set_dual_port(DualPortRead read_handler, DualPortWrite write_handler)
{
    m_dual_port_read  = std::move(read_handler);
    m_dual_port_write = std::move(write_handler);
}

void Model1io2::set_input(u32 index, InputHandler handler)
{
    if (index < kInputCount) m_input[index] = std::move(handler);
}

void Model1io2::set_analog(u32 channel, InputHandler handler)
{
    if (channel < kAnalogCount) m_analog[channel] = std::move(handler);
}

void Model1io2::set_output(OutputHandler handler)
{
    m_output = std::move(handler);
}

void Model1io2::set_drive(InputHandler read_handler, OutputHandler write_handler)
{
    m_drive_read  = std::move(read_handler);
    m_drive_write = std::move(write_handler);
}

void Model1io2::set_lightgun(u32 axis, LightgunHandler handler)
{
    if (axis < kLightgunAxes) m_lightgun[axis] = std::move(handler);
}

void Model1io2::set_lightgun_range(u32 axis, u16 minimum, u16 maximum)
{
    if (axis >= kLightgunAxes) return;
    m_lightgun_min[axis] = minimum;
    m_lightgun_max[axis] = maximum;
}

void Model1io2::set_dipswitches(u8 dsw2, u8 dsw3)
{
    m_dsw2 = dsw2;
    m_dsw3 = dsw3;
}

// ---------------------------------------------------------------------------
// Expander callbacks
// ---------------------------------------------------------------------------

u8 Model1io2::io_read(u32 address)
{
    ++m_counters.dual_port_reads;
    return m_dual_port_read ? m_dual_port_read(address) : kFloatingBus;
}

void Model1io2::io_write(u32 address, u8 value)
{
    ++m_counters.dual_port_writes;
    if (m_dual_port_write) m_dual_port_write(address, value);
}

u8 Model1io2::io_pa_read()
{
    return m_input[0] ? m_input[0]() : 0xff;
}

u8 Model1io2::io_pb_read()
{
    return m_input[1] ? m_input[1]() : 0xff;
}

u8 Model1io2::io_pc_read()
{
    return m_input[2] ? m_input[2]() : 0xff;
}

void Model1io2::io_pd_write(u8 value)
{
    ++m_counters.output_writes;
    if (m_output) m_output(value);
}

u8 Model1io2::io_pe_read()
{
    // CN6, which is the drive board on a cabinet that has one.
    return m_drive_read ? m_drive_read() : 0xff;
}

void Model1io2::io_pe_write(u8 value)
{
    // CN6 again, shared with the diagnostic LCD's data lines.
    m_lcd_data = value;
    if (m_drive_write) m_drive_write(value);
}

void Model1io2::io_pf_write(u8 value)
{
    // 7-------  eeprom pe
    // -6------  eeprom di
    // --5-----  eeprom clk
    // ---4----  eeprom cs
    // ----3---  cn6 enabled
    // -----2--  cn6 lcd e
    // ------1-  cn6 lcd rw
    // -------0  cn6 lcd rs
    //
    // Data and chip select settle before the clock edge that samples them, for
    // the same reason as on the first board.
    m_eeprom.set_di(bit(value, 6) != 0);
    m_eeprom.set_cs(bit(value, 4) != 0);
    m_eeprom.set_clk(bit(value, 5) != 0);
    // The LCD strobe -- bit 3 clear, bit 2 set, bit 1 clear -- is where MAME
    // writes m_lcd_data to the display. There is no display here.
}

void Model1io2::io_pg_write(u8 value)
{
    // 7-------  watchdog
    // -6------  control panel switch
    // --5-----  comm_err led
    // ---43210  test points
    //
    // The watchdog is not modelled: MAME's MB3773 only resets the board if the
    // firmware stops kicking it, which is a failure mode rather than a feature.
    m_secondary_controls = bit(value, 6) != 0;
}

u8 Model1io2::analog_read(u32 channel)
{
    ++m_counters.analog_samples;
    const u32           selected = m_secondary_controls ? channel + 4 : channel;
    const InputHandler& handler  = m_analog[selected];
    return handler ? handler() : 0xff;
}

// ---------------------------------------------------------------------------
// The light gun FPGA
// ---------------------------------------------------------------------------

u8 Model1io2::fpga_read(u32 offset)
{
    if (m_fpga_counter < kFpgaReadyAfter) {
        // Still taking its bitstream.
        return 0x80;
    }

    if (offset < 8) {
        ++m_counters.lightgun_reads;
        const u32              axis    = offset >> 1;
        const LightgunHandler& handler = m_lightgun[axis];
        const u16              value   = handler ? handler() : 0;
        return static_cast<u8>(value >> (8 * (offset & 1)));
    }
    if (offset == 8) {
        return lightgun_offscreen();
    }
    return 0xff;
}

void Model1io2::fpga_write(u8 /*value*/)
{
    // The bitstream itself is not modelled; only how much of it has arrived.
    ++m_fpga_counter;
    ++m_counters.fpga_words;
}

u8 Model1io2::lightgun_offscreen() const
{
    // MAME's test: a gun within five per cent of either end of an axis counts as
    // pointed off the screen, and the two players share one bit each. The unused
    // bits read as ones.
    u8 data = 0xfc;

    const auto outside = [this](u32 axis) {
        const LightgunHandler& handler = m_lightgun[axis];
        if (!handler) return false;
        const u16 minimum = m_lightgun_min[axis];
        const u16 maximum = m_lightgun_max[axis];
        if (maximum <= minimum) return false;
        const auto border =
            static_cast<u16>(static_cast<float>(maximum - minimum) * kBorderFraction);
        const u16 value = handler();
        return value <= static_cast<u16>(minimum + border)
            || value >= static_cast<u16>(maximum - border);
    };

    // Axis order is P1 Y, P1 X, P2 Y, P2 X.
    if (outside(0) || outside(1)) data |= 1;
    if (outside(2) || outside(3)) data |= 2;
    return data;
}

// ---------------------------------------------------------------------------
// Z80 bus
// ---------------------------------------------------------------------------

u8 Model1io2::read8(u16 address)
{
    if (address < kRomBase + kRomWindow) {
        const u32 offset = address - kRomBase;
        return offset < m_firmware.size() ? m_firmware[offset] : kFloatingBus;
    }
    if (address >= kExpanderBase && address < kExpanderBase + kExpanderSize) {
        return m_io.read(address - kExpanderBase);
    }
    if (address == kBoardPort) {
        // 7-------  eeprom nc, active low so it reads high
        // -6------  eeprom do
        // --5-----  MODE jumper JP3, off
        // ---4----  ROM_EMU jumper JP4, off
        // ----3210  board buttons 0 to 3, active low so released reads high
        //
        // The four buttons read as released and both jumpers as off, which is the
        // configuration that runs the game rather than the diagnostic menu.
        u8 data = 0x3f;
        if (m_eeprom.data_out()) data |= 0x40;
        data |= 0x80;
        return data;
    }
    if (address == kDsw1Port) {
        // The board's own DSW1. Virtua Cop reads reloading mode and enemy type
        // here; both read as their default position.
        return 0xff;
    }
    if (address >= kFpgaBase && address < kFpgaBase + kFpgaSize) {
        return fpga_read(address - kFpgaBase);
    }
    if (address >= kAdcBase && address < kAdcBase + kAdcSize * 2) {
        return m_adc.d0_read();
    }
    if (address >= kRamBase) {
        return m_ram[(address - kRamBase) & (kRamSize - 1)];
    }

    ++m_counters.unmapped_reads;
    SM2_TRACE("model1io2: unmapped read at %04x (pc %04x)", address, m_cpu.pc());
    return kFloatingBus;
}

void Model1io2::write8(u16 address, u8 value)
{
    if (address >= kRamBase) {
        m_ram[(address - kRamBase) & (kRamSize - 1)] = value;
        return;
    }
    if (address >= kExpanderBase && address < kExpanderBase + kExpanderSize) {
        m_io.write(address - kExpanderBase, value);
        return;
    }
    if (address >= kFpgaBase && address < kFpgaBase + kFpgaSize) {
        fpga_write(value);
        return;
    }
    if (address >= kAdcBase && address < kAdcBase + kAdcSize * 2) {
        // A write starts a conversion; the low two bits pick the channel.
        m_adc.address_write((address - kAdcBase) & 3);
        return;
    }
    if (address < kRomBase + kRomWindow) {
        return;  // writes to the firmware EPROM are discarded
    }

    ++m_counters.unmapped_writes;
    SM2_TRACE("model1io2: unmapped write at %04x = %02x (pc %04x)", address, value,
              m_cpu.pc());
}

u8 Model1io2::io_read8(u16 port)
{
    const u8 low = static_cast<u8>(port & 0xff);

    if (low >= kCtcBase && low < kCtcBase + 4) {
        return m_ctc.read(low - kCtcBase);
    }
    if (low >= kPioBase && low < kPioBase + 4) {
        return m_pio.read_alt(low - kPioBase);
    }
    if (low >= kSioBase && low < kSioBase + 4) {
        // Nothing is connected to either channel, so the receiver never has a
        // character and the transmitter is always free. This firmware never reads
        // the SIO at all; the value is here so that a read cannot look like data.
        return 0x00;
    }
    if (low == kWdtmrPort) {
        return 0xff;
    }

    SM2_TRACE("model1io2: unhandled I/O read from port %02x (pc %04x)", low,
              m_cpu.pc());
    return kFloatingBus;
}

void Model1io2::io_write8(u16 port, u8 value)
{
    const u8 low = static_cast<u8>(port & 0xff);

    if (low >= kCtcBase && low < kCtcBase + 4) {
        m_ctc.write(low - kCtcBase, value);
        return;
    }
    if (low >= kPioBase && low < kPioBase + 4) {
        m_pio.write_alt(low - kPioBase, value);
        return;
    }
    if (low >= kSioBase && low < kSioBase + 4) {
        // Track the register pointer so a data byte written to a control port
        // cannot be mistaken for a command. Nothing else needs modelling: the
        // firmware only configures a terminal that is not plugged in.
        const u32 channel = (low - kSioBase) & 1;
        if ((low - kSioBase) >= 2) {
            m_sio_pointer[channel] = static_cast<u8>(value & 0x07);
        }
        return;
    }
    if (low == kWdtmrPort || low == kWdtcrPort || low == kIrqPrioPort) {
        // Watchdog period, watchdog control and the daisy chain's priority order.
        // Only one peripheral here ever interrupts, so the priority is moot.
        return;
    }

    SM2_TRACE("model1io2: unhandled I/O write to port %02x = %02x (pc %04x)", low,
              value, m_cpu.pc());
}

u8 Model1io2::interrupt_vector()
{
    // Interrupt mode 2, with the CTC supplying the low byte of the vector table
    // index. The PIO and SIO never request, so the chain has one member.
    return m_ctc.acknowledge_vector();
}

void Model1io2::interrupt_return()
{
    // Without this the CTC's in-service latch never clears and the timer
    // interrupt fires exactly once: the board ran ten million instructions and
    // took one interrupt before this was wired.
    m_ctc.return_from_interrupt();
}

}  // namespace sm2::hw
