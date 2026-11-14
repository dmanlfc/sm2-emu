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

#include "render/model3/texture_key.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sm2::render::model3 {
namespace {

constexpr usize kMaxRegions = 1u << 16;

[[nodiscard]] u64 mix(u64 hash, u64 value)
{
    hash = (hash ^ value) * 0x9e3779b97f4a7c15ull;
    return hash ^ (hash >> 29);
}

[[nodiscard]] u64 region_key(const TextureRef& r)
{
    return static_cast<u64>(r.page & 1) | (static_cast<u64>(r.x & 2047) << 1) | (static_cast<u64>(r.y & 1023) << 12)
           | (static_cast<u64>(r.width) << 22) | (static_cast<u64>(r.height) << 34) | (static_cast<u64>(r.format) << 46);
}

}  // namespace

TextureRef texture_ref(const R3DUniforms& u)
{
    TextureRef r;
    r.page   = static_cast<u32>(u.texturePage) & 1u;
    r.x      = static_cast<u32>(u.baseTexInfo[0]) & 2047u;
    r.y      = static_cast<u32>(u.baseTexInfo[1]) & 1023u;
    r.width  = static_cast<u32>(std::clamp(u.baseTexInfo[2], 1, 2048));
    r.height = static_cast<u32>(std::clamp(u.baseTexInfo[3], 1, 1024));
    r.format = static_cast<u32>(std::clamp(u.baseTexType, 0, 15));
    return r;
}

void decode_rgba(const TextureSheets& sheets, const TextureRef& ref, bool keep_alpha, std::vector<u8>* out)
{
    out->resize(static_cast<usize>(ref.width) * ref.height * 4);
    u8* p = out->data();
    for (u32 y = 0; y < ref.height; y++) {
        for (u32 x = 0; x < ref.width; x++) {
            const std::array<float, 4> c = decode_texel(static_cast<int>(ref.format), sheets.texel(ref.page, ref.x + x, ref.y + y));
            for (int i = 0; i < 4; i++) {
                *p++ = static_cast<u8>(std::lround(std::clamp(c[static_cast<usize>(i)], 0.0f, 1.0f) * 255.0f));
            }
            if (!keep_alpha) {
                p[-1] = 0xff;
            }
        }
    }
}

u64 TextureHasher::lookup(const TextureSheets& sheets, const TextureRef& ref)
{
    if (m_epoch != sheets.epoch() || m_regions.size() > kMaxRegions) {
        m_epoch = sheets.epoch();
        m_regions.clear();
    }
    auto [slot, inserted] = m_regions.try_emplace(region_key(ref));
    if (!inserted && sheets.region_stamp(ref.page, ref.x, ref.y, ref.width, ref.height) <= slot->second.stamp) {
        return slot->second.hash;
    }

    u64 hash = mix(0xcbf29ce484222325ull, (static_cast<u64>(ref.format) << 32) | (ref.width << 16) | ref.height);
    const std::span<const u16> texels = sheets.texels();
    std::vector<u16> row(ref.width);
    for (u32 y = 0; y < ref.height; y++) {
        const usize base = (static_cast<usize>(ref.page) * TextureSheets::kPageHeight + ((ref.y + y) & 1023u))
                           * TextureSheets::kSize;
        const u32 first = std::min(ref.width, TextureSheets::kPageWidth - ref.x);
        std::memcpy(row.data(), &texels[base + ref.x], first * sizeof(u16));
        if (first < ref.width) {
            std::memcpy(row.data() + first, &texels[base], (ref.width - first) * sizeof(u16));
        }
        u32 x = 0;
        for (; x + 4 <= ref.width; x += 4) {
            u64 v = 0;
            std::memcpy(&v, &row[x], sizeof(v));
            hash = mix(hash, v);
        }
        for (; x < ref.width; x++) {
            hash = mix(hash, row[x]);
        }
    }
    slot->second.hash  = hash;
    slot->second.stamp = sheets.stamp();
    return hash;
}

}  // namespace sm2::render::model3
