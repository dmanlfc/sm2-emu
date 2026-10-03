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
//
// See outputs.h.

#include "osd/outputs.h"

#include "core/log.h"
#include "core/net.h"

#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#    include <windows.h>
#endif

namespace sm2::osd {

class Outputs::Backend {
public:
    virtual ~Backend() = default;

    virtual void start(const std::string& game, const std::vector<Item>& items) = 0;
    virtual void stop() = 0;
    virtual void changed(usize index, const Item& item) = 0;
    virtual void paused(bool paused) = 0;
    virtual void poll(const std::string& game, const std::vector<Item>& items,
                      bool paused) = 0;
    [[nodiscard]] virtual NetworkStatus status() const { return {}; }
};

namespace {

// ---------------------------------------------------------------------------
// Lamp names
// ---------------------------------------------------------------------------

constexpr int kRawDrive = -1;
constexpr int kRawLamps = -2;

struct NamedBit {
    const char* name;
    int         bit;
};

/// Friendly names for the lamp latch, per set family, from MAME's model2.cpp
/// output notes. Bit 2 is the start lamp everywhere.
std::vector<NamedBit> family_lamps(const std::string& family)
{
    if (family == "daytona") {
        return {{"LampView1", 3}, {"LampView2", 4}, {"LampView3", 5},
                {"LampView4", 6}, {"LampLeader", 7}};
    }
    if (family == "desert") {
        return {{"LampView3", 3}, {"LampView2", 4}, {"LampView1", 5},
                {"MachineGunMotor", 6}, {"CannonMotor", 7}};
    }
    if (family == "srallyc") {
        return {{"LampView1", 5}, {"LampLeader", 7}};
    }
    if (family == "stcc") {
        return {{"LampRevMax", 3}, {"LampView1", 4}, {"LampView2", 5}};
    }
    if (family == "indy500") {
        return {{"LampView1", 4}, {"LampView2", 5}, {"LampLeader", 7}};
    }
    if (family == "overrev" || family == "sgt24h") {
        return {{"LampView1", 4}, {"LampView2", 5}};
    }
    return {};
}

std::string line(std::string_view name, s32 value)
{
    return std::string(name) + " = " + std::to_string(value) + "\r";
}

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

class NetworkBackend final : public Outputs::Backend {
public:
    NetworkBackend(u16 port, u16 announce_port) : m_port(port), m_announce_port(announce_port)
    {
        m_started = net::startup();
        if (m_started && m_server.open(port)) {
            SM2_INFO("outputs: network outputs on port %u", port);
        } else if (m_server.address_in_use()) {
            m_error = "port " + std::to_string(port) + " is already in use";
            SM2_WARN("outputs: TCP port %u is already in use, probably by another emulator "
                     "or output tool; close it, or set outputs_network_port to a free port",
                     port);
        } else {
            m_error = m_server.last_error();
            SM2_WARN("outputs: %s", m_server.last_error().c_str());
        }

        // Supermodel's announcement goes to this machine, where the tools run.
        if (m_started && m_announce_port != 0 && !m_announce.open("127.0.0.1", 0)) {
            SM2_WARN("outputs: no UDP announcement: %s", m_announce.last_error().c_str());
        }
    }

    ~NetworkBackend() override
    {
        m_server.close();
        if (m_started) {
            net::shutdown();
        }
    }

    void start(const std::string& game, const std::vector<Outputs::Item>& items) override
    {
        m_server.broadcast("mame_start = " + game + "\r");
        for (const Outputs::Item& item : items) {
            if (item.known) {
                m_server.broadcast(line(item.name, item.value));
            }
        }
        announce(game);
    }

    void stop() override { m_server.broadcast("mame_stop = 1\r"); }

    void changed(usize /*index*/, const Outputs::Item& item) override
    {
        m_server.broadcast(line(item.name, item.value));
    }

    void paused(bool paused) override { m_server.broadcast(line("pause", paused ? 1 : 0)); }

