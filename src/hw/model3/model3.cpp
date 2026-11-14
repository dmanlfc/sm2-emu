// SPDX-License-Identifier: GPL-3.0-or-later
//
// A complete Sega Model 3.
//
// Derived from Supermodel's Src/Model3/Model3.cpp
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2003-2026 The Supermodel Team.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// PowerPC address map (may be incomplete):
//
//   00000000-007FFFFF  RAM
//   84000000-8400003F  Real3D Status Registers
//   88000000-88000007  Real3D Command Port
//   8C000000-8C3FFFFF  Real3D Culling RAM (Low)
//   8E000000-8E0FFFFF  Real3D Culling RAM (High)
//   90000000-9000000B  Real3D VROM Texture Port
//   94000000-940FFFFF  Real3D Texture FIFO
//   98000000-980FFFFF  Real3D Polygon RAM
//   9C000000-9C00000F  Real3D configuration registers
//   C0000000-C000FFFF  Netboard Shared RAM (Step 1.5+)
//   C0010000-C00101FF  Netboard Registers (Step 1.5+)
//   C0020000-C002FFFF  Netboard Program RAM (Step 1.5+)
//   C0000000-C00000FF  SCSI (Step 1.x)
//   C1000000-C10000FF  SCSI (Step 1.x) (Lost World expects it here)
//   C2000000-C20000FF  Real3D DMA (Step 2.x)
//   F0040000-F004003F  Input (Controls) Registers
//   F0080000-F0080007  Sound Board Registers
//   F00C0000-F00DFFFF  Backup RAM
//   F0100000-F010003F  System Registers
//   F0140000-F014003F  Real-Time Clock
//   F0180000-F019FFFF  Security Board RAM
//   F01A0000-F01A003F  Security Board Registers
//   F0800CF8-F0800CFF  MPC105 CONFIG_ADDR (Step 1.x)
//   F0C00CF8-F0800CFF  MPC105 CONFIG_DATA (Step 1.x)
//   F1000000-F10F7FFF  Tile Generator Pattern Table
//   F10F8000-F10FFFFF  Tile Generator Name Table
//   F1100000-F111FFFF  Tile Generator Palette
//   F1180000-F11800FF  Tile Generator Registers
//   F8FFF000-F8FFF0FF  MPC105 (Step 1.x) or MPC106 (Step 2.x) Registers
//   F9000000-F90000FF  NCR 53C810 Registers (Step 1.x?)
//   FEC00000-FEDFFFFF  MPC106 CONFIG_ADDR (Step 2.x)
//   FEE00000-FEFFFFFF  MPC106 CONFIG_DATA (Step 2.x)
//   FF000000-FF7FFFFF  Banked CROM (CROMxx)
//   FF800000-FFFFFFFF  Fixed CROM
//   (0xFE000000-0xFEFFFFFF mirrors the 0xF0 page decode.)
//
// F0100014, IRQ enable: bit 7 ?, 6 SND (SCSP), 5 ?, 4 NET, 3 VGP (geometry
// done), 2 VDP (display done), 1 VBL (vblank start), 0 VD0 (unknown video).
//
// Game buttons (see read_inputs()):
//   0x04 bank 0: 7 ?, 6 ?, 5 ST2, 4 ST1, 3 SVA, 2 TSA, 1 CN2, 0 CN1
//   0x04 bank 1: 7 TSB, 6 SVB, 5 EEPROM data, 4-0 ?
//   0x08: G27..G20, 0x0C: G37..G30, game-specific (the input layer's job).
//
// Game notes:
//   daytona2: base address of program in CROM 0x600000; 0x10019E in RAM holds
//   the link type; the region menu is reached by entering test mode, holding
//   start, and pressing VR4,4,2,3,1,3,2.
//   magtruck: if the MIDI data port returns 0 the game is locked to Japan; 1
//   lets the region change (export, USA, Australia). The region menu is Test,
//   then Start, Start, Service, Start, Service, Test.

#include "hw/model3/model3.h"

#include "core/archive.h"
#include "hw/model3/byteswap.h"
#include "core/log.h"
#include "hw/save_state_io.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace sm2::hw::model3 {
namespace {

constexpr u32 kNmiBit = IrqController::kNmiDma;  // SCSI and Real3D DMA

u16 load_be16(const u8* p) { return static_cast<u16>((u16{p[0]} << 8) | p[1]); }
u32 load_be32(const u8* p)
{
    return (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3];
}
void store_be16(u8* p, u16 v)
{
    p[0] = static_cast<u8>(v >> 8);
    p[1] = static_cast<u8>(v);
}
void store_be32(u8* p, u32 v)
{
    p[0] = static_cast<u8>(v >> 24);
    p[1] = static_cast<u8>(v >> 16);
    p[2] = static_cast<u8>(v >> 8);
    p[3] = static_cast<u8>(v);
}

int stepping_code(rom::Model3Step step)
{
    switch (step) {
        case rom::Model3Step::Step10: return 0x10;
        case rom::Model3Step::Step15: return 0x15;
        case rom::Model3Step::Step20: return 0x20;
        case rom::Model3Step::Step21: return 0x21;
    }
    return 0x10;
}

}  // namespace

Model3::Model3()
    : m_ram(kRamSize, 0)
    , m_crom_fixed(kCromSize, 0)
    , m_backup_ram(kBackupRamSize, 0)
    , m_security_ram(kSecurityRamSize, 0)
    , m_net_ram(kNetRamSize, 0)
    , m_vrom(Real3D::kVromWords, 0)
{
    m_irq.set_line_callback([this](bool level) { cpu_irq_line(level); });
    m_tilegen_copy[0].assign(TileGen::kVramBytes, 0);
    m_tilegen_copy[1].assign(TileGen::kVramBytes, 0);
}

Model3::~Model3() = default;

// =============================================================================
// Initialisation
// =============================================================================

void Model3::build_memory(const rom::RomSet& roms)
{
    // Copy in ROM data with mirroring as necessary:
    //  - VROM: 64MB. If <= 32MB, mirror to high 32MB.
    //  - Banked CROM: 128MB. If <= 64MB, mirror to high 64MB.
    //  - Fixed CROM: 8MB. If < 8MB, loaded only in high part of space and low
    //    part is a mirror of (banked) CROM0.
    // Regions arrive in guest byte order.
    const auto vrom   = roms.region("vrom");
    const auto banked = roms.region("banked_crom");
    const auto crom   = roms.region("crom");

    std::fill(m_vrom.begin(), m_vrom.end(), 0u);
    auto* vrom_bytes = reinterpret_cast<u8*>(m_vrom.data());
    constexpr usize kVromBytes = usize{Real3D::kVromWords} * 4;
    if (vrom.size() <= kVromBytes / 2) {
        std::memcpy(vrom_bytes, vrom.data(), vrom.size());
        std::memcpy(vrom_bytes + kVromBytes / 2, vrom.data(), vrom.size());
    } else {
        std::memcpy(vrom_bytes, vrom.data(), std::min(vrom.size(), kVromBytes));
    }

    constexpr usize k64M = 64 * 0x100000;
    const usize banked_size = std::min(banked.size(), 2 * k64M);
    m_banked_mirror         = banked_size <= k64M ? static_cast<u32>(k64M) : static_cast<u32>(2 * k64M);
    const usize padded      = (banked_size + kBankSize - 1) / kBankSize * kBankSize;
    m_crom_banked.assign(padded, 0);
    std::memcpy(m_crom_banked.data(), banked.data(), banked_size);
    if (m_zero_bank.empty()) m_zero_bank.assign(kBankSize, 0);

    std::fill(m_crom_fixed.begin(), m_crom_fixed.end(), u8{0});
    const usize crom_size = std::min(crom.size(), usize{kCromSize});
    if (crom.size() > kCromSize) SM2_WARN("model3: crom is larger than 8 MB; using the first 8 MB");
    std::memcpy(m_crom_fixed.data() + (kCromSize - crom_size), crom.data(), crom_size);
    if (crom_size < kCromSize) {
        std::memcpy(m_crom_fixed.data(), m_crom_banked.data(),
                    std::min(kCromSize - crom_size, m_crom_banked.size()));
    }
}

