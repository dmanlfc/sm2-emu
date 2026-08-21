// SPDX-License-Identifier: BSD-3-Clause
//
// Sega Model 1 I/O Board (Advanced), 837-11130 with 837-11131.
//
// Derived from MAME's src/mame/sega/model1io2.{h,cpp} (BSD-3-Clause,
// copyright-holder Dirk Best).
//
// The board Virtua Cop uses instead of the plain model1io. Same job -- read the
// panel, drive the lamps, fill a dual-port RAM the i960 reads -- but a different
// CPU and a different memory map, so the firmware is not interchangeable. Loading
// Virtua Cop's EPROM into a model1io got its Z80 writing to unmapped addresses
// thirteen million times a run and never touching the shared RAM at all.
//
// What is different from the first board:
//
//   * A TMPZ84C015 rather than a bare Z80: the same core with a Z80 CTC, PIO and
//     SIO on the die, reachable on the CPU's own I/O ports. The CTC's periodic
//     interrupt is what paces the firmware.
//   * A window at 0x8100 onto the FPGA that digitises the light guns. It reports
//     busy until enough configuration words have been written, then returns four
//     10-bit coordinates and an off-screen flag.
//   * 32 KB of firmware rather than 16, and its own RAM layout.
//   * A diagnostic LCD on port E, which is not modelled: it is a service aid,
//     reachable only by holding a board button at reset, and nothing on the
//     screen depends on it.
//
// What the firmware actually asks of the on-die peripherals was measured against
// MAME rather than assumed (tools/mame/iotap.lua): it programs the CTC's vector
// and two channels, initialises both SIO channels for a terminal that is not
// connected, puts both PIO ports into bit-control mode as inputs, and then reads
// only the two PIO data registers. Nothing reads the CTC or the SIO back.
#pragma once

#include "core/types.h"
#include "cpu/z80/z80.h"
#include "hw/eeprom_93c46.h"
#include "hw/msm6253.h"
#include "hw/sega_315_5338a.h"
#include "hw/z80ctc.h"
#include "hw/z80pio.h"

#include <array>
#include <functional>
#include <span>
#include <vector>

namespace sm2::hw {

class Model1io2 final : public cpu::z80::Bus {
public:
    /// 19.6608 MHz crystal divided by two, as MAME's machine config has it.
    static constexpr u32 kCpuClock = 19'660'800 / 2;

    /// The whole 32 KB EPROM is mapped here, unlike the first board where only
    /// the low quarter of a larger part is reachable.
    static constexpr u32 kRomWindow = 0x8000;

    /// Digital input ports the main board binds. MAME's in_callback<0..2>.
    static constexpr u32 kInputCount = 3;

    /// Analogue channels. Four reach the converter at a time; the control-panel
    /// switch selects the upper four.
    static constexpr u32 kAnalogCount = 8;

    /// Light gun axes, in MAME's order: P1 Y, P1 X, P2 Y, P2 X.
    static constexpr u32 kLightgunAxes = 4;

    using InputHandler    = std::function<u8()>;
    using OutputHandler   = std::function<void(u8)>;
    using LightgunHandler = std::function<u16()>;
    using DualPortRead    = std::function<u8(u32 address)>;
    using DualPortWrite   = std::function<void(u32 address, u8 value)>;

    Model1io2();
    ~Model1io2() override;
    Model1io2(const Model1io2&)            = delete;
    Model1io2& operator=(const Model1io2&) = delete;

    /// Attach the firmware. An empty span leaves the board inert with a warning
    /// rather than refusing to start.
    void attach(std::span<const u8> firmware);

    void reset();

    [[nodiscard]] bool present() const { return !m_firmware.empty(); }

    /// Advance the board by its share of `host_cycles` of the main board's 25 MHz
    /// clock, carrying the remainder between calls.
    void run(u32 host_cycles);

    // -- wiring ------------------------------------------------------------

    void set_dual_port(DualPortRead read_handler, DualPortWrite write_handler);
    void set_input(u32 index, InputHandler handler);
    void set_analog(u32 channel, InputHandler handler);
    void set_output(OutputHandler handler);
    void set_drive(InputHandler read_handler, OutputHandler write_handler);

    /// A light gun axis, 10 bits, in the order the FPGA reports them.
    void set_lightgun(u32 axis, LightgunHandler handler);