    void poll(const std::string& game, const std::vector<Outputs::Item>& items,
              bool paused) override
    {
        m_server.poll([&](u64 client) {
            if (game.empty()) {
                return;
            }
            m_server.send(client, "mame_start = " + game + "\r");
            for (const Outputs::Item& item : items) {
                if (item.known) {
                    m_server.send(client, line(item.name, item.value));
                }
            }
            if (paused) {
                m_server.send(client, "pause = 1\r");
            }
        });
    }

    [[nodiscard]] Outputs::NetworkStatus status() const override
    {
        return {true, m_server.valid(), m_server.client_count(), m_error};
    }

private:
    /// Supermodel's datagram, "mame_start = <set>\rtcp = <port>\r", sent once
    /// when a game starts, provided the server listens.
    void announce(const std::string& game)
    {
        if (game.empty() || !m_announce.valid() || !m_server.valid()) {
            return;
        }
        const std::string text =
            "mame_start = " + game + "\rtcp = " + std::to_string(m_port) + "\r";
        const auto* bytes = reinterpret_cast<const u8*>(text.data());
        static_cast<void>(
            m_announce.send_to(std::span<const u8>(bytes, text.size()), "127.0.0.1", m_announce_port));
    }

    net::TcpServer m_server;
    net::UdpSocket m_announce;
    u16            m_port          = 0;
    u16            m_announce_port = 0;
    std::string    m_error;
    bool           m_started = false;
};

// ---------------------------------------------------------------------------
// Windows messages
// ---------------------------------------------------------------------------

#if defined(_WIN32)

/// MAME numbers outputs from here; 0 is the set name and 1 is "pause".
constexpr u32 kFirstId = 12345;
constexpr u32 kPauseId = 1;

class WindowsBackend final : public Outputs::Backend {
public:
    WindowsBackend()
    {
        m_start      = RegisterWindowMessageA("MAMEOutputStart");
        m_stop       = RegisterWindowMessageA("MAMEOutputStop");
        m_update     = RegisterWindowMessageA("MAMEOutputUpdateState");
        m_register   = RegisterWindowMessageA("MAMEOutputRegister");
        m_unregister = RegisterWindowMessageA("MAMEOutputUnregister");
        m_get_id     = RegisterWindowMessageA("MAMEOutputGetIDString");

        WNDCLASSA wc{};
        wc.lpfnWndProc   = &WindowsBackend::window_proc;
        wc.hInstance     = GetModuleHandleA(nullptr);
        wc.lpszClassName = "MAMEOutput";
        RegisterClassA(&wc);  // Fails harmlessly if already registered.

        m_hwnd = CreateWindowExA(0, "MAMEOutput", "MAMEOutput", WS_OVERLAPPEDWINDOW, 0, 0,
                                 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
        if (m_hwnd == nullptr) {
            SM2_WARN("outputs: could not create the MAMEOutput window");
            return;
        }
        SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SM2_INFO("outputs: Windows message outputs enabled");
    }

    ~WindowsBackend() override
    {
        if (m_hwnd != nullptr) {
            DestroyWindow(m_hwnd);
        }
    }

    void start(const std::string& game, const std::vector<Outputs::Item>& items) override
    {
        m_game  = game;
        m_items = &items;
        if (m_hwnd != nullptr) {
            PostMessageA(HWND_BROADCAST, m_start, reinterpret_cast<WPARAM>(m_hwnd), 0);
        }
    }

    void stop() override
    {
        if (m_hwnd != nullptr) {
            PostMessageA(HWND_BROADCAST, m_stop, reinterpret_cast<WPARAM>(m_hwnd), 0);
        }
        m_clients.clear();
        m_game.clear();
        m_items = nullptr;
    }

    void changed(usize index, const Outputs::Item& item) override
    {
        for (const Client& client : m_clients) {
            PostMessageA(client.hwnd, m_update, kFirstId + index,
                         static_cast<LPARAM>(item.value));
        }
    }

    void paused(bool paused) override
    {
        m_paused = paused;
        for (const Client& client : m_clients) {
            PostMessageA(client.hwnd, m_update, kPauseId, paused ? 1 : 0);
        }
    }

    // SDL's event pump dispatches this thread's window messages.
    void poll(const std::string&, const std::vector<Outputs::Item>&, bool) override {}

private:
    struct Client {
        HWND   hwnd;
        LPARAM id;
    };

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam,
                                        LPARAM lparam)
    {
        auto* self = reinterpret_cast<WindowsBackend*>(GetWindowLongPtrA(hwnd, GWLP_USERDATA));
        if (self != nullptr) {
            if (message == self->m_register) {
                return self->register_client(reinterpret_cast<HWND>(wparam), lparam);
            }
            if (message == self->m_unregister) {
                return self->unregister_client(lparam);
            }
            if (message == self->m_get_id) {
                return self->send_id_string(reinterpret_cast<HWND>(wparam), lparam);
            }
        }
        return DefWindowProcA(hwnd, message, wparam, lparam);
    }

