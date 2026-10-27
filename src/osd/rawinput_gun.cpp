//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// The Windows backend of light_guns.h.
//
// Raw Input is registered once per process. SDL registers for raw mouse input
// only in relative mouse mode, which sm2-emu never uses, so a reader thread
// with its own message-only window can take the registration.

#include "osd/light_guns.h"

#include "core/log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <cfgmgr32.h>
#include <hidsdi.h>

namespace sm2::osd {
namespace {

/// A Raw Input button's transition flags and its evdev key code.
struct ButtonBit {
    USHORT down;
    USHORT up;
    u16    code;
};
constexpr std::array<ButtonBit, 5> kButtons = {{
    {RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0x110},      // BTN_LEFT
    {RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 0x111},    // BTN_RIGHT
    {RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 0x112},  // BTN_MIDDLE
    {RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 0x113},            // BTN_SIDE
    {RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 0x114},            // BTN_EXTRA
}};

[[nodiscard]] std::string narrow(const wchar_t* text)
{
    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<usize>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), len, nullptr, nullptr);
    return out;
}

/// Case-insensitive substring test.
[[nodiscard]] bool contains_ci(std::string haystack, const char* needle)
{
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(needle) != std::string::npos;
}

/// Whether a HID product name is a light gun's.
[[nodiscard]] bool is_gun_name(const std::string& name)
{
    return contains_ci(name, "gun") || contains_ci(name, "aimtrak");
}

/// The device interface path of a Raw Input device.
[[nodiscard]] std::wstring device_path(HANDLE device)
{
    UINT chars = 0;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &chars) != 0 || chars == 0) {
        return {};
    }
    std::wstring path(chars, L'\0');
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path.data(), &chars)
        == static_cast<UINT>(-1)) {
        return {};
    }
    path.resize(wcslen(path.c_str()));
    if (path.size() > 1 && path[1] == L'?') {
        path[1] = L'\\';  // the prefix can come back as \??\ instead of \\?\.
    }
    return path;
}

/// The HID product string of a device, empty if it cannot be read.
[[nodiscard]] std::string product_name(const std::wstring& path)
{
    // Windows opens mice exclusively, so ask for no access rights. The
    // product string can still be read.
    HANDLE file = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }
    wchar_t buffer[128] = {};
    std::string name;
    if (HidD_GetProductString(file, buffer, sizeof buffer - sizeof buffer[0])) {
        name = narrow(buffer);
    }
    CloseHandle(file);
    return name;
}

/// A key shared by every mouse of one physical device: the instance ID of its
/// USB device node, or the path itself if there is none.
[[nodiscard]] std::wstring physical_device_key(const std::wstring& path)
{
    // Turn the path into an instance ID: \\?\HID#a#b#{guid} becomes HID\a\b.
    if (path.size() < 4) {
        return path;
    }
    std::wstring id = path.substr(4);
    if (const usize guid = id.rfind(L"#{"); guid != std::wstring::npos) {
        id.resize(guid);
    }
    std::replace(id.begin(), id.end(), L'#', L'\\');

    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        return path;
    }
    for (int depth = 0; depth < 4; ++depth) {
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) {
            break;
        }
        node = parent;
        wchar_t buffer[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(node, buffer, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
            break;
        }
        const std::string parent_id = narrow(buffer);
        if (contains_ci(parent_id, "usb\\") && !contains_ci(parent_id, "&mi_")) {
            return buffer;
        }
    }
    return path;
}

/// One gun. The reader thread writes `state` and `last_pressed` under
/// Impl::mutex.
struct Device {
    HANDLE         handle = nullptr;
    LightGuns::Gun state;
    u16            last_pressed = 0;
    /// Buttons pressed since the last poll(), so a click shorter than a frame
    /// is still seen. One bit per kButtons entry.
    u8             pressed_since_poll = 0;
};

}  // namespace

struct LightGuns::Impl {
    std::vector<Device>         guns;
    std::vector<LightGuns::Gun> published;  ///< the state as of the last poll().
    std::mutex                  mutex;
    std::thread                 reader;
    HANDLE                      stop = nullptr;

    void run(HANDLE ready, bool* registered);
    void read_input(HRAWINPUT input);
};

LightGuns::LightGuns() : m_impl(std::make_unique<Impl>()) {}

LightGuns::~LightGuns()
{
    shutdown();
}

usize LightGuns::count() const
{
    return m_impl->published.size();
}

