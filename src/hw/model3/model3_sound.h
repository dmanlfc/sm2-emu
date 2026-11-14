// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sega Model 3 sound board: a 68000, two SCSPs with 1 MB of RAM each, the
// sample ROM, and the optional MPEG music board (DSB1 or DSB2) whose output is
// mixed in.
//
// Derived from Supermodel's Src/Model3/SoundBoard.cpp and SoundBoard.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c) 2011
// Bart Trzynadlowski, Nik Henson. The MIDI port, interrupt and output mixing
// follow Supermodel's Src/Model3/Model3.cpp, Src/Model3/DSB.cpp (Copyright (c)
// 2003-2026 The Supermodel Team; "based on code donated by R. Belmont") and
// Src/OSD/SDL/Audio.cpp.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// The 68000 is Musashi and the SCSPs come from MAME; the slave SCSP's IRQs
// are unconnected. Memory is kept big-endian. The board runs in host-cycle
// timeslices with an exact clock ratio. MIDI bytes queue in FIFOs here for the
// SCSP and the DSB, and the machine sets the MIDI interrupt timing through
// midi_irq_pulse(). Stereo output is folded down per Model3Audio.

#pragma once

#include "core/types.h"
#include "cpu/bus.h"
#include "cpu/m68000/m68000.h"
#include "hw/dsb2.h"
#include "hw/dsbz80.h"
#include "hw/scsp.h"
#include "hw/scsp_dsp.h"
#include "hw/sound_board.h"
#include "rom/game.h"

#include <array>
#include <functional>
#include <span>
#include <vector>

namespace sm2 {
class Archive;
}

namespace sm2::hw::model3 {

class Model3Sound final : public SoundBoard, public cpu::Bus {
public:
    /// 45.1584 MHz / 4; one 44.1 kHz sample is 256 68000 cycles.
    static constexpr u32 kCpuClock        = 11'289'600;
    static constexpr u32 kSampleRate      = 44'100;
    static constexpr u32 kCyclesPerSample = kCpuClock / kSampleRate;

    static constexpr u8 kMidiIrqBit = 0x40;

    /// The MIDI IRQ burst after an IRQ2 acknowledge: spacing and limit.
    static constexpr u32 kMidiIrqSpacingCycles = 1000;
    static constexpr u32 kMidiIrqBurstLimit    = 128;

    Model3Sound();
    ~Model3Sound() override;

    Model3Sound(const Model3Sound&)            = delete;
    Model3Sound& operator=(const Model3Sound&) = delete;

    // -- configuration -----------------------------------------------------

    /// Big-endian regions. With no program the board is inert.
    void attach(std::span<const u8> program_rom, std::span<const u8> samples);

    /// Call before reset().
    void attach_mpeg(rom::Model3MpegBoard board, std::span<const u8> program,
                     std::span<const u8> music);

    void set_audio_layout(rom::Model3Audio layout) { m_layout = layout; }

    /// The PowerPC clock run() counts in.
    void set_host_clock(u32 hz);

    /// Percent, 0-200.
    void set_sound_volume(u32 percent);
    void set_music_volume(u32 percent);

    // -- running -----------------------------------------------------------

    void reset();

    /// Slicing-independent: the clock ratio is carried exactly.
    void run(u32 host_cycles);

    void run_samples(u32 samples);

    // -- the PowerPC side: 0xF0080000-0xF00800FF --------------------------

    /// Offset 0 data, 4 control; others ignored.
    void write_port(u32 offset, u8 value);

    /// Constants 0x00 / 0x83 (Magical Truck's region check).
    [[nodiscard]] u8 read_port(u32 offset) const;

    /// To the master SCSP and the DSB. Deasserts the MIDI interrupt.
    void write_midi(u8 value);

    /// Bit 5 enables the MIDI interrupt; clearing it deasserts.
    void write_control(u8 value);

    [[nodiscard]] u8 control() const { return m_control; }

    [[nodiscard]] bool midi_irq_enabled() const { return (m_control & 0x20) != 0; }

    using MidiIrqHandler = std::function<void(bool asserted)>;
    void set_midi_irq_handler(MidiIrqHandler handler) { m_irq_handler = std::move(handler); }

    /// Asserts the interrupt if midi_irq_enabled(); called by the machine.
    void midi_irq_pulse();

    // -- SoundBoard ----------------------------------------------------------

    [[nodiscard]] std::span<const s16> pending_samples() const override { return m_pending; }
    void clear_pending_samples() override { m_pending.clear(); }
    [[nodiscard]] u32 sample_rate() const override { return kSampleRate; }
    [[nodiscard]] bool present() const override { return !m_program_rom.empty(); }
    [[nodiscard]] u32 active_voices() const override
    {
        return m_master.active_slots() + m_slave.active_slots();
    }

    // -- cpu::Bus: the 68000, big-endian -------------------------------------

