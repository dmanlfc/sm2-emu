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

#include "render/image_write.h"

#include "core/log.h"

#include <miniz.h>

#include <cstdio>

namespace sm2::render {

bool write_png_rgb(const std::string& path, u32 width, u32 height, const u8* rgb)
{
    return write_png(path, width, height, 3, rgb);
}

bool write_png(const std::string& path, u32 width, u32 height, u32 channels, const u8* pixels)
{
    if (pixels == nullptr || width == 0 || height == 0) {
        SM2_ERROR("png write: nothing to write");
        return false;
    }

    std::size_t png_size = 0;
    void*       png      = tdefl_write_image_to_png_file_in_memory(
        pixels, static_cast<int>(width), static_cast<int>(height), static_cast<int>(channels),
        &png_size);
    if (png == nullptr) {
        SM2_ERROR("png write: encode failed");
        return false;
    }

    std::FILE* handle = std::fopen(path.c_str(), "wb");
    if (handle == nullptr) {
        SM2_ERROR("png write: could not open '%s'", path.c_str());
        mz_free(png);
        return false;
    }
    const bool ok = std::fwrite(png, 1, png_size, handle) == png_size;
    std::fclose(handle);
    mz_free(png);

    if (!ok) {
        SM2_ERROR("png write: could not write '%s'", path.c_str());
    }
    return ok;
}

}  // namespace sm2::render
