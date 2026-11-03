// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// Sega Model 1 Communication Board, 837-8842.
//
// Derived from MAME's src/mame/sega/m1comm.{h,cpp} (BSD-3-Clause,
// copyright-holder Ariane Fugmann).
//
// A Z80 with an MB89374 link controller and 4 KB of RAM shared with the V60.
// As with the Model 2 board (m2comm.h), MAME keeps the shared RAM and the two
// handshake registers and simulates the ring protocol instead of running the
// Z80; this is a port of that simulation over the same CommTransport seam.
#pragma once

#include "core/types.h"
#include "hw/comm_board.h"
#include "hw/comm_transport.h"

#include <array>
#include <memory>

namespace sm2 {
class Archive;
}

namespace sm2::hw {

class M1Comm final : public CommBoard {
public:
    static constexpr u32 kSharedSize = 0x1000;

    M1Comm();

    void reset();

    void set_transport(std::unique_ptr<CommTransport> transport) override;
    void set_frame_sync(bool enabled) { m_frame_sync = enabled ? 1u : 0u; }

    // -- the V60's side: shared RAM at 0xb00000, CN at 0xb01000, FG at 0xb01002

    [[nodiscard]] u8 share_read(u32 offset) const { return m_shared[offset & (kSharedSize - 1)]; }
    void share_write(u32 offset, u8 value) { m_shared[offset & (kSharedSize - 1)] = value; }

    [[nodiscard]] u8 cn_read() const { return static_cast<u8>(m_cn | 0xfe); }
    void             cn_write(u8 value);

    [[nodiscard]] u8 fg_read() const
    {
        return static_cast<u8>(m_fg | ((m_zfg & 1) != 0 ? 0x00 : 0x80) | 0x7e);
    }
    void fg_write(u8 value)
    {
        if (m_cn != 0) m_fg = static_cast<u8>(value & 0x01);
    }

    /// MAME's check_vint_irq: steps the ring once per frame.
    void vblank() { tick(); }

    [[nodiscard]] bool link_alive() const override { return m_linkalive == 0x01; }
    [[nodiscard]] u8   link_id() const override { return m_linkid; }
    [[nodiscard]] u8   link_count() const override { return m_linkcount; }
    [[nodiscard]] bool enabled() const override { return m_linkenable != 0; }

    void serialize(Archive& ar);

private:
    void tick();
    int  read_frame(int data_size);
    void send_frame(int data_size);
    void send_data(u8 frame_type, int frame_start, int frame_size, int data_size);
    void lose_link();

    std::array<u8, kSharedSize> m_shared{};

    u8 m_zfg = 0;
    u8 m_cn  = 0;
    u8 m_fg  = 0;

    u8  m_linkenable = 0;
    u16 m_linktimer  = 0;
    u8  m_linkalive  = 0;  ///< 0 establishing, 1 alive, 2 failed
    u8  m_linkid     = 0;
    u8  m_linkcount  = 0;
    u8  m_frame_sync = 0;

    std::array<u8, 0x200> m_buffer{};

    std::unique_ptr<CommTransport> m_transport;
};

}  // namespace sm2::hw
