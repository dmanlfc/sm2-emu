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
#pragma once

#include "core/types.h"
#include "hw/segaic24.h"

#include <span>
#include <string>

namespace sm2::hw {

class Model2;

/// Decode the System 24 tilemap layers and write them out for inspection.
///
/// Produces one 512x512 greyscale image per layer plus a text summary. This is a
/// diagnostic, not part of rendering: it exists so that "did the program draw
/// anything?" can be answered independently of the Vulkan side, which makes a
/// later rendering bug easy to attribute to one or the other.
///
/// Tiles are drawn with their raw 4-bit pixel values scaled to full range rather
/// than through the palette, which is enough to make text legible while
/// depending on nothing but the tile format.
bool dump_tilemaps(const Model2& machine, const std::string& directory);

/// What the geometry engine produced this frame, and where it went.
///
/// Reports how many polygons were generated, culled and clipped away, the depth
/// and window range they occupy, and the screen-space extent they cover. A list
/// whose extent lies outside the 496x384 raster is the usual symptom of a wrong
/// projection or a wrong focal distance, and says so far more directly than a
/// blank screen does.
void print_render_list_summary(const Model2& machine);

/// Draw this frame's polygons as a wireframe and write it as a PPM.
///
/// A wireframe rather than filled triangles on purpose: it shows the geometry
/// itself, including polygons that a fill would hide behind nearer ones, so the
/// question "is the shape right?" can be answered before any pixel stage exists.
bool dump_render_list_wireframe(const Model2& machine, const std::string& directory);

/// Check the coprocessor's mathematical units against the standard library.
///
/// The units are lookup tables plus index and exponent arithmetic. Every part of
/// that can be wrong while still returning a plausible number, and the result
/// feeds straight into the geometry, so a wrong answer shows up as a scene that
/// is subtly the wrong shape rather than as a failure.
///
/// This is the one check that uses the real table ROM, so it is the only one that
/// can say the answers are actually right rather than merely self-consistent.
/// Needs a machine with ROMs loaded. Returns false if any unit is out of
/// tolerance.
bool run_copro_selftest(Model2& machine);

/// The tilemap chip's scroll, control and window mask state.
///
/// This is what says whether a layer is scrolling, disabled, using per-line
/// scroll, or splitting a pair between its two maps. A wrong picture is almost
/// always explained here before it is explained anywhere else.
void print_tilemap_registers(const Model2& machine);

/// One-line-per-layer summary of how much of each tilemap is populated, plus an
/// ASCII rendering of every populated layer. Written to stdout.
void print_tilemap_summary(const Model2& machine);

/// Write interleaved stereo samples to a RIFF WAV file.
///
/// The point of writing audio out rather than only counting samples is that a
/// counter cannot tell silence from noise from music. A file can be listened to,
/// and can be compared against the same passage from MAME.
bool write_wav(const std::string& path, std::span<const s16> samples, u32 sample_rate);

/// Write the composed 2D output as PPM images: the layers below the 3D pass, the
/// layers above it, and the two flattened over the background colour. This goes
/// through the real colour chain, so it is the check on the palette and the
/// compositor rather than on the tile format.
///
/// Takes a mutable machine because it re-draws individual layers into scratch
/// buffers, which uses the tile chip's own drawing path rather than a
/// reimplementation of it.
bool dump_composed_frame(Model2& machine, const std::string& directory);

}  // namespace sm2::hw