void Model3::map_pages()
{
    using Access = cpu::PageMap::Access;
    m_pages.unmap(0, u64{1} << 32);
    m_pages.map(0x00000000, kRamSize, m_ram.data(), Access::ReadWrite);
    m_pages.map(0xFF800000, kCromSize, m_crom_fixed.data(), Access::ReadOnly);
    for (const u32 base : {0xF0000000u, 0xFE000000u}) {
        m_pages.map(base + 0x0C0000, kBackupRamSize, m_backup_ram.data(), Access::ReadWrite);
        m_pages.map(base + 0x180000, kSecurityRamSize, m_security_ram.data(), Access::ReadWrite);
    }
    m_pages.map(0xF1000000, TileGen::kVramBytes, m_tilegen.vram(), Access::ReadWrite);
    set_crom_bank(m_crom_bank_reg);
}

// Set the CROM bank index (active low logic).
void Model3::set_crom_bank(u32 value)
{
    m_crom_bank_reg = value;
    if (m_banked_mirror == 0) return;  // before init
    const u32 index = (~value) & 0xF;
    const usize offset = (usize{index} * kBankSize) % m_banked_mirror;
    u8* host = offset < m_crom_banked.size() ? m_crom_banked.data() + offset : m_zero_bank.data();
    if (host == nullptr) return;  // before init
    m_pages.map(0xFF000000, kBankSize, host, cpu::PageMap::Access::ReadOnly);
}

bool Model3::init(const rom::GameSpec& game, rom::RomSet roms)
{
    m_game     = game;
    m_roms     = std::move(roms);
    m_stepping = stepping_code(game.model3.step);
    m_cpu_hz   = game.model3.cpu_hz();

    // Display timings: 57.524160 Hz, assumed to match System 24 -- 25 lines
    // top border, 384 active, 11 bottom border, 4 vsync: 424 lines, 384 display
    // and 40 blanking. Computed in single precision, which fixes the exact
    // cycle counts.
    m_frame_cycles  = static_cast<u32>(static_cast<float>(m_cpu_hz) / 57.524160f);
    m_line_cycles   = m_frame_cycles / 424;
    m_vblank_cycles = m_line_cycles * 40;

    if (!m_roms.has("crom")) {
        SM2_ERROR("model3: '%s' has no crom region", game.name.c_str());
        return false;
    }
    build_memory(m_roms);

    m_mpc.set_model(m_stepping >= 0x20 ? 0x106 : 0x105);
    m_mpc.attach_bus(&m_pci);
    m_pci = PciBus{};
    m_pci.attach(13, &m_real3d);
    m_pci.attach(14, &m_scsi);
    m_pci.attach(16, this);

    // SCSI and the Real3D DMA interrupt are non-maskable, so they are given a
    // bit outside the 8-bit range.
    m_scsi.attach(this, &m_irq, kNmiBit);
    m_real3d.attach(this, &m_irq, kNmiBit);
    m_real3d.set_stepping(m_stepping);
    m_real3d.set_vrom(m_vrom);
    m_jtag.set_sink(&m_real3d);
    m_jtag.set_stepping(m_stepping);
    m_tilegen.attach_irq(&m_irq);

    m_crypt.set_key(game.protection_key);
    m_crypt.set_read_callback([this](u32 word) { return read_security_ram(word); });

    m_has_gun = rom::has_input(game.inputs, rom::InputFlags::Gun1 | rom::InputFlags::Gun2);

#if SM2_MODEL3_HAVE_PPC
    if (!m_cpu) m_cpu = create_ppc_cpu();
#endif
#if SM2_MODEL3_HAVE_SOUND
    if (!m_sound) m_sound = create_sound_port(game, m_roms);
#endif
    if (!m_drive) m_drive = create_drive_board(m_game, m_roms, m_inputs, m_board_outputs);
    if (!m_net) attach_net_board(create_net_board(game, m_net_ram));

    m_initialised = true;
    if (m_cpu) attach_cpu(std::move(m_cpu));
    if (m_sound) m_sound->attach(m_cpu_hz);

    map_pages();
    reset();
    SM2_INFO("model3: '%s', step %d.%d, %u MHz%s", game.name.c_str(), m_stepping >> 4,
             m_stepping & 0xF, m_cpu_hz / 1000000, m_cpu ? "" : " (no PowerPC core in this build)");
    return true;
}

void Model3::attach_cpu(std::unique_ptr<MainCpu> cpu)
{
    m_cpu = std::move(cpu);
    if (!m_cpu || !m_initialised) return;
    cpu::ppc::PpcConfig config;
    // Step 1.0 and 2.x are a 603R ("Goldeneye", a 603ev part, PVR
    // 0x00071202); Step 1.5 a 603E ("Stretch", PVR 0x00060103).
    config.model  = m_stepping == 0x15 ? cpu::ppc::Model::Ppc603e : cpu::ppc::Model::Ppc603ev;
    config.cpu_hz = m_cpu_hz;
    config.bus_hz = 66'000'000;
    m_cpu->init(config, m_pages, *this);
    m_cpu->reset();
    m_cpu->set_irq(m_irq.line());
}

void Model3::attach_sound(std::unique_ptr<SoundPort> sound)
{
    m_sound = std::move(sound);
    if (m_sound && m_initialised) {
        m_sound->attach(m_cpu_hz);
        m_sound->reset();
    }
}

void Model3::attach_drive_board(std::unique_ptr<DriveBoardPort> board)
{
    m_drive = std::move(board);
    if (m_drive) m_drive->reset();
}

void Model3::attach_net_board(std::unique_ptr<NetBoardPort> board)
{
    m_net = std::move(board);
    m_net_comm = m_net ? m_net->comm() : nullptr;
    m_run_net_board = m_net != nullptr && m_stepping != 0x10;
    if (m_net) m_net->reset();
}

void Model3::set_exec_mode(cpu::ppc::ExecMode mode)
{
    if (m_cpu) m_cpu->set_exec_mode(mode);
}

void Model3::reset()
{
    // Clear memory (but do not modify backup RAM!)
    std::fill(m_ram.begin(), m_ram.end(), u8{0});
    if (m_cpu) m_cpu->invalidate(0, kRamSize);

    // Initial bank is bank 0.
    set_crom_bank(0xFF);

    m_security_ptr        = 0;
    m_security_first_read = true;
    m_eeprom_busy  = 0;
    m_input_bank   = 0;
    m_serial_fifo1 = 0;
    m_serial_fifo2 = 0;
    m_adc_channel  = 0;
    m_midi_ctrl    = 0;

    m_irq.reset();
    m_mpc.reset();
    m_scsi.reset();
    m_rtc.reset();
    {
        // The EEPROM's contents are non-volatile: keep them across the reset.
        std::array<u8, Eeprom93c46::kByteCount> saved{};
        std::copy_n(m_eeprom.bytes().begin(), saved.size(), saved.begin());
        m_eeprom.reset();
        std::copy(saved.begin(), saved.end(), m_eeprom.bytes().begin());
    }
    m_tilegen.reset();
    m_real3d.reset();
    m_jtag.reset();
    m_crypt.reset();
    if (m_sound) m_sound->reset();
    if (m_drive) m_drive->reset();
    m_run_net_board = m_net != nullptr && m_stepping != 0x10;
    if (m_net) m_net->reset();
    if (m_cpu) {
        m_cpu->reset();
        m_cpu->set_irq(false);
    }

    m_sched.reset();
    track_tilegen(false);
    m_gpus_ready  = false;
    m_frame_done  = false;
    m_in_frame    = false;
    m_frames      = 0;
    m_last_sound  = 0;
    m_published   = 0;
    m_published_count = 0;
}

