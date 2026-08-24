//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
// Reads a rendered image back off the GPU and writes it as a PPM. GL
// analogue of render::vk::FrameCapture -- see that class's own doc comment
// for why this exists (an unattended, diffable "does it look right?").
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

    /// Read `width` by `height` RGBA8 pixels from the currently bound
    /// framebuffer's colour attachment. The caller binds the FBO to capture
    /// (ordinarily PresentPass's native FBO, after that frame's drawing is
    /// complete) before calling this -- mirroring
    /// render::vk::FrameCapture::record()'s own "captures what was
    /// rendered, not a re-render" contract, just via a bound framebuffer
    /// instead of a named image.
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
