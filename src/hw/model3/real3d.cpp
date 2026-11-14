// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Model 3's Real3D Pro-1000 graphics hardware, emulation side.
//
// Derived from Supermodel's Src/Model3/Real3D.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2003-2026 The Supermodel Team.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// PCI IDs: the PCI configuration space appears to return the PCI ID of Mercury
// (0x16C311DB) on Step 1.x and of the DMA device (0x178611DB) on Step 2.x, while
// the Step 2.x DMA device register returns Mercury's ID. Step 2.x games by AM3
// expect this.
//
// Games can start writing data for the next frame when the ping_pong bit flips,
// often at 66% of the frame time, so the CPU queues the next frame while the
// current one is being generated. Those writes go to a temporary buffer at the
// start of low culling memory, sized by configuration registers written at boot.
// At the start of vblank the buffered data is copied to its real locations.

#include "hw/model3/real3d.h"

#include "core/archive.h"
#include "hw/model3/byteswap.h"
#include "core/log.h"
#include "cpu/ppc/ppc.h"
#include "hw/model3/irq.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace sm2::hw::model3 {
namespace {

// Mipmap coordinates for each reduction level (within a single 2048x1024 page).
constexpr int kMipXBase[]   = {0, 1024, 1536, 1792, 1920, 1984, 2016, 2032, 2040, 2044, 2046, 2047};
constexpr int kMipYBase[]   = {0, 512, 768, 896, 960, 992, 1008, 1016, 1020, 1022, 1023};
constexpr int kMipDivisor[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024};

// Tables of texel offsets corresponding to an NxN texel texture tile.
constexpr unsigned kDecode8x8[64] = {
     1, 0, 5, 4, 9, 8,13,12,
     3, 2, 7, 6,11,10,15,14,
    17,16,21,20,25,24,29,28,
    19,18,23,22,27,26,31,30,
    33,32,37,36,41,40,45,44,
    35,34,39,38,43,42,47,46,
    49,48,53,52,57,56,61,60,
    51,50,55,54,59,58,63,62,
};
constexpr unsigned kDecode8x4[32] = {
     1, 0, 5, 4,
     3, 2, 7, 6,
     9, 8,13,12,
    11,10,15,14,
    17,16,21,20,
    19,18,23,22,
    25,24,29,28,
    27,26,31,30,
};
constexpr unsigned kDecode8x2[16] = {
     1, 0,
     3, 2,
     5, 4,
     7, 6,
     9, 8,
    11,10,
    13,12,
    15,14,
};

std::span<const u16> as_u16(std::span<const u32> words)
{
    return {reinterpret_cast<const u16*>(words.data()), words.size() * 2};
}

}  // namespace

Real3D::Real3D()
    : m_cull_lo(kCullLoWords)
    , m_cull_hi(kCullHiWords)
    , m_poly(kPolyWords)
    , m_tex(kTexels)
    , m_fifo(kFifoWords)
    , m_dirty_lo(kCullLoWords * 4 / 4096, 3)
    , m_dirty_hi(kCullHiWords * 4 / 4096, 3)
    , m_dirty_poly(kPolyWords * 4 / 4096, 3)
    , m_dirty_tex(kTexels * 2 / 4096, 3)
{
    for (Snapshot& s : m_snap) {
        s.cull_lo.assign(kCullLoWords, 0);
        s.cull_hi.assign(kCullHiWords, 0);
        s.poly.assign(kPolyWords, 0);
        s.tex.assign(kTexels, 0);
    }
}

void Real3D::attach(cpu::ppc::PpcBus* bus, IrqController* irq, u32 dma_irq_bit)
{
    m_bus     = bus;
    m_irq     = irq;
    m_dma_irq = dma_irq_bit;
}

