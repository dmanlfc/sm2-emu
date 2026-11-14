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
//
// Model3Sound as the machine's SoundPort. The machine owns the MIDI
// interrupt, so the board's IRQ handler is left unset.

#include "core/archive.h"
#include "hw/model3/model3_sound.h"
#include "hw/model3/ports.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <algorithm>
#include <limits>

namespace sm2::hw::model3 {
namespace {

class SoundBoardPort final : public SoundPort {
public:
    SoundBoardPort(const rom::GameSpec& game, const rom::RomSet& roms)
    {
        m_board.attach(roms.region("sound_program"), roms.region("sound_samples"));
        m_board.attach_mpeg(game.model3.mpeg_board, roms.region("mpeg_program"),
                            roms.region("mpeg_music"));
        m_board.set_audio_layout(game.model3.audio);
    }

    void attach(u32 ppc_hz) override { m_board.set_host_clock(ppc_hz); }
    void reset() override { m_board.reset(); }
    void write_midi(u8 data) override { m_board.write_midi(data); }
    void write_control(u8 data) override { m_board.write_control(data); }
    [[nodiscard]] u8 midi_read(u32 offset) override { return m_board.read_port(offset); }

    void run(u64 ppc_cycles) override
    {
        while (ppc_cycles > 0) {
            const u64 step = std::min<u64>(ppc_cycles, std::numeric_limits<u32>::max());
            m_board.run(static_cast<u32>(step));
            ppc_cycles -= step;
        }
    }

    void serialize(Archive& ar) override { m_board.serialize(ar); }
    [[nodiscard]] SoundBoard* sound_board() override { return &m_board; }
    void set_music_volume(u32 percent) override { m_board.set_music_volume(percent); }

    Model3Sound m_board;
};

}  // namespace

std::unique_ptr<SoundPort> create_sound_port(const rom::GameSpec& game, const rom::RomSet& roms)
{
    if (roms.region("sound_program").empty()) return nullptr;
    return std::make_unique<SoundBoardPort>(game, roms);
}

}  // namespace sm2::hw::model3