    LRESULT register_client(HWND hwnd, LPARAM id)
    {
        LRESULT result = 0;
        bool    found  = false;
        for (Client& client : m_clients) {
            if (client.id == id) {
                client.hwnd = hwnd;
                found       = true;
                result      = 1;
            }
        }
        if (!found) {
            m_clients.push_back({hwnd, id});
        }
        if (m_items != nullptr) {
            for (usize i = 0; i < m_items->size(); ++i) {
                if ((*m_items)[i].known) {
                    PostMessageA(hwnd, m_update, kFirstId + i,
                                 static_cast<LPARAM>((*m_items)[i].value));
                }
            }
        }
        PostMessageA(hwnd, m_update, kPauseId, m_paused ? 1 : 0);
        return result;
    }

    LRESULT unregister_client(LPARAM id)
    {
        for (usize i = 0; i < m_clients.size(); ++i) {
            if (m_clients[i].id == id) {
                m_clients.erase(m_clients.begin() + static_cast<std::ptrdiff_t>(i));
                return 0;
            }
        }
        return 1;
    }

    LRESULT send_id_string(HWND hwnd, LPARAM id)
    {
        std::string name;
        if (id == 0) {
            name = m_game;
        } else if (id == kPauseId) {
            name = "pause";
        } else if (m_items != nullptr && id >= static_cast<LPARAM>(kFirstId)
                   && static_cast<usize>(id - kFirstId) < m_items->size()) {
            name = (*m_items)[static_cast<usize>(id - kFirstId)].name;
        }

        // MAME's copydata_id_string: a u32 id followed by the NUL-terminated name.
        std::vector<char> buffer(sizeof(u32) + name.size() + 1, '\0');
        const u32 id32 = static_cast<u32>(id);
        std::memcpy(buffer.data(), &id32, sizeof id32);
        std::memcpy(buffer.data() + sizeof id32, name.data(), name.size());

        COPYDATASTRUCT copydata{};
        copydata.dwData = 1;  // COPYDATA_MESSAGE_ID_STRING
        copydata.cbData = static_cast<DWORD>(buffer.size());
        copydata.lpData = buffer.data();
        SendMessageA(hwnd, WM_COPYDATA, reinterpret_cast<WPARAM>(m_hwnd),
                     reinterpret_cast<LPARAM>(&copydata));
        return 0;
    }

