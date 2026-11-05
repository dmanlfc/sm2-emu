//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 1 & 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This header must not be removed. sm2-emu is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.

#include "render/texture_dump.h"

#include "core/log.h"
#include "hw/geometrizer.h"
#include "hw/model2_machine_base.h"
#include "hw/model2_softrender.h"
#include "hw/model2_video.h"
#include "render/image_write.h"
#include "render/texture_replace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace sm2::render {
namespace {

constexpr u32   kTransparentTexel = 0xf;
constexpr usize kMaxOutlines      = 48;

extern const char* const kIndexHtml;

[[nodiscard]] std::string hex(u64 value, int digits)
{
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%0*llx", digits,
                  static_cast<unsigned long long>(value));
    return buffer;
}

[[nodiscard]] double polygon_area(const hw::RenderPolygon& poly)
{
    double twice = 0.0;
    for (u32 i = 0; i < poly.num_vertices; ++i) {
        const hw::PolyVertex& a = poly.v[i];
        const hw::PolyVertex& b = poly.v[(i + 1) % poly.num_vertices];
        twice += static_cast<double>(a.x) * static_cast<double>(b.y)
               - static_cast<double>(b.x) * static_cast<double>(a.y);
    }
    return std::abs(twice) * 0.5;
}

/// d(u,v)/d(x,y) across the polygon's first three corners, scaled to unit
/// norm so only its direction votes. Perspective is ignored: only the signs
/// and the dominant axes matter.
[[nodiscard]] bool screen_to_texel(const hw::RenderPolygon& poly, std::array<double, 4>* out)
{
    const hw::PolyVertex& a = poly.v[0];
    const hw::PolyVertex& b = poly.v[1];
    const hw::PolyVertex& c = poly.v[2];
    const double ex0 = b.x - a.x, ey0 = b.y - a.y;
    const double ex1 = c.x - a.x, ey1 = c.y - a.y;
    const double det = ex0 * ey1 - ex1 * ey0;
    if (std::abs(det) < 1e-6) {
        return false;
    }
    const double fu0 = b.p[1] - a.p[1], fv0 = b.p[2] - a.p[2];
    const double fu1 = c.p[1] - a.p[1], fv1 = c.p[2] - a.p[2];
    // F * inverse(E), E's columns being the two screen edges.
    const double j00 = (fu0 * ey1 - fu1 * ey0) / det;
    const double j01 = (fu1 * ex0 - fu0 * ex1) / det;
    const double j10 = (fv0 * ey1 - fv1 * ey0) / det;
    const double j11 = (fv1 * ex0 - fv0 * ex1) / det;
    const double norm = std::sqrt(j00 * j00 + j01 * j01 + j10 * j10 + j11 * j11);
    if (norm < 1e-9) {
        return false;
    }
    *out = {j00 / norm, j01 / norm, j10 / norm, j11 / norm};
    return true;
}

/// Bit 0 transposes, bit 1 then flips x, bit 2 then flips y.
[[nodiscard]] u32 pick_orientation(const std::array<double, 4>& s)
{
    const double straight   = std::abs(s[0]) + std::abs(s[3]);
    const double transposed = std::abs(s[2]) + std::abs(s[1]);
    if (transposed > straight) {
        return 1u | (s[2] < 0 ? 2u : 0u) | (s[1] < 0 ? 4u : 0u);
    }
    return (s[0] < 0 ? 2u : 0u) | (s[3] < 0 ? 4u : 0u);
}

[[nodiscard]] std::string css_colour(u32 rgba)
{
    return "#" + hex(((rgba & 0xffu) << 16) | (rgba & 0xff00u) | ((rgba >> 16) & 0xffu), 6);
}

}  // namespace

TextureDumper::TextureDumper(std::string directory, std::string game)
    : m_directory(std::move(directory))
    , m_game(std::move(game))
    , m_tone(static_cast<usize>(hw::Model2Video::kToneShades) * hw::Model2Video::kToneComponents)
    , m_renderer(std::make_unique<hw::SoftRenderer>())
    , m_writer([this] { write_loop(); })
{
    std::error_code error;
    std::filesystem::create_directories(m_directory + "/frames", error);
    if (error) {
        SM2_ERROR("texture dump: could not create '%s'", m_directory.c_str());
    } else {
        SM2_INFO("texture dump: collecting into '%s'", m_directory.c_str());
    }
}

