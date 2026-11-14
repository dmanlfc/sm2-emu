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
// Names a Model 3 texture by the content of its level-0 texels and format, so
// dumps and custom textures match wherever the game uploaded it.
#pragma once

#include "core/types.h"
#include "render/model3/frame_plan.h"
#include "render/model3/textures.h"

#include <unordered_map>
#include <vector>

namespace sm2::render::model3 {

struct TextureRef {
    u32 page   = 0;
    u32 x      = 0;
    u32 y      = 0;
    u32 width  = 0;
    u32 height = 0;
    u32 format = 0;
};

[[nodiscard]] TextureRef texture_ref(const R3DUniforms& u);

/// The texture as RGBA8 rows, top row first, through decode_texel(). Alpha
/// is forced opaque unless `keep_alpha`.
void decode_rgba(const TextureSheets& sheets, const TextureRef& ref, bool keep_alpha, std::vector<u8>* out);

class TextureHasher {
public:
    /// Content hash of `ref`, cached until a write reaches its texels.
    u64 lookup(const TextureSheets& sheets, const TextureRef& ref);

private:
    struct Entry {
        u64 hash  = 0;
        u32 stamp = 0;
    };
    u64                            m_epoch = 0;
    std::unordered_map<u64, Entry> m_regions;
};

}  // namespace sm2::render::model3
