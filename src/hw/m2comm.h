// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Sega Model 2 Communication Board, 837-10537 and its later revisions.
//
// Derived from MAME's src/mame/sega/m2comm.{h,cpp} (BSD-3-Clause,
// copyright-holder Ariane Fugmann).
//
// The board is a Z80 with 16 KB of RAM shared with the i960 and a uPD72103 HDLC
// controller driving the ring of cabinets. MAME does not emulate the Z80; it
// keeps the shared RAM, the two handshake registers and a simulation of the
// ring protocol. This is a port of that simulation.
//
// Linked titles poll the handshake registers at boot and hang on "CHECKING
// NETWORK NOW" if they read as unmapped, so every Model 2 board needs one.
//
// Deviation: MAME's default socket addresses point at each other, so an
// unconfigured MAME links to itself as node 1 of 1. The default here reproduces
// that with an in-process loopback queue instead of sockets.
#pragma once

#include "core/types.h"
#include "hw/comm_board.h"
#include "hw/comm_transport.h"

#include <array>
#include <memory>
#include <span>

namespace sm2 {
class Archive;
}

namespace sm2::hw {

class M2Comm final : public CommBoard {
public:
    /// The shared RAM the i960 sees at 0x01a00000. It lives with the board's
    /// other RAM (the i960 bursts into it) and is attached here.
    static constexpr u32 kSharedSize = 0x4000;

    /// Where in the shared RAM a received frame is deposited. 0x1c0 on every
    /// title MAME configures except Power Sled, which uses 0x180.
    static constexpr u16 kDefaultFrameOffset = 0x1c0;

    M2Comm();

    void reset();

    void attach_shared(std::span<u8> shared) { m_shared = shared; }
    void set_frame_offset(u16 offset) { m_frame_offset = offset; }

    /// Swap the frame transport (takes ownership); nullptr restores the default
    /// loopback. A LAN transport (comm_udp.h) is injected when linking is on.
    void set_transport(std::unique_ptr<CommTransport> transport) override;

    /// `comm_framesync`. MAME defaults it off, so the board does not make the
    /// host wait for the ring before finishing a frame.
    void set_frame_sync(bool enabled) { m_frame_sync = enabled ? 1u : 0u; }

    // -- the two handshake registers, on the i960's bus --------------------

    /// 0x01a04000. Bit 0 enables the board; the rest reads as ones.
    [[nodiscard]] u8 cn_read() const { return static_cast<u8>(m_cn | 0xfe); }
    void             cn_write(u8 value);

    /// 0x01a04002. Bit 0 is the i960's flip gate, bit 7 the inverse of the
    /// Z80's, which toggles once per received frame.
    [[nodiscard]] u8 fg_read();
    void             fg_write(u8 value) { m_fg = static_cast<u8>(value & 0x01); }

    /// MAME's check_vint_irq, called at vertical blank. It only steps the ring:
    /// the IRQ lines are unconnected on Model 2.
    void vblank() { tick(); }

    // -- diagnostics ------------------------------------------------------

    [[nodiscard]] bool link_alive() const override { return m_linkalive == 0x01; }
    [[nodiscard]] u8   link_id() const override { return m_linkid; }
    [[nodiscard]] u8   link_count() const override { return m_linkcount; }
    [[nodiscard]] bool enabled() const override { return m_linkenable != 0; }

    /// Whether the underlying transport still believes it can carry frames.
    /// Always true for the loopback; a socket transport reports the peer state.
    [[nodiscard]] bool transport_connected() const;

    /// Save/restore the link registers and frame buffer. The shared RAM is
    /// saved by the machine; queued transport frames are not saved.
    void serialize(Archive& ar);

private:
    void tick();
    void read_fg();

    /// Pops one whole frame from the transport; returns its size, or zero when
    /// nothing is waiting. Deviation: MAME reassembles partial frames here; the
    /// transports always deliver whole ones.
    int  read_frame(int data_size);
    void send_frame(int data_size);
    void send_data(u8 frame_type, int frame_start, int frame_size, int data_size);

    [[nodiscard]] u8 shared(u32 offset) const
    {
        return offset < m_shared.size() ? m_shared[offset] : 0;
    }
    void set_shared(u32 offset, u8 value)
    {
        if (offset < m_shared.size()) m_shared[offset] = value;
    }

    std::span<u8> m_shared;

    u8 m_zfg = 0;  ///< Z80 flip gate. Bit 0 also selects the Z80's RAM bank.
    u8 m_cn  = 0;
    u8 m_fg  = 0;

    u8  m_linkenable = 0;
    u16 m_linktimer  = 0;
    u8  m_linkalive  = 0;  ///< 0 establishing, 1 alive, 2 failed
    u8  m_linkid     = 0;
    u8  m_linkcount  = 0;
    u8  m_zfg_delay  = 0;

    u8  m_frame_sync   = 0;
    u16 m_frame_offset = kDefaultFrameOffset;

    /// MAME's m_buffer0, the frame being assembled or examined.
    std::array<u8, 0x1000> m_buffer{};

    /// MAME's sockets, abstracted: an in-process loopback by default, or a UDP
    /// transport for LAN play.
    std::unique_ptr<CommTransport> m_transport;
};

}  // namespace sm2::hw
