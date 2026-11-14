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

#include "render/texture_replace.h"

#include "core/log.h"
#include "hw/geometrizer.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_video.h"

#include <stb_image.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace sm2::render {
namespace {

/// Wrapped copy of each image around it, so filtering and the first mip levels
/// at an edge read the image's own repeat rather than a neighbour.
constexpr u32 kGutter = 16;
/// Placement granularity, so mip levels stay aligned to entry boundaries.
constexpr u32 kAlign = 32;

constexpr std::array<const char*, 8> kOrientationTags = {"",  "t",    "fx",   "r90",
                                                         "fy", "r270", "r180", "at"};

struct Image {
    std::string     path;
    u64             hash        = 0;
    u32             colour_base = 0;
    u32             luma_table  = 0;
    u32             luma        = 0;
    u32             orientation = 0;
    bool            direct      = false;
    u32             width       = 0;
    u32             height      = 0;
    std::vector<u8> rgba;
};

[[nodiscard]] bool parse_hex(const std::string& text, u64* out)
{
    if (text.empty() || text.size() > 16) {
        return false;
    }
    u64 value = 0;
    for (const char c : text) {
        u64 digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<u64>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<u64>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<u64>(c - 'A' + 10);
        } else {
            return false;
        }
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

[[nodiscard]] bool parse_name(const std::string& stem, Image* out)
{
    if (stem.size() == 4 + 16 && stem.rfind("tex_", 0) == 0) {
        out->direct = true;
        return parse_hex(stem.substr(4), &out->hash);
    }
    // tex_<16 hex>_<7 hex>[_<orientation>]
    if (stem.rfind("tex_", 0) != 0 || stem.size() < 4 + 16 + 1 + 7) {
        return false;
    }
    u64 hash   = 0;
    u64 colour = 0;
    if (!parse_hex(stem.substr(4, 16), &hash) || stem[20] != '_'
        || !parse_hex(stem.substr(21, 7), &colour)) {
        return false;
    }
    u32 orientation = 0;
    if (stem.size() > 28) {
        if (stem[28] != '_') {
            return false;
        }
        const std::string tag = stem.substr(29);
        const auto found = std::find(kOrientationTags.begin(), kOrientationTags.end(), tag);
        if (found == kOrientationTags.end() || tag.empty()) {
            return false;
        }
        orientation = static_cast<u32>(found - kOrientationTags.begin());
    }
    out->hash        = hash;
    out->colour_base = static_cast<u32>(colour >> 16) & 0x3ffu;
    out->luma_table  = static_cast<u32>(colour >> 8) & 0xffu;
    out->luma        = static_cast<u32>(colour) & 0xffu;
    out->orientation = orientation;
    return true;
}

/// The dump wrote stored texel (u,v) at displayed (x,y); read it back the same
/// way to recover the stored layout the polygon's texture points address.
void to_stored(Image* image)
{
    const bool transpose = (image->orientation & 1u) != 0;
    const u32  dw = image->width;
    const u32  dh = image->height;
    const u32  sw = transpose ? dh : dw;
    const u32  sh = transpose ? dw : dh;
    std::vector<u8> stored(image->rgba.size());
    for (u32 v = 0; v < sh; ++v) {
        for (u32 u = 0; u < sw; ++u) {
            u32 x = transpose ? v : u;
            u32 y = transpose ? u : v;
            if ((image->orientation & 2u) != 0) {
                x = dw - 1 - x;
            }
            if ((image->orientation & 4u) != 0) {
                y = dh - 1 - y;
            }
            std::memcpy(&stored[(static_cast<usize>(v) * sw + u) * 4],
                        &image->rgba[(static_cast<usize>(y) * dw + x) * 4], 4);
        }
    }
    image->rgba   = std::move(stored);
    image->width  = sw;
    image->height = sh;
}

/// Give see-through texels the colour of their opaque neighbours, so filtering
/// and mip levels do not pull in whatever colour they happened to hold.
void bleed_transparent(Image* image)
{
    const u32 w = image->width;
    const u32 h = image->height;
    std::vector<u8>& px = image->rgba;
    std::vector<u8>  known(static_cast<usize>(w) * h);
    bool any_hidden = false;
    bool any_known  = false;
    for (usize i = 0; i < known.size(); ++i) {
        known[i] = px[i * 4 + 3] != 0 ? 1 : 0;
        any_hidden = any_hidden || known[i] == 0;
        any_known  = any_known || known[i] != 0;
    }
    if (!any_hidden || !any_known) {
        return;
    }
    std::vector<usize> frontier;
    for (u32 pass = 0; pass < 16; ++pass) {
        frontier.clear();
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const usize at = static_cast<usize>(y) * w + x;
                if (known[at] != 0) {
                    continue;
                }
                u32 sum[3] = {0, 0, 0};
                u32 n      = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const u32 nx = (x + w + static_cast<u32>(dx)) % w;
                        const u32 ny = (y + h + static_cast<u32>(dy)) % h;
                        const usize from = static_cast<usize>(ny) * w + nx;
                        if (known[from] == 1) {
                            for (u32 c = 0; c < 3; ++c) {
                                sum[c] += px[from * 4 + c];
                            }
                            ++n;
                        }
                    }
                }
                if (n != 0) {
                    for (u32 c = 0; c < 3; ++c) {
                        px[at * 4 + c] = static_cast<u8>((sum[c] + n / 2) / n);
                    }
                    frontier.push_back(at);
                }
            }
        }
        if (frontier.empty()) {
            break;
        }
        for (const usize at : frontier) {
            known[at] = 1;
        }
    }
    u64 sum[3] = {0, 0, 0};
    u64 n      = 0;
    for (usize i = 0; i < known.size(); ++i) {
        if (px[i * 4 + 3] != 0) {
            for (u32 c = 0; c < 3; ++c) {
                sum[c] += px[i * 4 + c];
            }
            ++n;
        }
    }
    for (usize i = 0; i < known.size(); ++i) {
        if (known[i] == 0) {
            for (u32 c = 0; c < 3; ++c) {
                px[i * 4 + c] = static_cast<u8>(sum[c] / n);
            }
        }
    }
}