    /// The range an axis reports, which the off-screen test needs: MAME derives
    /// the border from the port's declared minimum and maximum.
    void set_lightgun_range(u32 axis, u16 minimum, u16 maximum);

    /// The two board dipswitch banks the PIO reads. MAME's dsw2 and dsw3.
    void set_dipswitches(u8 dsw2, u8 dsw3);

    // -- inspection --------------------------------------------------------

    [[nodiscard]] cpu::z80::Z80&       cpu() { return m_cpu; }
    [[nodiscard]] const cpu::z80::Z80& cpu() const { return m_cpu; }

    [[nodiscard]] Eeprom93c46&       eeprom() { return m_eeprom; }
    [[nodiscard]] const Eeprom93c46& eeprom() const { return m_eeprom; }

    [[nodiscard]] bool secondary_controls() const { return m_secondary_controls; }

    struct Counters {
        u64 dual_port_reads  = 0;
        u64 dual_port_writes = 0;
        u64 analog_samples   = 0;
        u64 output_writes    = 0;
        u64 fpga_words       = 0;  ///< Configuration words the firmware sent.
        u64 lightgun_reads   = 0;
        u64 unmapped_reads   = 0;
        u64 unmapped_writes  = 0;
        u64 interrupts       = 0;
    };
    [[nodiscard]] const Counters& counters() const { return m_counters; }

    // -- cpu::z80::Bus -----------------------------------------------------

    u8   read8(u16 address) override;
    void write8(u16 address, u8 value) override;
    u8   io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;
    u8   interrupt_vector() override;
    void interrupt_return() override;

private:
    // Expander callbacks, named after MAME's model1io2_device members.
    [[nodiscard]] u8 io_read(u32 address);
    void             io_write(u32 address, u8 value);
    [[nodiscard]] u8 io_pa_read();
    [[nodiscard]] u8 io_pb_read();
    [[nodiscard]] u8 io_pc_read();
    void             io_pd_write(u8 value);
    [[nodiscard]] u8 io_pe_read();
    void             io_pe_write(u8 value);
    void             io_pf_write(u8 value);
    void             io_pg_write(u8 value);

    [[nodiscard]] u8 analog_read(u32 channel);

    [[nodiscard]] u8 fpga_read(u32 offset);
    void             fpga_write(u8 value);
    [[nodiscard]] u8 lightgun_offscreen() const;

    cpu::z80::Z80  m_cpu;
    Sega3155338a   m_io;
    Msm6253        m_adc;
    Eeprom93c46    m_eeprom;
    Z80Ctc         m_ctc;
    Z80Pio         m_pio;

    std::span<const u8> m_firmware;

    /// 0xe000-0xefff is battery-backed on the board and 0xf000-0xffff is not.
    /// Nothing here can tell them apart, so they are one block.
    std::vector<u8> m_ram;

    std::array<InputHandler, kInputCount>     m_input{};
    std::array<InputHandler, kAnalogCount>    m_analog{};
    std::array<LightgunHandler, kLightgunAxes> m_lightgun{};
    std::array<u16, kLightgunAxes> m_lightgun_min{};
    std::array<u16, kLightgunAxes> m_lightgun_max{};

    DualPortRead  m_dual_port_read;
    DualPortWrite m_dual_port_write;
    OutputHandler m_output;
    InputHandler  m_drive_read;
    OutputHandler m_drive_write;

    u8 m_dsw2 = 0xff;
    u8 m_dsw3 = 0xff;

    bool m_secondary_controls = false;

    /// MAME's m_fpga_counter. The FPGA reports busy until 0x1400 words have been
    /// written, which is how the firmware knows its bitstream took.
    u32 m_fpga_counter = 0;

    /// Latched on the expander's port E, for the diagnostic LCD that is not
    /// modelled. Kept because the firmware writes it either way.
    u8 m_lcd_data = 0;

    /// SIO register pointer per channel. The SIO is write-only from this
    /// firmware's point of view, so tracking the pointer is enough to keep a
    /// register write from being mistaken for a command.
    std::array<u8, 2> m_sio_pointer{};

    /// Host cycles owed to this board, times a fixed multiple. See run().
    u64 m_cycle_debt = 0;

    Counters m_counters;
};

}  // namespace sm2::hw
