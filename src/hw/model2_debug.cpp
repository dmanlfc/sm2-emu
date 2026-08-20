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
//
// Tile format transcribed from MAME's src/mame/sega/segaic24.cpp (BSD-3-Clause,
// Olivier Galibert).

#include "hw/model2_debug.h"

#include "core/log.h"
#include "core/types.h"
#include "hw/model2.h"
#include "hw/copro_tgp.h"
#include "hw/model2_video.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <bit>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace sm2::hw {
namespace {

constexpr u32 kCellsAcross = 64;
constexpr u32 kCellsDown   = 64;
constexpr u32 kTileSize    = 8;
constexpr u32 kMapPixels   = kCellsAcross * kTileSize;  // 512

/// Render a whole layer at 512x512, one byte per pixel.
[[nodiscard]] std::vector<u8> render_layer(const Model2& machine, u32 layer)
{
    const std::span<const u8> tile_ram = machine.tile_ram();
    const std::span<const u8> char_ram = machine.char_ram();

    std::vector<u8> image(static_cast<usize>(kMapPixels) * kMapPixels, 0);

    for (u32 cell_y = 0; cell_y < kCellsDown; ++cell_y) {
        for (u32 cell_x = 0; cell_x < kCellsAcross; ++cell_x) {
            const TileCell cell = decode_tile_cell(tile_ram, layer, cell_y * kCellsAcross + cell_x);

            for (u32 y = 0; y < kTileSize; ++y) {
                u8* row = image.data()
                        + (static_cast<usize>(cell_y * kTileSize + y) * kMapPixels)
                        + cell_x * kTileSize;
                for (u32 x = 0; x < kTileSize; ++x) {
                    // Scaled to full range rather than run through the palette, so
                    // that glyphs are legible without depending on the colour
                    // pipeline being right.
                    row[x] = static_cast<u8>(decode_tile_pixel(char_ram, cell.tile, x, y) * 17);
                }
            }
        }
    }
    return image;
}

struct LayerStats {
    u32 cells_used       = 0;  ///< Name table entries referencing a non-zero tile.
    u32 cells_foreground = 0;  ///< Entries with the priority bit set.
    u32 pixels_set       = 0;
};

[[nodiscard]] LayerStats measure_layer(const Model2& machine, u32 layer,
                                       const std::vector<u8>& image)
{
    LayerStats stats;
    for (u32 index = 0; index < kCellsAcross * kCellsDown; ++index) {
        const TileCell cell = decode_tile_cell(machine.tile_ram(), layer, index);
        if (cell.tile != 0) {
            ++stats.cells_used;
        }
        if (cell.foreground) {
            ++stats.cells_foreground;
        }
    }
    for (const u8 pixel : image) {
        if (pixel != 0) {
            ++stats.pixels_set;
        }
    }
    return stats;
}

/// Write RGBA8 pixels as a binary PPM, flattening them over `background`.
///
/// Alpha is either zero or full, so this is a select rather than a blend, but
/// writing it as a blend keeps it honest if that ever changes.
[[nodiscard]] bool write_ppm(const std::filesystem::path& path,
                             std::span<const u32>         pixels,
                             u32                          width,
                             u32                          height,
                             u32                          background)
{
    std::FILE* handle = std::fopen(path.string().c_str(), "wb");
    if (handle == nullptr) {
        SM2_ERROR("could not write '%s'", path.string().c_str());
        return false;
    }
    std::fprintf(handle, "P6\n%u %u\n255\n", width, height);

    std::vector<u8> row(static_cast<usize>(width) * 3);
    bool            ok = true;
    for (u32 y = 0; y < height && ok; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const usize index = static_cast<usize>(y) * width + x;
            const u32   value = index < pixels.size() ? pixels[index] : 0;
            const u32   alpha = (value >> 24) & 0xff;
            const u32   under = 255 - alpha;
            for (u32 channel = 0; channel < 3; ++channel) {
                const u32 shift = channel * 8;
                const u32 top   = (value >> shift) & 0xff;
                const u32 below = (background >> shift) & 0xff;
                row[static_cast<usize>(x) * 3 + channel] =
                    static_cast<u8>(top + below * under / 255);
            }
        }
        ok = std::fwrite(row.data(), 1, row.size(), handle) == row.size();
    }
    std::fclose(handle);
    return ok;
}