void halve(Image* image)
{
    const u32 w = std::max(image->width / 2, 1u);
    const u32 h = std::max(image->height / 2, 1u);
    std::vector<u8> out(static_cast<usize>(w) * h * 4);
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            for (u32 c = 0; c < 4; ++c) {
                u32 sum = 0;
                for (u32 k = 0; k < 4; ++k) {
                    const u32 sx = std::min(x * 2 + (k & 1u), image->width - 1);
                    const u32 sy = std::min(y * 2 + (k >> 1), image->height - 1);
                    sum += image->rgba[(static_cast<usize>(sy) * image->width + sx) * 4 + c];
                }
                out[(static_cast<usize>(y) * w + x) * 4 + c] = static_cast<u8>((sum + 2) / 4);
            }
        }
    }
    image->rgba   = std::move(out);
    image->width  = w;
    image->height = h;
}

[[nodiscard]] u32 round_up(u32 value, u32 step)
{
    return (value + step - 1) / step * step;
}

[[nodiscard]] u32 pack_tint(float ratio)
{
    return static_cast<u32>(std::clamp(ratio * 512.0F + 0.5F, 0.0F, 1023.0F));
}

}  // namespace

const char* orientation_tag(u32 orientation)
{
    return kOrientationTags[orientation & 7u];
}

std::string replacement_file_name(u64 hash)
{
    char name[32];
    std::snprintf(name, sizeof(name), "tex_%016llx.png", static_cast<unsigned long long>(hash));
    return name;
}

std::string replacement_file_name(u64 hash, u32 colour_base, u32 luma_table, u32 luma,
                                  u32 orientation)
{
    char name[64];
    std::snprintf(name, sizeof(name), "tex_%016llx_%03x%02x%02x",
                  static_cast<unsigned long long>(hash), colour_base & 0x3ffu, luma_table & 0xffu,
                  luma & 0xffu);
    std::string out = name;
    if ((orientation & 7u) != 0) {
        out += std::string("_") + orientation_tag(orientation);
    }
    return out + ".png";
}

u32 TextureReplacements::mip_levels()
{
    return static_cast<u32>(std::bit_width(kReplacementLayerSize));
}