const LightGuns::Gun& LightGuns::gun(usize index) const
{
    return m_impl->published[index];
}

bool LightGuns::init()
{
    UINT total = 0;
    if (GetRawInputDeviceList(nullptr, &total, sizeof(RAWINPUTDEVICELIST)) != 0) {
        SM2_WARN("rawinput: cannot list devices; light guns via Raw Input unavailable");
        return false;
    }
    std::vector<RAWINPUTDEVICELIST> list(total);
    if (total > 0) {
        total = GetRawInputDeviceList(list.data(), &total, sizeof(RAWINPUTDEVICELIST));
        if (total == static_cast<UINT>(-1)) {
            SM2_WARN("rawinput: cannot list devices; light guns via Raw Input unavailable");
            return false;
        }
        list.resize(total);
    }

    // A gun can present more than one mouse, so keep one per physical device.
    // Sorting by path keeps the player numbering stable across runs.
    struct Candidate {
        std::wstring path;
        std::wstring phys;
        std::string  name;
        HANDLE       handle = nullptr;
        int          prio   = 1;
    };
    std::vector<Candidate> candidates;
    for (const RAWINPUTDEVICELIST& entry : list) {
        if (entry.dwType != RIM_TYPEMOUSE) {
            continue;
        }
        std::wstring path = device_path(entry.hDevice);
        if (path.empty()) {
            continue;
        }
        std::string name = product_name(path);
        // A device pinned with gun1_device / gun2_device is a gun whatever its
        // product string (a Wiimote adapter, a gun without "gun" in its name).
        const bool pinned = preferred_rank(m_preferred, narrow(path.c_str())) < 2;
        if (!is_gun_name(name) && !pinned) {
            continue;
        }
        const int prio = contains_ci(name, "mouse") ? 1 : 0;
        std::wstring phys = physical_device_key(path);
        candidates.push_back({std::move(path), std::move(phys), std::move(name),
                              entry.hDevice, prio});
    }
    std::vector<Candidate> chosen;
    for (Candidate& c : candidates) {
        const auto same = std::find_if(chosen.begin(), chosen.end(),
                                       [&](const Candidate& o) { return o.phys == c.phys; });
        if (same == chosen.end()) {
            chosen.push_back(std::move(c));
        } else if (c.prio < same->prio || (c.prio == same->prio && c.path < same->path)) {
            *same = std::move(c);
        }
    }
    std::sort(chosen.begin(), chosen.end(),
              [](const Candidate& a, const Candidate& b) { return a.path < b.path; });

    // Pinned guns (gun1_device / gun2_device) first, the others keep the path order.
    std::stable_sort(chosen.begin(), chosen.end(), [this](const Candidate& a, const Candidate& b) {
        return preferred_rank(m_preferred, narrow(a.path.c_str()))
            < preferred_rank(m_preferred, narrow(b.path.c_str()));
        });

    for (const Candidate& c : chosen) {
        if (m_impl->guns.size() >= kMaxGuns) {
            break;
        }
        Device device;
        device.handle     = c.handle;
        device.state.name = c.name;
        for (const ButtonBit& b : kButtons) {
            device.state.pressed[b.code] = false;  // fixed key set: no later allocation
        }
        m_impl->guns.push_back(device);
    }
    if (m_impl->guns.empty()) {
        return true;
    }

    m_impl->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool registered = false;
    if (m_impl->stop != nullptr && ready != nullptr) {
        m_impl->reader = std::thread([this, ready, &registered] {
            m_impl->run(ready, &registered);
        });
        WaitForSingleObject(ready, INFINITE);
    }
    if (ready != nullptr) {
        CloseHandle(ready);
    }
    if (!registered) {
        SM2_WARN("rawinput: cannot register for mouse input; light guns unavailable");
        shutdown();
        return true;
    }

    for (const Device& device : m_impl->guns) {
        m_impl->published.push_back(device.state);
    }
    SM2_INFO("rawinput: opened %zu light gun(s)", m_impl->guns.size());
    return true;
}