[[nodiscard]] bool write_pgm(const std::filesystem::path& path,
                             const std::vector<u8>&       image,
                             u32                          width,
                             u32                          height)
{
    std::FILE* handle = std::fopen(path.string().c_str(), "wb");
    if (handle == nullptr) {
        SM2_ERROR("could not write '%s'", path.string().c_str());
        return false;
    }
    std::fprintf(handle, "P5\n%u %u\n255\n", width, height);
    const usize written = std::fwrite(image.data(), 1, image.size(), handle);
    std::fclose(handle);
    return written == image.size();
}

}  // namespace

bool dump_tilemaps(const Model2& machine, const std::string& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        SM2_ERROR("could not create '%s': %s", directory.c_str(),
                  error.message().c_str());
        return false;
    }
    const std::filesystem::path base(directory);

    for (u32 layer = 0; layer < kTileLayers; ++layer) {
        const std::vector<u8> image = render_layer(machine, layer);
        const LayerStats      stats = measure_layer(machine, layer, image);

        const std::string name = "tilemap" + std::to_string(layer) + ".pgm";
        if (!write_pgm(base / name, image, kMapPixels, kMapPixels)) {
            return false;
        }
        SM2_INFO("layer %u: %u/%u cells used, %u foreground, %u pixels set -> %s",
                 layer, stats.cells_used, kCellsAcross * kCellsDown,
                 stats.cells_foreground, stats.pixels_set, name.c_str());
    }

    // The whole character set, 64 tiles across, so a font is easy to spot.
    {
        constexpr u32 kTilesAcross = 64;
        constexpr u32 kTileCount   = 0x4000;
        constexpr u32 kRows        = kTileCount / kTilesAcross;
        constexpr u32 width        = kTilesAcross * kTileSize;
        constexpr u32 height       = kRows * kTileSize;

        std::vector<u8> sheet(static_cast<usize>(width) * height, 0);
        for (u32 tile = 0; tile < kTileCount; ++tile) {
            const u32 origin_x = (tile % kTilesAcross) * kTileSize;
            const u32 origin_y = (tile / kTilesAcross) * kTileSize;
            for (u32 y = 0; y < kTileSize; ++y) {
                u8* row = sheet.data() + static_cast<usize>(origin_y + y) * width + origin_x;
                for (u32 x = 0; x < kTileSize; ++x) {
                    row[x] = static_cast<u8>(
                        decode_tile_pixel(machine.char_ram(), static_cast<u16>(tile), x, y) * 17);
                }
            }
        }
        if (!write_pgm(base / "characters.pgm", sheet, width, height)) {
            return false;
        }
        SM2_INFO("character RAM -> characters.pgm (%ux%u)", width, height);
    }

    return true;
}

