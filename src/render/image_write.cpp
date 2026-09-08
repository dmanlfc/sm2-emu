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

#include "render/image_write.h"

#include "core/log.h"

#include <miniz.h>

#include <cstdio>

namespace sm2::render {

bool write_png_rgb(const std::string& path, u32 width, u32 height, const u8* rgb)
{
    if (rgb == nullptr || width == 0 || height == 0) {
        SM2_ERROR("png write: nothing to write");
        return false;
    }

    std::size_t png_size = 0;
    void*       png      = tdefl_write_image_to_png_file_in_memory(
        rgb, static_cast<int>(width), static_cast<int>(height), 3, &png_size);
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