void LightGuns::Impl::run(HANDLE ready, bool* registered)
{
    HWND window = CreateWindowExW(0, L"Message", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE,
                                  nullptr, nullptr, nullptr);
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // generic desktop
    rid.usUsage     = 0x02;  // mouse
    // Keep receiving input when focus moves, so a button release is not lost
    // and left held.
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = window;
    *registered = window != nullptr && RegisterRawInputDevices(&rid, 1, sizeof rid);
    const bool ok = *registered;
    SetEvent(ready);  // `registered` belongs to init() and is not valid after this.
    if (!ok) {
        if (window != nullptr) {
            DestroyWindow(window);
        }
        return;
    }

    while (MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_RAWINPUT)
           == WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, window, WM_INPUT, WM_INPUT, PM_REMOVE)) {
            read_input(reinterpret_cast<HRAWINPUT>(msg.lParam));
            DefWindowProcW(window, msg.message, msg.wParam, msg.lParam);
        }
    }

    rid.dwFlags    = RIDEV_REMOVE;
    rid.hwndTarget = nullptr;
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    DestroyWindow(window);
}

void LightGuns::Impl::read_input(HRAWINPUT input)
{
    RAWINPUT raw{};
    UINT size = sizeof raw;
    if (GetRawInputData(input, RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER))
            == static_cast<UINT>(-1)
        || raw.header.dwType != RIM_TYPEMOUSE) {
        return;
    }
    const auto it = std::find_if(guns.begin(), guns.end(), [&](const Device& d) {
        return d.handle == raw.header.hDevice;
    });
    if (it == guns.end()) {
        return;
    }
    const RAWMOUSE& mouse = raw.data.mouse;

    // An absolute position is 0..65535 across the primary monitor, or across
    // the whole virtual desktop when flagged. In the second case, rescale it
    // to the monitor it lands on. Relative motion is ignored.
    float x = -1.0f;
    float y = -1.0f;
    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) {
        x = static_cast<float>(mouse.lLastX) / 65535.0f;
        y = static_cast<float>(mouse.lLastY) / 65535.0f;
        if ((mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0) {
            const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            const POINT pt{vx + static_cast<LONG>(x * static_cast<float>(vw - 1)),
                           vy + static_cast<LONG>(y * static_cast<float>(vh - 1))};
            MONITORINFO info{};
            info.cbSize = sizeof info;
            if (GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &info)) {
                const RECT& r = info.rcMonitor;
                x = static_cast<float>(pt.x - r.left) / static_cast<float>(r.right - r.left);
                y = static_cast<float>(pt.y - r.top) / static_cast<float>(r.bottom - r.top);
            }
        }
    }

    std::lock_guard lock(mutex);
    Device& device = *it;
    if (x >= 0.0f) {
        device.state.x = std::clamp(x, 0.0f, 1.0f);
        device.state.y = std::clamp(y, 0.0f, 1.0f);
    }
    for (usize i = 0; i < kButtons.size(); ++i) {
        const ButtonBit& b = kButtons[i];
        if ((mouse.usButtonFlags & b.down) != 0) {
            if (!device.state.pressed[b.code]) {
                device.last_pressed = b.code;  // rising edge
            }
            device.state.pressed[b.code] = true;
            device.pressed_since_poll |= static_cast<u8>(1u << i);
        }
        if ((mouse.usButtonFlags & b.up) != 0) {
            device.state.pressed[b.code] = false;
        }
    }
}

void LightGuns::poll()
{
    // Copy only what changes, so the name and button map are never reallocated.
    std::lock_guard lock(m_impl->mutex);
    for (usize i = 0; i < m_impl->published.size(); ++i) {
        LightGuns::Gun& out    = m_impl->published[i];
        Device&         device = m_impl->guns[i];
        out.x = device.state.x;
        out.y = device.state.y;
        for (usize b = 0; b < kButtons.size(); ++b) {
            const u16 code = kButtons[b].code;
            out.pressed[code] = device.state.pressed.at(code)
                                || (device.pressed_since_poll & (1u << b)) != 0;
        }
        device.pressed_since_poll = 0;
    }
}

bool LightGuns::has_recoil(usize) const
{
    return false;
}

void LightGuns::fire_recoil(usize, u32) {}

u16 LightGuns::take_last_pressed(usize index)
{
    std::lock_guard lock(m_impl->mutex);
    if (index >= m_impl->guns.size()) {
        return 0;
    }
    const u16 code = m_impl->guns[index].last_pressed;
    m_impl->guns[index].last_pressed = 0;
    return code;
}

void LightGuns::shutdown()
{
    if (m_impl->reader.joinable()) {
        SetEvent(m_impl->stop);
        m_impl->reader.join();
    }
    if (m_impl->stop != nullptr) {
        CloseHandle(m_impl->stop);
        m_impl->stop = nullptr;
    }
    m_impl->guns.clear();
    m_impl->published.clear();
}

}  // namespace sm2::osd