bool dump_composed_frame(Model2& machine, const std::string& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        SM2_ERROR("could not create '%s': %s", directory.c_str(), error.message().c_str());
        return false;
    }
    const std::filesystem::path base(directory);

    const Model2Video& video = machine.video();
    const u32          width  = Model2Video::kWidth;
    const u32          height = Model2Video::kHeight;
    const u32          background = video.background();

    // The raw contents of the two RAMs the tilemap chip reads, so a suspect
    // picture can be traced to what the program actually wrote.
    {
        const std::span<const u8> tile_ram = machine.tile_ram();
        const std::span<const u8> char_ram = machine.char_ram();
        std::FILE* handle = std::fopen((base / "tile_ram.bin").string().c_str(), "wb");
        if (handle != nullptr) {
            std::fwrite(tile_ram.data(), 1, tile_ram.size(), handle);
            std::fclose(handle);
        }
        handle = std::fopen((base / "char_ram.bin").string().c_str(), "wb");
        if (handle != nullptr) {
            std::fwrite(char_ram.data(), 1, char_ram.size(), handle);
            std::fclose(handle);
        }
    }

    // The two halves on their own, over a mid grey so that a transparent pixel is
    // distinguishable from a black one, and then the pair over the real
    // background, which is what the screen shows.
    constexpr u32 kGrey = 0xff404040u;
    if (!write_ppm(base / "frame_below.ppm", video.below(), width, height, kGrey)
        || !write_ppm(base / "frame_above.ppm", video.above(), width, height, kGrey)) {
        return false;
    }

    std::vector<u32> flattened(static_cast<usize>(width) * height, 0);
    const std::span<const u32> below = video.below();
    const std::span<const u32> above = video.above();
    for (usize index = 0; index < flattened.size(); ++index) {
        u32 value = index < below.size() ? below[index] : 0;
        if (index < above.size() && ((above[index] >> 24) & 0xff) != 0) {
            value = above[index];
        }
        flattened[index] = value;
    }
    if (!write_ppm(base / "frame.ppm", flattened, width, height, background)) {
        return false;
    }

    // Each layer and category on its own, which is what attributes a missing or
    // misplaced element to a specific layer rather than to the compositor.
    {
        Segaic24Tile&    tiles = machine.video().tiles();
        std::vector<u32> single(static_cast<usize>(width) * height, 0);

        std::printf("\n=== composed contributions ===\n");
        std::printf("%-6s %-9s %-12s %s\n", "LAYER", "CATEGORY", "PIXELS", "FILE");
        for (u32 layer = 0; layer < kTileLayers; ++layer) {
            for (u32 category = 0; category < 2; ++category) {
                std::fill(single.begin(), single.end(), 0u);
                tiles.draw((layer << 1) | category, /*opaque=*/false, video.pens(),
                           single.data(), width);

                u32 drawn = 0;
                for (const u32 value : single) {
                    drawn += ((value >> 24) & 0xff) != 0 ? 1u : 0u;
                }
                if (drawn == 0) {
                    continue;
                }
                const std::string name = "layer" + std::to_string(layer) + "_cat"
                                       + std::to_string(category) + ".ppm";
                if (!write_ppm(base / name, single, width, height, kGrey)) {
                    return false;
                }
                std::printf("%-6u %-9u %-12u %s\n", layer, category, drawn, name.c_str());
            }
        }
        std::printf("\n");
    }

    u32 drawn_below = 0;
    u32 drawn_above = 0;
    for (const u32 value : below) {
        drawn_below += ((value >> 24) & 0xff) != 0 ? 1u : 0u;
    }
    for (const u32 value : above) {
        drawn_above += ((value >> 24) & 0xff) != 0 ? 1u : 0u;
    }
    SM2_INFO("composed frame: %u/%u pixels below the 3D, %u above, background %08x",
             drawn_below, width * height, drawn_above, background);
    return true;
}

