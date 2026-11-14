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
// SPDX-License-Identifier: GPL-3.0-or-later AND BSD-3-Clause
//
// This header must not be removed. Keisho is licensed under the GNU General
// Public License, version 3 or later, with additional terms under section 7 of
// that licence. See LICENSE and NOTICE.
//
#pragma once

#include "core/types.h"
#include "hw/segaic24.h"

#include <span>
#include <string>

namespace sm2::hw {

class CoproTgp;
class Model2MachineBase;

/// Decode the System 24 tilemap layers and write them out for inspection.
///
/// Produces one 512x512 greyscale image per layer plus a text summary, using raw
/// 4-bit pixel values rather than the palette.
bool dump_tilemaps(const Model2MachineBase& machine, const std::string& directory);

/// What the geometry engine produced this frame, and where it went.
///
/// Reports polygon counts (generated, culled, clipped), depth and window range,
/// and screen-space extent.
void print_render_list_summary(const Model2MachineBase& machine);

/// Write the geometry engine's input verbatim, plus a disassembly of it.
///
/// Comparable byte for byte with MAME's `bufferram` share at 0x00900000.
bool dump_display_list(const Model2MachineBase& machine, const std::string& directory);

/// Write both framebuffer banks verbatim. MAME's equivalent lives at 0x11600000
/// and 0x11680000 in i960 space, so the two are comparable as bytes.
bool dump_framebuffer(const Model2MachineBase& machine, const std::string& directory);

/// Render the frame on the CPU with the software port of MAME's rasteriser and
/// write it as a PPM.
///
/// `frame_number` of -1 writes software_frame.ppm; anything else numbers the file
/// after the frame.
bool dump_software_frame(const Model2MachineBase& machine, const std::string& directory,
                         int frame_number = -1);

/// Draw this frame's polygons as a wireframe, including hidden ones, and write
/// it as a PPM.
bool dump_render_list_wireframe(const Model2MachineBase& machine, const std::string& directory);

/// Check the coprocessor's mathematical units against the standard library.
///
/// Needs the table ROM attached. Returns false if any unit is out of tolerance.
/// Takes the coprocessor because both Model 2A and the original Model 2 carry it.
bool run_copro_selftest(CoproTgp& copro);

/// The tilemap chip's scroll, control and window mask state.
void print_tilemap_registers(const Model2MachineBase& machine);

/// One-line-per-layer summary of how much of each tilemap is populated, plus an
/// ASCII rendering of every populated layer. Written to stdout.
void print_tilemap_summary(const Model2MachineBase& machine);

/// Write interleaved stereo samples to a RIFF WAV file.
bool write_wav(const std::string& path, std::span<const s16> samples, u32 sample_rate);

/// Write the composed 2D output as PPM images: the layers below the 3D pass, the
/// layers above it, and the two flattened over the background colour, through
/// the real colour chain. The machine is mutable because layers are re-drawn into
/// scratch buffers.
bool dump_composed_frame(Model2MachineBase& machine, const std::string& directory);

}  // namespace sm2::hw