void TextureReplacements::release_pixels()
{
    for (std::vector<u8>& layer : m_layers) {
        layer.clear();
        layer.shrink_to_fit();
    }
}

usize TextureReplacements::load(const std::string& directory)
{
    const auto start = std::chrono::steady_clock::now();
    m_entries.clear();
    m_by_hash.clear();
    m_layers.clear();
    ++m_generation;

    std::vector<Image> images;
    std::error_code    error;
    if (!std::filesystem::is_directory(directory, error)) {
        return 0;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(directory, error);
         !error && it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
        if (!it->is_regular_file(error)) {
            continue;
        }
        std::string extension = it->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".png") {
            continue;
        }
        Image image;
        if (!parse_name(it->path().stem().string(), &image)) {
            SM2_WARN("custom textures: skipping '%s', not a dumped texture name",
                     it->path().filename().string().c_str());
            continue;
        }
        image.path = it->path().string();
        images.push_back(std::move(image));
    }
    if (images.empty()) {
        return 0;
    }

    std::atomic<usize> next{0};
    const auto decode = [&] {
        for (usize i = next++; i < images.size(); i = next++) {
            Image& image = images[i];
            int w = 0;
            int h = 0;
            int channels = 0;
            stbi_uc* data = stbi_load(image.path.c_str(), &w, &h, &channels, 4);
            if (data == nullptr) {
                continue;
            }
            image.width  = static_cast<u32>(w);
            image.height = static_cast<u32>(h);
            image.rgba.assign(data, data + static_cast<usize>(w) * static_cast<usize>(h) * 4);
            stbi_image_free(data);
            to_stored(&image);
            if (image.direct) {
                bleed_transparent(&image);
            }
            while (image.width + 2 * kGutter > kReplacementLayerSize
                   || image.height + 2 * kGutter > kReplacementLayerSize) {
                halve(&image);
            }
        }
    };
    const u32 workers = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
    std::vector<std::thread> pool;
    for (u32 i = 1; i < workers; ++i) {
        pool.emplace_back(decode);
    }
    decode();
    for (std::thread& thread : pool) {
        thread.join();
    }

    std::erase_if(images, [](const Image& image) {
        if (image.rgba.empty()) {
            SM2_WARN("custom textures: could not decode '%s'", image.path.c_str());
            return true;
        }
        return false;
    });

    // Shelf packing, tallest first.
    std::vector<usize> order(images.size());
    for (usize i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&](usize a, usize b) { return images[a].height > images[b].height; });

    u32 shelf_x = 0;
    u32 shelf_y = 0;
    u32 shelf_h = 0;
    for (const usize index : order) {
        const Image& image = images[index];
        const u32 cell_w = round_up(image.width + 2 * kGutter, kAlign);
        const u32 cell_h = round_up(image.height + 2 * kGutter, kAlign);
        if (m_layers.empty() || shelf_x + cell_w > kReplacementLayerSize) {
            shelf_x = 0;
            shelf_y += shelf_h;
            shelf_h = 0;
        }
        if (m_layers.empty() || shelf_y + cell_h > kReplacementLayerSize) {
            m_layers.emplace_back(static_cast<usize>(kReplacementLayerSize)
                                  * kReplacementLayerSize * 4, u8{0});
            shelf_x = 0;
            shelf_y = 0;
            shelf_h = 0;
        }

        Entry entry;
        entry.hash        = image.hash;
        entry.colour_base = image.colour_base;
        entry.luma_table  = image.luma_table;
        entry.luma        = image.luma;
        entry.layer       = static_cast<u32>(m_layers.size() - 1);
        entry.x           = shelf_x + kGutter;
        entry.y           = shelf_y + kGutter;
        entry.width       = image.width;
        entry.height      = image.height;
        entry.max_lod =
            static_cast<u32>(std::bit_width(std::min(image.width, image.height))) - 1;

        std::vector<u8>& layer = m_layers.back();
        const s32 w = static_cast<s32>(image.width);
        const s32 h = static_cast<s32>(image.height);
        for (s32 gy = -static_cast<s32>(kGutter); gy < h + static_cast<s32>(kGutter); ++gy) {
            const s32 sy = ((gy % h) + h) % h;
            const usize row = static_cast<usize>(static_cast<s32>(entry.y) + gy)
                            * kReplacementLayerSize;
            for (s32 gx = -static_cast<s32>(kGutter); gx < w + static_cast<s32>(kGutter);
                 ++gx) {
                const s32 sx = ((gx % w) + w) % w;
                std::memcpy(&layer[(row + static_cast<usize>(static_cast<s32>(entry.x) + gx)) * 4],
                            &image.rgba[(static_cast<usize>(sy) * image.width
                                         + static_cast<usize>(sx)) * 4],
                            4);
            }
        }

        m_by_hash[entry.hash].push_back(static_cast<u32>(m_entries.size()));
        m_entries.push_back(entry);

        shelf_x += cell_w;
        shelf_h = std::max(shelf_h, cell_h);
    }

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    SM2_INFO("custom textures: %zu loaded from '%s' into %zu atlas layer(s) in %.2fs",
             m_entries.size(), directory.c_str(), m_layers.size(), seconds);
    return m_entries.size();
}

