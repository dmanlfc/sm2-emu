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
#include "render/gl/gl_frame_capture.h"

#include "core/log.h"
#include "render/gl/gl_common.h"
#include "render/image_write.h"

#include <vector>

namespace sm2::render::gl {

bool FrameCapture::record(u32 width, u32 height)
{
    m_valid = false;
    if (width == 0 || height == 0) {
        return false;
    }

    m_pixels.assign(static_cast<usize>(width) * height * 4, 0);
    ReadBuffer(GL_COLOR_ATTACHMENT0);
    ReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA,
              GL_UNSIGNED_BYTE, m_pixels.data());

    m_width  = width;
    m_height = height;
    m_valid  = true;
    return true;
}

bool FrameCapture::save(const std::string& path) const
{
    if (!m_valid) {
        SM2_ERROR("frame capture: nothing was captured");
        return false;
    }

    // No row flip here: fullscreen_quad.vert's SM2_TARGET_GL clip-Y flip makes
    // the FBO's row 0 the top of the image, matching render::vk::FrameCapture.
    std::vector<u8> rgb(static_cast<usize>(m_width) * m_height * 3);
    for (u32 y = 0; y < m_height; ++y) {
        const u8* line = m_pixels.data() + static_cast<usize>(y) * m_width * 4;
        u8*       out  = rgb.data() + static_cast<usize>(y) * m_width * 3;
        for (u32 x = 0; x < m_width; ++x) {
            const u8* pixel = line + static_cast<usize>(x) * 4;
            out[static_cast<usize>(x) * 3 + 0] = pixel[0];
            out[static_cast<usize>(x) * 3 + 1] = pixel[1];
            out[static_cast<usize>(x) * 3 + 2] = pixel[2];
        }
    }

    if (!write_png_rgb(path, m_width, m_height, rgb.data())) {
        return false;
    }
    SM2_INFO("captured %ux%u frame to %s", m_width, m_height, path.c_str());
    return true;
}

}  // namespace sm2::render::gl