void Real3D::set_stepping(int step)
{
    m_step = step;
    if (m_step != 0x10 && m_step != 0x15 && m_step != 0x20 && m_step != 0x21) {
        SM2_WARN("real3d: unrecognized stepping %d.%d", (step >> 4) & 0xF, step & 0xF);
        m_step = 0x10;
    }
    m_pci_id = m_step >= 0x20 ? kPciIdStep2x : kPciIdStep1x;
}

void Real3D::reset()
{
    m_error                = false;
    m_ping_pong            = 0;
    m_ping_pong_copy       = 0;
    m_command_port_written = false;
    m_tilegen_draw_frame   = false;
    m_block_culling        = false;
    m_fifo_idx             = 0;
    m_vrom_fifo_idx        = 0;
    m_dma_src = m_dma_dest = m_dma_length = m_dma_data = m_dma_unknown = 0;
    m_dma_status = m_dma_config = 0;
    m_config_regs.fill(0);
    m_poly_update_block = -1;
    m_high_update_block = -1;
    std::fill(m_cull_lo.begin(), m_cull_lo.end(), 0u);
    std::fill(m_cull_hi.begin(), m_cull_hi.end(), 0u);
    std::fill(m_poly.begin(), m_poly.end(), 0u);
    std::fill(m_tex.begin(), m_tex.end(), u16{0});
    std::fill(m_fifo.begin(), m_fifo.end(), 0u);
    m_vrom_fifo.fill(0);
    m_internal_render_config.fill(0);
    m_modeword.fill(0);
    m_los.fill(0.0f);
    m_uploads.clear();
    invalidate_all();
}

void Real3D::invalidate_all()
{
    for (auto* d : {&m_dirty_lo, &m_dirty_hi, &m_dirty_poly, &m_dirty_tex}) {
        std::fill(d->begin(), d->end(), u8{3});
    }
    m_texture_reset = true;
}

// -- vblank, ping-pong and frame triggering ----------------------------------------

void Real3D::begin_vblank()
{
    // Kept to tell whether the bit flips during the frame.
    m_ping_pong_copy = m_ping_pong;

    sync_buffered(m_high_update_block, 0, m_cull_hi, m_dirty_hi);
    sync_buffered(m_poly_update_block, m_config_regs[0], m_poly, m_dirty_poly);
    m_poly_update_block = -1;
    m_high_update_block = -1;

    // Update any textures if they were written after the ping_pong time.
    if (m_command_port_written) flush_textures();

    // Check to see if a complete frame was written after the ping_pong time.
    if (m_tilegen_draw_frame && m_command_port_written) draw_frame();
}

void Real3D::flip_ping_pong()
{
    m_ping_pong = m_ping_pong != 0 ? 0 : 1;
    // This is the last time writes can happen for this frame; any writes after
    // are buffered for the next frame, so the drawing flags reset here.
    m_tilegen_draw_frame   = false;
    m_command_port_written = false;
}

void Real3D::draw_frame()
{
    // Real hardware starts drawing here; drawing at the end of the frame gives
    // the same result.
    m_tilegen_draw_frame   = false;
    m_command_port_written = false;
    // Block culling is latched when a frame is triggered.
    m_block_culling = (m_modeword[static_cast<int>(Asic::Mercury)] & 4) == 4;
}

void Real3D::tilegen_draw_frame(u32 flags)
{
    (void)flags;
    m_tilegen_draw_frame = true;
    if (!poll_ping_pong() && m_command_port_written) draw_frame();
}

void Real3D::flush()
{
    m_command_port_written = true;
    if (!poll_ping_pong()) {
        flush_textures();
        if (m_tilegen_draw_frame) draw_frame();
    }
}

// -- update buffers ------------------------------------------------------------------
//
// An update block is [startAddr, lastAddr, data...] in low culling RAM; the next
// block follows the last one's data.

u32 Real3D::lo(s64 i) const
{
    return i >= 0 && i < static_cast<s64>(kCullLoWords) ? m_cull_lo[static_cast<usize>(i)] : 0u;
}

