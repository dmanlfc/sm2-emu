//
// Transport seam for the Model 2 communication board (see m2comm.h).
//
// The ring protocol in M2Comm does not care how a frame reaches the next
// cabinet; it only needs to hand one off and pick one up. Every send goes
// through send() and every receive through recv(), so a single interface
// abstracts the two socket ends MAME opens.
//
// LoopbackTransport is the default and reproduces the historical behaviour: a
// frame sent goes straight onto a queue the same board reads back, so a lone
// cabinet settles as node 1 of 1 exactly as an unconfigured MAME does. A real
// LAN transport (see comm_udp.h) implements the same interface over sockets.
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

    /// Hand a frame to the next node in the ring. A copy is taken; the caller's
    /// buffer is free to change afterwards. An implementation that cannot accept
    /// it (a full send buffer) reports so through connected() going false, which
    /// is how the board learns the link has dropped.
    virtual void send(std::span<const u8> frame) = 0;

    /// Take the next frame from the previous node, or nullopt when none is
    /// waiting. Never blocks.
    virtual std::optional<std::vector<u8>> recv() = 0;

    /// Whether the transport still believes it can carry frames. The loopback is
    /// always connected; a socket transport drops this when a peer goes away or
    /// its send buffer backs up, mirroring MAME treating a failed socket write as
    /// the link dying.
    [[nodiscard]] virtual bool connected() const = 0;

    /// Drop every queued frame and return to the just-constructed state. Called
    /// when the board is enabled or disabled, since the host cycles CN rather
    /// than resetting the machine.
    virtual void reset() = 0;
};

/// The historical in-process behaviour: frames sent are read straight back by
/// the same board. Talks to no network. This is what keeps a single-cabinet
/// game working with no configuration, and what the m2comm tests pin.
class LoopbackTransport final : public CommTransport {
public:
    /// Frames the queue holds before a send is refused, standing in for the
    /// socket buffer MAME relies on. Steady state is two.
    static constexpr std::size_t kDepth = 64;

    void send(std::span<const u8> frame) override
    {
        if (m_queue.size() >= kDepth) {
            // A socket whose buffer has filled fails the write, which MAME reads
            // as the transmit side going away. The board polls connected() and
            // drops the link. Reachable only if the host reconfigures itself as
            // a relay after the link came up, forwarding its own frames back.
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