TextureDumper::~TextureDumper()
{
    {
        const std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_stopping = true;
    }
    m_queue_ready.notify_one();
    m_writer.join();
}

void TextureDumper::queue_png(std::string path, u32 width, u32 height, u32 channels,
                              std::vector<u8> pixels)
{
    {
        const std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_queue.push_back(PendingPng{std::move(path), width, height, channels, std::move(pixels)});
    }
    m_queue_ready.notify_one();
}

void TextureDumper::write_loop()
{
    for (;;) {
        PendingPng job;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_ready.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) {
                return;
            }
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        (void)write_png(job.path, job.width, job.height, job.channels, job.pixels.data());
    }
}

void TextureDumper::scan(hw::Model2MachineBase& machine)
{
    ++m_frame;

    const hw::Model2Video&    video = machine.video();
    const std::span<const u8> luma  = machine.luma_ram();
    bool tone_built  = false;
    bool new_texture = false;

    for (const hw::RenderPolygon& poly : machine.render_list().polygons) {
        if (poly.num_vertices < 3) {
            continue;
        }
        const PolyParams p = describe_polygon(poly, video);
        if ((p.flags & kFlagTextured) == 0) {
            continue;
        }

        const u64 data_hash = m_hasher.lookup(machine, p).hash;
        auto [entry, inserted] = m_textures.try_emplace(data_hash);
        Texture& texture = entry->second;
        if (inserted) {
            texture.width       = p.tex_width;
            texture.height      = p.tex_height;
            texture.texels      = read_texels(machine, p);
            texture.first_frame = m_frame;
            m_order.push_back(data_hash);
            new_texture = true;
        }

        const double area = polygon_area(poly);
        texture.area += area;
        texture.translucent = texture.translucent || (p.flags & kFlagTranslucent) != 0;
        if (texture.last_frame != m_frame) {
            texture.last_frame = m_frame;
            ++texture.frames_seen;
        }
        if (std::array<double, 4> j{}; screen_to_texel(poly, &j)) {
            const double weight = std::max(area, 1.0);
            for (usize i = 0; i < 4; ++i) {
                texture.orientation[i] += j[i] * weight;
            }
        }
        if (texture.first_frame == m_frame && texture.outlines.size() < kMaxOutlines) {
            Outline outline;
            for (u32 i = 0; i < poly.num_vertices; ++i) {
                outline.points.push_back(poly.v[i].x);
                outline.points.push_back(poly.v[i].y);
            }
            texture.outlines.push_back(std::move(outline));
        }

        const u32 colour_base = (poly.texheader[3] >> 6) & 0x3ffu;
        const u32 luma_table  = poly.texheader[1] & 0xffu;
        auto variant = std::find_if(texture.variants.begin(), texture.variants.end(),
                                    [&](const Variant& v) {
                                        return v.colour_base == colour_base
                                            && v.luma_table == luma_table;
                                    });
        const bool fresh = variant == texture.variants.end();
        if (fresh) {
            texture.variants.push_back(Variant{colour_base, luma_table, 0, 0.0, {}});
            variant = texture.variants.end() - 1;
        }
        variant->area += area;
        if (fresh || poly.luma > variant->luma) {
            if (!tone_built) {
                video.build_tone_curve(m_tone);
                tone_built = true;
            }
            variant->luma    = std::max(variant->luma, poly.luma);
            variant->palette = level_palette(luma, m_tone, p.colour, p.luma_base, variant->luma);
        }
    }

    if (new_texture) {
        machine.compose_video();
        std::vector<u32> frame(static_cast<usize>(hw::SoftRenderer::kWidth)
                               * hw::SoftRenderer::kHeight);
        m_renderer->render(machine, machine.render_list(), frame);
        std::vector<u8> rgb(frame.size() * 3);
        for (usize i = 0; i < frame.size(); ++i) {
            rgb[i * 3 + 0] = static_cast<u8>(frame[i]);
            rgb[i * 3 + 1] = static_cast<u8>(frame[i] >> 8);
            rgb[i * 3 + 2] = static_cast<u8>(frame[i] >> 16);
        }
        char name[40];
        std::snprintf(name, sizeof(name), "/frames/frame_%06u.png", m_frame);
        queue_png(m_directory + name, hw::SoftRenderer::kWidth, hw::SoftRenderer::kHeight, 3,
                  std::move(rgb));
        ++m_frames_captured;
    }
}