// =============================================================================
// Frame schedule
// =============================================================================
//
// With the GPUs ready (every frame but the first after reset):
//
//   VblankStart  Begin vblank (40 lines).
//   Irq2Poll     Run 1000-cycle steps until IRQ2 is acknowledged (Ski Champ
//                hangs if MIDI is checked too early).
//   MidiPoll     While MIDI control bit 0x20 and IRQ 0x40 are enabled, assert
//                IRQ 0x40 and run 1000 cycles, at most 129 times.
//   VblankRest   The rest of vblank.
//   VblankEnd    Assert IRQ 0x0D, end vblank.
//   Line 0..383  Flip ping-pong at tilegen reg 0x08; assert IRQ 2 at line 383
//                (The Lost World soft-locks otherwise); latch; run one line.
//   FrameEnd     Publish, then run the sound, drive and net boards, in that order.

void Model3::begin_frame()
{
    m_frame_done  = false;
    m_frame_start = m_sched.now();
    if (m_gpus_ready) {
        m_sched.schedule(kEvVblankStart, m_frame_start);
    } else {
        start_active(m_frame_start);
    }
    schedule_sound_slices();
}

void Model3::schedule_sound_slices()
{
    m_sound_slice = 0;
    m_sched.cancel(kEvSoundSlice);
    if (!m_sound) return;
    const u32 slices = std::max(1u, m_sound->timeslices_per_frame());
    if (slices <= 1) return;
    const u64 length = u64{m_gpus_ready ? 424u : 384u} * m_line_cycles;
    m_sound_slice = 1;
    m_sched.schedule(kEvSoundSlice, m_frame_start + length / slices);
}

void Model3::start_active(u64 when)
{
    m_flip_line = m_tilegen.read_register(0x08);
    m_line      = 0;
    std::memcpy(m_tilegen_copy[m_published ^ 1u].data(), m_tilegen.vram(), TileGen::kVramBytes);
    m_tilegen_writes.clear();
    track_tilegen(true);
    dispatch(kEvLine, when);
}

// Each 2D line is drawn from VRAM as it is at that line. VRAM stores
// during lines 0-382 are sent to the slow path and journalled for the renderer.
void Model3::track_tilegen(bool on)
{
    if (on == m_tilegen_tracking) return;
    m_tilegen_tracking = on;
    m_pages.map(0xF1000000, TileGen::kVramBytes, m_tilegen.vram(),
                on ? cpu::PageMap::Access::ReadOnly : cpu::PageMap::Access::ReadWrite);
}

bool Model3::tilegen_store(u32 addr, u32 size, u32 data)
{
    if (!m_tilegen_tracking || addr < 0xF1000000 || addr >= 0xF1000000 + TileGen::kVramBytes) return false;
    const u32 offset = addr - 0xF1000000;
    u8* const p      = m_tilegen.vram() + offset;
    if (size == 1) *p = static_cast<u8>(data);
    else if (size == 2) store_be16(p, static_cast<u16>(data));
    else store_be32(p, data);
    TileGenWrite w;
    w.line   = m_line;
    w.offset = offset & ~3u;
    std::memcpy(w.bytes.data(), m_tilegen.vram() + w.offset, 4);
    m_tilegen_writes.push_back(w);
    return true;
}

void Model3::dispatch(u32 event, u64 when)
{
    switch (event) {
        case kEvVblankStart:
            m_real3d.begin_vblank();
            m_vblank_left = m_vblank_cycles;
            dispatch(kEvIrq2Poll, when);
            break;

        case kEvIrq2Poll:
            if ((m_irq.read_enable() & 0x2) != 0 && (m_irq.read_state() & 0x2) != 0
                && m_vblank_left > 1000) {
                m_vblank_left -= 1000;
                m_sched.schedule(kEvIrq2Poll, when + 1000);
            } else {
                m_midi_count = 0;
                dispatch(kEvMidiPoll, when);
            }
            break;

        case kEvMidiPoll:
            // Don't waste time firing MIDI interrupts if the game disabled them.
            if ((m_midi_ctrl & 0x20) != 0 && (m_irq.read_enable() & 0x40) != 0) {
                m_irq.assert_irq(IrqController::kMidi);
                m_vblank_left -= 1000;
                ++m_midi_count;
                m_sched.schedule(m_midi_count > 128 ? kEvVblankRest : kEvMidiPoll, when + 1000);
            } else {
                dispatch(kEvVblankRest, when);
            }
            break;

        case kEvVblankRest:
            m_sched.schedule(kEvVblankEnd, when + static_cast<u64>(std::max<s64>(0, m_vblank_left)));
            break;

        case kEvVblankEnd:
            m_irq.assert_irq(0x0D);
            m_real3d.end_vblank();
            start_active(when);
            break;

        case kEvLine:
            if (m_line == m_flip_line) m_real3d.flip_ping_pong();
            if (m_line == 383) m_irq.assert_irq(IrqController::kVblank);
            m_tilegen.latch_line(m_line);
            ++m_line;
            if (m_line == VideoFrame::kActiveLines) track_tilegen(false);
            m_sched.schedule(m_line < VideoFrame::kActiveLines ? kEvLine : kEvFrameEnd,
                             when + m_line_cycles);
            break;

        case kEvSoundSlice: {
            run_sound_to_now();
            const u32 slices = m_sound ? std::max(1u, m_sound->timeslices_per_frame()) : 1;
            ++m_sound_slice;
            if (m_sound_slice < slices) {
                const u64 length = u64{m_gpus_ready ? 424u : 384u} * m_line_cycles;
                m_sched.schedule(kEvSoundSlice, m_frame_start + length * m_sound_slice / slices);
            }
            break;
        }

        case kEvFrameEnd:
            m_frame_done = true;
            break;

        default: break;
    }
}

void Model3::run_cpu_until_frame_end()
{
    while (!m_frame_done) {
        while (auto due = m_sched.pop_due()) {
            dispatch(due->id, due->when);
            if (m_frame_done) return;
        }
        const u64 next  = m_sched.next_deadline();
        const s64 slice = next == Scheduler::kNever ? s64{m_line_cycles}
                                                    : static_cast<s64>(next - m_sched.now());
        s64 ran = slice;
        if (m_cpu) {
            CoreScope scope(m_core_profile, m_core_profile.i960_ns);
            m_in_cpu = true;
            ran      = m_cpu->run(slice);
            m_in_cpu = false;
        }
        // Always advance, so the schedule cannot stall.
        m_sched.advance(static_cast<u64>(std::max<s64>(ran, 1)));
    }
}

void Model3::run_sound_to_now()
{
    const u64 now = m_sched.now();
    if (m_sound && now > m_last_sound) {
        CoreScope scope(m_core_profile, m_core_profile.sound_ns);
        m_sound->run(now - m_last_sound);
    }
    m_last_sound = now;
}

