// ██╗  ██╗███████╗██╗███████╗██╗  ██╗ ██████╗
// ██║ ██╔╝██╔════╝██║██╔════╝██║  ██║██╔═══██╗
// █████╔╝ █████╗  ██║███████╗███████║██║   ██║
// ██╔═██╗ ██╔══╝  ██║╚════██║██╔══██║██║   ██║
// ██║  ██╗███████╗██║███████║██║  ██║╚██████╔╝
// ╚═╝  ╚═╝╚══════╝╚═╝╚══════╝╚═╝  ╚═╝ ╚═════╝
//            継 承  ( k e i s h ō )
//
// A Sega Model 1, 2 & 3 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
// Real3D line-of-sight results, measured by the renderer and handed back to
// the machine a frame or more later.
#pragma once

#include "core/types.h"

#include <array>
#include <mutex>

namespace sm2::render::model3 {

// range[i]: the float Real3D register 20 + 4 * i reads back; 0 if not sampled.
struct LineOfSight {
    u64 frame_number = 0;
    std::array<float, 4> range{};
};

// Written on the render thread, read from any.
class LineOfSightMailbox {
public:
    void publish(const LineOfSight& value)
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        m_value = value;
    }
    [[nodiscard]] LineOfSight latest() const
    {
        const std::lock_guard<std::mutex> lock(m_mutex);
        return m_value;
    }

private:
    mutable std::mutex m_mutex;
    LineOfSight        m_value;
};

}  // namespace sm2::render::model3
