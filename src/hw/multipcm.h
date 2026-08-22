// SPDX-License-Identifier: BSD-3-Clause
//
// Yamaha YMW-258-F "GEW8", which Sega call the 315-5560 and everyone calls
// MultiPCM. Twenty-eight PCM voices with a per-voice envelope generator, two
// LFOs and interpolation.
//
// Ported from MAME's src/devices/sound/gew.cpp and multipcm.cpp (BSD-3-Clause,
// copyright-holders Miguel Angel Horna), kept close to the original so the two
// can be diffed. The differences are structural rather than behavioural: this is
// a plain object rather than a device, the sample ROM is a span with the board's
// bank applied here instead of a banked address map, and generate() accumulates
// into a caller-supplied buffer at a route gain, which is how the Model 1 audio
// board mixes two of these and a YM3438 in one pass rather than through three
// streams and a speaker device.
//
// One guard is not in the original. VALUE_TO_CHANNEL has -1 in every eighth
// entry, and upstream stores that straight into m_cur_slot and then indexes
// m_slots with it; writing 0x07 to the voice-select register is enough to read
// out of bounds. Here a -1 parks m_cur_slot past the last voice and data writes
// to it are dropped, which is the same as having selected nothing.
//
// The sample ROM begins with a metadata table, twelve bytes per instrument:
// three bytes of start address (bit 22 selects 12-bit rather than 8-bit linear),
// two of loop point, two of negated length, then the envelope and LFO defaults.

#pragma once

#include "core/types.h"

#include <array>
#include <span>

namespace sm2::hw {

class MultiPcm {
public:
    /// 28 voices, and the sample rate is the clock divided by 224 -- 44643 Hz on
    /// the Model 1 audio board's 10 MHz.
    static constexpr u32 kVoices        = 28;
    static constexpr u32 kClockDivider  = 224;

    explicit MultiPcm(u32 clock);

    /// The whole sample region. Reads apply the bank below rather than going
    /// through a banked address map.
    void attach(std::span<const u8> samples);

    void reset();

    /// Bank register: entry N maps region offset N * 1 MB into the upper half of
    /// the chip's 2 MB window. MAME configures four entries of 0x100000.
    void set_bank(u32 entry) { m_bank = entry & 3; }

    void write(u32 offset, u8 value);
    [[nodiscard]] u8 read() const { return 0; }

    /// Add `frames` interleaved stereo frames into `accum`, two s32 per frame,
    /// at `gain_percent` of full scale.
    ///
    /// This is where time passes for the chip: envelopes and LFOs advance per
    /// frame, so it has to be called whether or not anyone is listening.
    ///
    /// The chip's own sum is clamped to 16 bits before the gain, which is where
    /// MAME clamps too -- put_int_clamp on the way into the stream, then the
    /// speaker's route gain. Clamping after the gain instead would let one chip
    /// swing further than it can on hardware.
    void generate(s32* accum, u32 frames, s32 gain_percent);

    [[nodiscard]] u32 sample_rate() const { return m_rate_hz; }
    [[nodiscard]] u32 active_voices() const;

    /// Not in the MAME original. Enough to tell "the chip is running and making
    /// noise" from "the chip is running and silent" in the headless report, which
    /// is the difference that matters when bringing a board up.
    struct Stats {
        u64 key_ons     = 0;
        u64 samples     = 0;
        s32 peak_output = 0;  ///< Largest absolute value of either channel.
    };
    [[nodiscard]] const Stats& stats() const { return m_stats; }

private:
    static constexpr u32 kTlShift  = 12;
    static constexpr u32 kEgShift  = 16;
    static constexpr u32 kLfoShift = 8;

    enum class State : u8 { Attack, Decay1, Decay2, Release };

    struct Sample {
        u32 start           = 0;
        u32 loop            = 0;
        u32 end             = 0;
        u8  attack_reg      = 0;
        u8  decay1_reg      = 0;
        u8  decay2_reg      = 0;
        u8  decay_level     = 0;
        u8  release_reg     = 0;
        u8  key_rate_scale  = 0;
        u8  lfo_vibrato_reg = 0;
        u8  lfo_amp_reg     = 0;
        u8  format          = 0;
    };

    struct Envelope {
        s32   volume       = 0;
        State state        = State::Attack;
        u8    reverb       = 0;
        s32   attack_rate  = 0;
        s32   decay1_rate  = 0;
        s32   decay2_rate  = 0;
        s32   release_rate = 0;
        s32   decay_level  = 0;
    };

    struct Lfo {
        u16        phase      = 0;
        u32        phase_step = 0;
        const s32* table      = nullptr;
        const s32* scale      = nullptr;
    };

    struct Slot {
        std::array<u8, 8> regs{};
        bool     playing          = false;
        Sample   sample;
        u32      offset           = 0;
        u8       octave           = 0;
        u16      pitch            = 0;
        u32      step             = 0;
        bool     reverse          = false;
        u32      pan              = 0;
        u32      total_level      = 0;
        u32      dest_total_level = 0;
        s32      total_level_step = 0;
        s32      prev_sample      = 0;
        Envelope envelope;
        u8       lfo_frequency    = 0;
        Lfo      pitch_lfo;
        u8       vibrato          = 0;
        Lfo      amplitude_lfo;
        u8       tremolo          = 0;
    };

    [[nodiscard]] u8 read_byte(u32 offset) const;

    void init_sample(Sample& sample, u32 index);
    void write_slot(Slot& slot, u32 reg, u8 value);
    void retrigger_sample(Slot& slot);
    void update_step(Slot& slot);

    void envelope_generator_init(double attack_decay_ratio);
    [[nodiscard]] s32 envelope_generator_update(Slot& slot);
    void envelope_generator_calc(Slot& slot);
    [[nodiscard]] u32 get_rate(const u32* steps, s32 rate, u32 value) const;

    void lfo_init();
    void lfo_compute_step(Lfo& lfo, u32 frequency, u32 scale, bool amplitude);
    [[nodiscard]] s32 pitch_lfo_step(Lfo& lfo) const;
    [[nodiscard]] s32 amplitude_lfo_step(Lfo& lfo) const;

    static u32 value_to_fixed(u32 bits, float value);

    std::span<const u8> m_samples;
    u32                 m_bank = 0;

    Stats m_stats;

    /// Both derived from the clock in the constructor. Upstream keeps the clock
    /// itself because a device can be reclocked; nothing here does, so only the
    /// two derived rates are kept.
    float m_rate    = 0.0F;
    u32   m_rate_hz = 0;

    std::array<Slot, kVoices> m_slots;
    u32 m_cur_slot = 0;
    u32 m_address  = 0;

    std::array<u32, 0x40>  m_attack_step{};
    std::array<u32, 0x40>  m_decay_release_step{};
    std::array<u32, 0x400> m_freq_step_table{};
    std::array<s32, 0x800> m_left_pan_table{};
    std::array<s32, 0x800> m_right_pan_table{};
    std::array<s32, 0x400> m_linear_to_exp_volume{};
    std::array<s32, 2>     m_total_level_steps{};
    std::array<s32, 256>   m_pitch_table{};
    std::array<s32, 256>   m_amplitude_table{};
    std::array<std::array<s32, 256>, 8> m_pitch_scale_tables{};
    std::array<std::array<s32, 256>, 8> m_amplitude_scale_tables{};
};

}  // namespace sm2::hw
