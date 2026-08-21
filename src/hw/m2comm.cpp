// SPDX-License-Identifier: BSD-3-Clause
//
// See m2comm.h for what this is and why the ring runs over a loopback.

#include "hw/m2comm.h"

#include "core/log.h"

#include <algorithm>
#include <cstddef>

namespace sm2::hw {

namespace {

/// Where the host puts the frame it wants transmitted, and the base the receive
/// offset in shared[0x14..0x15] is added to. MAME hard-codes both.
constexpr int kFrameStart = 0x2000;

/// A ring frame carries an id byte followed by the payload, so one more than the
/// frame size the host asked for. MAME's buffers are 0x1000 bytes, which caps
/// how much of a nonsense frame size would be honoured.
constexpr int kMaxFrame = 0x1000;

/// How many frames the loopback holds before a send fails, standing in for the
/// socket buffer MAME relies on. Steady state is two.
constexpr std::size_t kLoopbackDepth = 64;

}  // namespace

void M2Comm::reset()
{
    // MAME's device_reset clears only these three. The link state is
    // constructor-initialised and survives a reset, which matters because the
    // host resets the board by clearing CN rather than by resetting the machine.
    m_zfg = 0;
    m_cn  = 0;
    m_fg  = 0;

    m_linkenable = 0;
    m_linktimer  = 0;
    m_linkalive  = 0;
    m_linkid     = 0;
    m_linkcount  = 0;
    m_zfg_delay  = 0;
    m_loopback.clear();
}

void M2Comm::cn_write(u8 value)
{
    m_cn = static_cast<u8>(value & 0x01);

    if (m_cn == 0) {
        SM2_DEBUG("m2comm: board disabled");
        m_linkenable = 0;
        m_zfg        = 0;
        m_cn         = 0;
        m_fg         = 0;
        m_loopback.clear();
        return;
    }

    SM2_DEBUG("m2comm: board enabled");
    m_linkenable = 0x01;
    m_linkid     = 0x00;
    m_linkalive  = 0x00;
    m_linkcount  = 0x00;
    m_linktimer  = 0x00e8;  // 58 fps for about four seconds
    m_loopback.clear();

    std::fill(m_shared.begin(), m_shared.end(), u8{0});
    set_shared(0x01, 0x02);
    // Frame size 0x0e00 and the receive offset, both as the host expects to find
    // them. MAME notes it has not confirmed these against EPR-16726.
    set_shared(0x12, 0x00);
    set_shared(0x13, 0x0e);
    set_shared(0x14, static_cast<u8>(m_frame_offset & 0xff));
    set_shared(0x15, static_cast<u8>(m_frame_offset >> 8));

    tick();
}

u8 M2Comm::fg_read()
{
    read_fg();
    // Bit 7 is the Z80's flip gate inverted, so it reads as one while that gate
    // is clear. MAME writes this as (~m_zfg << 7), which is the same thing.
    return static_cast<u8>(m_fg | ((m_zfg & 1) != 0 ? 0x00 : 0x80) | 0x7e);
}

int M2Comm::read_frame(int data_size)
{
    if (m_loopback.empty()) return 0;
    const std::vector<u8> frame = std::move(m_loopback.front());
    m_loopback.pop_front();
    const int count = static_cast<int>(std::min<std::size_t>(frame.size(),
                                                             std::size_t(data_size)));
    std::copy_n(frame.begin(), count, m_buffer.begin());
    return count;
}

void M2Comm::send_frame(int data_size)
{
    if (m_loopback.size() >= kLoopbackDepth) {
        // A socket whose buffer has filled fails the write, and MAME treats that
        // as the transmit side going away. Reachable only if the host reconfigures
        // itself as a relay after the link came up, which makes it forward
        // everything it receives back to itself.
        if (m_linkalive == 0x01) {
            SM2_DEBUG("m2comm: transmit backed up, dropping the link");
            m_linkalive = 0x02;
            m_linktimer = 0x00;
        }
        return;
    }
    const int count = std::clamp(data_size, 0, kMaxFrame);
    m_loopback.emplace_back(m_buffer.begin(), m_buffer.begin() + count);
}

void M2Comm::send_data(u8 frame_type, int frame_start, int frame_size, int data_size)
{
    m_buffer[0] = frame_type;
    for (int i = 0; i < frame_size && 1 + i < kMaxFrame; i++) {
        m_buffer[std::size_t(1 + i)] = shared(u32(frame_start + i));
    }
    send_frame(data_size);
}

void M2Comm::tick()
{
    if (m_linkenable != 0x01) return;

    const int frame_size = (shared(0x13) << 8) | shared(0x12);
    const int frame_offset = kFrameStart | (shared(0x15) << 8) | shared(0x14);
    const int data_size = frame_size + 1;

    // EPR-16726 uses the i960's flip gate to pick master or slave; EPR-18643(A)
    // looks at shared[1] and falls back on the flip gate.
    const bool is_master = (m_fg == 0x01 || shared(0x01) == 0x01);
    const bool is_slave  = (m_fg == 0x00 && shared(0x01) == 0x02);
    const bool is_relay  = (m_fg == 0x00 && shared(0x01) == 0x00);

    if (m_linkalive == 0x02) {
        set_shared(0, 0xff);
        return;
    }

    if (m_linkalive == 0x00) {
        set_shared(0, 0x00);
        set_shared(2, 0xff);
        set_shared(3, 0xff);

        // MAME opens its two sockets here. The loopback needs no opening, and
        // MAME's guard on both being present is therefore always satisfied.
        m_zfg ^= 0x01;

        int recv = read_frame(data_size);
        while (recv > 0) {
            const int idx = m_buffer[0];
            if (idx == 0xff) {
                // Link id assignment going round the ring.
                if (is_master) {
                    m_linkid    = 0x01;
                    m_linkcount = m_buffer[1];
                    m_linktimer = 0x00;
                } else {
                    if (is_slave) m_buffer[1]++;
                    send_frame(data_size);
                }
            } else if (idx == 0xfe) {
                // Ring size, which is what settles the link.
                if (is_slave) {
                    m_linkid    = m_buffer[1];
                    m_linkcount = m_buffer[2];
                    m_buffer[1]--;
                    send_frame(data_size);
                } else if (is_relay) {
                    m_linkid    = 0x00;
                    m_linkcount = m_buffer[2];
                    send_frame(data_size);
                }
                SM2_DEBUG("m2comm: link established, id %02x of %02x", m_linkid,
                          m_linkcount);
                m_linkalive = 0x01;
                set_shared(0, 0x01);
                set_shared(2, m_linkid);
                set_shared(3, m_linkcount);
            }
            recv = (m_linkalive == 0x00) ? read_frame(data_size) : 0;
        }

        if (is_master && m_linkalive == 0x00) {
            if (m_linktimer == 0x01) {
                m_buffer[0] = 0xff;
                m_buffer[1] = 0x01;
                m_buffer[2] = 0x00;
                send_frame(data_size);
            } else if (m_linktimer == 0x00) {
                m_buffer[0] = 0xfe;
                m_buffer[1] = m_linkcount;
                m_buffer[2] = m_linkcount;
                send_frame(data_size);
                SM2_DEBUG("m2comm: link established, id %02x of %02x", m_linkid,
                          m_linkcount);
                m_linkalive = 0x01;
                set_shared(0, 0x01);
                set_shared(2, m_linkid);
                set_shared(3, m_linkcount);
            } else if (m_linktimer > 0x01) {
                m_linktimer--;
            }
        }
    }

    if (m_linkalive == 0x01) {
        do {
            int recv = read_frame(data_size);
            while (recv > 0) {
                const int idx = m_buffer[0];
                if (idx <= static_cast<int>(m_linkcount)) {
                    for (int j = 0; j < frame_size && 1 + j < kMaxFrame; j++) {
                        set_shared(u32(frame_offset + j), m_buffer[std::size_t(1 + j)]);
                    }
                    m_zfg ^= 0x01;
                    if (is_slave) {
                        send_data(m_linkid, kFrameStart, frame_size, data_size);
                    } else if (is_relay) {
                        send_frame(data_size);
                    }
                } else if (idx == 0xfc) {
                    // Vertical sync going round the ring.
                    m_linktimer = 0x00;
                    if (!is_master) send_frame(data_size);
                }
                recv = read_frame(data_size);
            }
        } while (m_linktimer == 0x01);

        m_linktimer = m_frame_sync;
        if (is_master) {
            send_data(m_linkid, kFrameStart, frame_size, data_size);
            m_buffer[0] = 0xfc;
            m_buffer[1] = 0x01;
            send_frame(data_size);
        }
        m_zfg_delay = 0x02;
    }
}

void M2Comm::read_fg()
{
    // Two reads after a frame arrived report the gate unchanged, which is how the
    // host is given time to consume what is already there.
    if (m_zfg_delay > 0x00) {
        m_zfg_delay--;
        return;
    }
    if (m_linkalive != 0x01) return;

    const int frame_size = (shared(0x13) << 8) | shared(0x12);
    const int frame_offset = kFrameStart | (shared(0x15) << 8) | shared(0x14);
    const int data_size = frame_size + 1;

    const bool is_master = (m_fg == 0x01 || shared(0x01) == 0x01);
    const bool is_slave  = (m_fg == 0x00 && shared(0x01) == 0x02);
    const bool is_relay  = (m_fg == 0x00 && shared(0x01) == 0x00);

    do {
        int recv = read_frame(data_size);
        while (recv > 0) {
            const int idx = m_buffer[0];
            if (idx <= static_cast<int>(m_linkcount)) {
                for (int j = 0; j < frame_size && 1 + j < kMaxFrame; j++) {
                    set_shared(u32(frame_offset + j), m_buffer[std::size_t(1 + j)]);
                }
                m_zfg ^= 0x01;
                if (is_slave) {
                    send_data(m_linkid, kFrameStart, frame_size, data_size);
                } else if (is_relay) {
                    send_frame(data_size);
                }
            } else if (idx == 0xfc) {
                m_linktimer = 0x00;
                if (!is_master) send_frame(data_size);
            }
            recv = read_frame(data_size);
        }
    } while (m_linktimer == 0x01);
}

}  // namespace sm2::hw