void Model3::end_frame()
{
    publish_frame();
    m_gpus_ready = true;
    run_sound_to_now();
    if (m_drive) m_drive->run_frame();
    if (m_net && m_run_net_board) m_net->run_frame();
    m_rtc.advance(m_sched.now() - m_frame_start, m_cpu_hz);
    ++m_frames;
}

void Model3::run_frame()
{
    if (!m_initialised) return;
    reset_core_profile();
    m_in_frame = true;
    begin_frame();
    run_cpu_until_frame_end();
    end_frame();
    m_in_frame = false;
}

void Model3::publish_frame()
{
    const unsigned b = m_published ^ 1u;
    VideoFrame& f    = m_frames_out[b];
    m_real3d.publish(b, f);
    f.tilegen_vram  = m_tilegen_copy[b];
    f.tilegen_writes.swap(m_tilegen_writes);
    m_tilegen_writes.clear();
    f.tilegen_regs  = m_tilegen.registers();
    f.tilegen_lines = m_tilegen.line_latch();
    f.step          = m_game.model3.step;
    f.centre_letterbox = m_game.model3.centre_letterbox;
    f.frame_number  = ++m_published_count;
    m_published     = b;
}

void Model3::cpu_irq_line(bool level)
{
    if (!m_cpu) return;
    m_cpu->set_irq(level);
    // The external interrupt is taken as soon as the line rises, even during
    // a bus access, so end the slice.
    if (level && m_in_cpu) m_cpu->end_timeslice();
}

CpuStatus Model3::main_cpu_status() const
{
    if (m_cpu) return m_cpu->status();
    CpuStatus s;
    s.state_string = "no PowerPC core";
    s.halted       = true;
    return s;
}

// =============================================================================
// Inputs
// =============================================================================
//
// hw::Inputs to ports (active low):
//   0x04 bank 0   in0 (bits 6/7: Ski Champ's select buttons)
//   0x04 bank 1   in3 bit 6 service B, bit 7 test B; bit 5 EEPROM DO
//   0x08 / 0x0C   in1 / in2: G2x / G3x (G3x ANDed with the drive board)
//   0x3C          analog[channel], auto-incrementing
//   light guns    gun_p*; off-screen bits from set_gun_offscreen() (Inputs
//                 is stored raw in Model 1/2 states, so no field there)

// Gear shifter codes on G24-G26.
u8 Model3::game_inputs_g2() const
{
    u8 data = m_inputs.in1;
    // Get Bass's cast and select are active high on G24/G25.
    if (rom::has_input(m_game.inputs, rom::InputFlags::Fishing) && m_game.name != "getbassur"
        && m_game.parent != "getbassur") {
        const bool cast   = (data & 0x01) == 0;
        const bool select = (data & 0x02) == 0;
        data = static_cast<u8>((data | 0x03) & ~0x30u);
        if (cast) data |= 0x10;
        if (select) data |= 0x20;
    }
    if (m_game.gearbox) {
        static constexpr u8 kGearBits[5] = {0x70, 0x20, 0x10, 0x60, 0x50};
        u32 gear = 0;
        for (u32 g = 0; g < 5; ++g) {
            if ((m_inputs.gears & (1u << g)) != 0) gear = g;
        }
        data = static_cast<u8>((data & ~0x70u) | kGearBits[gear]);
    } else if (m_game.shift_buttons) {
        const bool up   = (data & 0x10) == 0;
        const bool down = (data & 0x20) == 0;
        data |= 0x70;
        if (rom::has_input(m_game.inputs, rom::InputFlags::Harley)) {
            if (up) data &= ~0x20;
            else if (down) data &= ~0x10;
        } else {
            if (up) data &= ~0x50;
            else if (down) data &= ~0x60;
        }
    }
    return data;
}

u8 Model3::read_inputs(unsigned reg)
{
    reg &= 0x3F;
    switch (reg) {
        case 0x00: return m_input_bank;

        case 0x04:
            if ((m_input_bank & 1) == 0) return m_inputs.in0;
        {
            bool data_out = m_eeprom.data_out();
            if (m_eeprom_busy > 0 && !m_eeprom.reading()) {
                --m_eeprom_busy;
                data_out = false;
            }
            return static_cast<u8>((m_inputs.in3 & 0xC0) | 0x1F | (data_out ? 0x20 : 0));
        }

        case 0x08: return game_inputs_g2();

        case 0x0C: {
            u8 data = 0xFF;
            if (m_drive && !m_drive->is_billboard()) data = m_drive->read();
            return static_cast<u8>(data & m_inputs.in2);
        }

        case 0x10: return m_output_reg[0];  // drive board
        case 0x14: return m_output_reg[1];  // lamps

        case 0x18: {
            // swtrilgy and getbass: removes the IO board error on getbass; when
            // this is wrong Ocean Hunter shows a sort of interlaced 3D effect.
            const std::string& name = m_game.name;
            if (name == "bassdx" || name == "getbassdx" || name == "getbass") return 0x01;
            return 0x7F;
        }

        case 0x2C: return m_serial_fifo1;
        case 0x30: return m_serial_fifo2;
        case 0x34: return m_has_gun ? 0x0C : 0x00;  // serial FIFO full/empty flags

        case 0x3C: {
            const u8 data = m_inputs.analog[static_cast<usize>(m_adc_channel & 7)];
            ++m_adc_channel;
            return data;
        }

        default: break;
    }
    return 0xFF;  // controls are active low
}

void Model3::write_inputs(unsigned reg, u8 data)
{
    switch (reg & 0x3F) {
        case 0x00:
            m_eeprom.set_cs(((data >> 6) & 1) != 0);
            if (((data >> 6) & 1) == 0) m_eeprom_busy = 5;
            m_eeprom.set_di(((data >> 5) & 1) != 0);
            m_eeprom.set_clk(((data >> 7) & 1) != 0);
            m_input_bank = data;
            break;

        case 0x10:  // drive board
            if (m_drive) m_drive->write(data);
            record_drive_board_write(data);
            m_output_reg[0] = data;
            break;

        case 0x14:  // lamp outputs (Daytona 2 / Scud Race / Sega Rally 2 / Le Mans 24)
            // 0x04 start, 0x08-0x40 view 1-4, 0x80 leader.
            record_lamp_latch(data);
            m_output_reg[1] = data;
            break;

        case 0x24:  // serial FIFO 1: command
            switch (data) {
                case 0x00: m_gun_reg = m_serial_fifo2; break;  // light gun register select
                case 0x87:                                     // read light gun register
                    m_serial_fifo1 = 0;
                    m_serial_fifo2 = 0;
                    if (m_has_gun) {
                        switch (m_gun_reg) {
                            case 0: m_serial_fifo2 = static_cast<u8>(m_inputs.gun_p1y & 0xFF); break;
                            case 1: m_serial_fifo2 = static_cast<u8>((m_inputs.gun_p1y >> 8) & 3); break;
                            case 2: m_serial_fifo2 = static_cast<u8>(m_inputs.gun_p1x & 0xFF); break;
                            case 3: m_serial_fifo2 = static_cast<u8>((m_inputs.gun_p1x >> 8) & 3); break;
                            case 4: m_serial_fifo2 = static_cast<u8>(m_inputs.gun_p2y & 0xFF); break;
                            case 5: m_serial_fifo2 = static_cast<u8>((m_inputs.gun_p2y >> 8) & 3); break;
                            case 6: m_serial_fifo2 = static_cast<u8>(m_inputs.gun_p2x & 0xFF); break;
                            case 7: m_serial_fifo2 = static_cast<u8>((m_inputs.gun_p2x >> 8) & 3); break;
                            case 8: m_serial_fifo2 = static_cast<u8>(m_gun_offscreen & 3); break;
                            default: SM2_DEBUG("model3: unknown gun register %X", m_gun_reg); break;
                        }
                    }
                    break;
                default: SM2_DEBUG("model3: unknown serial FIFO command %02X", data); break;
            }
            break;

        case 0x28: m_serial_fifo2 = data; break;
        case 0x3C: m_adc_channel = data & 7; break;
        default: break;
    }
}