void TextureDumper::finish(const hw::Model2MachineBase* machine)
{
    // One orientation for the whole game: textures are authored consistently,
    // while a single texture's vote is skewed by being seen from behind or on
    // mirrored geometry. Each texture gets one equal vote.
    std::array<double, 4> vote{};
    for (const auto& [hash, texture] : m_textures) {
        const std::array<double, 4>& s = texture.orientation;
        const double norm = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2] + s[3] * s[3]);
        if (norm > 0.0) {
            for (usize i = 0; i < 4; ++i) {
                vote[i] += s[i] / norm;
            }
        }
    }
    const u32 orientation = pick_orientation(vote);
    const bool transpose  = (orientation & 1u) != 0;

    std::ostringstream json;
    json << "{\"game\":\"" << m_game << "\",\"textures\":[";

    usize variants = 0;
    bool  first    = true;
    for (const u64 data_hash : m_order) {
        const Texture& texture = m_textures.at(data_hash);
        const Variant& shown = *std::max_element(
            texture.variants.begin(), texture.variants.end(),
            [](const Variant& a, const Variant& b) { return a.area < b.area; });
        variants += texture.variants.size();

        const u32 out_w = transpose ? texture.height : texture.width;
        const u32 out_h = transpose ? texture.width : texture.height;

        std::vector<u8> rgba(static_cast<usize>(out_w) * out_h * 4);
        for (u32 v = 0; v < texture.height; ++v) {
            for (u32 u = 0; u < texture.width; ++u) {
                u32 x = transpose ? v : u;
                u32 y = transpose ? u : v;
                if ((orientation & 2u) != 0) {
                    x = out_w - 1 - x;
                }
                if ((orientation & 4u) != 0) {
                    y = out_h - 1 - y;
                }
                const u8  level = texture.texels[static_cast<usize>(v) * texture.width + u];
                const u32 c     = shown.palette[level];
                u8* dest = &rgba[(static_cast<usize>(y) * out_w + x) * 4];
                dest[0] = static_cast<u8>(c);
                dest[1] = static_cast<u8>(c >> 8);
                dest[2] = static_cast<u8>(c >> 16);
                dest[3] = texture.translucent && level == kTransparentTexel ? 0 : 0xff;
            }
        }

        const std::string file =
            replacement_file_name(data_hash, shown.colour_base, shown.luma_table, shown.luma,
                                  orientation);
        queue_png(m_directory + "/" + file, out_w, out_h, 4, std::move(rgba));

        json << (first ? "" : ",") << "{\"file\":\"" << file << "\",\"w\":" << texture.width
             << ",\"h\":" << texture.height << ",\"orient\":\""
             << orientation_tag(orientation)
             << "\",\"cut\":" << (texture.translucent ? "true" : "false")
             << ",\"area\":" << static_cast<u64>(texture.area)
             << ",\"frames\":" << texture.frames_seen << ",\"first\":" << texture.first_frame
             << ",\"variants\":[";
        first = false;
        for (usize i = 0; i < texture.variants.size(); ++i) {
            const Variant& v = texture.variants[i];
            json << (i == 0 ? "" : ",") << "{\"key\":\"" << hex(v.colour_base, 3)
                 << hex(v.luma_table, 2) << "\",\"area\":" << static_cast<u64>(v.area)
                 << ",\"shown\":" << (&v == &shown ? "true" : "false") << ",\"palette\":[";
            for (usize level = 0; level < 16; ++level) {
                json << (level == 0 ? "" : ",") << '"' << css_colour(v.palette[level]) << '"';
            }
            json << "]}";
        }
        json << "],\"outlines\":[";
        for (usize i = 0; i < texture.outlines.size(); ++i) {
            json << (i == 0 ? "" : ",") << '[';
            const std::vector<float>& points = texture.outlines[i].points;
            for (usize k = 0; k < points.size(); ++k) {
                char number[24];
                std::snprintf(number, sizeof(number), "%.1f", static_cast<double>(points[k]));
                json << (k == 0 ? "" : ",") << number;
            }
            json << ']';
        }
        json << "]}";
    }
    json << "]}";

    // Both whole sheets in the logical 2048x1024 space the polygons address,
    // including what was never drawn.
    if (machine != nullptr) {
        constexpr u32 kSheetWidth  = 2048;
        constexpr u32 kSheetHeight = 1024;
        for (int sheet_index = 0; sheet_index < 2; ++sheet_index) {
            const std::span<const u32> sheet = machine->texture_ram(sheet_index);
            std::vector<u8> grey(static_cast<usize>(kSheetWidth) * kSheetHeight);
            for (u32 y = 0; y < kSheetHeight; ++y) {
                for (u32 x = 0; x < kSheetWidth; ++x) {
                    grey[static_cast<usize>(y) * kSheetWidth + x] =
                        static_cast<u8>(fetch_texel(sheet, 0, 0, x, y) * 17);
                }
            }
            queue_png(m_directory + "/frames/sheet" + std::to_string(sheet_index) + ".png",
                      kSheetWidth, kSheetHeight, 1, std::move(grey));
        }
    }

    std::string html = kIndexHtml;
    const std::string marker = "/*DATA*/";
    html.replace(html.find(marker), marker.size(), json.str());
    std::ofstream(m_directory + "/index.html") << html;

    SM2_INFO("texture dump: %zu texture(s), %zu colour variant(s), %u frame(s), "
             "orientation '%s'; open '%s/index.html'",
             m_order.size(), variants, m_frames_captured, orientation_tag(orientation),
             m_directory.c_str());
}

