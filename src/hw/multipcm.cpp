// SPDX-License-Identifier: BSD-3-Clause
//
// Ported from MAME's src/devices/sound/gew.cpp and multipcm.cpp (BSD-3-Clause,
// copyright-holders Miguel Angel Horna). See multipcm.h for what differs.

#include "hw/multipcm.h"

#include "core/log.h"

#include <algorithm>
#include <cmath>

namespace sm2::hw {

namespace {

// Envelope times in milliseconds on a 44100 Hz timebase, adjusted to the real
// rate in envelope_generator_init.
constexpr double kBaseTimes[64] = {
    0,       0,       0,       0,
    6222.95, 4978.37, 4148.66, 3556.01,
    3111.47, 2489.21, 2074.33, 1778.00,
    1555.74, 1244.63, 1037.19, 889.02,
    777.87,  622.31,  518.59,  444.54,
    388.93,  311.16,  259.32,  222.27,
    194.47,  155.60,  129.66,  111.16,
    97.23,   77.82,   64.85,   55.60,
    48.62,   38.91,   32.43,   27.80,
    24.31,   19.46,   16.24,   13.92,
    12.15,   9.75,    8.12,    6.98,
    6.08,    4.90,    4.08,    3.49,
    3.04,    2.49,    2.13,    1.90,
    1.72,    1.41,    1.18,    1.04,
    0.91,    0.73,    0.59,    0.50,
    0.45,    0.45,    0.45,    0.45,
};

constexpr float kLfoFreq[8] = {  // Hz
    0.168F, 2.019F, 3.196F, 4.206F, 5.215F, 5.888F, 6.224F, 7.066F,
};

constexpr float kPhaseScaleLimit[8] = {  // cents
    0.0F, 3.378F, 5.065F, 6.750F, 10.114F, 20.170F, 40.180F, 79.307F,
};

constexpr float kAmplitudeScaleLimit[8] = {  // decibels
    0.0F, 0.4F, 0.8F, 1.5F, 3.0F, 6.0F, 12.0F, 24.0F,
};

// Seven voices per group of eight register slots; the eighth selects nothing.
constexpr s32 kValueToChannel[32] = {
    0,  1,  2,  3,  4,  5,  6,  -1,
    7,  8,  9,  10, 11, 12, 13, -1,
    14, 15, 16, 17, 18, 19, 20, -1,
    21, 22, 23, 24, 25, 26, 27, -1,
};

}  // namespace

MultiPcm::MultiPcm(u32 clock)
{
    m_rate    = static_cast<float>(clock) / static_cast<float>(kClockDivider);
    m_rate_hz = clock / kClockDivider;

    // Volume and pan. Level is 7 bits of attenuation at -24 dB per 64 steps, pan
    // is 4 bits either side of centre, and the two are indexed together.
    for (s32 level = 0; level < 0x80; ++level) {
        const float vol_db      = static_cast<float>(level) * -24.0F / 64.0F;
        const float total_level = std::pow(10.0F, vol_db / 20.0F) / 4.0F;

        for (s32 pan = 0; pan < 0x10; ++pan) {
            float pan_left  = 0.0F;
            float pan_right = 0.0F;
            if (pan == 0x8) {
                pan_left  = 0.0F;
                pan_right = 0.0F;
            } else if (pan == 0x0) {
                pan_left  = 1.0F;
                pan_right = 1.0F;
            } else if ((pan & 0x8) != 0) {
                pan_left                = 1.0F;
                const s32   inverted    = 0x10 - pan;
                const float pan_vol_db  = static_cast<float>(inverted) * -12.0F / 4.0F;
                pan_right               = std::pow(10.0F, pan_vol_db / 20.0F);
                if ((inverted & 0x7) == 7) {
                    pan_right = 0.0F;
                }
            } else {
                pan_right              = 1.0F;
                const float pan_vol_db = static_cast<float>(pan) * -12.0F / 4.0F;
                pan_left               = std::pow(10.0F, pan_vol_db / 20.0F);
                if ((pan & 0x7) == 7) {
                    pan_left = 0.0F;
                }
            }

            const usize index = (static_cast<usize>(pan) << 7) | static_cast<usize>(level);
            m_left_pan_table[index] =
                static_cast<s32>(value_to_fixed(kTlShift, pan_left * total_level));
            m_right_pan_table[index] =
                static_cast<s32>(value_to_fixed(kTlShift, pan_right * total_level));
        }
    }

    for (s32 i = 0; i < 0x400; ++i) {
        const float fcent = m_rate * (1024.0F + static_cast<float>(i)) / 1024.0F;
        m_freq_step_table[static_cast<usize>(i)] = value_to_fixed(kTlShift, fcent);
    }

    envelope_generator_init(14.32833);

    m_total_level_steps[0] =
        static_cast<s32>(-static_cast<float>(0x80 << kTlShift) / (78.2F * 44100.0F / 1000.0F));
    m_total_level_steps[1] =
        static_cast<s32>(static_cast<float>(0x80 << kTlShift) / (78.2F * 2 * 44100.0F / 1000.0F));

    for (s32 i = 0; i < 0x400; ++i) {
        const float db = -(96.0F - (96.0F * static_cast<float>(i) / static_cast<float>(0x400)));
        const float exp_volume = std::pow(10.0F, db / 20.0F);
        m_linear_to_exp_volume[static_cast<usize>(i)] =
            static_cast<s32>(value_to_fixed(kTlShift, exp_volume));
    }

    lfo_init();
}

void MultiPcm::attach(std::span<const u8> samples)
{
    m_samples = samples;
}

void MultiPcm::reset()
{
    for (Slot& slot : m_slots) {
        slot = Slot{};
    }
    m_cur_slot = 0;
    m_address  = 0;
    m_bank     = 0;
    m_stats    = Stats{};
}

u32 MultiPcm::value_to_fixed(u32 bits, float value)
{
    const float shift = static_cast<float>(1U << bits);
    return static_cast<u32>(shift * value);
}

u8 MultiPcm::read_byte(u32 offset) const
{
    if (m_samples.empty()) {
        return 0;
    }
    // The chip sees 2 MB: the first megabyte is the region's own start and the
    // second is one of four banked megabytes. MAME expresses this as an address
    // map with bankr(); doing it here keeps the ROM a plain span.
    u32 index = 0;
    if (offset < 0x100000) {
        index = offset;
    } else if (offset < 0x200000) {
        index = (m_bank * 0x100000u) + (offset - 0x100000u);
    } else {
        return 0;
    }
    return index < m_samples.size() ? m_samples[index] : 0u;
}

void MultiPcm::init_sample(Sample& sample, u32 index)
{
    const u32 address = index * 12;

    sample.start = (static_cast<u32>(read_byte(address)) << 16)
                 | (static_cast<u32>(read_byte(address + 1)) << 8)
                 | read_byte(address + 2);
    sample.format = static_cast<u8>((sample.start >> 20) & 0xfe);
    sample.start &= 0x3fffff;
    sample.loop = (static_cast<u32>(read_byte(address + 3)) << 8) | read_byte(address + 4);
    sample.end =
        0x10000u - ((static_cast<u32>(read_byte(address + 5)) << 8) | read_byte(address + 6));
    sample.attack_reg     = static_cast<u8>((read_byte(address + 8) >> 4) & 0xf);
    sample.decay1_reg     = static_cast<u8>(read_byte(address + 8) & 0xf);
    sample.decay2_reg     = static_cast<u8>(read_byte(address + 9) & 0xf);
    sample.decay_level    = static_cast<u8>((read_byte(address + 9) >> 4) & 0xf);
    sample.release_reg    = static_cast<u8>(read_byte(address + 10) & 0xf);
    sample.key_rate_scale = static_cast<u8>((read_byte(address + 10) >> 4) & 0xf);
    sample.lfo_vibrato_reg = read_byte(address + 7);
    sample.lfo_amp_reg     = static_cast<u8>(read_byte(address + 11) & 0xf);
}

void MultiPcm::retrigger_sample(Slot& slot)
{
    slot.offset      = 0;
    slot.prev_sample = 0;
    slot.total_level = slot.dest_total_level << kTlShift;

    envelope_generator_calc(slot);
    slot.envelope.state  = State::Attack;
    slot.envelope.volume = 0;
}

void MultiPcm::update_step(Slot& slot)
{
    const u8 oct   = static_cast<u8>((slot.octave - 1) & 0xf);
    u32      pitch = m_freq_step_table[slot.pitch & 0x3ff];
    if ((oct & 0x8) != 0) {
        pitch >>= (16 - oct);
    } else {
        pitch <<= oct;
    }
    slot.step = static_cast<u32>(static_cast<float>(pitch) / m_rate);
}

void MultiPcm::envelope_generator_init(double attack_decay_ratio)
{
    for (s32 i = 4; i < 0x40; ++i) {
        m_attack_step[static_cast<usize>(i)] = static_cast<u32>(
            static_cast<float>(0x400 << kEgShift)
            / static_cast<float>(kBaseTimes[i] * 44100.0 / 1000.0));
        m_decay_release_step[static_cast<usize>(i)] = static_cast<u32>(
            static_cast<float>(0x400 << kEgShift)
            / static_cast<float>(kBaseTimes[i] * attack_decay_ratio * 44100.0 / 1000.0));
    }
    m_attack_step[0] = m_attack_step[1] = m_attack_step[2] = m_attack_step[3] = 0;
    m_attack_step[0x3f]                                                      = 0x400 << kEgShift;
    m_decay_release_step[0] = m_decay_release_step[1] = m_decay_release_step[2] =
        m_decay_release_step[3]                       = 0;
}

u32 MultiPcm::get_rate(const u32* steps, s32 rate, u32 value) const
{
    if (value == 0) {
        return steps[0];
    }
    if (value == 0xf) {
        return steps[0x3f];
    }
    const s32 r = std::clamp(4 * static_cast<s32>(value) + rate, 0, 0x3f);
    return steps[r];
}

void MultiPcm::envelope_generator_calc(Slot& slot)
{
    s32 octave = slot.octave;
    if ((octave & 8) != 0) {
        octave = octave - 16;
    }

    s32 rate = 0;
    if (slot.sample.key_rate_scale != 0xf) {
        rate = (octave + slot.sample.key_rate_scale) * 2 + ((slot.pitch >> 9) & 1);
    }

    slot.envelope.attack_rate =
        static_cast<s32>(get_rate(m_attack_step.data(), rate, slot.sample.attack_reg));
    slot.envelope.decay1_rate =
        static_cast<s32>(get_rate(m_decay_release_step.data(), rate, slot.sample.decay1_reg));
    slot.envelope.decay2_rate =
        static_cast<s32>(get_rate(m_decay_release_step.data(), rate, slot.sample.decay2_reg));
    slot.envelope.release_rate =
        static_cast<s32>(get_rate(m_decay_release_step.data(), rate, slot.sample.release_reg));
    slot.envelope.decay_level = 0xf - slot.sample.decay_level;
    slot.envelope.reverb      = 0;
}

s32 MultiPcm::envelope_generator_update(Slot& slot)
{
    switch (slot.envelope.state) {
        case State::Attack:
            slot.envelope.volume += slot.envelope.attack_rate;
            if (slot.envelope.volume >= (0x3ff << kEgShift)) {
                slot.envelope.state = State::Decay1;
                if (slot.envelope.decay1_rate >= (0x400 << kEgShift)) {
                    // Decay1 is instant, so go straight to Decay2.
                    slot.envelope.state = State::Decay2;
                }
                slot.envelope.volume = 0x3ff << kEgShift;
            }
            break;
        case State::Decay1:
            slot.envelope.volume -= slot.envelope.decay1_rate;
            if (slot.envelope.volume <= 0) {
                slot.envelope.volume = 0;
            }
            if ((slot.envelope.volume >> (kEgShift + 6)) <= slot.envelope.decay_level) {
                slot.envelope.state = State::Decay2;
            }
            break;
        case State::Decay2:
            slot.envelope.volume -= slot.envelope.decay2_rate;
            if (slot.envelope.volume <= 0) {
                slot.envelope.volume = 0;
            }
            break;
        case State::Release:
            slot.envelope.volume -= slot.envelope.release_rate;
            if (slot.envelope.volume <= 0) {
                slot.envelope.volume = 0;
                slot.playing         = false;
            }
            break;
        default:
            return 1 << kTlShift;
    }

    return m_linear_to_exp_volume[static_cast<usize>((slot.envelope.volume >> kEgShift) & 0x3ff)];
}

void MultiPcm::lfo_init()
{
    for (s32 i = 0; i < 256; ++i) {
        if (i < 64) {
            m_pitch_table[static_cast<usize>(i)] = i * 2 + 128;
        } else if (i < 128) {
            m_pitch_table[static_cast<usize>(i)] = 383 - i * 2;
        } else if (i < 192) {
            m_pitch_table[static_cast<usize>(i)] = 384 - i * 2;
        } else {
            m_pitch_table[static_cast<usize>(i)] = i * 2 - 383;
        }

        if (i < 128) {
            m_amplitude_table[static_cast<usize>(i)] = 255 - (i * 2);
        } else {
            m_amplitude_table[static_cast<usize>(i)] = (i * 2) - 256;
        }
    }

    for (s32 table = 0; table < 8; ++table) {
        float limit = kPhaseScaleLimit[table];
        for (s32 i = -128; i < 128; ++i) {
            const float value     = (limit * static_cast<float>(i)) / 128.0F;
            const float converted = std::pow(2.0F, value / 1200.0F);
            m_pitch_scale_tables[static_cast<usize>(table)][static_cast<usize>(i + 128)] =
                static_cast<s32>(value_to_fixed(kLfoShift, converted));
        }

        limit = -kAmplitudeScaleLimit[table];
        for (s32 i = 0; i < 256; ++i) {
            const float value     = (limit * static_cast<float>(i)) / 256.0F;
            const float converted = std::pow(10.0F, value / 20.0F);
            m_amplitude_scale_tables[static_cast<usize>(table)][static_cast<usize>(i)] =
                static_cast<s32>(value_to_fixed(kLfoShift, converted));
        }
    }
}

void MultiPcm::lfo_compute_step(Lfo& lfo, u32 frequency, u32 scale, bool amplitude)
{
    const float step = kLfoFreq[frequency & 7] * 256.0F / m_rate;
    lfo.phase_step   = static_cast<u32>(static_cast<float>(1U << kLfoShift) * step);
    if (amplitude) {
        lfo.table = m_amplitude_table.data();
        lfo.scale = m_amplitude_scale_tables[scale & 7].data();
    } else {
        lfo.table = m_pitch_table.data();
        lfo.scale = m_pitch_scale_tables[scale & 7].data();
    }
}

s32 MultiPcm::pitch_lfo_step(Lfo& lfo) const
{
    if (lfo.table == nullptr || lfo.scale == nullptr) {
        return 1 << kTlShift;
    }
    lfo.phase = static_cast<u16>(lfo.phase + lfo.phase_step);
    s32 p     = lfo.table[(lfo.phase >> kLfoShift) & 0xff];
    p         = lfo.scale[p & 0xff];
    return p << (kTlShift - kLfoShift);
}

s32 MultiPcm::amplitude_lfo_step(Lfo& lfo) const
{
    if (lfo.table == nullptr || lfo.scale == nullptr) {
        return 1 << kTlShift;
    }
    lfo.phase = static_cast<u16>(lfo.phase + lfo.phase_step);
    s32 p     = lfo.table[(lfo.phase >> kLfoShift) & 0xff];
    p         = lfo.scale[p & 0xff];
    return p << (kTlShift - kLfoShift);
}

void MultiPcm::write_slot(Slot& slot, u32 reg, u8 value)
{
    if (reg > 7) {
        return;
    }
    slot.regs[reg] = value;

    switch (reg) {
        case 0:  // Pan
            slot.pan = (value >> 4) & 0xf;
            break;

        case 1: {  // Sample select
            // As on the YMF278, selecting a sample also loads the envelope and
            // LFO defaults out of its metadata.
            init_sample(slot.sample,
                        static_cast<u32>(slot.regs[1]) | ((slot.regs[2] & 1u) << 8));
            write_slot(slot, 6, slot.sample.lfo_vibrato_reg);
            write_slot(slot, 7, slot.sample.lfo_amp_reg);
            if (slot.playing) {
                retrigger_sample(slot);
            }
            break;
        }

        case 2:  // Pitch
        case 3:
            slot.octave = static_cast<u8>(slot.regs[3] >> 4);
            slot.pitch  = static_cast<u16>(((slot.regs[3] & 0xf) << 6) | (slot.regs[2] >> 2));
            update_step(slot);
            break;

        case 4:  // Key on / off
            if ((value & 0x80) != 0) {
                ++m_stats.key_ons;
                slot.playing = true;
                retrigger_sample(slot);
            } else if (slot.playing) {
                if (slot.sample.release_reg != 0xf) {
                    slot.envelope.state = State::Release;
                } else {
                    slot.playing = false;
                }
            }
            break;

        case 5:  // Total level, and whether to interpolate to it
            slot.dest_total_level = (value >> 1) & 0x7f;
            if ((value & 1) == 0) {
                slot.total_level_step = (slot.total_level >> kTlShift) > slot.dest_total_level
                                            ? m_total_level_steps[0]
                                            : m_total_level_steps[1];
            } else {
                slot.total_level = slot.dest_total_level << kTlShift;
            }
            break;

        case 6:  // LFO frequency and pitch depth
        case 7:  // Amplitude depth
            slot.lfo_frequency = static_cast<u8>((slot.regs[6] >> 3) & 7);
            slot.vibrato       = static_cast<u8>(slot.regs[6] & 7);
            slot.tremolo       = static_cast<u8>(slot.regs[7] & 7);
            if (value != 0) {
                lfo_compute_step(slot.pitch_lfo, slot.lfo_frequency, slot.vibrato, false);
                lfo_compute_step(slot.amplitude_lfo, slot.lfo_frequency, slot.tremolo, true);
            }
            break;

        default:
            break;
    }
}

void MultiPcm::write(u32 offset, u8 value)
{
    switch (offset) {
        case 0:  // Data
            if (m_cur_slot < kVoices) {
                write_slot(m_slots[m_cur_slot], m_address, value);
            }
            break;
        case 1: {  // Voice select
            const s32 channel = kValueToChannel[value & 0x1f];
            m_cur_slot        = channel < 0 ? kVoices : static_cast<u32>(channel);
            break;
        }
        case 2:  // Register select
            m_address = value > 7 ? 7u : value;
            break;
        default:
            break;
    }
}

u32 MultiPcm::active_voices() const
{
    u32 count = 0;
    for (const Slot& slot : m_slots) {
        if (slot.playing) {
            ++count;
        }
    }
    return count;
}

void MultiPcm::generate(s32* accum, u32 frames, s32 gain_percent)
{
    for (u32 frame = 0; frame < frames; ++frame) {
        s32 left  = 0;
        s32 right = 0;

        for (Slot& slot : m_slots) {
            if (!slot.playing) {
                continue;
            }

            const u32 vol =
                (slot.total_level >> kTlShift) | (slot.pan << 7);
            u32 spos    = slot.offset >> kTlShift;
            u32 step    = slot.step;
            s32 csample = 0;
            const s32 fpart = static_cast<s32>(slot.offset & ((1U << kTlShift) - 1));

            if (slot.reverse) {
                spos = slot.sample.end - spos - 1;
            }

            if ((slot.sample.format & 4) != 0) {
                // 12-bit linear: two samples share three bytes.
                const u32 adr = slot.sample.start + (spos >> 1) * 3;
                if ((spos & 1) == 0) {
                    csample = static_cast<s16>((read_byte(adr) << 8)
                                               | ((read_byte(adr + 1) & 0xf) << 4));
                } else {
                    csample = static_cast<s16>((read_byte(adr + 2) << 8)
                                               | (read_byte(adr + 1) & 0xf0));
                }
            } else {
                csample = static_cast<s16>(read_byte(slot.sample.start + spos) << 8);
            }

            s32 sample = (csample * fpart + slot.prev_sample * ((1 << kTlShift) - fpart))
                       >> kTlShift;

            if (slot.vibrato != 0) {
                step = static_cast<u32>((static_cast<u64>(step)
                                         * static_cast<u64>(pitch_lfo_step(slot.pitch_lfo)))
                                        >> kTlShift);
            }

            slot.offset += step;

            if ((spos ^ (slot.offset >> kTlShift)) != 0) {
                slot.prev_sample = csample;
            }

            if (slot.offset >= (slot.sample.end << kTlShift)) {
                slot.offset -= (slot.sample.end - slot.sample.loop) << kTlShift;
                slot.reverse = false;
            }

            if ((slot.total_level >> kTlShift) != slot.dest_total_level) {
                slot.total_level = static_cast<u32>(
                    static_cast<s32>(slot.total_level) + slot.total_level_step);
            }

            if (slot.tremolo != 0) {
                sample = static_cast<s32>((static_cast<s64>(sample)
                                           * amplitude_lfo_step(slot.amplitude_lfo))
                                          >> kTlShift);
            }

            sample = static_cast<s32>((static_cast<s64>(sample) * envelope_generator_update(slot))
                                      >> 10);

            const usize index = static_cast<usize>(vol) & 0x7ff;
            left  += static_cast<s32>((static_cast<s64>(m_left_pan_table[index]) * sample)
                                      >> kTlShift);
            right += static_cast<s32>((static_cast<s64>(m_right_pan_table[index]) * sample)
                                      >> kTlShift);
        }

        // MAME: stream.put_int_clamp(0, i, smpl, 32768).
        left  = std::clamp(left, -32768, 32767);
        right = std::clamp(right, -32768, 32767);

        accum[frame * 2 + 0] += left * gain_percent / 100;
        accum[frame * 2 + 1] += right * gain_percent / 100;

        // Not in the original: the peak is this chip's own output before the
        // board's route gain, which is what says whether this chip in particular
        // is producing anything.
        m_stats.peak_output =
            std::max({m_stats.peak_output, std::abs(left), std::abs(right)});
    }
    m_stats.samples += frames;
}

}  // namespace sm2::hw