// =============================================================================
// Security board
// =============================================================================
//
// Present in some games. Virtual On and Dirt Devils read tile pattern data
// from it. Spikeout calls a routine at PC=0x6FAC8 that writes/reads the
// security device and, if the return value in R3 is 0, prints "ILLEGAL ROM"
// and locks the game. Returning all ones for unknown reads avoids this.

u16 Model3::read_security_ram(u32 word)
{
    // The upper half of each 32-bit word, as the PowerPC stored it.
    if (word < 0x8000) return load_be16(&m_security_ram[usize{word} * 4]);
    return 0;
}

u32 Model3::read_security(unsigned reg)
{
    switch (reg) {
        case 0x00: return 0;  // status
        case 0x1C:            // data
            if (m_security_first_read) {
                m_security_first_read = false;
                return 0xFFFF0000;
            }
            if (m_game.protection_key == 0) return 0xFFFF0000;
            return u32{m_crypt.decrypt_be_r()} << 16;
        default: break;
    }
    return 0xFFFFFFFF;
}

void Model3::write_security(unsigned reg, u32 data)
{
    switch (reg) {
        case 0x10:
        case 0x14:
            m_crypt.set_addr_low(0);
            m_crypt.set_addr_high(0);
            m_security_first_read = true;
            break;
        case 0x18: {
            const u16 subkey = static_cast<u16>(data >> 16);
            m_crypt.set_subkey(static_cast<u16>(((subkey & 0xFF00) >> 8) | ((subkey & 0x00FF) << 8)));
            break;
        }
        default: SM2_DEBUG("model3: security write reg=%X data=%08X", reg, data); break;
    }
}

// =============================================================================
// PCI device 16 and the system registers
// =============================================================================

u32 Model3::read_pci_config(unsigned device, unsigned reg, unsigned bits, unsigned offset) const
{
    (void)offset;
    if (bits == 8 || bits == 16) return 0;
    if (device == 16 && reg == 0) return 0x182711DB;  // used by Daytona 2
    return 0;
}

void Model3::write_pci_config(unsigned device, unsigned reg, unsigned bits, unsigned offset, u32 data)
{
    SM2_DEBUG("model3: PCI %u-bit write device=%u reg=%02X offset=%u data=%08X", bits, device, reg,
              offset, data);
}

u8 Model3::read_system_register(unsigned reg) const
{
    switch (reg & 0x3F) {
        case 0x08: return static_cast<u8>(m_crom_bank_reg);
        case 0x14: return m_irq.read_enable();
        case 0x18: return m_irq.read_state();
        case 0x1C: return 0xFF;  // unknown (apparently expects some or all bits set)
        case 0x10: return static_cast<u8>(m_jtag.read() ? 0x20 : 0);  // JTAG TDO
        default: break;
    }
    return 0xFF;
}

void Model3::write_system_register(unsigned reg, u8 data)
{
    switch (reg & 0x3F) {
        case 0x08: set_crom_bank(data); break;
        case 0x14: m_irq.write_enable(data); break;
        case 0x18: m_irq.deassert_irq(data); break;  // IRQ acknowledge
        case 0x0C: {                                 // JTAG test access port
            const bool tck  = ((data >> 6) & 1) != 0;
            const bool tms  = ((data >> 2) & 1) != 0;
            const bool tdi  = ((data >> 5) & 1) != 0;
            const bool trst = ((data >> 7) & 1) != 0;  // not sure about this one
            m_jtag.write(tck, tms, tdi, trst);
            break;
        }
        default: break;  // 0x0D-0x0F, 0x1C: LED control
    }
}

void Model3::write_midi(unsigned offset, u8 data)
{
    if (offset == 0) {  // MIDI data port
        if (m_sound) m_sound->write_midi(data);
        m_irq.deassert_irq(IrqController::kMidi);
    } else if (offset == 4) {  // MIDI control port
        m_midi_ctrl = data;
        if (m_sound) m_sound->write_control(data);
        if ((data & 0x20) == 0) m_irq.deassert_irq(IrqController::kMidi);
    }
}

bool Model3::scsi_at_c0() const
{
    return m_stepping <= 0x15 && m_scsi.base_address() == 0xC0;
}

// =============================================================================
// Network board window
// =============================================================================

u8 Model3::net_read8(u32 addr)
{
    switch ((addr & 0x3FFFF) >> 16) {
        case 0: return m_net->read_comm_ram8((addr & 0xFFFF) ^ 2);
        case 1: return static_cast<u8>(m_net->read_io_register((addr & 0x1FF) / 2));
        case 2:
        case 3: return m_net_ram[(addr & 0x1FFFF) / 2];
        default: return 0xFF;
    }
}

u32 Model3::net_read32(u32 addr)
{
    switch ((addr & 0x3FFFF) >> 16) {
        case 0: {
            const u32 r = bswap(m_net->read_comm_ram32(addr & 0xFFFF));
            return (r << 16) | (r >> 16);
        }
        case 1: return bswap(u32{m_net->read_io_register((addr & 0x1FF) / 2)});
        case 2:
        case 3: {
            const usize o = (addr & 0x1FFFF) / 2;
            return (u32{m_net_ram[o]} << 24) | (u32{m_net_ram[(o + 1) & 0xFFFF]} << 16);
        }
        default: return 0xFFFFFFFF;
    }
}

void Model3::net_write8(u32 addr, u8 data)
{
    switch ((addr & 0x3FFFF) >> 16) {
        case 0: m_net->write_comm_ram8((addr & 0xFFFF) ^ 2, data); break;
        case 1: m_net->write_io_register((addr & 0x1FF) / 2, data); break;
        case 2:
        case 3: m_net_ram[(addr & 0x1FFFF) / 2] = data; break;
        default: break;
    }
}

void Model3::net_write32(u32 addr, u32 data)
{
    switch ((addr & 0x3FFFF) >> 16) {
        case 0: {
            const u32 t = bswap(data);
            m_net->write_comm_ram32(addr & 0xFFFF, (t << 16) | (t >> 16));
            break;
        }
        case 1: m_net->write_io_register((addr & 0x1FF) / 2, bswap(static_cast<u16>(data >> 16))); break;
        case 2:
        case 3: {
            const usize o = (addr & 0x1FFFF) / 2;
            m_net_ram[o]                = static_cast<u8>(data >> 24);
            m_net_ram[(o + 1) & 0xFFFF] = static_cast<u8>(data >> 16);
            break;
        }
        default: break;
    }
}

// =============================================================================
// PpcBus: the slow path
// =============================================================================
//
// Paged regions are served here too (DMA, slow-path cores).

void Model3::unmapped(const char* what, u32 addr, u32 data)
{
    ++m_unmapped;
    if (m_log_unmapped) SM2_WARN("model3: unmapped %s %08X (%08X)", what, addr, data);
}

u8 Model3::read8(u32 addr)
{
    if (const u8* p = m_pages.read_ptr(addr)) return *p;
    return mmio_read8(addr);
}

u16 Model3::read16(u32 addr)
{
    if ((addr & 1) != 0) return static_cast<u16>((u16{read8(addr)} << 8) | read8(addr + 1));
    if (const u8* p = m_pages.read_ptr(addr)) return load_be16(p);
    return mmio_read16(addr);
}

