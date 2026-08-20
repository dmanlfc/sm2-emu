// SPDX-License-Identifier: BSD-3-Clause
//
// Sega Model 1 I/O board (837-8950-01 / 837-8936 / 837-10539).
//
// Derived from MAME's src/mame/sega/model1io.{h,cpp} (BSD-3-Clause,
// copyright-holders Dirk Best).
//
// A whole small computer bolted to the side of the main board: a 4 MHz Z80, 16 KB
// of its own firmware, 8 KB of RAM, a 315-5338A I/O expander for the switches and
// lamps, an MSM6253 for the analogue controls, and a 93C45 holding the operator
// settings. The main board never sees any of it directly -- the only connection is
// a 2K dual-port RAM, which the Z80 fills in and the i960 reads.
//
// That indirection is the reason this board exists as a device rather than as a
// handful of registers: on the CRX boards Sega replaced the whole thing with a
// 315-5649 wired straight onto the i960's bus, so on the original Model 2 the
// input path runs through a second CPU executing a real program, and inputs only
// appear if that program is actually running.
//
// What is deliberately not modelled:
//
//   * The four on-board push buttons (MAME's "buttons" port, SW4 to SW7). They
//     read as released, which is their idle state.
//   * The three on-board dipswitch banks. They read as all ones, which is what
//     every Model 2 title's PORT_DIPUNUSED default gives them; no title in scope
//     reads a meaningful value from them.
//   * The opto-isolators, the watchdog and the network connector.
//
// Note that only the low 16 KB of the 64 KB EPROM is addressable: MAME's mem_map
// maps 0x0000-0x3fff and the ROM region is 0x10000 bytes. That is the board, not
// an oversight -- the part is a 27C512 in a socket wired for a smaller device.
#pragma once

#include "core/types.h"
#include "cpu/z80/z80.h"
#include "hw/eeprom_93c46.h"
#include "hw/msm6253.h"
#include "hw/sega_315_5338a.h"

#include <array>
#include <functional>
#include <span>
#include <vector>

namespace sm2::hw {

class Model1io final : public cpu::z80::Bus {
public:
    /// 32 MHz crystal divided by eight, as marked on the board.
    static constexpr u32 kCpuClock = 32'000'000 / 8;
    static_assert(kCpuClock == 4'000'000, "the I/O board's Z80 runs at 4 MHz");

    /// Bytes of the firmware EPROM the Z80 can reach.
    static constexpr u32 kRomWindow = 0x4000;

    /// The MB8464 beside the CPU.
    static constexpr u32 kRamSize = 0x2000;

    /// Digital input ports the main board's machine config binds. MAME's
    /// in_callback<0..2>, read through the expander's ports B, C and D.
    static constexpr u32 kInputCount = 3;

    /// Analogue channels. Only four reach the converter at a time; the upper four
    /// are the second set the control-panel switch selects.
    static constexpr u32 kAnalogCount = 8;

    using InputHandler  = std::function<u8()>;
    using OutputHandler = std::function<void(u8)>;

    /// The dual-port RAM's left-hand side, as seen from this board.
    using DualPortRead  = std::function<u8(u32 address)>;
    using DualPortWrite = std::function<void(u32 address, u8 value)>;

    Model1io();
    ~Model1io() override;

    Model1io(const Model1io&)            = delete;
    Model1io& operator=(const Model1io&) = delete;

    /// Attach the firmware. `firmware` is the `ioboard` ROM region; only its low
    /// 16 KB is mapped, and an empty span leaves the board inert with a warning
    /// rather than refusing to start, so a synthetic machine stays usable.
    void attach(std::span<const u8> firmware);

    void reset();

    /// True when firmware was supplied and the Z80 has something to execute.
    [[nodiscard]] bool present() const { return !m_firmware.empty(); }

    /// Advance the board by its share of `host_cycles` of the main board's 25 MHz
    /// clock.
    ///
    /// The board's Z80 runs at 4 MHz against that, so the remainder is carried
    /// between calls rather than rounded away; at the fine interleave the machine
    /// uses, rounding would lose most of the board's time.
    void run(u32 host_cycles);