void print_render_list_summary(const Model2& machine)
{
    const RenderList& list = machine.render_list();

    std::printf("\n=== geometry ===\n");
    std::printf("polygons kept      : %zu\n", list.polygons.size());
    std::printf("culled             : %u\n", list.culled);
    std::printf("clipped to nothing : %u\n", list.clipped_away);
    std::printf("in the sort buckets: %u\n", list.generated);

    if (list.polygons.empty()) {
        std::printf("\nThe geometry engine produced nothing this frame.\n");
        return;
    }

    float min_x = 1e30F;
    float max_x = -1e30F;
    float min_y = 1e30F;
    float max_y = -1e30F;
    float min_z = 1e30F;
    float max_z = -1e30F;
    u32   vertices = 0;
    u32   windows  = 0;
    u32   on_screen = 0;
    std::array<u32, 9> by_vertex_count{};

    // The two bits the pixel stage dispatches on, counted as the four paths they
    // select, plus the stipple flag that cuts across them.
    std::array<u32, 4> by_path{};
    u32                checkered = 0;

    for (const RenderPolygon& poly : list.polygons) {
        vertices += poly.num_vertices;
        windows = std::max<u32>(windows, poly.window);
        ++by_path[(poly.texheader[0] >> 13) & 3];
        checkered += ((poly.texheader[0] >> 15) & 1);
        if (poly.num_vertices < by_vertex_count.size()) {
            ++by_vertex_count[poly.num_vertices];
        }

        bool visible = false;
        for (u32 index = 0; index < poly.num_vertices; ++index) {
            const float x = poly.v[index].x;
            const float y = poly.v[index].y;
            min_x = std::min(min_x, x);
            max_x = std::max(max_x, x);
            min_y = std::min(min_y, y);
            max_y = std::max(max_y, y);
            min_z = std::min(min_z, poly.v[index].p[0]);
            max_z = std::max(max_z, poly.v[index].p[0]);
            visible = visible || (x >= 0.0F && x < static_cast<float>(Model2::kVisibleWidth)
                                  && y >= 0.0F
                                  && y < static_cast<float>(Model2::kVisibleHeight));
        }
        on_screen += visible ? 1u : 0u;
    }

    std::printf("vertices           : %u (mean %.2f per polygon)\n", vertices,
                static_cast<double>(vertices) / static_cast<double>(list.polygons.size()));
    std::printf("vertex counts      :");
    for (u32 count = 3; count < by_vertex_count.size(); ++count) {
        if (by_vertex_count[count] != 0) {
            std::printf(" %u-gon %u", count, by_vertex_count[count]);
        }
    }
    std::printf("\n");
    // The pixel path each polygon asks for. Flat shading serves the two opaque
    // paths; the translucent ones need a texel before they can decide anything, so
    // until the textured stage exists they are left undrawn.
    std::printf("pixel paths        : solid %u, solid translucent %u,"
                " textured %u, textured translucent %u\n",
                by_path[0], by_path[1], by_path[2], by_path[3]);
    std::printf("stippled           : %u of %zu\n", checkered, list.polygons.size());
    std::printf("windows used       : %u\n", windows + 1);
    {
        std::printf("colour bases       :");
        u32 shown = 0;
        for (const RenderPolygon& poly : list.polygons) {
            const u32 colour_base = (poly.texheader[3] >> 6) & 0x3ff;
            const u32 components  = machine.video().polygon_colour_components(colour_base);
            if (shown < 20) {
                std::printf(" %u[r%u g%u b%u]", colour_base, components & 0x1f,
                            (components >> 8) & 0x1f, (components >> 16) & 0x1f);
                ++shown;
            }
        }
        std::printf(" ...\n");
        u32 nonzero = 0;
        for (u32 idx = 0; idx < 0x400; ++idx) {
            if (machine.video().polygon_colour_components(idx) != 0) {
                ++nonzero;
            }
        }
        std::printf("polygon palette    : %u/1024 non-zero entries\n", nonzero);
        u32 nonzero_all = 0;
        for (const u16 entry : machine.palette_ram()) {
            if (entry != 0) {
                ++nonzero_all;
            }
        }
        std::printf("full palette RAM   : %u/%zu non-zero entries\n", nonzero_all,
                    machine.palette_ram().size());
    }
    std::printf("screen extent      : x %.1f..%.1f, y %.1f..%.1f (raster is %ux%u)\n",
                static_cast<double>(min_x), static_cast<double>(max_x),
                static_cast<double>(min_y), static_cast<double>(max_y),
                Model2::kVisibleWidth, Model2::kVisibleHeight);
    std::printf("view depth         : %.4f..%.4f\n", static_cast<double>(min_z),
                static_cast<double>(max_z));
    std::printf("touching the raster: %u of %zu\n", on_screen, list.polygons.size());
}