u32 Model3::read32(u32 addr)
{
    if ((addr & 3) != 0) return (u32{read16(addr)} << 16) | read16(addr + 2);
    if (const u8* p = m_pages.read_ptr(addr)) return load_be32(p);
    return mmio_read32(addr);
}

u64 Model3::read64(u32 addr)
{
    return (u64{read32(addr)} << 32) | read32(addr + 4);
}

void Model3::write8(u32 addr, u8 data)
{
    if (u8* p = m_pages.ram_write_ptr(addr)) {
        *p = data;
        if (m_cpu && m_pages.is_code_page(addr)) m_cpu->invalidate(addr, 1);
        return;
    }
    if (tilegen_store(addr, 1, data)) return;
    mmio_write8(addr, data);
}

void Model3::write16(u32 addr, u16 data)
{
    if ((addr & 1) != 0) {
        write8(addr, static_cast<u8>(data >> 8));
        write8(addr + 1, static_cast<u8>(data));
        return;
    }
    if (u8* p = m_pages.ram_write_ptr(addr)) {
        store_be16(p, data);
        if (m_cpu && m_pages.is_code_page(addr)) m_cpu->invalidate(addr, 2);
        return;
    }
    if (tilegen_store(addr, 2, data)) return;
    mmio_write16(addr, data);
}

void Model3::write32(u32 addr, u32 data)
{
    if ((addr & 3) != 0) {
        write16(addr, static_cast<u16>(data >> 16));
        write16(addr + 2, static_cast<u16>(data));
        return;
    }
    if (u8* p = m_pages.ram_write_ptr(addr)) {
        store_be32(p, data);
        if (m_cpu && m_pages.is_code_page(addr)) m_cpu->invalidate(addr, 4);
        return;
    }
    if (tilegen_store(addr, 4, data)) return;
    mmio_write32(addr, data);
}

void Model3::write64(u32 addr, u64 data)
{
    write32(addr, static_cast<u32>(data >> 32));
    write32(addr + 4, static_cast<u32>(data));
}

u8 Model3::mmio_read8(u32 addr)
{
    switch (addr >> 24) {
        case 0xC2: return m_real3d.read_dma8(addr & 0xFF);  // Real3D DMA

        case 0xF0:
        case 0xFE:  // mirror
            switch ((addr >> 16) & 0xFF) {
                case 0x04: return read_inputs(addr & 0x3F);
                case 0x08:  // sound board: MIDI data / control
                    if ((addr & 0xF) == 0 || (addr & 0xF) == 4) {
                        return m_sound ? m_sound->midi_read(addr & 0xF) : ((addr & 0xF) == 4 ? 0x83 : 0x00);
                    }
                    return 0;
                case 0x10: return read_system_register(addr & 0x3F);
                case 0x14:  // RTC
                    if ((addr & 3) == 1) return 0x03;  // battery voltage test
                    if ((addr & 3) == 0) return m_rtc.read_register((addr >> 2) & 0xF);
                    return 0;
                default: break;
            }
            break;

        case 0xC0:  // Step 1.x SCSI, or the net board
            if (m_run_net_board) return net_read8(addr);
            if (!scsi_at_c0()) break;
            [[fallthrough]];
        case 0xF9:
        case 0xC1: return m_scsi.read_register(addr & 0xFF);

        default: break;
    }
    unmapped("read8", addr);
    return 0xFF;
}

u16 Model3::mmio_read16(u32 addr)
{
    switch (addr >> 24) {
        case 0xF0:
        case 0xFE: {
            const u32 sub = (addr >> 16) & 0xFF;
            if (sub == 0xC0 && m_mpc.model() == 0x105) return static_cast<u16>(m_mpc.read_config_data(16, addr & 3));
            if (sub >= 0xE0 && sub <= 0xEF && m_mpc.model() == 0x106) {
                return static_cast<u16>(m_mpc.read_config_data(16, addr & 3));
            }
            break;
        }

        case 0xC0:  // Spikeout reads the net board's shared RAM 16 bits at a time
            if (m_net && ((addr & 0x3FFFF) >> 16) == 0) {
                return bswap(m_net->read_comm_ram16((addr & 0xFFFF) ^ 2));
            }
            break;

        default: break;
    }
    unmapped("read16", addr);
    return 0xFFFF;
}

u32 Model3::mmio_read32(u32 addr)
{
    switch (addr >> 24) {
        case 0x84: return bswap(m_real3d.read_register(addr & 0x3F));  // Real3D status
        case 0xC2: return bswap(m_real3d.read_dma32(addr & 0xFF));     // Real3D DMA

        case 0xF0:
        case 0xFE: {
            const u32 sub = (addr >> 16) & 0xFF;
            switch (sub) {
                case 0x04:
                    return (u32{read_inputs((addr & 0x3F) + 0)} << 24) | (u32{read_inputs((addr & 0x3F) + 1)} << 16)
                           | (u32{read_inputs((addr & 0x3F) + 2)} << 8) | read_inputs((addr & 0x3F) + 3);
                case 0x10:
                    return (u32{read_system_register((addr & 0x3F) + 0)} << 24)
                           | (u32{read_system_register((addr & 0x3F) + 1)} << 16)
                           | (u32{read_system_register((addr & 0x3F) + 2)} << 8)
                           | read_system_register((addr & 0x3F) + 3);
                case 0xC0:
                    if (m_mpc.model() == 0x105) return m_mpc.read_config_data(32, 0);
                    break;
                case 0x14:  // RTC; bits set to pass the battery voltage test
                    return (u32{m_rtc.read_register((addr >> 2) & 0xF)} << 24) | 0x00030000;
                case 0x1A: return read_security(addr & 0x3F);
                default:
                    if (sub >= 0xE0 && sub <= 0xEF && m_mpc.model() == 0x106) return m_mpc.read_config_data(32, 0);
                    break;
            }
            break;
        }

        case 0xF1:  // tile generator registers (VRAM is paged)
            if (addr >= 0xF1180000 && addr < 0xF1180100) return bswap(m_tilegen.read_register(addr & 0xFF));
            break;

        case 0xC0:
            if (m_run_net_board) return net_read32(addr);
            if (!scsi_at_c0()) break;
            [[fallthrough]];
        case 0xF9:
        case 0xC1:
            return (u32{m_scsi.read_register((addr + 0) & 0xFF)} << 24)
                   | (u32{m_scsi.read_register((addr + 1) & 0xFF)} << 16)
                   | (u32{m_scsi.read_register((addr + 2) & 0xFF)} << 8)
                   | m_scsi.read_register((addr + 3) & 0xFF);

        default: break;
    }
    unmapped("read32", addr);
    return 0xFFFFFFFF;
}

void Model3::mmio_write8(u32 addr, u8 data)
{
    switch (addr >> 24) {
        case 0xC2: m_real3d.write_dma8(addr & 0xFF, data); return;

        case 0xF0:
        case 0xFE:
            switch ((addr >> 16) & 0xFF) {
                case 0x04: write_inputs(addr & 0x3F, data); return;
                case 0x08: write_midi(addr & 0xF, data); return;
                case 0x10: write_system_register(addr & 0x3F, data); return;
                case 0x14:
                    if ((addr & 3) == 0) m_rtc.write_register((addr >> 2) & 0xF, data);
                    return;
                default: break;
            }
            break;

        case 0xF8:  // MPC105 registers
            if (m_mpc.model() == 0x105) m_mpc.write_register(addr & 0xFF, data);
            return;

        case 0xC0:
            if (m_run_net_board) {
                net_write8(addr, data);
                return;
            }
            if (!scsi_at_c0()) break;
            [[fallthrough]];
        case 0xF9:
        case 0xC1: m_scsi.write_register(addr & 0xFF, data); return;

        default: break;
    }
    unmapped("write8", addr, data);
}