    // -- wiring ------------------------------------------------------------

    /// The dual-port RAM shared with the main board. Bound to its left-hand port.
    void set_dual_port(DualPortRead read_handler, DualPortWrite write_handler);

    /// One of the three digital input ports. Index 0 is IN0 (coins, service, test,
    /// start), 1 is IN1 (player 1) and 2 is IN2 (player 2, unbound on Desert
    /// Tank, matching MAME's model2o config which only binds 0 and 1).
    void set_input(u32 index, InputHandler handler);

    /// One of the eight analogue channels. MAME's an_callback<N>.
    void set_analog(u32 channel, InputHandler handler);

    /// The lamp and coin-counter latch on the expander's port F. MAME's
    /// output_callback.
    void set_output(OutputHandler handler);

    /// The force-feedback drive board on the expander's port E. Only Daytona
    /// binds these; Desert Tank leaves them unconnected.
    void set_drive(InputHandler read_handler, OutputHandler write_handler);

    // -- inspection --------------------------------------------------------

    [[nodiscard]] cpu::z80::Z80&       cpu() { return m_cpu; }
    [[nodiscard]] const cpu::z80::Z80& cpu() const { return m_cpu; }

    [[nodiscard]] Eeprom93c46&       eeprom() { return m_eeprom; }
    [[nodiscard]] const Eeprom93c46& eeprom() const { return m_eeprom; }

    /// True while the control-panel switch selects the second set of controls,
    /// which also swaps the dipswitch banks in over ports B, C and D.
    [[nodiscard]] bool secondary_controls() const { return m_secondary_controls; }

    struct Counters {
        u64 dual_port_reads  = 0;
        u64 dual_port_writes = 0;
        u64 analog_samples   = 0;  ///< Conversions started.
        u64 output_writes    = 0;  ///< Lamp latch updates.
        u64 unmapped_reads   = 0;
        u64 unmapped_writes  = 0;
        u64 io_port_reads    = 0;  ///< Z80 IN instructions, which this board has none for.
        u64 io_port_writes   = 0;
    };
    [[nodiscard]] const Counters& counters() const { return m_counters; }

    // -- cpu::z80::Bus -----------------------------------------------------

    u8   read8(u16 address) override;
    void write8(u16 address, u8 value) override;
    u8   io_read8(u16 port) override;
    void io_write8(u16 port, u8 value) override;

private:
    // Expander callbacks, named after MAME's model1io_device members.
    [[nodiscard]] u8 io_read(u32 address);
    void             io_write(u32 address, u8 value);
    void             io_pa_write(u8 value);
    [[nodiscard]] u8 io_pb_read();
    [[nodiscard]] u8 io_pc_read();
    [[nodiscard]] u8 io_pd_read();
    [[nodiscard]] u8 io_pe_read();
    void             io_pe_write(u8 value);
    void             io_pf_write(u8 value);
    [[nodiscard]] u8 io_pg_read();

    /// The pair of 74HC4066 analogue switches in front of the converter: the
    /// control-panel switch picks channels 0-3 or 4-7.
    [[nodiscard]] u8 analog_read(u32 channel);

    cpu::z80::Z80  m_cpu;
    Sega3155338a   m_io;
    Msm6253        m_adc;
    Eeprom93c46    m_eeprom;

    std::span<const u8> m_firmware;
    std::vector<u8>     m_ram;

    std::array<InputHandler, kInputCount>  m_input{};
    std::array<InputHandler, kAnalogCount> m_analog{};

    DualPortRead  m_dual_port_read;
    DualPortWrite m_dual_port_write;
    OutputHandler m_output;
    InputHandler  m_drive_read;
    OutputHandler m_drive_write;

    /// MAME's m_secondary_controls, set from bit 0 of the expander's port A.
    bool m_secondary_controls = false;

    /// Host cycles owed to this board, times four. See run().
    u32 m_cycle_debt = 0;

    Counters m_counters;
};

}  // namespace sm2::hw