    u8   read8(u32 address) override;
    u16  read16(u32 address) override;
    u32  read32(u32 address) override;
    void write8(u32 address, u8 value) override;
    void write16(u32 address, u16 value) override;
    void write32(u32 address, u32 value) override;

    // -- inspection ----------------------------------------------------------

    struct Counters {
        u64 midi_bytes      = 0;  ///< Data-port writes from the PowerPC.
        u64 control_writes  = 0;
        u64 irq_pulses      = 0;  ///< midi_irq_pulse() calls that asserted.
        u64 fifo_overflows  = 0;  ///< Bytes lost because a FIFO was full.
        u64 midi_out_bytes  = 0;  ///< Sent by the 68000; nothing listens.
        u64 bank_writes     = 0;
        u64 unmapped_reads  = 0;
        u64 unmapped_writes = 0;
        u64 samples         = 0;
        u64 samples_dropped = 0;  ///< Pending audio nobody drained (headless).
    };
    [[nodiscard]] const Counters& counters() const { return m_counters; }

    [[nodiscard]] const cpu::m68000::M68000& cpu() const { return m_cpu; }
    [[nodiscard]] const Scsp& master() const { return m_master; }
    [[nodiscard]] const Scsp& slave() const { return m_slave; }
    [[nodiscard]] const DsbZ80& dsb1() const { return m_dsb1; }
    [[nodiscard]] const Dsb2& dsb2() const { return m_dsb2; }
    [[nodiscard]] u8 bank_register() const { return m_bank; }

    [[nodiscard]] u32 scsp_fifo_level() const { return m_scsp_fifo.count; }
    [[nodiscard]] u32 dsb_fifo_level() const { return m_dsb_fifo.count; }

    /// Pre-mix FL FR RL RR of the last run(), when enabled.
    void keep_quad(bool enabled) { m_keep_quad = enabled; }
    [[nodiscard]] std::span<const s16> quad_samples() const { return m_quad; }
    void clear_quad_samples() { m_quad.clear(); }

    /// Between timeslices only. Excludes ROMs, callbacks, pending audio.
    void serialize(Archive& ar);

private:
    class ScspRam final : public ScspMemory {
    public:
        explicit ScspRam(std::vector<u8>& ram) : m_ram(&ram) {}
        u8   scsp_read_byte(u32 address) override;
        u16  scsp_read_word(u32 address) override;
        void scsp_write_word(u32 address, u16 value) override;

    private:
        std::vector<u8>* m_ram;
    };

    struct Fifo {
        std::array<u8, 1024> data{};
        u32 head  = 0;
        u32 count = 0;

        [[nodiscard]] bool push(u8 value);
        [[nodiscard]] u8 pop();
    };

    void advance();
    void generate_sample();
    void flush_chunk();
    void feed_scsp();
    void mix_down(u32 frames);

    [[nodiscard]] u8 sample_byte(u32 offset) const;
    [[nodiscard]] u16 scsp_read(Scsp& scsp, u32 address);
    void bank_write(u8 value);

    cpu::m68000::M68000 m_cpu;

    std::vector<u8> m_ram1;  ///< Master SCSP's RAM, 68000 0x000000.
    std::vector<u8> m_ram2;  ///< Slave SCSP's RAM, 68000 0x200000.
    ScspRam m_ram1_view;
    ScspRam m_ram2_view;
    Scsp    m_master;
    Scsp    m_slave;

    DsbZ80 m_dsb1;
    Dsb2   m_dsb2;
    rom::Model3MpegBoard m_mpeg = rom::Model3MpegBoard::None;
    rom::Model3Audio m_layout   = rom::Model3Audio::Stereo;

    std::span<const u8> m_program_rom;
    std::span<const u8> m_samples;

    u8 m_bank = 0;  ///< 0x400001; bit 4 selects the upper 8 MB of samples.

    // PowerPC port.
    u8             m_control = 0;
    MidiIrqHandler m_irq_handler;
    Fifo           m_scsp_fifo;
    Fifo           m_dsb_fifo;

    // Clocks.
    u32 m_host_hz      = 66'000'000;
    u64 m_cycle_debt   = 0;  ///< host cycles * kCpuClock, remainder mod m_host_hz.
    s64 m_cycle_budget = 0;  ///< 68000 cycles granted but not yet run.
    u64 m_sample_phase = 0;  ///< 68000 cycles into the current sample.
    u64 m_dsb_debt     = 0;  ///< For the DSB's 25 MHz time base; see flush_chunk.

    // Volume, 1/256 units.
    s32 m_sound_gain = 256;

    // Front (master + DSB) and rear (slave), stereo.
    std::vector<s16> m_front;
    std::vector<s16> m_rear;
    std::vector<s16> m_pending;
    std::vector<s16> m_quad;
    bool             m_keep_quad = false;

    Counters m_counters;
    bool     m_warned_no_program = false;
};

}  // namespace sm2::hw::model3