void Real3D::set_lo(s64 i, u32 v)
{
    if (i < 0 || i >= static_cast<s64>(kCullLoWords)) {
        if (!m_error) SM2_WARN("real3d: update buffer overflow");
        m_error = true;
        return;
    }
    m_cull_lo[static_cast<usize>(i)] = v;
    mark(m_dirty_lo, static_cast<usize>(i) * 4);
}

void Real3D::buffer_update(s64& block, u32 base, u32 addr, u32 data)
{
    if (block < 0) {
        block = base;
        set_lo(block, addr);
    } else if (lo(block + 1) + 1 != addr) {
        const u32 count = lo(block + 1) - lo(block) + 1;
        block += 2 + static_cast<s64>(count);
        set_lo(block, addr);
    }
    set_lo(block + 1, addr);
    const u32 offset = addr - lo(block);
    set_lo(block + 2 + static_cast<s64>(offset), data);
}

void Real3D::sync_buffered(s64 block, u32 base, std::vector<u32>& dst, std::vector<u8>& dirty)
{
    if (block < 0) return;
    s64 cur = base;
    while (cur <= block) {
        const u32 start = lo(cur);
        const u32 count = lo(cur + 1) - start + 1;
        for (u32 i = 0; i < count; ++i) {
            const u32 a = start + i;
            if (a >= dst.size()) break;
            dst[a] = lo(cur + 2 + i);
            mark(dirty, usize{a} * 4);
        }
        if (cur == block) break;
        cur += 2 + static_cast<s64>(count);
    }
}

// -- memories --------------------------------------------------------------------------

void Real3D::write_low_culling(u32 addr, u32 data)
{
    const u32 i = (addr & 0x3FFFFF) / 4;
    m_cull_lo[i] = data;
    mark(m_dirty_lo, usize{i} * 4);
}

void Real3D::write_high_culling(u32 addr, u32 data)
{
    addr &= 0xFFFFF;
    // After the ping-pong flip the data is buffered; before it, written directly.
    if (poll_ping_pong()) {
        buffer_update(m_high_update_block, 0, addr / 4, data);
    } else {
        m_cull_hi[addr / 4] = data;
        mark(m_dirty_hi, addr);
    }
}

void Real3D::write_polygon(u32 addr, u32 data)
{
    addr &= 0x3FFFFF;
    if (poll_ping_pong()) {
        buffer_update(m_poly_update_block, m_config_regs[0], addr / 4, data);
    } else {
        m_poly[addr / 4] = data;
        mark(m_dirty_poly, addr);
    }
}

void Real3D::write_config_register(u32 addr, u32 data)
{
    // reg 0 = ping pong buffer size (high culling RAM) in 32-bit words
    // reg 4 = flags, probably overload mode etc
    // reg 8 = combined size of the ping pong buffer and the update buffer
    const u32 reg = addr & 0xF;
    m_config_regs[reg / 4] = data;
}

void Real3D::write_texture_fifo(u32 data)
{
    if (m_fifo_idx >= kFifoWords) {
        if (!m_error) SM2_WARN("real3d: overflow in texture FIFO");
        m_error = true;
    } else {
        m_fifo[m_fifo_idx++] = data;
    }
}

void Real3D::write_texture_port(unsigned reg, u32 data)
{
    (void)reg;
    if (m_vrom.empty()) return;
    if (m_step == 0x10) {
        const u32 addr      = data & 0xFFFFFF;
        const u32 num_words = (2 + m_vrom[addr] / 2) / 4;
        if (num_words == 0) {
            SM2_DEBUG("real3d: 0-length VROM texture upload (%08X)", data);
            return;
        }
        for (u32 i = 0; i < num_words; ++i) write_texture_fifo(m_vrom[(addr + i) & 0xFFFFFF]);
    } else if (m_vrom_fifo_idx == 2) {
        const u32 addr   = m_vrom_fifo[0];
        const u32 header = m_vrom_fifo[1];
        // The first 4 MB maps to polygon RAM rather than VROM. Daytona 2 PE uses
        // zeroed data to replace unused Dreamcast logo textures.
        if (addr < 0x100000) {
            upload_texture(header, as_u16(std::span<const u32>(m_poly)).subspan(usize{addr} * 2));
        } else {
            upload_texture(header, as_u16(m_vrom).subspan(usize{addr & 0xFFFFFF} * 2));
        }
        m_vrom_fifo_idx = 0;
    } else {
        m_vrom_fifo[m_vrom_fifo_idx++] = data;
    }
}