void Model3::mmio_write16(u32 addr, u16 data)
{
    switch (addr >> 24) {
        case 0xF0:
        case 0xFE:
            if (((addr >> 16) & 0xFF) == 0xC0 && m_mpc.model() == 0x105) {
                m_mpc.write_config_data(16, addr & 2, data);
                return;
            }
            break;

        case 0xF8:
            if (m_mpc.model() == 0x105) {
                // Write in big endian order, like a real PowerPC.
                m_mpc.write_register((addr & 0xFF) + 0, static_cast<u8>(data >> 8));
                m_mpc.write_register((addr & 0xFF) + 1, static_cast<u8>(data));
            }
            return;

        case 0xC0:  // Ski Champ only
            if (m_net && ((addr & 0x3FFFF) >> 16) == 0) {
                m_net->write_comm_ram16((addr & 0xFFFF) ^ 2, bswap(data));
                return;
            }
            break;

        default: break;
    }
    unmapped("write16", addr, data);
}

void Model3::mmio_write32(u32 addr, u32 data)
{
    switch (addr >> 24) {
        case 0x88: m_real3d.flush(); return;  // Real3D command port
        case 0x8C: m_real3d.write_low_culling(addr & 0x3FFFFF, bswap(data)); return;
        case 0x8E: m_real3d.write_high_culling(addr & 0xFFFFF, bswap(data)); return;
        case 0x90: m_real3d.write_texture_port(addr & 0xFF, bswap(data)); return;
        case 0x94: m_real3d.write_texture_fifo(bswap(data)); return;
        case 0x98: m_real3d.write_polygon(addr & 0x3FFFFF, bswap(data)); return;
        case 0x9C: m_real3d.write_config_register(addr, bswap(data)); return;
        case 0xC2: m_real3d.write_dma32(addr & 0xFF, bswap(data)); return;

        case 0xF0:
        case 0xFE: {
            const u32 sub = (addr >> 16) & 0xFF;
            switch (sub) {
                case 0x04:
                    write_inputs((addr & 0x3F) + 0, static_cast<u8>(data >> 24));
                    write_inputs((addr & 0x3F) + 1, static_cast<u8>(data >> 16));
                    write_inputs((addr & 0x3F) + 2, static_cast<u8>(data >> 8));
                    write_inputs((addr & 0x3F) + 3, static_cast<u8>(data));
                    return;
                case 0x08: return;  // sound board: 32-bit writes ignored
                case 0x80:          // F0800CF8, MPC105 CONFIG_ADDR
                    if (m_mpc.model() == 0x105) m_mpc.write_config_address(data);
                    return;
                case 0x10:
                    write_system_register((addr & 0x3F) + 0, static_cast<u8>(data >> 24));
                    write_system_register((addr & 0x3F) + 1, static_cast<u8>(data >> 16));
                    write_system_register((addr & 0x3F) + 2, static_cast<u8>(data >> 8));
                    write_system_register((addr & 0x3F) + 3, static_cast<u8>(data));
                    return;
                case 0x14: m_rtc.write_register((addr >> 2) & 0xF, static_cast<u8>(data)); return;
                case 0x1A: write_security(addr & 0x3F, data); return;
                default:
                    if (sub >= 0xC0 && sub <= 0xEF) {
                        if (m_mpc.model() == 0x105 && addr >= 0xF0C00CF8 && addr < 0xF0C00D00) {
                            m_mpc.write_config_data(32, 0, data);  // MPC105
                        } else if (m_mpc.model() == 0x106 && addr >= 0xFEC00000 && addr < 0xFEE00000) {
                            m_mpc.write_config_address(data);  // MPC106
                        } else if (m_mpc.model() == 0x106 && addr >= 0xFEE00000 && addr < 0xFEF00000) {
                            m_mpc.write_config_data(32, 0, data);
                        }
                        return;
                    }
                    break;
            }
            break;
        }

        case 0xF1:  // tile generator registers (VRAM is paged)
            if (addr >= 0xF1180000 && addr < 0xF1180100) {
                m_tilegen.write_register(addr & 0xFF, bswap(data));
                if (addr == 0xF118000C) m_real3d.tilegen_draw_frame(bswap(data));
                return;
            }
            break;

        case 0xF8:  // F8FFF000-F8FFF0FF
            if (m_mpc.model() == 0x105) {
                m_mpc.write_register((addr & 0xFF) + 0, static_cast<u8>(data >> 24));
                m_mpc.write_register((addr & 0xFF) + 1, static_cast<u8>(data >> 16));
                m_mpc.write_register((addr & 0xFF) + 2, static_cast<u8>(data >> 8));
                m_mpc.write_register((addr & 0xFF) + 3, static_cast<u8>(data));
            }
            return;

        case 0xC0:
            if (m_run_net_board) {
                net_write32(addr, data);
                return;
            }
            if (!scsi_at_c0()) break;
            [[fallthrough]];
        case 0xF9:
        case 0xC1:
            m_scsi.write_register((addr & 0xFF) + 0, static_cast<u8>(data >> 24));
            m_scsi.write_register((addr & 0xFF) + 1, static_cast<u8>(data >> 16));
            m_scsi.write_register((addr & 0xFF) + 2, static_cast<u8>(data >> 8));
            m_scsi.write_register((addr & 0xFF) + 3, static_cast<u8>(data));
            return;

        default: break;
    }
    unmapped("write32", addr, data);
}

// =============================================================================
// NVRAM: the .nv block file
// =============================================================================
//
// Blocks of: u32 total length, u32 name length, u32 comment length (both with
// NUL), name, comment, data. A header block, "93C46", "Backup RAM"
// (word-reversed) and "Keisho RTC" (s64), which other readers ignore.

namespace {

void put_u32(std::vector<u8>& out, u32 v)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

void put_block(std::vector<u8>& out, const std::string& name, std::span<const u8> data)
{
    const std::string comment = "Keisho";
    put_u32(out, static_cast<u32>(12 + name.size() + 1 + comment.size() + 1 + data.size()));
    put_u32(out, static_cast<u32>(name.size() + 1));
    put_u32(out, static_cast<u32>(comment.size() + 1));
    out.insert(out.end(), name.begin(), name.end());
    out.push_back(0);
    out.insert(out.end(), comment.begin(), comment.end());
    out.push_back(0);
    out.insert(out.end(), data.begin(), data.end());
}

u32 get_u32(std::span<const u8> in, usize at)
{
    return u32{in[at]} | (u32{in[at + 1]} << 8) | (u32{in[at + 2]} << 16) | (u32{in[at + 3]} << 24);
}

/// The data of block `name`, or an empty span.
std::span<const u8> find_block(std::span<const u8> in, const std::string& name)
{
    usize pos = 0;
    while (pos + 12 <= in.size()) {
        const u32 length   = get_u32(in, pos);
        const u32 name_len = get_u32(in, pos + 4);
        const u32 comm_len = get_u32(in, pos + 8);
        if (length == 0 || pos + length > in.size() || 12u + name_len + comm_len > length) break;
        const char* block_name = reinterpret_cast<const char*>(in.data() + pos + 12);
        if (name_len > 0 && std::string(block_name, name_len - 1) == name) {
            const usize data = pos + 12 + name_len + comm_len;
            return in.subspan(data, pos + length - data);
        }
        pos += length;
    }
    return {};
}

/// Backup RAM between the PowerPC's order and the file's word-reversed one.
void reverse_words(std::span<const u8> src, std::span<u8> dst)
{
    for (usize i = 0; i + 3 < src.size() && i + 3 < dst.size(); i += 4) {
        dst[i]     = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i];
    }
}

}  // namespace

