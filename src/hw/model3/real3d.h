// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Model 3's Real3D Pro-1000, emulation side. Drawing is the renderer's,
// from the VideoFrame snapshot.
//
// Derived from Supermodel's Src/Model3/Real3D.cpp and Real3D.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2003-2026 The Supermodel Team.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// Renderer state is published in VideoFrame, and line-of-sight is pushed back
// through set_line_of_sight(). There are two alternating published snapshots,
// each with per-4 KiB dirty bits. Update-buffer pointers are word indices, and
// all buffer and texture source accesses are bounds-checked.
#pragma once

#include "core/types.h"
#include "hw/model3/jtag.h"
#include "hw/model3/mpc10x.h"
#include "hw/model3/video_frame.h"

#include <array>
#include <span>
#include <vector>

namespace sm2 {
class Archive;
}

namespace sm2::cpu::ppc {
class PpcBus;
}

namespace sm2::hw::model3 {

class IrqController;

class Real3D : public PciDevice, public JtagModewordSink {
public:
    /// See real3d.cpp.
    static constexpr u32 kPciIdStep1x = 0x16C311DB;
    static constexpr u32 kPciIdStep2x = 0x178611DB;

    static constexpr u32 kCullLoWords  = VideoFrame::kCullingLoWords;
    static constexpr u32 kCullHiWords  = VideoFrame::kCullingHiWords;
    static constexpr u32 kPolyWords    = VideoFrame::kPolygonWords;
    static constexpr u32 kTexels       = VideoFrame::kTextureSize * VideoFrame::kTextureSize;
    static constexpr u32 kFifoWords    = 0x100000 / 4;
    static constexpr u32 kVromWords    = 0x4000000 / 4;

    Real3D();

    void attach(cpu::ppc::PpcBus* bus, IrqController* irq, u32 dma_irq_bit);
    void set_stepping(int step);
    /// `vrom`: 64 MB of host-native words, owned by the caller.
    void set_vrom(std::span<const u32> vrom) { m_vrom = vrom; }
    [[nodiscard]] int stepping() const { return m_step; }

    void reset();

    // -- PowerPC-facing ------------------------------------------------------------
    // Data is as the Real3D sees it: the byte-reverse of the PowerPC's store.

    void write_low_culling(u32 addr, u32 data);
    void write_high_culling(u32 addr, u32 data);
    void write_polygon(u32 addr, u32 data);
    void write_texture_fifo(u32 data);
    void write_texture_port(unsigned reg, u32 data);
    void write_config_register(u32 addr, u32 data);
    /// 0x88000000: the command port.
    void flush();

    [[nodiscard]] u32 read_register(unsigned reg) const;

    [[nodiscard]] u8  read_dma8(unsigned reg) const;
    [[nodiscard]] u32 read_dma32(unsigned reg) const;
    void write_dma8(unsigned reg, u8 data);
    void write_dma32(unsigned reg, u32 data);

    // -- frame timing (driven by the machine's schedule) ---------------------------

    void begin_vblank();
    void end_vblank() { m_error = false; }
    void flip_ping_pong();
    /// The tile generator's register 0x0C was written.
    void tilegen_draw_frame(u32 flags);

    // -- PCI / JTAG ----------------------------------------------------------------

    [[nodiscard]] u32 read_pci_config(unsigned device, unsigned reg, unsigned bits,
                                      unsigned offset) const override;
    void write_pci_config(unsigned device, unsigned reg, unsigned bits, unsigned offset,
                          u32 data) override;
    void write_jtag_modeword(Asic device, u32 data) override;

    // -- renderer feedback and publication -----------------------------------------

    /// Priority level 0-3. Between frames only.
    void set_line_of_sight(unsigned index, float value);

    /// Syncs snapshot `buffer` (0/1) into `frame`; clears the upload list.
    void publish(unsigned buffer, VideoFrame& frame);

    /// After a load; the next publish sets texture_reset.
    void invalidate_all();

    void serialize(Archive& ar);