bool dump_render_list_wireframe(const Model2& machine, const std::string& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        SM2_ERROR("could not create '%s': %s", directory.c_str(), error.message().c_str());
        return false;
    }

    constexpr u32 width  = Model2::kVisibleWidth;
    constexpr u32 height = Model2::kVisibleHeight;

    std::vector<u32> image(static_cast<usize>(width) * height, 0);

    const auto plot = [&](int x, int y, u32 colour) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
            return;
        }
        image[static_cast<usize>(y) * width + static_cast<usize>(x)] = colour;
    };

    // Bresenham, so a line is drawn without any floating point in the inner loop
    // and every pixel of it lands on the raster grid.
    const auto line = [&](float x0f, float y0f, float x1f, float y1f, u32 colour) {
        int  x0 = static_cast<int>(std::lround(x0f));
        int  y0 = static_cast<int>(std::lround(y0f));
        const int x1 = static_cast<int>(std::lround(x1f));
        const int y1 = static_cast<int>(std::lround(y1f));
        const int dx = std::abs(x1 - x0);
        const int dy = -std::abs(y1 - y0);
        const int sx = x0 < x1 ? 1 : -1;
        const int sy = y0 < y1 ? 1 : -1;
        int       err = dx + dy;

        // A polygon whose projection went wrong can span millions of pixels, so
        // the walk is bounded rather than trusted.
        for (u32 step = 0; step < 4096; ++step) {
            plot(x0, y0, colour);
            if (x0 == x1 && y0 == y1) {
                return;
            }
            const int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    };

    const RenderList& list = machine.render_list();
    for (const RenderPolygon& poly : list.polygons) {
        // Colour by depth so overlapping geometry is separable: near is warm.
        const float depth = poly.v[0].p[0];
        const float scale = 1.0F / (1.0F + std::max(0.0F, depth) * 0.02F);
        const u8    warm  = static_cast<u8>(std::clamp(scale * 255.0F, 40.0F, 255.0F));
        const u32   colour =
            0xff000000u | static_cast<u32>(warm) | (static_cast<u32>(warm / 2) << 8)
            | (static_cast<u32>(255 - warm) << 16);

        for (u32 index = 0; index < poly.num_vertices; ++index) {
            const PolyVertex& a = poly.v[index];
            const PolyVertex& b = poly.v[(index + 1) % poly.num_vertices];
            line(a.x, a.y, b.x, b.y, colour);
        }
    }

    const std::filesystem::path base(directory);
    if (!write_ppm(base / "wireframe.ppm", image, width, height, 0xff101018u)) {
        return false;
    }
    SM2_INFO("wireframe of %zu polygon(s) -> wireframe.ppm", list.polygons.size());
    return true;
}

