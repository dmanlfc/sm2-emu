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
// Reads a rendered image back off the GPU and writes it as a PPM; the GL
// counterpart of render::vk::FrameCapture.
#pragma once

#include "core/types.h"

#include <string>
#include <vector>

namespace sm2::render::gl {

class FrameCapture {
public:
    FrameCapture() = default;

    FrameCapture(const FrameCapture&)            = delete;
    FrameCapture& operator=(const FrameCapture&) = delete;

    /// Read `width` by `height` RGBA8 pixels from the bound framebuffer's colour
    /// attachment, normally PresentPass's native FBO once the frame is drawn.
    [[nodiscard]] bool record(u32 width, u32 height);

    /// Write what record() captured.
    [[nodiscard]] bool save(const std::string& path) const;

private:
    std::vector<u8> m_pixels;
    u32             m_width  = 0;
    u32             m_height = 0;
    bool            m_valid  = false;
};

}  // namespace sm2::render::gl
