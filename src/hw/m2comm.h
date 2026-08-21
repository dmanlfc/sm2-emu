// SPDX-License-Identifier: BSD-3-Clause
//
// Sega Model 2 Communication Board, 837-10537 and its later revisions.
//
// Derived from MAME's src/mame/sega/m2comm.{h,cpp} (BSD-3-Clause,
// copyright-holder Ariane Fugmann).
//
// The board is a Z80 with 16 KB of RAM shared with the i960 and a uPD72103 HDLC
// controller driving the ring of cabinets. MAME does not emulate the Z80 at all:
// it keeps the shared RAM, the two single-bit handshake registers, and a
// simulation of the ring protocol that talks over a socket. This is a port of
// that simulation.
//
// Every Model 2 machine configuration in MAME instantiates one, so every board
// here needs one too. Leaving it out is not harmless: a linked title polls the
// two handshake registers at boot and sits on "CHECKING NETWORK NOW" forever
// when they read back as unmapped. That was stopping Super GT 24h, Rail Chase 2,
// Dynamite Baseball, Pilot Kids, Zero Gunner, Over Rev, Ski Super G, Manx TT and
// Daytona USA from ever reaching a 3D scene.
//
// -- why a loopback rather than a socket -----------------------------------
//
// MAME opens a listening socket on comm_localhost:comm_localport and connects to
// comm_remotehost:comm_remoteport, whose defaults are 0.0.0.0:15112 and
// 127.0.0.1:15112. Those defaults point at each other, so an unconfigured MAME
// connects to itself and the ring protocol completes with one node: the board
// reports link alive, id 1 of 1. Confirmed by reading the shared RAM through the
// i960's own address space while MAME ran Super GT 24h (tools/mame/commdump.lua):
// shared[0..3] goes 00 00 00 00, then 00 00 ff ff after the game enables the
// board, then 01 00 01 01 about four seconds later, and the game proceeds.
//
// Reproducing that outcome needs no networking, only the loopback the defaults
// happen to describe, so the two sockets are replaced by an in-process frame
// queue. Nothing is sent to or received from the network.
#pragma once

#include "core/types.h"

#include <array>
#include <deque>
#include <span>
#include <vector>

namespace sm2::hw {

class M2Comm {
public:
    /// The shared RAM the i960 sees at 0x01a00000. The board does not own it:
    /// the i960 reaches it as a burst-capable memory window, so it lives with
    /// the rest of the board's RAM and is handed over here.
    static constexpr u32 kSharedSize = 0x4000;

    /// Where in the shared RAM a received frame is deposited. 0x1c0 on every
    /// title MAME configures except Power Sled, which uses 0x180.
    static constexpr u16 kDefaultFrameOffset = 0x1c0;

    void reset();

    void attach_shared(std::span<u8> shared) { m_shared = shared; }
    void set_frame_offset(u16 offset) { m_frame_offset = offset; }

    /// `comm_framesync`. MAME defaults it off, so the board does not make the
    /// host wait for the ring before finishing a frame.
    void set_frame_sync(bool enabled) { m_frame_sync = enabled ? 1u : 0u; }

    // -- the two handshake registers, on the i960's bus --------------------

    /// 0x01a04000. Bit 0 enables the board; the rest reads as ones.
    [[nodiscard]] u8 cn_read() const { return static_cast<u8>(m_cn | 0xfe); }
    void             cn_write(u8 value);

    /// 0x01a04002. Bit 0 is the i960's own flip gate, bit 7 is the inverse of
    /// the Z80's. The Z80's toggles once per received frame, which is how the
    /// host notices new data without an interrupt.
    [[nodiscard]] u8 fg_read();
    void             fg_write(u8 value) { m_fg = static_cast<u8>(value & 0x01); }

    /// MAME's check_vint_irq, called from the board's vertical blank. Despite
    /// the name it raises nothing: the IRQ lines are unconnected on Model 2 and
    /// MAME only uses the call to step the ring simulation.
    void vblank() { tick(); }

    // -- diagnostics ------------------------------------------------------

    [[nodiscard]] bool link_alive() const { return m_linkalive == 0x01; }
    [[nodiscard]] u8   link_id() const { return m_linkid; }
    [[nodiscard]] u8   link_count() const { return m_linkcount; }
    [[nodiscard]] bool enabled() const { return m_linkenable != 0; }

private:
    void tick();
    void read_fg();

    /// Pops one frame from the loopback. Returns the byte count, which is zero
    /// when there is nothing queued.
    ///
    /// MAME reads a byte stream and reassembles a partial frame across calls.
    /// Here every frame arrives whole because it was queued whole, so that path
    /// cannot be reached. Neither can its connection-lost branch: a real socket
    /// reports end-of-file by returning zero bytes with no error, while an empty
    /// receive queue reports would-block, and the loopback never closes.
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

    /// Stands in for the pair of sockets. Frames written go on the back and are
    /// read from the front, which is what MAME's own defaults arrange.
    std::deque<std::vector<u8>> m_loopback;
};

}  // namespace sm2::hw