bool run_copro_selftest(Model2& machine)
{
    CoproTgp& tgp = machine.copro();

    // The units live in the io space, which the bank register must not be pointing
    // at external memory for.
    tgp.write_rf(3, 0);

    struct Unit {
        const char* name;
        double      worst    = 0.0;
        double      tolerance = 0.0;
        u32         samples  = 0;
        double      at       = 0.0;
        u32         reported = 0;
    };

    // Tolerances are set by the tables themselves: a quarter turn in 16384 entries
    // is about 0.0001 radians of resolution, and the reciprocal tables hold 14
    // bits of mantissa. These are what the hardware can do, not what we would like.
    Unit sine{"sine", 0.0, 2.0e-4};
    Unit cosine{"cosine", 0.0, 2.0e-4};
    // The reciprocal units do not return 1/x and 1/sqrt(x) but fixed multiples of
    // them: 2/x and 1.5/sqrt(x). Those constants come out of how the mantissa
    // tables are normalised, and the coprocessor program folds them into its own
    // arithmetic rather than dividing them out.
    //
    // The factors are worth checking rather than assuming, because they hold to
    // five decimal places across four decades of argument. An index or exponent
    // mistake would not do that; it would break at a binade boundary.
    constexpr double kReciprocalScale   = 2.0;
    constexpr double kInverseSqrtScale  = 1.5;

    Unit reciprocal{"2/x", 0.0, 1.0e-3};
    Unit inverse_sqrt{"1.5/sqrt", 0.0, 1.0e-3};
    Unit arctangent{"arctangent", 0.0, 3.0e-4};

    // Show the first few disagreements. A worst-case figure says a unit is wrong;
    // examples say how, and the pattern usually names the mistake outright.
    const auto note = [](Unit& unit, double error, double at, double measured,
                         double expected) {
        ++unit.samples;
        if (error > unit.worst) {
            unit.worst = error;
            unit.at    = at;
        }
        if (error > unit.tolerance && unit.reported < 6) {
            ++unit.reported;
            std::printf("  %-11s at %12.6f: got %14.8g, expected %14.8g\n", unit.name,
                        at, measured, expected);
        }
    };

    constexpr double kTau = 6.283185307179586;

    std::printf("\n=== coprocessor mathematics ===\n");


    // Sine and cosine. The argument is a 16-bit angle over a full turn, and one
    // write answers both because the second port adds a quarter turn.
    for (u32 step = 0; step < 512; ++step) {
        const u32    angle   = step * (0x10000 / 512);
        const double radians = static_cast<double>(angle) * kTau / 65536.0;

        tgp.write_io(0x20, angle);
        const float measured_sine   = u2f(tgp.read_io(0x20));
        const float measured_cosine = u2f(tgp.read_io(0x21));

        const double got_sine   = static_cast<double>(measured_sine);
        const double got_cosine = static_cast<double>(measured_cosine);
        note(sine, std::abs(got_sine - std::sin(radians)), radians, got_sine,
             std::sin(radians));
        note(cosine, std::abs(got_cosine - std::cos(radians)), radians, got_cosine,
             std::cos(radians));
    }

    // Reciprocal and inverse square root, over several binades so the exponent
    // correction is exercised rather than just the mantissa table.
    for (u32 step = 1; step <= 400; ++step) {
        const double value = 0.05 + static_cast<double>(step) * 0.05;
        const u32    bits  = f2u(static_cast<float>(value));

        tgp.write_io(0x28, bits);

        const double measured = static_cast<double>(u2f(tgp.read_io(0x29)));
        const double expected = kReciprocalScale / value;
        note(reciprocal, std::abs(measured - expected) / expected, value, measured,
             expected);

        tgp.write_io(0x2a, bits);

        const double measured_sqrt = static_cast<double>(u2f(tgp.read_io(0x2b)));
        const double expected_sqrt = kInverseSqrtScale / std::sqrt(value);
        note(inverse_sqrt, std::abs(measured_sqrt - expected_sqrt) / expected_sqrt, value,
             measured_sqrt, expected_sqrt);
    }

    // Arctangent. The unit wants the two operands and, separately, the ratio of
    // the smaller magnitude to the larger, which the coprocessor program divides
    // out itself.
    for (u32 step = 0; step < 256; ++step) {
        const double radians = static_cast<double>(step) * kTau / 256.0;
        const double x       = std::cos(radians);
        const double y       = std::sin(radians);

        const double larger  = std::max(std::abs(x), std::abs(y));
        const double smaller = std::min(std::abs(x), std::abs(y));
        if (larger < 1.0e-6) {
            continue;
        }

        tgp.write_io(0x24, f2u(static_cast<float>(x)));
        tgp.write_io(0x25, f2u(static_cast<float>(y)));
        tgp.write_io(0x27, f2u(static_cast<float>(smaller / larger)));
        const u32 measured = tgp.read_io(0x24) & 0xffff;

        // The answer is a 16-bit angle. Compare as a fraction of a turn, taking
        // the shorter way round so a result just below zero is not an error of a
        // whole revolution.
        const double measured_turns = static_cast<double>(measured) / 65536.0;
        const double expected_turns = radians / kTau;
        double       difference     = measured_turns - expected_turns;
        difference -= std::round(difference);
        note(arctangent, std::abs(difference), radians, measured_turns, expected_turns);
    }

    std::printf("%-12s %-8s %-12s %-12s %s\n", "UNIT", "SAMPLES", "WORST ERROR",
                "TOLERANCE", "RESULT");

    bool ok = true;
    for (const Unit* unit : {&sine, &cosine, &reciprocal, &inverse_sqrt, &arctangent}) {
        const bool passed = unit->samples != 0 && unit->worst <= unit->tolerance;
        ok                = ok && passed;
        std::printf("%-12s %-8u %-12.3g %-12.3g %s (worst near %.4f)\n", unit->name,
                    unit->samples, unit->worst, unit->tolerance,
                    passed ? "pass" : "FAIL", unit->at);
    }
    std::printf("\n");
    return ok;
}