// -- textures --------------------------------------------------------------------------

void Real3D::flush_textures()
{
    // If the texture header/data aren't present, discard the texture (prevents
    // garbage textures in Ski Champ).
    if (m_fifo_idx > 2) {
        const auto fifo16 = as_u16(std::span<const u32>(m_fifo));
        for (u32 i = 0; i < m_fifo_idx - 2;) {
            const u32 size   = (2 + m_fifo[i] / 2) / 4;
            const u32 header = m_fifo[i + 1];
            // Spikeout uploads zero-length textures.
            if (size == 0) {
                SM2_DEBUG("real3d: 0-length texture upload (%08X %08X %08X)", m_fifo[i],
                          m_fifo[i + 1], m_fifo[i + 2]);
                break;
            }
            upload_texture(header, fifo16.subspan(usize{i + 2} * 2));
            i += size;
        }
    }
    m_fifo_idx = 0;
}

void Real3D::store_texture(unsigned level, u32 x_pos, u32 y_pos, u32 width, u32 height,
                           TexSource& src, bool sixteen_bit, bool write_lsb, bool write_msb,
                           u32& consumed)
{
    const u32 tile_x = std::min(8u, width);
    const u32 tile_y = std::min(8u, height);
    const unsigned* const decode = tile_x == 8 ? kDecode8x8 : tile_x == 4 ? kDecode8x4 : kDecode8x2;
    const usize decode_size = tile_x == 8 ? 64 : tile_x == 4 ? 32 : 16;
    auto dec = [&](u32 i) { return i < decode_size ? decode[i] : 0u; };

    consumed = 0;
    if (sixteen_bit) {
        for (u32 y = y_pos; y < y_pos + height; y += tile_y) {
            for (u32 x = x_pos; x < x_pos + width; x += tile_x) {
                u32 dest = y * 2048 + x;
                for (u32 yy = 0; yy < tile_y; ++yy) {
                    for (u32 xx = 0; xx < tile_x; ++xx) {
                        if (dest < kTexels) {
                            m_tex[dest] = src.at(dec(yy * tile_x + xx));
                            mark(m_dirty_tex, usize{dest} * 2);
                        }
                        ++dest;
                        ++consumed;
                    }
                    dest += 2048 - tile_x;  // next line
                }
                src.pos += tile_y * tile_x;  // next tile
            }
        }
    } else {
        // 8-bit textures appear to be unpacked into 16-bit words in the texture
        // RAM. Oddly, the rows of the decoding table seem to be swapped.
        if (write_lsb && write_msb) SM2_DEBUG("real3d: 8-bit texture with byte_select=3");
        const u8 byte_select = static_cast<u8>((write_lsb ? 1 : 0) | (write_msb ? 2 : 0));
        static constexpr u16 kByteMask[4] = {0xFFFF, 0xFF00, 0x00FF, 0x0000};
        const u32 step = std::max(1u, (tile_y * tile_x) / 2);
        for (u32 y = y_pos; y < y_pos + height; y += tile_y) {
            for (u32 x = x_pos; x < x_pos + width; x += tile_x) {
                u32 dest = y * 2048 + x;
                for (u32 yy = 0; yy < tile_y; ++yy) {
                    for (u32 xx = 0; xx < tile_x; ++xx) {
                        if ((write_lsb || write_msb) && dest < kTexels) {
                            u16& texel = m_tex[dest];
                            mark(m_dirty_tex, usize{dest} * 2);
                            texel &= kByteMask[byte_select];
                            const u32 shift = 8 * ((xx & 1) ^ 1);
                            const u32 index = (yy ^ 1) * tile_x + (xx ^ 1) - (tile_x & 1);
                            u16 temp = static_cast<u16>((src.at(dec(index) / 2) >> shift) & 0xFF);
                            temp = static_cast<u16>(temp | (temp << 8));
                            temp &= static_cast<u16>(kByteMask[byte_select] ^ 0xFFFF);
                            texel |= temp;
                        }
                        ++dest;
                    }
                    dest += 2048 - tile_x;
                }
                src.pos += step;
                consumed += step;
            }
        }
    }

    // Tell the renderer which region changed.
    m_uploads.push_back(TextureRect{x_pos, y_pos, width, height, level});
}