    // -- views for debugging ---------------------------------------------------------
    [[nodiscard]] std::span<const u32> culling_lo() const { return m_cull_lo; }
    [[nodiscard]] std::span<const u32> culling_hi() const { return m_cull_hi; }
    [[nodiscard]] std::span<const u32> polygon_ram() const { return m_poly; }
    [[nodiscard]] std::span<const u16> texture_ram() const { return m_tex; }
    [[nodiscard]] const std::vector<TextureRect>& pending_uploads() const { return m_uploads; }
    [[nodiscard]] bool ping_pong() const { return m_ping_pong != 0; }
    [[nodiscard]] u32 fifo_index() const { return m_fifo_idx; }

    /// Decode one texture upload (header plus data) into texture RAM.
    void upload_texture(u32 header, std::span<const u16> data);

private:
    /// Reads past the end return 0.
    struct TexSource {
        std::span<const u16> data;
        usize                pos = 0;
        [[nodiscard]] u16 at(usize i) const
        {
            return pos + i < data.size() ? data[pos + i] : u16{0};
        }
    };

    void store_texture(unsigned level, u32 x, u32 y, u32 width, u32 height, TexSource& src,
                       bool sixteen_bit, bool write_lsb, bool write_msb, u32& consumed);
    void flush_textures();
    [[nodiscard]] bool poll_ping_pong() const { return m_ping_pong != m_ping_pong_copy; }
    void draw_frame();
    void dma_copy();
    void buffer_update(s64& block, u32 base, u32 addr, u32 data);
    void sync_buffered(s64 block, u32 base, std::vector<u32>& dst, std::vector<u8>& dirty);

    // Low culling RAM access for the update blocks, bounds-checked.
    [[nodiscard]] u32 lo(s64 i) const;
    void set_lo(s64 i, u32 v);

    // Dirty pages: one byte per 4 KiB page, bit n = published buffer n stale.
    static void mark(std::vector<u8>& dirty, usize byte_offset) { dirty[byte_offset >> 12] = 3; }

    cpu::ppc::PpcBus* m_bus = nullptr;
    IrqController*    m_irq = nullptr;
    u32               m_dma_irq = 0;

    std::span<const u32> m_vrom;
    int m_step   = 0x10;
    u32 m_pci_id = kPciIdStep1x;
    bool m_error = false;

    std::vector<u32> m_cull_lo;
    std::vector<u32> m_cull_hi;
    std::vector<u32> m_poly;
    std::vector<u16> m_tex;
    std::vector<u32> m_fifo;
    u32 m_fifo_idx = 0;
    std::array<u32, 2> m_vrom_fifo{};
    u32 m_vrom_fifo_idx = 0;

    /// pingPongMemSize, flags, pingPongAndUpdateMemSize, unused.
    std::array<u32, 4> m_config_regs{};

    u32 m_dma_src = 0, m_dma_dest = 0, m_dma_length = 0, m_dma_data = 0, m_dma_unknown = 0;
    u8  m_dma_status = 0, m_dma_config = 0;

    bool m_command_port_written = false;
    bool m_tilegen_draw_frame   = false;
    u32  m_ping_pong      = 0;
    u32  m_ping_pong_copy = 0;
    bool m_block_culling  = false;
    std::array<u64, 2> m_internal_render_config{};
    std::array<u32, 5> m_modeword{};
    std::array<float, 4> m_los{};

    /// Word index of the last update block in low culling RAM, or -1.
    s64 m_poly_update_block = -1;
    s64 m_high_update_block = -1;

    std::vector<TextureRect> m_uploads;
    bool m_texture_reset = true;

    std::vector<u8> m_dirty_lo, m_dirty_hi, m_dirty_poly, m_dirty_tex;
    struct Snapshot {
        std::vector<u32> cull_lo, cull_hi, poly;
        std::vector<u16> tex;
    };
    std::array<Snapshot, 2> m_snap;
};

}  // namespace sm2::hw::model3