    HWND                              m_hwnd  = nullptr;
    UINT                              m_start = 0, m_stop = 0, m_update = 0;
    UINT                              m_register = 0, m_unregister = 0, m_get_id = 0;
    std::vector<Client>               m_clients;
    std::string                       m_game;
    const std::vector<Outputs::Item>* m_items  = nullptr;
    bool                              m_paused = false;
};

#endif

}  // namespace

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------

Outputs::Outputs() = default;

Outputs::~Outputs()
{
    set_game({}, {});
}

void Outputs::configure(bool network, u16 port, u16 announce_port, bool windows)
{
    if (network && (m_network == nullptr || port != m_port || announce_port != m_announce_port)) {
        m_network.reset();
        m_network       = std::make_unique<NetworkBackend>(port, announce_port);
        m_port          = port;
        m_announce_port = announce_port;
        if (!m_game.empty()) {
            m_network->start(m_game, m_items);
        }
    } else if (!network && m_network != nullptr) {
        if (!m_game.empty()) {
            m_network->stop();
            m_network->poll(m_game, m_items, m_paused);
        }
        m_network.reset();
    }

#if defined(_WIN32)
    if (windows && m_windows == nullptr) {
        m_windows = std::make_unique<WindowsBackend>();
        if (!m_game.empty()) {
            m_windows->start(m_game, m_items);
        }
    } else if (!windows && m_windows != nullptr) {
        if (!m_game.empty()) {
            m_windows->stop();
        }
        m_windows.reset();
    }
#else
    static_cast<void>(windows);
#endif
}

void Outputs::set_game(const std::string& name, const std::string& parent)
{
    if (name == m_game) {
        return;
    }
    for (Backend* backend : {m_network.get(), m_windows.get()}) {
        if (backend != nullptr && !m_game.empty()) {
            backend->stop();
        }
    }
    m_game = name;
    m_items.clear();
    m_bits.clear();
    if (m_game.empty()) {
        poll();
        return;
    }
    build_items(parent.empty() ? name : parent);
    for (Backend* backend : {m_network.get(), m_windows.get()}) {
        if (backend != nullptr) {
            backend->start(m_game, m_items);
        }
    }
}

void Outputs::build_items(const std::string& family)
{
    const auto add = [this](std::string name, int bit) {
        m_items.push_back({std::move(name), 0, true});
        m_bits.push_back(bit);
    };
    for (int i = 0; i < 6; ++i) {
        add("lamp" + std::to_string(i), i + 2);
    }
    add("LampStart", 2);
    for (const NamedBit& named : family_lamps(family)) {
        add(named.name, named.bit);
    }
    add("RawLamps", kRawLamps);
    add("RawDrive", kRawDrive);
}

void Outputs::set_paused(bool paused)
{
    if (paused == m_paused) {
        return;
    }
    m_paused = paused;
    if (m_game.empty()) {
        return;
    }
    for (Backend* backend : {m_network.get(), m_windows.get()}) {
        if (backend != nullptr) {
            backend->paused(paused);
        }
    }
}

void Outputs::update(u8 lamp_latch, std::span<const u8> drive_writes)
{
    if (m_game.empty() || (m_network == nullptr && m_windows == nullptr)) {
        return;
    }
    for (usize i = 0; i < m_items.size(); ++i) {
        if (m_bits[i] >= 0) {
            set(i, (lamp_latch >> m_bits[i]) & 1);
        } else if (m_bits[i] == kRawLamps) {
            set(i, lamp_latch);
        } else {
            // Every byte in order: a force command can be followed by its
            // parameters within one frame.
            for (const u8 value : drive_writes) {
                set(i, value);
            }
        }
    }
}

void Outputs::set(usize index, s32 value)
{
    Item& item = m_items[index];
    if (item.known && item.value == value) {
        return;
    }
    item.value = value;
    item.known = true;
    for (Backend* backend : {m_network.get(), m_windows.get()}) {
        if (backend != nullptr) {
            backend->changed(index, item);
        }
    }
}

void Outputs::poll()
{
    if (m_network != nullptr) {
        m_network->poll(m_game, m_items, m_paused);
    }
}

Outputs::NetworkStatus Outputs::network_status() const
{
    return m_network != nullptr ? m_network->status() : NetworkStatus{};
}

}  // namespace sm2::osd
