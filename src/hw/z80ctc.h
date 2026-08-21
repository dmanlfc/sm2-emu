// SPDX-License-Identifier: BSD-3-Clause
//
// Zilog Z80 CTC (Z8430), four counter/timer channels.
//
// Derived from MAME's src/devices/machine/z80ctc.{h,cpp} (BSD-3-Clause,
// copyright-holders Wilbert Pol, from Tatsuyuki Satoh's original).
//
// Here for the TMPZ84C015 on Virtua Cop's I/O board, which puts a CTC, a PIO and
// an SIO on one die with the Z80. What that board's firmware asks of the CTC was
// measured rather than guessed (tools/mame/iotap.lua): it writes the interrupt
// vector, programs channel 1 as an interrupting timer with a time constant of 38
// and the 256 prescaler, programs channel 2 as the SIO's baud clock, and never
// reads a channel back. So the read path exists for completeness and the
// interrupting timer is the part that has to be right.
//
// MAME drives each channel from a scheduler timer. sm2-emu has no scheduler, so
// this counts host cycles instead, the way hw::I8251 does; a channel fires when
// its accumulated prescaler ticks reach the time constant.
//
// What is deliberately not modelled: the zero-cross/timeout output pulse width
// (the pulse is reported as an event rather than held for one clock, because the
// only consumer here is a stubbed SIO), and the daisy-chain's interrupt
// acknowledge/return-from-interrupt handshake beyond what one CTC needs.
#pragma once

#include "core/types.h"

#include <array>
#include <functional>

namespace sm2::hw {

class Z80Ctc {
public:
    static constexpr u32 kChannels = 4;

    /// Called when a channel's down counter reaches zero, for whatever the
    /// channel's ZC/TO pin drives. MAME's zc_callback<N>.
    using ZeroCrossHandler = std::function<void(u32 channel)>;

    /// Called whenever the interrupt request changes state.
    using InterruptHandler = std::function<void(bool asserted)>;

    void reset();

    void set_zero_cross_handler(ZeroCrossHandler handler)
    {
        m_zero_cross = std::move(handler);
    }
    void set_interrupt_handler(InterruptHandler handler)
    {
        m_interrupt = std::move(handler);
    }

    /// Registers 0 to 3, one per channel.
    [[nodiscard]] u8 read(u32 channel);
    void             write(u32 channel, u8 value);

    /// An external clock or trigger edge on a channel's CLK/TRG pin.
    void trigger(u32 channel, bool level);

    /// Advance every timer channel by `cycles` of the CTC's own clock.
    void run(u32 cycles);

    /// True while any channel is requesting an interrupt.
    [[nodiscard]] bool interrupt_pending() const;

    /// The vector the highest-priority requesting channel supplies, and the
    /// acknowledgement that goes with fetching it. Channel 0 has the highest
    /// priority and contributes bits 2:1 of the vector.
    [[nodiscard]] u8 acknowledge_vector();

    /// Reti, which releases the channel currently being serviced.
    void return_from_interrupt();

private:
    // Control word bits, named as MAME names them.
    enum : u16 {
        kInterrupt     = 0x80,
        kModeCounter   = 0x40,
        kPrescaler256  = 0x20,
        kEdgeRising    = 0x10,
        kTriggerClock  = 0x08,
        kConstantLoad  = 0x04,
        kResetActive   = 0x02,
        kControlWord   = 0x01,
        /// Not a hardware bit: MAME's marker for a channel that has its time
        /// constant but is still waiting for the trigger edge that starts it.
        kWaitingForTrigger = 0x100,
    };

    struct Channel {
        u16  mode    = kResetActive;
        u32  tconst  = 0x100;
        u32  down    = 0x100;
        u32  fraction = 0;   ///< Prescaler ticks accumulated toward the next count.
        bool extclk  = false;
        bool running = false;
        bool int_pending = false;
        bool int_serviced = false;
    };

    void count_down(u32 channel);
    void update_interrupt();

    [[nodiscard]] u32 prescale(const Channel& channel) const
    {
        return (channel.mode & kPrescaler256) != 0 ? 256u : 16u;
    }

    std::array<Channel, kChannels> m_channel{};
    u8   m_vector = 0;
    bool m_int_line = false;

    ZeroCrossHandler m_zero_cross;
    InterruptHandler m_interrupt;
};

}  // namespace sm2::hw