void print_tilemap_registers(const Model2& machine)
{
    const std::span<const u8> tile_ram = machine.tile_ram();
    const auto word = [&](u32 index) -> u16 {
        const u32 offset = index * 2;
        if (offset + 1 >= tile_ram.size()) {
            return 0;
        }
        return static_cast<u16>(tile_ram[offset] | (tile_ram[offset + 1] << 8));
    };

    std::printf("\n=== tilemap control registers ===\n");
    std::printf("%-6s %-8s %-8s %-8s %-9s %-6s %-6s %s\n", "LAYER", "HSCROLL", "VSCROLL",
                "CTRL", "ROWSCROLL", "OFF", "MODE", "ROW TABLE 0..7");
    for (u32 layer = 0; layer < kTileLayers; ++layer) {
        const u16 hscroll = word(0x5000 + layer);
        const u16 vscroll = word(0x5004 + layer);
        const u16 ctrl    = word(0x5004 + (layer & 2));

        std::string rows;
        for (u32 line = 0; line < 8; ++line) {
            rows += (line != 0 ? " " : "");
            char text[8];
            std::snprintf(text, sizeof(text), "%03x", word(0x4000 + 0x200 * layer + line) & 0x1ff);
            rows += text;
        }

        std::printf("%-6u %04x     %04x     %04x     %-9s %-6s %-6u %s\n", layer, hscroll,
                    vscroll, ctrl, (hscroll & 0x8000) != 0 ? "yes" : "no",
                    (vscroll & 0x8000) != 0 ? "yes" : "no", (ctrl & 0x6000) >> 13,
                    rows.c_str());
    }

    // Non-zero mask words mean the pair is being split by position rather than
    // one layer covering everything.
    for (u32 pair = 0; pair < 2; ++pair) {
        const u32 base = pair == 0 ? 0x6000 : 0x6800;
        u32       set  = 0;
        for (u32 line = 0; line < Model2::kVisibleHeight; ++line) {
            for (u32 index = 0; index < 4; ++index) {
                set += word(base + line * 4 + index) != 0 ? 1u : 0u;
            }
        }
        std::printf("pair %c window mask: %u of %u words non-zero\n", pair == 0 ? 'A' : 'B',
                    set, Model2::kVisibleHeight * 4);
    }
}

