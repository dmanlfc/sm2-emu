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
// See net.h. Platform sockets and interface enumeration behind one set of
// #ifdefs.

#include "core/net.h"

#include "core/log.h"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#    include <winsock2.h>
#    include <ws2tcpip.h>
#    include <iphlpapi.h>
#    pragma comment(lib, "Ws2_32.lib")
#    pragma comment(lib, "Iphlpapi.lib")
#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <ifaddrs.h>
#    include <net/if.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace sm2::net {

namespace {

#if defined(_WIN32)
std::string last_socket_error()
{
    return "winsock error " + std::to_string(WSAGetLastError());
}
#else
std::string last_socket_error()
{
    return std::strerror(errno);
}
#endif

}  // namespace

// ---------------------------------------------------------------------------
// Interface enumeration
// ---------------------------------------------------------------------------

#if defined(_WIN32)

std::vector<Interface> interfaces()
{
    std::vector<Interface> result;

    ULONG                 size  = 0;
    const ULONG           flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST
                                | GAA_FLAG_SKIP_DNS_SERVER;
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, nullptr, &size)
        != ERROR_BUFFER_OVERFLOW) {
        return result;
    }
    std::vector<u8> buffer(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) != NO_ERROR) {
        return result;
    }

    for (auto* a = adapters; a != nullptr; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        const bool loopback = a->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        for (auto* ua = a->FirstUnicastAddress; ua != nullptr; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            char  ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);

            // Prefix length -> dotted-quad mask.
            std::string mask;
            {
                const ULONG bits = ua->OnLinkPrefixLength;
                const u32   m    = bits == 0 ? 0u : (0xffffffffu << (32 - bits));
                char        mbuf[INET_ADDRSTRLEN] = {};
                in_addr     ma{};
                ma.s_addr = htonl(m);
                inet_ntop(AF_INET, &ma, mbuf, sizeof mbuf);
                mask = mbuf;
            }

            char nbuf[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, nbuf, sizeof nbuf,
                                nullptr, nullptr);
            result.push_back(Interface{nbuf, ip, mask, loopback});
        }
    }
    return result;
}

#else

std::vector<Interface> interfaces()
{
    std::vector<Interface> result;

    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0 || list == nullptr) {
        return result;
    }

    for (ifaddrs* ifa = list; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if ((ifa->ifa_flags & IFF_UP) == 0) continue;

        auto* sin = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        char  ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);

        std::string mask;
        if (ifa->ifa_netmask != nullptr) {
            auto* nm = reinterpret_cast<sockaddr_in*>(ifa->ifa_netmask);
            char  mbuf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &nm->sin_addr, mbuf, sizeof mbuf);
            mask = mbuf;
        }

        const bool loopback = (ifa->ifa_flags & IFF_LOOPBACK) != 0;
        result.push_back(Interface{ifa->ifa_name ? ifa->ifa_name : "", ip, mask,
                                    loopback});
    }

    freeifaddrs(list);
    return result;
}

#endif

std::optional<Interface> primary_interface()
{
    for (const Interface& iface : interfaces()) {
        if (!iface.loopback && !iface.ipv4.empty()) {
            return iface;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// UDP socket
// ---------------------------------------------------------------------------

UdpSocket::~UdpSocket()
{
    close();
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept
    : m_fd(other.m_fd), m_last_error(std::move(other.m_last_error))
{
    other.m_fd = kInvalid;
}

UdpSocket& UdpSocket::operator=(UdpSocket&& other) noexcept
{
    if (this != &other) {
        close();
        m_fd         = other.m_fd;
        m_last_error = std::move(other.m_last_error);
        other.m_fd   = kInvalid;
    }
    return *this;
}

bool UdpSocket::open(const std::string& bind_ip, u16 port)
{
    close();

#if defined(_WIN32)
    const Fd fd = static_cast<Fd>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd == static_cast<Fd>(INVALID_SOCKET)) {
        m_last_error = last_socket_error();
        return false;
    }
#else
    const Fd fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        m_last_error = last_socket_error();
        return false;
    }
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (bind_ip.empty()) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bind_ip.c_str(), &addr.sin_addr) != 1) {
        m_last_error = "invalid bind address '" + bind_ip + "'";
#if defined(_WIN32)
        ::closesocket(static_cast<SOCKET>(fd));
#else
        ::close(fd);
#endif
        return false;
    }

    if (::bind(
#if defined(_WIN32)
            static_cast<SOCKET>(fd),
#else
            fd,
#endif
            reinterpret_cast<sockaddr*>(&addr), sizeof addr)
        != 0) {
        m_last_error = "bind " + (bind_ip.empty() ? std::string("*") : bind_ip) + ":"
                     + std::to_string(port) + ": " + last_socket_error();
#if defined(_WIN32)
        ::closesocket(static_cast<SOCKET>(fd));
#else
        ::close(fd);
#endif
        return false;
    }

    // Non-blocking.
#if defined(_WIN32)
    u_long nonblock = 1;
    ioctlsocket(static_cast<SOCKET>(fd), FIONBIO, &nonblock);
#else
    const int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
#endif

    m_fd = fd;
    m_last_error.clear();
    return true;
}

void UdpSocket::close()
{
    if (m_fd != kInvalid) {
#if defined(_WIN32)
        ::closesocket(static_cast<SOCKET>(m_fd));
#else
        ::close(m_fd);
#endif
        m_fd = kInvalid;
    }
}

bool UdpSocket::send_to(std::span<const u8> data, const std::string& dest_ip, u16 port)
{
    if (m_fd == kInvalid) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (dest_ip.empty() || inet_pton(AF_INET, dest_ip.c_str(), &addr.sin_addr) != 1) {
        return false;
    }

    const auto n = ::sendto(
#if defined(_WIN32)
        static_cast<SOCKET>(m_fd),
        reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()),
#else
        m_fd, data.data(), data.size(),
#endif
        0, reinterpret_cast<sockaddr*>(&addr), sizeof addr);

    if (n < 0) {
        m_last_error = last_socket_error();
        return false;
    }
    return true;
}

bool UdpSocket::recv_from(std::vector<u8>& out)
{
    if (m_fd == kInvalid) return false;

    // Model 2 comm frames top out at 0x1000+1 bytes; a page is ample headroom.
    u8 buffer[0x1200];
    const auto n = ::recvfrom(
#if defined(_WIN32)
        static_cast<SOCKET>(m_fd), reinterpret_cast<char*>(buffer), sizeof buffer,
#else
        m_fd, buffer, sizeof buffer,
#endif
        0, nullptr, nullptr);

    if (n <= 0) {
        // n==0 empty datagram, or n<0 would-block (the normal no-data path).
        return false;
    }
    out.assign(buffer, buffer + n);
    return true;
}

// ---------------------------------------------------------------------------
// Process startup / teardown
// ---------------------------------------------------------------------------

#if defined(_WIN32)

namespace {
int g_wsa_refcount = 0;
}

bool startup()
{
    if (g_wsa_refcount++ > 0) return true;
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        SM2_WARN("net: WSAStartup failed");
        g_wsa_refcount = 0;
        return false;
    }
    return true;
}

void shutdown()
{
    if (g_wsa_refcount > 0 && --g_wsa_refcount == 0) {
        WSACleanup();
    }
}

#else

bool startup() { return true; }
void shutdown() {}

#endif

}  // namespace sm2::net