/*
Texture header:
-------- -------- -------- --xxxxxx X-position
-------- -------- ----xxxx x------- Y-position
-------- -------x xx------ -------- Width
-------- ----xxx- -------- -------- Height
-------- ---x---- -------- -------- Texture page
-------- --x----- -------- -------- Write 8-bit data to the lower byte of texel
-------- -x------ -------- -------- Write 8-bit data to the upper byte of texel
-------- x------- -------- -------- Bitdepth, 0 = 8-bit, 1 = 16-bit
xxxxxxxx -------- -------- -------- Texture type:
                                      0x00 = texture with mipmaps
                                      0x01 = texture without mipmaps
                                      0x02 = only mipmaps
                                      0x80 = possibly gamma table
*/
void Real3D::upload_texture(u32 header, std::span<const u16> data)
{
    // Position: texture RAM is arranged as 2 2048x1024 texel sheets.
    const u32 x           = 32 * (header & 0x3F);
    const u32 y           = 32 * ((header >> 7) & 0x1F);
    const u32 page        = (header >> 20) & 1;
    u32       width       = 32u << ((header >> 14) & 7);
    u32       height      = 32u << ((header >> 17) & 7);
    const u32 type        = (header >> 24) & 0xFF;
    const bool sixteen    = ((header >> 23) & 1) != 0;
    const bool write_msb  = ((header >> 22) & 1) != 0;
    const bool write_lsb  = ((header >> 21) & 1) != 0;
    u32 consumed = 0;
    TexSource src{data, 0};

    switch (type) {
        case 0x00:  // texture w/ mipmaps
        case 0x01:  // texture w/out mipmaps
            store_texture(0, x, y + page * 1024, width, height, src, sixteen, write_lsb, write_msb, consumed);
            if (type == 0x01) break;
            [[fallthrough]];
        case 0x02:  // mipmaps only
            for (int i = 1; width >= 4 && height >= 4 && i < 11; ++i) {
                const u32 xpos = static_cast<u32>(kMipXBase[i]) + x / static_cast<u32>(kMipDivisor[i]);
                const u32 ypos = static_cast<u32>(kMipYBase[i]) + y / static_cast<u32>(kMipDivisor[i]);
                width /= 2;
                height /= 2;
                store_texture(static_cast<unsigned>(i), xpos, ypos + page * 1024, width, height, src,
                              sixteen, write_lsb, write_msb, consumed);
            }
            break;
        case 0x80:  // MAME thinks these might be a gamma table (vf3 uploads this first)
            break;
        default:
            SM2_DEBUG("real3d: unknown texture format %02X", type);
            break;
    }
}

// -- DMA device ------------------------------------------------------------------------
//
// Register 0xC: bit 7 BUS (busy, see von2 0x18A104), bit 0 IRQ pending.

