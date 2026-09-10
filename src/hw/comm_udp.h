//
// A CommTransport that carries link-board frames over the LAN by UDP.
//
// The Model 2 comms board is a ring: each cabinet receives from the one before
// it and sends to the one after. This transport binds a local UDP port to
// receive on and sends each outgoing frame to the next cabinet's address. A
// frame is small (~0xe00 bytes) and maps one-to-one onto a datagram, so no
// stream framing is needed; the ring protocol in M2Comm already re-sends state
// every frame and tolerates the odd lost datagram, which is why UDP suits it
// better than a TCP ring that would head-of-line block when a cabinet booted
// late.
//
// Sending and receiving are driven from the board's per-frame vblank() tick, so
// this never blocks: recv() drains whatever datagrams have arrived and hands
// them over one at a time.
#pragma once

#include "core/net.h"
#include "core/types.h"
#include "hw/comm_transport.h"

#include <cstddef>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {

class UdpTransport final : public CommTransport {
public:
    /// Received datagrams held before the oldest is dropped. The ring's steady
    /// state is a couple of frames in flight; a deeper backlog means the far end
    /// is racing ahead and the stale frames are worthless, so dropping them is
    /// correct rather than merely tolerable.
    static constexpr std::size_t kBacklog = 64;

    /// Bind local_ip:local_port to receive on, and remember where to send. An
    /// empty local_ip listens on every interface; an empty next_ip means this
    /// node does not forward (the tail of a chain). Call ok() afterwards; on
    /// failure the transport reports not-connected and behaves inertly, so the
    /// board simply never links rather than crashing.
    UdpTransport(const std::string& local_ip, u16 local_port,
                 std::string next_ip, u16 next_port);

    [[nodiscard]] bool               ok() const { return m_socket.valid(); }
    [[nodiscard]] const std::string& error() const { return m_socket.last_error(); }

    void                           send(std::span<const u8> frame) override;
    std::optional<std::vector<u8>> recv() override;
    [[nodiscard]] bool             connected() const override;
    void                           reset() override;

private:
    void drain();  ///< Pull every waiting datagram off the socket into m_rx.

    net::UdpSocket m_socket;
    std::string    m_next_ip;
    u16            m_next_port = 0;

    std::deque<std::vector<u8>> m_rx;
    bool                        m_send_failed = false;
};

}  // namespace sm2::hw
