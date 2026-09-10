//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
// Transport seam for the Model 2 communication board (see m2comm.h): M2Comm
// hands whole frames to send()/recv() and does not care how they travel.
// LoopbackTransport (default) reads a sent frame straight back, so a lone
// cabinet settles as node 1 of 1; comm_udp.h carries frames over a LAN instead.
#pragma once

#include "core/types.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <vector>

namespace sm2::hw {

/// The seam M2Comm sends and receives whole frames through. A frame is an id
/// byte followed by its payload, queued and delivered as one unit.
class CommTransport {
public:
    virtual ~CommTransport() = default;

    /// Hand a frame (copied) to the next node. A transport that cannot accept it
    /// drops connected() to false, which is how the board learns the link died.
    virtual void send(std::span<const u8> frame) = 0;

    /// Next frame from the previous node, or nullopt when none waits. Never blocks.
    virtual std::optional<std::vector<u8>> recv() = 0;

    /// Whether the transport can still carry frames. Loopback is always true; a
    /// socket transport drops it when a peer goes away or its buffer backs up.
    [[nodiscard]] virtual bool connected() const = 0;

    /// Drop queued frames and return to the constructed state. Called when the
    /// board is enabled/disabled (the host cycles CN, it does not reset).
    virtual void reset() = 0;
};

/// In-process loopback: a sent frame is read straight back by the same board,
/// which keeps a single-cabinet game working with no configuration.
class LoopbackTransport final : public CommTransport {
public:
    /// Queue depth before a send is refused, standing in for the socket buffer.
    static constexpr std::size_t kDepth = 64;

    void send(std::span<const u8> frame) override
    {
        if (m_queue.size() >= kDepth) {
            // A full buffer fails the write, which the board reads as the link
            // dying. Only reachable if the host relays its own frames back.
            m_overflowed = true;
            return;
        }
        m_queue.emplace_back(frame.begin(), frame.end());
    }

    std::optional<std::vector<u8>> recv() override
    {
        if (m_queue.empty()) return std::nullopt;
        std::vector<u8> front = std::move(m_queue.front());
        m_queue.pop_front();
        return front;
    }

    [[nodiscard]] bool connected() const override { return !m_overflowed; }

    void reset() override
    {
        m_queue.clear();
        m_overflowed = false;
    }

private:
    std::deque<std::vector<u8>> m_queue;
    bool                        m_overflowed = false;
};

}  // namespace sm2::hw
