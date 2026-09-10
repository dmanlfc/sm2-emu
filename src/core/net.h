//
// Small cross-platform networking helpers.
//
// Two things live here: enumerating the machine's network interfaces (so the
// cabinet-link settings can prefill this host's own IPv4 address and subnet
// mask), and a thin non-blocking UDP socket the link transport is built on.
// Neither pulls in the machine or the OSD, so both sit in sm2_core.
#pragma once

#include "core/types.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sm2::net {

/// One network interface as seen by the host, IPv4 only (the Model 2 comms ring
/// is IPv4).
struct Interface {
    std::string name;         ///< OS interface name, e.g. "en0" or "eth0".
    std::string ipv4;         ///< Dotted-quad address, e.g. "192.168.1.42".
    std::string subnet_mask;  ///< Dotted-quad mask, e.g. "255.255.255.0".
    bool        loopback = false;
};

/// Every up, address-carrying IPv4 interface on this machine. Loopback is
/// included but flagged, so a caller can prefer a real LAN address.
[[nodiscard]] std::vector<Interface> interfaces();

/// The interface most likely to be the one on the cabinet LAN: the first
/// non-loopback IPv4 interface, or nullopt if the machine has only loopback.
/// Used to prefill the link settings on first run.
[[nodiscard]] std::optional<Interface> primary_interface();

// -- non-blocking UDP socket ------------------------------------------------

/// A datagram socket, bound to a local address, that never blocks. Thin wrapper
/// over the platform sockets so the link transport carries no #ifdefs.
///
/// Errors are reported through valid()/last_error() rather than exceptions: a
/// cabinet link that cannot bind should degrade to "not linked", not crash the
/// emulator mid-session.
class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&)            = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    /// Bind to bind_ip:port for receiving. An empty bind_ip binds every
    /// interface (INADDR_ANY). Returns false and sets last_error() on failure.
    bool open(const std::string& bind_ip, u16 port);

    void close();

    [[nodiscard]] bool valid() const { return m_fd != kInvalid; }

    /// Send one datagram to dest_ip:port. Returns false on error (including a
    /// full send buffer, which the caller treats as back-pressure). The
    /// destination is resolved once and cached per (ip,port).
    bool send_to(std::span<const u8> data, const std::string& dest_ip, u16 port);

    /// Receive one datagram into out, resized to the payload. Returns false when
    /// nothing is waiting (the common non-blocking case) or on error; check
    /// valid() to tell them apart.
    bool recv_from(std::vector<u8>& out);

    [[nodiscard]] const std::string& last_error() const { return m_last_error; }

private:
#if defined(_WIN32)
    // A SOCKET is an unsigned pointer-sized handle on Windows; store it wide and
    // compare against the platform's INVALID_SOCKET in the .cpp.
    using Fd                       = std::uintptr_t;
    static constexpr Fd kInvalid   = static_cast<Fd>(~0ull);
#else
    using Fd                       = int;
    static constexpr Fd kInvalid   = -1;
#endif

    Fd          m_fd = kInvalid;
    std::string m_last_error;
};

/// Process-wide network startup/teardown. A no-op everywhere except Windows,
/// where it drives WSAStartup/WSACleanup. Safe to call more than once; the
/// last matching shutdown does the real teardown. main() calls startup() once.
bool startup();
void shutdown();

}  // namespace sm2::net
