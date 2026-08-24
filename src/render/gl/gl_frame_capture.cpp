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
#include "render/gl/gl_frame_capture.h"

#include "core/log.h"
#include "render/gl/gl_common.h"

#include <cstdio>

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

    std::FILE* handle = std::fopen(path.c_str(), "wb");
    if (handle == nullptr) {
        SM2_ERROR("frame capture: could not write '%s'", path.c_str());
        return false;
    }
    std::fprintf(handle, "P6\n%u %u\n255\n", m_width, m_height);

    // No row flip: confirmed empirically (not assumed from general
    // Vulkan/GL Y-convention folklore, which is a genuine trap -- see
    // design.md's own account of checking this directly before writing
    // this code) that with this project's exact fullscreen_quad.vert
    // sampling logic, a buffer's row 0 lands at output row 0 through the
    // whole TexSubImage2D-upload -> sample -> ReadPixels-readback chain, no
    // different from render::vk::FrameCapture's own unflipped row order.
    std::vector<u8> row(static_cast<usize>(m_width) * 3);
    bool ok = true;
    for (u32 y = 0; y < m_height && ok; ++y) {
        const u8* line = m_pixels.data() + static_cast<usize>(y) * m_width * 4;
        for (u32 x = 0; x < m_width; ++x) {
            const u8* pixel = line + static_cast<usize>(x) * 4;
            row[static_cast<usize>(x) * 3 + 0] = pixel[0];
            row[static_cast<usize>(x) * 3 + 1] = pixel[1];
            row[static_cast<usize>(x) * 3 + 2] = pixel[2];
        }
        ok = std::fwrite(row.data(), 1, row.size(), handle) == row.size();
    }
    std::fclose(handle);

    if (ok) {
        SM2_INFO("captured %ux%u frame to %s", m_width, m_height, path.c_str());
    }
    return ok;
}

}  // namespace sm2::render::gl