void Real3D::dma_copy()
{
    SM2_TRACE("real3d: DMA %08X -> %08X, %X%s", m_dma_src, m_dma_dest, m_dma_length * 4,
              (m_dma_config & 0x80) ? " (byte reversed)" : "");
    const bool reverse = (m_dma_config & 0x80) != 0;
    while (m_dma_length != 0) {
        const u32 data = m_bus->read32(m_dma_src);
        m_bus->write32(m_dma_dest, reverse ? bswap(data) : data);
        m_dma_src += 4;
        m_dma_dest += 4;
        --m_dma_length;
    }
}

u8 Real3D::read_dma8(unsigned reg) const
{
    switch (reg) {
        case 0xC: return m_dma_status;
        case 0xE: return m_dma_config;
        default: break;
    }
    return 0;
}

void Real3D::write_dma8(unsigned reg, u8 data)
{
    switch (reg) {
        case 0xD:  // IRQ acknowledge
            if ((data & 1) != 0) {
                m_dma_status &= static_cast<u8>(~1);
                m_irq->deassert_irq(m_dma_irq);
            }
            break;
        case 0xE: m_dma_config = data; break;
        default: break;
    }
}

u32 Real3D::read_dma32(unsigned reg) const
{
    return reg == 0x14 ? m_dma_data : 0;  // command result
}

void Real3D::write_dma32(unsigned reg, u32 data)
{
    switch (reg) {
        case 0x00: m_dma_src = data; break;
        case 0x04: m_dma_dest = data; break;
        case 0x08:
            m_dma_length = data;
            dma_copy();
            if ((m_dma_config & 1) != 0) {  // only fire an IRQ if the low bit is set
                m_dma_status |= 1;
                m_irq->assert_irq(m_dma_irq);
            }
            break;
        case 0x10:  // command register
            if ((data & 0x20000000) != 0) {
                // DMA ID command: games expect 0x16C311DB, even on step 2.x boards.
                m_dma_data = kPciIdStep1x;
            } else if ((data & 0x80000000) != 0) {
                m_dma_data = read_register(data & 0x3F);
            }
            break;
        case 0x14: m_dma_data = 0xFFFFFFFF; break;
        default: break;
    }
}

// -- status registers ------------------------------------------------------------------
//
// Registers correspond to the Stat_Pckt in the Real3D SDK:
//
// 0x00:   xxxx---- -------- -------- --------     spare1
//         ----x--- -------- -------- --------     gp_done
//         -----x-- -------- -------- --------     dp_done
//         ------x- -------- -------- --------     ping_pong
//         -------x -------- -------- --------     update_done
//         -------- x------- -------- --------     rend_done
//         -------- -xxxxxxx xxxxxxxx xxxxxxxx     tot_clks 23bit val (0x7FFFFF). A 33.33 MHz
//                                                 clock value, thought to be the time the GPU
//                                                 takes to process the frame.
// 0x01-0x04: spare / vpt0-3_clks (not used by the SDK)
// 0x05-0x08: range0-3 (float) line of sight value for priority levels 0-3
// 0x09:   ls_cycle (uint32), thought to be the frame number; never read by Model 3 games.

u32 Real3D::read_register(unsigned reg) const
{
    if (reg == 0) {
        const u32 ping_pong = m_ping_pong != 0 ? 0x02000000 : 0;
        return 0xfdffffff | ping_pong;
    }
    if (reg >= 20 && reg <= 32) {  // line of sight registers
        const unsigned index = (reg - 20) / 4;
        return std::bit_cast<u32>(m_los[index]);
    }
    return 0xffffffff;
}

void Real3D::set_line_of_sight(unsigned index, float value)
{
    if (index < m_los.size()) m_los[index] = value;
}

// -- PCI / JTAG --------------------------------------------------------------------------

u32 Real3D::read_pci_config(unsigned device, unsigned reg, unsigned bits, unsigned offset) const
{
    (void)device;
    if (bits == 8) return 0;
    // This is a little endian device, so it returns little endian words.
    if (reg == 0x00) {
        u32 d = bswap(m_pci_id);
        if (bits == 16) d = (d >> ((2 - (offset & 2)) * 8)) & 0xFFFF;
        return d;
    }
    return 0;
}