bool TextureReplacements::find(u64 hash, Placement* out) const
{
    const auto found = m_by_hash.find(hash);
    if (found == m_by_hash.end()) {
        return false;
    }
    const Entry& e = m_entries[found->second.front()];
    *out = Placement{e.layer, e.x, e.y, e.width, e.height, e.max_lod};
    return true;
}

void TextureReplacements::begin_frame(const hw::Model2MachineBase& machine,
                                      const hw::Model2Video&       video)
{
    ++m_frame;
    m_tone.resize(static_cast<usize>(hw::Model2Video::kToneShades)
                  * hw::Model2Video::kToneComponents);
    video.build_tone_curve(m_tone);
    m_luma = machine.luma_ram();
}

void TextureReplacements::resolve(const hw::Model2MachineBase& machine,
                                  const hw::Model2Video& video, const hw::RenderPolygon& poly,
                                  PolyParams* params)
{
    const TextureHasher::Entry& key = m_hasher.lookup(machine, *params);
    const auto found = m_by_hash.find(key.hash);
    if (found == m_by_hash.end()) {
        return;
    }

    const u32 colour_base = (poly.texheader[3] >> 6) & 0x3ffu;
    const u32 luma_table  = poly.texheader[1] & 0xffu;
    u32 chosen = found->second.front();
    for (const u32 index : found->second) {
        if (m_entries[index].colour_base == colour_base
            && m_entries[index].luma_table == luma_table) {
            chosen = index;
            break;
        }
    }
    Entry& entry = m_entries[chosen];

    if (entry.palette_frame != m_frame) {
        entry.palette_frame = m_frame;
        entry.reference =
            level_palette(m_luma, m_tone, video.polygon_colour_components(entry.colour_base),
                          entry.luma_table << 7, entry.luma);
    }
    const std::array<u32, 16> current =
        level_palette(m_luma, m_tone, params->colour, params->luma_base, poly.luma);

    // The image was painted over the reference colouring, so another colouring
    // is applied as the ratio between the two, averaged over the levels used.
    // One ratio per polygon keeps the original's level boundaries out.
    const bool translucent = (params->flags & kFlagTranslucent) != 0;
    std::array<float, 3> cur{};
    std::array<float, 3> ref{};
    float weight = 0.0F;
    for (u32 level = 0; level < 16; ++level) {
        if (translucent && level == 0xf) {
            continue;
        }
        const float n = static_cast<float>(key.histogram[level]);
        weight += n;
        for (u32 c = 0; c < 3; ++c) {
            cur[c] += n * static_cast<float>((current[level] >> (c * 8)) & 0xffu);
            ref[c] += n * static_cast<float>((entry.reference[level] >> (c * 8)) & 0xffu);
        }
    }
    const float bias = std::max(weight, 1.0F) * 4.0F;
    u32 tint = 0;
    for (u32 c = 0; c < 3; ++c) {
        tint |= pack_tint((cur[c] + bias) / (ref[c] + bias)) << (c * 10);
    }

    params->replace    = (entry.layer + 1) | (entry.max_lod << 8);
    params->replace_xy = entry.x | (entry.y << 16);
    params->replace_wh = entry.width | (entry.height << 16);
    params->tint       = tint;
}

}  // namespace sm2::render
