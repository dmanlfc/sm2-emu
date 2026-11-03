// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// See m1comm.h.

#include "hw/m1comm.h"

#include "core/archive.h"
#include "core/log.h"

#include <algorithm>
#include <optional>
#include <span>
#include <vector>

namespace sm2::hw {

namespace {

constexpr int kFrameStart = 0x0010;
constexpr int kFrameSize  = 0x01c4;
constexpr int kDataSize   = kFrameSize + 1;

}  // namespace

M1Comm::M1Comm()
    : m_transport(std::make_unique<LoopbackTransport>())
{
}

void M1Comm::set_transport(std::unique_ptr<CommTransport> transport)
{
    m_transport = transport ? std::move(transport) : std::make_unique<LoopbackTransport>();
    m_transport->reset();
}

void M1Comm::reset()
{
    m_zfg = 0;
    m_cn  = 0;
    m_fg  = 0;

    m_linkenable = 0;
    m_linktimer  = 0;
    m_linkalive  = 0;
    m_linkid     = 0;
    m_linkcount  = 0;
    m_shared.fill(0);
    m_transport->reset();
}

void M1Comm::serialize(Archive& ar)
{
    ar.bytes(m_shared.data(), m_shared.size());
    ar.raw(m_zfg);
    ar.raw(m_cn);
    ar.raw(m_fg);
    ar.raw(m_linkenable);
    ar.raw(m_linktimer);
    ar.raw(m_linkalive);
    ar.raw(m_linkid);
    ar.raw(m_linkcount);
    ar.raw(m_frame_sync);
    ar.bytes(m_buffer.data(), m_buffer.size());
}

void M1Comm::cn_write(u8 value)
{
    m_cn = static_cast<u8>(value & 0x01);
    m_transport->reset();
    if (m_cn == 0) {
        SM2_DEBUG("m1comm: board disabled");
        m_linkenable = 0x00;
        m_zfg        = 0x00;
        return;
    }
    SM2_DEBUG("m1comm: board enabled");
    m_linkenable = 0x01;
    m_linkid     = 0x00;
    m_linkalive  = 0x00;
    m_linkcount  = 0x00;
    m_linktimer  = 0x00e8;  // 58 fps for about four seconds
}

int M1Comm::read_frame(int data_size)
{
    std::optional<std::vector<u8>> frame = m_transport->recv();
    if (!frame) {
        if (!m_transport->connected() && m_linkalive == 0x01) lose_link();
        return 0;
    }
    const int count = static_cast<int>(std::min<std::size_t>(frame->size(), std::size_t(data_size)));
    std::copy_n(frame->begin(), count, m_buffer.begin());
    return count;
}

void M1Comm::send_frame(int data_size)
{
    m_transport->send(std::span<const u8>(m_buffer.data(), std::size_t(data_size)));
    if (!m_transport->connected() && m_linkalive == 0x01) lose_link();
}

void M1Comm::lose_link()
{
    SM2_DEBUG("m1comm: link lost");
    m_linkalive = 0x02;
    m_linktimer = 0x00;
    m_shared[0] = 0xff;
}

void M1Comm::send_data(u8 frame_type, int frame_start, int frame_size, int data_size)
{
    m_buffer[0] = frame_type;
    for (int i = 0; i < frame_size; i++) {
        m_buffer[std::size_t(1 + i)] = m_shared[std::size_t(frame_start + i)];
    }
    send_frame(data_size);
}

void M1Comm::tick()
{
    if (m_linkenable != 0x01) return;

    const bool is_master = m_shared[1] == 0x01;
    const bool is_slave  = m_shared[1] == 0x02;
    const bool is_relay  = m_shared[1] == 0x00;

    if (m_linkalive == 0x02) {
        m_shared[0] = 0xff;
        return;
    }

    const auto established = [this] {
        SM2_DEBUG("m1comm: link established, id %02x of %02x", m_linkid, m_linkcount);
        m_linkalive = 0x01;
        m_zfg       = 0x01;
        m_shared[0] = 0x01;
        m_shared[2] = m_linkid;
        m_shared[3] = m_linkcount;
    };

    if (m_linkalive == 0x00) {
        m_shared[0] = 0x05;

        int recv = read_frame(kDataSize);
        while (recv > 0) {
            const int idx = m_buffer[0];
            if (idx == 0xff) {
                // Link id assignment going round the ring.
                if (is_master) {
                    m_linkid    = 0x01;
                    m_linkcount = m_buffer[1];
                    m_linktimer = 0x00;
                } else {
                    if (is_slave) {
                        m_buffer[1]++;
                        m_linkid = m_buffer[1];
                    }
                    send_frame(kDataSize);
                }
            } else if (idx == 0xfe) {
                // Ring size, which settles the link.
                if (is_slave || is_relay) {
                    m_linkcount = m_buffer[1];
                    send_frame(kDataSize);
                }
                established();
            }
            recv = m_linkalive == 0x00 ? read_frame(kDataSize) : 0;
        }

        if (is_master && m_linkalive == 0x00) {
            if (m_linktimer == 0x01) {
                m_buffer[0] = 0xff;
                m_buffer[1] = 0x01;
                send_frame(kDataSize);
            } else if (m_linktimer == 0x00) {
                m_buffer[0] = 0xfe;
                m_buffer[1] = m_linkcount;
                send_frame(kDataSize);
                established();
            } else if (m_linktimer > 0x01) {
                m_linktimer--;
            }
        }
    }

    if (m_linkalive != 0x01) return;

    do {
        int recv = read_frame(kDataSize);
        while (recv > 0) {
            const int idx = m_buffer[0];
            if (idx > 0 && idx <= m_linkcount) {
                if (idx != m_linkid) {
                    const int offset = kFrameStart + idx * kFrameSize;
                    for (int j = 0; j < kFrameSize && offset + j < int(kSharedSize); j++) {
                        m_shared[std::size_t(offset + j)] = m_buffer[std::size_t(1 + j)];
                    }
                    send_frame(kDataSize);
                }
            } else if (idx == 0xfc) {
                // Vertical sync going round the ring.
                m_linktimer = 0x00;
                if (!is_master) send_frame(kDataSize);
            } else if (idx == 0xfd && !is_master) {
                // The master's status bytes.
                for (int j = 0; j < 0x0a; j++) {
                    m_shared[std::size_t(0x06 + j)] = m_buffer[std::size_t(1 + j)];
                }
                send_frame(kDataSize);
            }
            recv = read_frame(kDataSize);
        }
    } while (m_linktimer == 0x01);

    m_linktimer = m_frame_sync;

    // A relay sends nothing of its own.
    if (m_linkid != 0x00) {
        if (m_shared[4] != 0x00) {
            send_data(m_linkid, kFrameStart, kFrameSize, kDataSize);
            const int offset = kFrameStart + m_linkid * kFrameSize;
            for (int j = 0; j < kFrameSize && offset + j < int(kSharedSize); j++) {
                m_shared[std::size_t(offset + j)] = m_buffer[std::size_t(1 + j)];
            }
        }
        if (is_master) {
            send_data(0xfd, 0x06, 0x0a, kDataSize);
            m_buffer[0] = 0xfc;
            m_buffer[1] = 0x01;
            send_frame(kDataSize);
        }
    }

    m_shared[5] = 0x00;
}

}  // namespace sm2::hw
