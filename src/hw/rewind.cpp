//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#include "hw/rewind.h"

#include "hw/model2_machine_base.h"

#include <algorithm>
#include <cstring>

namespace sm2::hw {
namespace {

/// Granularity of the comparison; memcmp on blocks this size is close to
/// memory bandwidth.
constexpr usize kBlock = 64;

void put_u32(std::vector<u8>& out, u32 value)
{
    const usize at = out.size();
    out.resize(at + sizeof(value));
    std::memcpy(out.data() + at, &value, sizeof(value));
}

u32 get_u32(const u8* data)
{
    u32 value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

}  // namespace

std::vector<u8> rewind_delta(const std::vector<u8>& newer, const std::vector<u8>& older)
{
    std::vector<u8> out;
    put_u32(out, static_cast<u32>(older.size()));

    const usize shared = std::min(newer.size(), older.size());
    usize       offset = 0;
    while (offset < older.size()) {
        // Skip what matches.
        while (offset < shared) {
            const usize length = std::min(kBlock, shared - offset);
            if (std::memcmp(newer.data() + offset, older.data() + offset, length) != 0) {
                break;
            }
            offset += length;
        }
        if (offset >= older.size()) {
            break;
        }
        // Then one run of what does not; past the end of `newer` all of it.
        usize end = offset;
        while (end < older.size()) {
            if (end >= shared) {
                end = older.size();
                break;
            }
            const usize length = std::min(kBlock, shared - end);
            if (std::memcmp(newer.data() + end, older.data() + end, length) == 0) {
                break;
            }
            end += length;
        }
        put_u32(out, static_cast<u32>(offset));
        put_u32(out, static_cast<u32>(end - offset));
        out.insert(out.end(), older.begin() + static_cast<std::ptrdiff_t>(offset),
                   older.begin() + static_cast<std::ptrdiff_t>(end));
        offset = end;
    }
    return out;
}

void apply_rewind_delta(std::vector<u8>& state, const std::vector<u8>& delta)
{
    state.resize(get_u32(delta.data()));
    usize at = sizeof(u32);
    while (at + 2 * sizeof(u32) <= delta.size()) {
        const u32 offset = get_u32(delta.data() + at);
        const u32 length = get_u32(delta.data() + at + sizeof(u32));
        at += 2 * sizeof(u32);
        std::memcpy(state.data() + offset, delta.data() + at, length);
        at += length;
    }
}

void Rewind::configure(usize budget_bytes, u32 interval_frames)
{
    interval_frames = std::max<u32>(interval_frames, 1);
    if (budget_bytes == m_budget && interval_frames == m_interval) {
        return;
    }
    m_budget   = budget_bytes;
    m_interval = interval_frames;
    clear();
}

void Rewind::clear()
{
    m_current.clear();
    m_current.shrink_to_fit();
    m_deltas.clear();
    m_delta_bytes = 0;
    m_counter     = 0;
}

void Rewind::capture(const Model2MachineBase& machine)
{
    if (m_budget == 0 || ++m_counter < m_interval) {
        return;
    }
    m_counter = 0;
    if (!machine.save_state_data(m_scratch)) {
        return;
    }
    if (!m_current.empty()) {
        std::vector<u8> delta = rewind_delta(m_scratch, m_current);
        m_delta_bytes += delta.size();
        m_deltas.push_back(std::move(delta));
    }
    m_current.swap(m_scratch);

    while (!m_deltas.empty() && bytes_used() > m_budget) {
        m_delta_bytes -= m_deltas.front().size();
        m_deltas.pop_front();
    }
}

bool Rewind::step_back(Model2MachineBase& machine)
{
    if (m_current.empty() || !machine.load_state_data(m_current)) {
        return false;
    }
    if (!m_deltas.empty()) {
        apply_rewind_delta(m_current, m_deltas.back());
        m_delta_bytes -= m_deltas.back().size();
        m_deltas.pop_back();
    }
    m_counter = 0;
    return true;
}

}  // namespace sm2::hw