void Real3D::write_pci_config(unsigned device, unsigned reg, unsigned bits, unsigned offset, u32 data)
{
    (void)device;
    (void)reg;
    (void)bits;
    (void)offset;
    (void)data;
}

void Real3D::write_jtag_modeword(Asic device, u32 data)
{
    if (device == Asic::Dummy) return;
    m_modeword[static_cast<usize>(static_cast<int>(device))] = data;
    // Mercury bit 2: block culling (disables rendering); Mars 0x40000: sun clamp.
}

// -- publication -------------------------------------------------------------------------

namespace {

template <class T>
void copy_dirty(const std::vector<T>& src, std::vector<T>& dst, std::vector<u8>& dirty, unsigned buffer)
{
    constexpr usize kPerPage = 4096 / sizeof(T);
    const u8 bit = static_cast<u8>(1u << buffer);
    for (usize page = 0; page < dirty.size(); ++page) {
        if ((dirty[page] & bit) == 0) continue;
        std::memcpy(dst.data() + page * kPerPage, src.data() + page * kPerPage, 4096);
        dirty[page] = static_cast<u8>(dirty[page] & ~bit);
    }
}

}  // namespace

void Real3D::publish(unsigned buffer, VideoFrame& frame)
{
    buffer &= 1;
    Snapshot& s = m_snap[buffer];
    copy_dirty(m_cull_lo, s.cull_lo, m_dirty_lo, buffer);
    copy_dirty(m_cull_hi, s.cull_hi, m_dirty_hi, buffer);
    copy_dirty(m_poly, s.poly, m_dirty_poly, buffer);
    copy_dirty(m_tex, s.tex, m_dirty_tex, buffer);

    frame.culling_lo  = s.cull_lo;
    frame.culling_hi  = s.cull_hi;
    frame.polygon_ram = s.poly;
    frame.texture_ram = s.tex;
    frame.vrom        = m_vrom;
    frame.texture_uploads.swap(m_uploads);
    m_uploads.clear();
    frame.texture_reset = m_texture_reset;
    m_texture_reset     = false;
    frame.sun_clamp     = (m_modeword[static_cast<int>(Asic::Mars)] & 0x40000) == 0;
    frame.block_culling = m_block_culling;
    frame.modewords     = m_modeword;
}

// -- save states ---------------------------------------------------------------------------

void Real3D::serialize(Archive& ar)
{
    ar.bytes(m_cull_lo.data(), m_cull_lo.size());
    ar.bytes(m_cull_hi.data(), m_cull_hi.size());
    ar.bytes(m_poly.data(), m_poly.size());
    ar.bytes(m_tex.data(), m_tex.size());
    ar.bytes(m_fifo.data(), m_fifo.size());
    ar.raw(m_fifo_idx);
    ar.raw(m_vrom_fifo);
    ar.raw(m_vrom_fifo_idx);
    ar.raw(m_config_regs);
    ar.raw(m_dma_src);
    ar.raw(m_dma_dest);
    ar.raw(m_dma_length);
    ar.raw(m_dma_data);
    ar.raw(m_dma_unknown);
    ar.raw(m_dma_status);
    ar.raw(m_dma_config);
    ar.raw(m_command_port_written);
    ar.raw(m_tilegen_draw_frame);
    ar.raw(m_ping_pong);
    ar.raw(m_ping_pong_copy);
    ar.raw(m_block_culling);
    ar.raw(m_internal_render_config);
    ar.raw(m_modeword);
    ar.raw(m_los);
    ar.raw(m_poly_update_block);
    ar.raw(m_high_update_block);
    if (ar.loading()) {
        if (m_fifo_idx > kFifoWords || m_vrom_fifo_idx > 2) ar.mark_failed();
        m_uploads.clear();
        invalidate_all();
    }
}

}  // namespace sm2::hw::model3