std::vector<u8> Model3::nvram_image() const
{
    std::vector<u8> out;

    std::vector<u8> header;
    put_u32(header, 0);  // format version
    header.insert(header.end(), m_game.name.begin(), m_game.name.end());
    header.push_back(0);
    put_block(out, "Supermodel NVRAM State", header);

    // The 93C46 block: regs[64], CS, CLK, DI, DO, bitBufferOut,
    // bitBufferIn, bitsOut, receiving (bool), addr, busyCycles, locked (bool).
    // Only the words matter; the rest is written idle.
    std::vector<u8> eeprom(m_eeprom.bytes().begin(), m_eeprom.bytes().end());
    put_u32(eeprom, 0);  // CS
    put_u32(eeprom, 0);  // CLK
    put_u32(eeprom, 0);  // DI
    put_u32(eeprom, 1);  // DO
    put_u32(eeprom, 0);  // bitBufferOut
    put_u32(eeprom, 0);  // bitBufferIn
    put_u32(eeprom, 0);  // bitsOut
    eeprom.push_back(1);  // receiving
    put_u32(eeprom, 0);  // addr
    put_u32(eeprom, 0);  // busyCycles
    eeprom.push_back(1);  // locked
    put_block(out, "93C46", eeprom);

    std::vector<u8> backup(kBackupRamSize);
    reverse_words(m_backup_ram, backup);
    put_block(out, "Backup RAM", backup);

    std::vector<u8> rtc(8);
    const s64 t = m_rtc.time();
    std::memcpy(rtc.data(), &t, 8);
    put_block(out, "Keisho RTC", rtc);
    return out;
}

bool Model3::apply_nvram_image(std::span<const u8> image)
{
    if (find_block(image, "Supermodel NVRAM State").size() < 4) return false;
    if (get_u32(find_block(image, "Supermodel NVRAM State"), 0) != 0) return false;

    const auto eeprom = find_block(image, "93C46");
    if (eeprom.size() >= Eeprom93c46::kByteCount) {
        std::copy_n(eeprom.begin(), Eeprom93c46::kByteCount, m_eeprom.bytes().begin());
    }
    const auto backup = find_block(image, "Backup RAM");
    if (backup.size() >= kBackupRamSize) reverse_words(backup.first(kBackupRamSize), m_backup_ram);
    const auto rtc = find_block(image, "Keisho RTC");
    if (rtc.size() >= 8) {
        s64 t = 0;
        std::memcpy(&t, rtc.data(), 8);
        m_rtc.set_time(t);
    }
    return true;
}

void Model3::load_nvram()
{
    // Shipped "backup_ram" (.nv layout) and "eeprom" (64 LE words) regions
    // first; a saved file overrides them.
    {
        const auto shipped = m_roms.region("backup_ram");
        if (shipped.size() == kBackupRamSize) reverse_words(shipped, m_backup_ram);
        (void)apply_default_image(m_roms.region("eeprom"), m_eeprom.bytes());
    }

    if (m_nvram_directory.empty() || m_game.name.empty()) return;
    const std::filesystem::path path = std::filesystem::path(m_nvram_directory) / (m_game.name + ".nv");
    std::FILE* handle = std::fopen(path.string().c_str(), "rb");
    if (handle == nullptr) return;
    std::vector<u8> image;
    u8 buffer[65536];
    usize n = 0;
    while ((n = std::fread(buffer, 1, sizeof(buffer), handle)) > 0) image.insert(image.end(), buffer, buffer + n);
    std::fclose(handle);
    if (!apply_nvram_image(image)) {
        SM2_WARN("model3: '%s' is not a valid NVRAM file; ignoring it", path.string().c_str());
    }
}

void Model3::save_nvram() const
{
    if (m_nvram_directory.empty() || m_game.name.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(m_nvram_directory, error);
    const std::filesystem::path path = std::filesystem::path(m_nvram_directory) / (m_game.name + ".nv");
    const std::vector<u8> image = nvram_image();
    if (std::FILE* handle = std::fopen(path.string().c_str(), "wb")) {
        std::fwrite(image.data(), 1, image.size(), handle);
        std::fclose(handle);
    } else {
        SM2_WARN("model3: cannot write NVRAM '%s'", path.string().c_str());
    }
}

// =============================================================================
// Save states
// =============================================================================

void Model3::serialize(Archive& ar)
{
    // Which optional boards are present must match.
    u8 present = static_cast<u8>((m_cpu ? 1 : 0) | (m_sound ? 2 : 0) | (m_drive ? 4 : 0) | (m_net ? 8 : 0));
    const u8 mine = present;
    ar.raw(present);
    if (ar.loading() && present != mine) {
        SM2_ERROR("model3: save-state was made with different boards attached");
        ar.mark_failed();
        return;
    }

    ar.bytes(m_ram.data(), m_ram.size());
    ar.bytes(m_backup_ram.data(), m_backup_ram.size());
    ar.bytes(m_security_ram.data(), m_security_ram.size());
    ar.bytes(m_net_ram.data(), m_net_ram.size());
    ar.raw(m_crom_bank_reg);
    ar.raw(m_input_bank);
    ar.raw(m_serial_fifo1);
    ar.raw(m_serial_fifo2);
    ar.raw(m_gun_reg);
    ar.raw(m_adc_channel);
    ar.raw(m_midi_ctrl);
    ar.raw(m_output_reg);
    ar.raw(m_security_first_read);
    ar.raw(m_security_ptr);
    ar.raw(m_eeprom_busy);
    ar.raw(m_inputs);
    ar.raw(m_gun_offscreen);
    ar.raw(m_run_net_board);

    m_irq.serialize(ar);
    m_mpc.serialize(ar);
    m_scsi.serialize(ar);
    m_rtc.serialize(ar);
    m_eeprom.serialize(ar);
    m_tilegen.serialize(ar);
    m_real3d.serialize(ar);
    m_jtag.serialize(ar);
    m_crypt.serialize(ar);
    m_sched.serialize(ar);

    ar.raw(m_gpus_ready);
    ar.raw(m_frame_start);
    ar.raw(m_vblank_left);
    ar.raw(m_midi_count);
    ar.raw(m_line);
    ar.raw(m_flip_line);
    ar.raw(m_last_sound);
    ar.raw(m_sound_slice);
    ar.raw(m_frames);
    ar.raw(m_published_count);

    if (m_cpu) m_cpu->serialize(ar);
    if (m_sound) m_sound->serialize(ar);
    if (m_drive) m_drive->serialize(ar);
    if (m_net) m_net->serialize(ar);

    if (ar.loading()) {
        set_crom_bank(m_crom_bank_reg);
        if (m_cpu) {
            m_cpu->invalidate(0, kRamSize);
            m_cpu->set_irq(m_irq.line());
        }
    }
}

bool Model3::save_state_data(std::vector<u8>& out) const
{
    auto* self = const_cast<Model3*>(this);
    return save_state_to_buffer(out, m_game.name, static_cast<u32>(m_game.board), m_in_frame,
                                [self](Archive& ar) { self->serialize(ar); });
}

bool Model3::load_state_data(std::span<const u8> data)
{
    return load_state_from_buffer(data, m_game.name, static_cast<u32>(m_game.board), m_in_frame,
                                  [this](Archive& ar) { serialize(ar); });
}

}  // namespace sm2::hw::model3