namespace {

const char* const kIndexHtml = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Texture Dump</title>
<style>
:root {
  --bg: #f4f4f2; --panel: #ffffff; --ink: #1d1d1b; --muted: #6b6b66;
  --line: #deded9; --accent: #d9480f; --chip: #ecece8;
  --check-a: #cfcfca; --check-b: #e6e6e2;
}
@media (prefers-color-scheme: dark) {
  :root:not([data-theme="light"]) {
    --bg: #151514; --panel: #1f1f1d; --ink: #ecece8; --muted: #9a9a93;
    --line: #33332f; --accent: #ff7a3d; --chip: #2a2a27;
    --check-a: #2b2b28; --check-b: #353531;
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--ink);
  font: 14px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
header { position: sticky; top: 0; z-index: 2; background: var(--bg);
  border-bottom: 1px solid var(--line); padding: 12px 16px; }
h1 { font-size: 18px; margin: 0 0 2px; }
.stats { color: var(--muted); font-size: 13px; }
.controls { display: flex; flex-wrap: wrap; gap: 8px 16px; margin-top: 10px; align-items: center; }
.controls label { color: var(--muted); font-size: 13px; display: flex; gap: 6px; align-items: center; }
select, input[type=search] { background: var(--panel); color: var(--ink);
  border: 1px solid var(--line); border-radius: 6px; padding: 5px 8px; font: inherit; }
main { padding: 16px; }
.grid { display: grid; gap: 12px;
  grid-template-columns: repeat(auto-fill, minmax(var(--cell, 168px), 1fr)); }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 8px;
  overflow: hidden; cursor: pointer; display: flex; flex-direction: column; }
.card:hover { border-color: var(--accent); }
.thumb { aspect-ratio: 1; display: flex; align-items: center; justify-content: center; padding: 8px; }
.checker { background-color: var(--check-b);
  background-image: linear-gradient(45deg, var(--check-a) 25%, transparent 25%, transparent 75%, var(--check-a) 75%),
                    linear-gradient(45deg, var(--check-a) 25%, transparent 25%, transparent 75%, var(--check-a) 75%);
  background-size: 16px 16px; background-position: 0 0, 8px 8px; }
.thumb img { max-width: 100%; max-height: 100%; image-rendering: pixelated; }
.meta { padding: 6px 8px 8px; font-size: 12px; color: var(--muted);
  display: flex; flex-wrap: wrap; gap: 4px 6px; align-items: center; }
.meta b { color: var(--ink); font-weight: 600; }
.tag { background: var(--chip); border-radius: 4px; padding: 0 5px; }
.swatches { display: flex; gap: 2px; }
.swatches i { width: 10px; height: 10px; border-radius: 2px; display: block;
  outline: 1px solid rgb(0 0 0 / .15); }
dialog { border: 1px solid var(--line); border-radius: 10px; padding: 0; background: var(--panel);
  color: var(--ink); width: min(1100px, calc(100vw - 32px)); max-height: calc(100vh - 32px); }
dialog::backdrop { background: rgb(0 0 0 / .55); }
.detail { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 16px; padding: 16px; }
@media (max-width: 760px) { .detail { grid-template-columns: 1fr; } }
.detail h2 { font-size: 15px; margin: 0 0 8px; font-weight: 600; }
.big { display: flex; align-items: center; justify-content: center; min-height: 220px;
  border-radius: 8px; padding: 12px; }
.big img { image-rendering: pixelated; max-width: 100%; }
.shot { position: relative; border-radius: 8px; overflow: hidden; background: #000; }
.shot img { display: block; width: 100%; image-rendering: pixelated; }
.shot svg { position: absolute; inset: 0; width: 100%; height: 100%; }
.shot polygon { fill: rgb(255 122 61 / .25); stroke: #ff7a3d; stroke-width: 1.5;
  vector-effect: non-scaling-stroke; }
.shot.plain polygon { display: none; }
dl { display: grid; grid-template-columns: auto 1fr; gap: 4px 12px; margin: 12px 0; font-size: 13px; }
dt { color: var(--muted); }
dd { margin: 0; }
code { font: 12px ui-monospace, SFMono-Regular, Menlo, monospace; background: var(--chip);
  padding: 2px 5px; border-radius: 4px; word-break: break-all; }
button { font: inherit; background: var(--chip); color: var(--ink); border: 1px solid var(--line);
  border-radius: 6px; padding: 4px 10px; cursor: pointer; }
button:hover { border-color: var(--accent); }
.variants { display: flex; flex-direction: column; gap: 6px; margin-top: 8px; }
.variant { display: flex; gap: 8px; align-items: center; font-size: 12px; color: var(--muted); }
.variant.on { color: var(--ink); font-weight: 600; }
.variant .swatches i { width: 14px; height: 14px; }
.close { position: absolute; top: 10px; right: 12px; }
.row { display: flex; gap: 8px; align-items: center; flex-wrap: wrap; margin-top: 8px; }
.hint { color: var(--muted); font-size: 12px; margin-top: 8px; }
</style>
</head>
<body>
<header>
  <h1 id="title">Textures</h1>
  <div class="stats" id="stats"></div>
  <div class="controls">
    <label>Sort <select id="sort">
      <option value="area">Most visible</option>
      <option value="first">First seen</option>
      <option value="size">Largest</option>
      <option value="variants">Most colour variants</option>
    </select></label>
    <label>Size <select id="size"><option value="">All</option></select></label>
    <label>Thumbnails <input type="range" id="zoom" min="96" max="320" value="168"></label>
    <label><input type="search" id="search" placeholder="Filter by filename"></label>
  </div>
</header>
<main><div class="grid" id="grid"></div></main>
<dialog id="dialog"><div style="position:relative">
  <button class="close" id="close">Close</button>
  <div class="detail">
    <div>
      <h2>Texture</h2>
      <div class="big checker"><img id="d-img" alt=""></div>
      <dl id="d-info"></dl>
      <div class="row"><code id="d-file"></code><button id="d-copy">Copy name</button></div>
      <div class="hint">To replace it, save an image with this exact name in
        <code>textures/<span class="d-game"></span>/load/</code>, at the same orientation and
        any whole multiple of this size (2x, 4x, 8x...).</div>
      <h2 style="margin-top:16px">Colour variants</h2>
      <div class="variants" id="d-variants"></div>
    </div>
    <div>
      <h2>First seen</h2>
      <div class="shot" id="d-shot"><img id="d-frame" alt="">
        <svg viewBox="0 0 496 384" preserveAspectRatio="none" id="d-svg"></svg></div>
      <div class="row"><label style="color:var(--muted);font-size:13px">
        <input type="checkbox" id="d-outline" checked> Outline where it is drawn</label></div>
    </div>
  </div>
</div></dialog>
<script>
const DATA = /*DATA*/;
const HZ = 57.52;
const $ = id => document.getElementById(id);
const time = f => { const s = Math.floor(f / HZ); return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); };
const orientText = { "": "as stored", t: "transposed", fx: "flipped horizontally", fy: "flipped vertically",
  r90: "rotated 90°", r270: "rotated 270°", r180: "rotated 180°", at: "anti-transposed" };
const dims = t => t.orient && "t r90 r270 at".includes(t.orient) ? [t.h, t.w] : [t.w, t.h];
const swatch = pal => '<span class="swatches">' + [0, 5, 10, 14].map(i => `<i style="background:${pal[i]}"></i>`).join("") + "</span>";
const total = DATA.textures.reduce((a, t) => a + t.area, 0) || 1;

document.title = DATA.game + " textures";
$("title").textContent = DATA.game + " textures";
document.querySelectorAll(".d-game").forEach(e => e.textContent = DATA.game);
const nVariants = DATA.textures.reduce((a, t) => a + t.variants.length, 0);
const stored = DATA.textures.length ? orientText[DATA.textures[0].orient] : "";
$("stats").textContent = `${DATA.textures.length} textures · ${nVariants} colour variants · ` +
  `stored ${stored}, shown upright in the colouring used most on screen`;
[...new Set(DATA.textures.map(t => t.w + "x" + t.h))]
  .sort((a, b) => { const [aw, ah] = a.split("x"), [bw, bh] = b.split("x"); return bw * bh - aw * ah; })
  .forEach(s => $("size").insertAdjacentHTML("beforeend", `<option>${s}</option>`));

function render() {
  const sort = $("sort").value, size = $("size").value, q = $("search").value.trim().toLowerCase();
  const list = DATA.textures.filter(t => (!size || t.w + "x" + t.h === size) && (!q || t.file.includes(q)));
  const key = { area: t => -t.area, first: t => t.first, size: t => -t.w * t.h, variants: t => -t.variants.length }[sort];
  list.sort((a, b) => key(a) - key(b));
  $("grid").innerHTML = list.map(t => {
    const [w, h] = dims(t);
    const shown = t.variants.find(v => v.shown);
    return `<div class="card" data-file="${t.file}">
      <div class="thumb checker"><img loading="lazy" src="${t.file}" alt=""></div>
      <div class="meta"><b>${w}×${h}</b>${swatch(shown.palette)}
        ${t.variants.length > 1 ? `<span class="tag">${t.variants.length} colours</span>` : ""}
        ${t.cut ? '<span class="tag">cut-out</span>' : ""}</div></div>`;
  }).join("");
}

function open(t) {
  const [w, h] = dims(t);
  $("d-img").src = t.file;
  const scale = Math.max(1, Math.floor(480 / Math.max(w, h)));
  $("d-img").style.width = w * scale + "px";
  $("d-file").textContent = t.file;
  $("d-info").innerHTML = `
    <dt>Size</dt><dd>${w}×${h} (stored ${t.w}×${t.h}, ${orientText[t.orient]})</dd>
    <dt>On screen</dt><dd>${(100 * t.area / total).toFixed(2)}% of all textured area, in ${t.frames} frames</dd>
    <dt>First seen</dt><dd>${time(t.first)} (frame ${t.first})</dd>
    <dt>Transparency</dt><dd>${t.cut ? "brightest level is see-through" : "none"}</dd>`;
  $("d-variants").innerHTML = t.variants.slice().sort((a, b) => b.area - a.area).map(v =>
    `<div class="variant${v.shown ? " on" : ""}"><span class="swatches">${v.palette.map(c => `<i style="background:${c}"></i>`).join("")}</span>
      ${v.key} · ${(100 * v.area / (t.area || 1)).toFixed(1)}%${v.shown ? " · shown" : ""}</div>`).join("");
  $("d-frame").src = `frames/frame_${String(t.first).padStart(6, "0")}.png`;
  $("d-svg").innerHTML = t.outlines.map(p => {
    const pts = []; for (let i = 0; i < p.length; i += 2) pts.push(p[i] + "," + p[i + 1]);
    return `<polygon points="${pts.join(" ")}"/>`;
  }).join("");
  $("dialog").showModal();
}

$("grid").addEventListener("click", e => {
  const card = e.target.closest(".card");
  if (card) open(DATA.textures.find(t => t.file === card.dataset.file));
});
$("close").onclick = () => $("dialog").close();
$("dialog").addEventListener("click", e => { if (e.target === $("dialog")) $("dialog").close(); });
$("d-copy").onclick = () => navigator.clipboard && navigator.clipboard.writeText($("d-file").textContent);
$("d-outline").onchange = e => $("d-shot").classList.toggle("plain", !e.target.checked);
$("zoom").oninput = e => document.documentElement.style.setProperty("--cell", e.target.value + "px");
["sort", "size"].forEach(id => $(id).onchange = render);
$("search").oninput = render;
render();
</script>
</body>
</html>
)HTML";

}  // namespace

}  // namespace sm2::render