void print_tilemap_summary(const Model2& machine)
{
    print_tilemap_registers(machine);

    std::printf("\n=== tilemap layers ===\n");
    std::printf("%-6s %-12s %-12s %-12s\n", "LAYER", "CELLS USED", "FOREGROUND",
                "PIXELS SET");

    std::array<std::vector<u8>, kTileLayers> images;
    std::array<LayerStats, kTileLayers>      stats;
    bool anything_drawn = false;

    for (u32 layer = 0; layer < kTileLayers; ++layer) {
        images[layer] = render_layer(machine, layer);
        stats[layer]  = measure_layer(machine, layer, images[layer]);
        std::printf("%-6u %-12u %-12u %-12u\n", layer, stats[layer].cells_used,
                    stats[layer].cells_foreground, stats[layer].pixels_set);
        anything_drawn = anything_drawn || stats[layer].pixels_set != 0;
    }

    if (!anything_drawn) {
        std::printf("\nNothing has been drawn into any layer.\n");
        return;
    }

    // A coarse ASCII rendering of each populated layer's visible area, so the
    // answer to "is there text on screen?" needs no image viewer.
    //
    // Any non-zero cell maps to at least the first shading step. Scaling linearly
    // instead would round a uniform low-intensity fill down to blank and hide a
    // whole layer, which is exactly what happened the first time.
    static constexpr char kRamp[] = ".:-=+*#%@";
    static constexpr u32  kSteps  = sizeof(kRamp) - 1;

    for (u32 layer = 0; layer < kTileLayers; ++layer) {
        if (stats[layer].pixels_set == 0) {
            continue;
        }
        std::printf("\nLayer %u, visible area, one character per 8x8 tile:\n", layer);

        const std::vector<u8>& image = images[layer];
        for (u32 cell_y = 0; cell_y < Model2::kVisibleHeight / kTileSize; ++cell_y) {
            std::string line;
            for (u32 cell_x = 0; cell_x < Model2::kVisibleWidth / kTileSize; ++cell_x) {
                u32 total = 0;
                for (u32 y = 0; y < kTileSize; ++y) {
                    const u8* row = image.data()
                                  + static_cast<usize>(cell_y * kTileSize + y) * kMapPixels
                                  + cell_x * kTileSize;
                    for (u32 x = 0; x < kTileSize; ++x) {
                        total += row[x];
                    }
                }
                if (total == 0) {
                    line += ' ';
                    continue;
                }
                const u32 average = total / (kTileSize * kTileSize);
                const u32 step    = std::min(average * kSteps / 256, kSteps - 1);
                line += kRamp[step];
            }
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
            if (!line.empty()) {
                std::printf("%3u |%s\n", cell_y, line.c_str());
            }
        }
    }
    std::printf("\n");
}

bool write_wav(const std::string& path, std::span<const s16> samples, u32 sample_rate)
{
    // Canonical 44-byte RIFF header, little-endian throughout, which is also the
    // sample order, so the data can be written straight out on either platform we
    // build for.
    static_assert(std::endian::native == std::endian::little,
                  "the WAV writer assumes a little-endian host");

    constexpr u16 kChannels      = 2;
    constexpr u16 kBitsPerSample = 16;
    constexpr u16 kPcm           = 1;

    const u32 data_bytes  = static_cast<u32>(samples.size_bytes());
    const u32 byte_rate   = sample_rate * kChannels * (kBitsPerSample / 8);
    const u16 block_align = kChannels * (kBitsPerSample / 8);

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        SM2_ERROR("could not open '%s' for writing", path.c_str());
        return false;
    }

    const auto put32 = [file](u32 value) { std::fwrite(&value, 4, 1, file); };
    const auto put16 = [file](u16 value) { std::fwrite(&value, 2, 1, file); };

    std::fwrite("RIFF", 1, 4, file);
    put32(36 + data_bytes);
    std::fwrite("WAVE", 1, 4, file);
    std::fwrite("fmt ", 1, 4, file);
    put32(16);
    put16(kPcm);
    put16(kChannels);
    put32(sample_rate);
    put32(byte_rate);
    put16(block_align);
    put16(kBitsPerSample);
    std::fwrite("data", 1, 4, file);
    put32(data_bytes);
    if (data_bytes != 0) {
        std::fwrite(samples.data(), 1, data_bytes, file);
    }

    const bool ok = std::ferror(file) == 0;
    std::fclose(file);
    if (!ok) {
        SM2_ERROR("could not write '%s'", path.c_str());
        return false;
    }

    SM2_INFO("wrote %s (%zu frame(s), %.2f s at %u Hz)", path.c_str(),
             samples.size() / kChannels,
             double(samples.size() / kChannels) / double(sample_rate), sample_rate);
    return true;
}

}  // namespace sm2::hw
