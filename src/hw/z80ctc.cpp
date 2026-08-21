// SPDX-License-Identifier: BSD-3-Clause
//
// See z80ctc.h.

#include "hw/z80ctc.h"

namespace sm2::hw {

void Z80Ctc::reset()
{
    for (Channel& channel : m_channel) {
        channel = Channel{};
    }
    m_vector    = 0;
    m_int_line  = false;
}

u8 Z80Ctc::read(u32 channel)
{
    return static_cast<u8>(m_channel[channel & 3].down & 0xff);
}

void Z80Ctc::write(u32 index, u8 value)
{
    Channel& channel = m_channel[index & 3];

    if ((channel.mode & kConstantLoad) != 0) {
        // The byte after a control word that asked for one is the time constant,
        // where zero means 256.
        channel.tconst = value != 0 ? value : 0x100u;
        channel.mode &= static_cast<u16>(~kConstantLoad);
        // The constant also lifts the reset, which is what gets the channel going.
        channel.mode &= static_cast<u16>(~kResetActive);
        channel.down     = channel.tconst;
        channel.fraction = 0;

        if ((channel.mode & kModeCounter) != 0 || (channel.mode & kTriggerClock) == 0) {
            channel.running = true;
        } else {
            channel.mode |= kWaitingForTrigger;
            channel.running = false;
        }
        return;
    }

    if ((value & kControlWord) == 0) {
        // Only channel 0 carries the vector, and its low three bits are supplied
        // by the channel number at acknowledge time.
        if ((index & 3) == 0) {
            m_vector = static_cast<u8>(value & 0xf8);
        }
        return;
    }

    if ((value & kResetActive) != 0) {
        channel.running = false;
        // The interrupt state deliberately survives a reset; MAME notes this.
    }

    channel.mode = value;

    if ((value & kInterrupt) == 0 && channel.int_pending) {
        // Clearing the enable drops the request whether or not it was serviced.
        channel.int_pending  = false;
        channel.int_serviced = false;
        update_interrupt();
    }
}

void Z80Ctc::trigger(u32 index, bool level)
{
    Channel& channel = m_channel[index & 3];
    if (level == channel.extclk) return;
    channel.extclk = level;

    const bool active = ((channel.mode & kEdgeRising) != 0) == level;
    if (!active) return;

    if ((channel.mode & kWaitingForTrigger) != 0 && (channel.mode & kModeCounter) == 0) {
        channel.running  = true;
        channel.fraction = 0;
    }
    channel.mode &= static_cast<u16>(~kWaitingForTrigger);

    if ((channel.mode & kModeCounter) != 0) {
        count_down(index & 3);
    }
}

void Z80Ctc::run(u32 cycles)
{
    for (u32 index = 0; index < kChannels; ++index) {
        Channel& channel = m_channel[index];
        if (!channel.running || (channel.mode & kModeCounter) != 0) continue;
        if ((channel.mode & kResetActive) != 0) continue;

        const u32 step = prescale(channel);
        channel.fraction += cycles;
        while (channel.fraction >= step) {
            channel.fraction -= step;
            count_down(index);
        }
    }
}

void Z80Ctc::count_down(u32 index)
{
    Channel& channel = m_channel[index];
    if (channel.down > 0) --channel.down;
    if (channel.down != 0) return;

    channel.down = channel.tconst;

    if ((channel.mode & kInterrupt) != 0) {
        channel.int_pending = true;
        update_interrupt();
    }
    if (m_zero_cross) m_zero_cross(index);
}

bool Z80Ctc::interrupt_pending() const
{
    for (const Channel& channel : m_channel) {
        // A channel already being serviced blocks the ones below it rather than
        // asking again.
        if (channel.int_serviced) return false;
        if (channel.int_pending) return true;
    }
    return false;
}

void Z80Ctc::update_interrupt()
{
    const bool asserted = interrupt_pending();
    if (asserted == m_int_line) return;
    m_int_line = asserted;
    if (m_interrupt) m_interrupt(asserted);
}

u8 Z80Ctc::acknowledge_vector()
{
    for (u32 index = 0; index < kChannels; ++index) {
        Channel& channel = m_channel[index];
        if (channel.int_serviced) break;
        if (!channel.int_pending) continue;
        channel.int_pending  = false;
        channel.int_serviced = true;
        update_interrupt();
        return static_cast<u8>(m_vector | (index << 1));
    }
    return m_vector;
}

void Z80Ctc::return_from_interrupt()
{
    for (Channel& channel : m_channel) {
        if (channel.int_serviced) {
            channel.int_serviced = false;
            update_interrupt();
            return;
        }
    }
}

}  // namespace sm2::hw
