// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Model 3 drive boards: the Z80 force-feedback boards for wheels and the
// Star Wars flight stick, the billboard (score digits and lamps) and Ski
// Champ's simulated vibration pad, with the Z80 CTC subset they use.
//
// Derived from Supermodel's Src/Model3/DriveBoard/DriveBoard.cpp, WheelBoard.cpp,
// JoystickBoard.cpp, BillBoard.cpp, SkiBoard.cpp, Z80CTC.cpp and their headers
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2011-2021 Bart Trzynadlowski, Nik Henson, Ian Curtis, Harry Tuttle, and
// Spindizzi (Z80CTC: Copyright (c) 2003-2026 The Supermodel Team).
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// The Z80's interrupt-acknowledge cycle drops INT. The boards are always
// enabled, since games complain when one is missing. Forces and billboard
// state go to Machine::BoardOutputs and controls come from hw::Inputs. Save
// states include the DIP switches.

#include "hw/model3/ports.h"

#include "core/archive.h"
#include "cpu/z80/z80.h"
#include "rom/game.h"
#include "rom/rom_set.h"

#include <algorithm>
#include <array>
#include <vector>

namespace sm2::hw::model3 {
namespace {

// -- Z80 CTC (info: http://www.z80.info/zip/z80ctc.pdf; partial) --------------

class Z80Ctc {
public:
    void write(u32 channel, u8 value)
    {
        Channel& c = m_ch[channel];
        if (value & 0x01) {
            c.control          = value;
            c.interrupt        = (value & 0x80) != 0;
            c.counter_mode     = (value & 0x40) != 0;
            c.prescaler256     = (value & 0x20) != 0;
            c.trigger_rising   = (value & 0x10) != 0;
            c.manual_trigger   = (value & 0x08) != 0;
            c.waiting_constant = (value & 0x04) != 0;
            if (value & 0x02) {
                c.counter = 0;
                c.running = false;
            }
        } else if (c.waiting_constant) {
            c.time_constant    = value ? value : 256;
            c.counter          = c.time_constant;
            c.waiting_constant = false;
            c.running          = !c.manual_trigger;
        }
    }

    [[nodiscard]] u32 frequency(u32 channel, u32 input) const
    {
        const Channel& c = m_ch[channel];
        if (!c.counter_mode) {
            const u32 div = u32{c.time_constant} * (c.prescaler256 ? 256u : 16u);
            return div ? input / div : 0;
        }
        if (c.trigger_rising && channel > 0 && c.counter != 0) {
            return frequency(channel - 1, input) / c.counter;
        }
        return 0;
    }

    [[nodiscard]] bool running(u32 channel) const { return m_ch[channel].running; }
    void reset() { m_ch = {}; }

    void serialize(Archive& ar)
    {
        for (Channel& c : m_ch) {
            ar.raw(c.control);
            ar.raw(c.time_constant);
            ar.raw(c.counter);
            ar.raw(c.interrupt);
            ar.raw(c.counter_mode);
            ar.raw(c.prescaler256);
            ar.raw(c.trigger_rising);
            ar.raw(c.manual_trigger);
            ar.raw(c.waiting_constant);
            ar.raw(c.running);
        }
    }

private:
    struct Channel {
        u8   control          = 0;
        u16  time_constant    = 0;
        u16  counter          = 0;
        bool interrupt        = false;
        bool counter_mode     = false;
        bool prescaler256     = false;
        bool trigger_rising   = false;
        bool manual_trigger   = false;
        bool waiting_constant = false;
        bool running          = false;
    };
    std::array<Channel, 4> m_ch{};
};

u8 analog_value(const rom::GameSpec& game, const Inputs& inputs, rom::AnalogControl control)
{
    for (usize i = 0; i < game.analog.size() && i < inputs.analog.size(); ++i) {
        if (game.analog[i].control == control) return inputs.analog[i];
    }
    return 0x80;
}

// -- DriveBoard ------------------------------------------------------------------

class DriveBoard : public DriveBoardPort, public cpu::z80::Bus {
public:
    static constexpr u32 kRomSize = 0x9000;
    static constexpr u32 kRamSize = 0x2000;
    static constexpr u32 kClock   = 8'000'000;

    DriveBoard(const rom::GameSpec& game, std::span<const u8> rom, const Inputs& inputs,
               Machine::BoardOutputs& outputs)
        : m_game(game), m_inputs(inputs), m_out(outputs), m_rom(kRomSize, 0xFF), m_ram(kRamSize, 0),
          m_z80(*this)
    {
        std::copy_n(rom.begin(), std::min<usize>(rom.size(), kRomSize), m_rom.begin());
    }

    void reset() override
    {
        m_initialized    = false;
        m_allow_int      = false;
        m_allow_nmi      = false;
        m_data_sent      = 0;
        m_data_received  = 0;
        m_nmi_timer      = 0;
        m_z80.reset();
        m_z80.set_irq_line(false);
        m_z80.set_nmi_line(false);
        m_ctc.reset();
        set_ffb_strength(5);
    }

    void write(u8 data) override { m_data_sent = data; }
    [[nodiscard]] u8 read() override { return m_data_received; }

    void run_frame() override
    {
        s32 frame = static_cast<s32>(kClock / 57.524);
        while (frame > 0) {
            if (m_allow_nmi) {
                const s32 n = std::max<s32>(1, std::min(frame, m_nmi_timer));
                (void)m_z80.run(n);
                frame -= n;
                m_nmi_timer -= n;
            } else {
                (void)m_z80.run(frame);
                frame = 0;
            }
            if (m_allow_nmi && m_nmi_timer <= 0) {
                m_z80.set_nmi_line(true);
                m_z80.set_nmi_line(false);
                m_nmi_timer = nmi_period();
            }
            if (m_allow_int) m_z80.set_irq_line(true);
        }
    }

    void serialize(Archive& ar) override
    {
        ar.bytes(m_ram.data(), m_ram.size());
        ar.raw(m_initialized);
        ar.raw(m_allow_nmi);
        ar.raw(m_allow_int);
        ar.raw(m_data_sent);
        ar.raw(m_data_received);
        ar.raw(m_nmi_timer);
        ar.raw(m_dip1);
        ar.raw(m_dip2);
        m_ctc.serialize(ar);
        m_z80.serialize(ar);
    }

    // -- cpu::z80::Bus --
    [[nodiscard]] u8 read8(u16 addr) override
    {
        if (addr < kRomSize) return m_rom[addr];
        if (addr >= 0xE000) return m_ram[(addr - 0xE000) & 0x1FFF];
        return 0xFF;
    }
    void write8(u16 addr, u8 data) override
    {
        if (addr >= 0xE000) m_ram[(addr - 0xE000) & 0x1FFF] = data;
    }
    [[nodiscard]] u8 io_read8(u16) override { return 0xFF; }
    void io_write8(u16 port, u8 data) override
    {
        port &= 0xFF;
        if (port >= 0x10 && port <= 0x13) {
            const u32 channel = port & 3;
            m_ctc.write(channel, data);
            if (channel == 1) {
                m_allow_nmi = m_ctc.running(1);
                if (m_allow_nmi) m_nmi_timer = nmi_period();
            }
        }
    }
    [[nodiscard]] u8 interrupt_vector() override
    {
        m_z80.set_irq_line(false);
        return 0xFF;
    }

protected:
    [[nodiscard]] s32 nmi_period() const
    {
        const u32 f = m_ctc.frequency(1, kClock);
        return f ? static_cast<s32>(kClock / f) : static_cast<s32>(kClock);
    }
    void set_ffb_strength(unsigned strength)
    {
        m_dip1 = static_cast<u8>((m_dip1 & 0xE3) | (((~(strength - 1)) & 7) << 2));
    }
    [[nodiscard]] u8 analog(rom::AnalogControl control) const { return analog_value(m_game, m_inputs, control); }

    const rom::GameSpec&   m_game;
    const Inputs&          m_inputs;
    Machine::BoardOutputs& m_out;
    std::vector<u8>        m_rom;
    std::vector<u8>        m_ram;
    cpu::z80::Z80          m_z80;
    Z80Ctc                 m_ctc;

    bool m_initialized   = false;
    bool m_allow_int     = false;
    bool m_allow_nmi     = false;
    u8   m_data_sent     = 0;
    u8   m_data_received = 0;
    u8   m_dip1          = 0;
    u8   m_dip2          = 0;
    s32  m_nmi_timer     = 0;
};

// -- the ADC, 7-segment and encoder ports shared by the wheel and joystick boards --

class FeedbackBoard : public DriveBoard {
public:
    FeedbackBoard(const rom::GameSpec& game, std::span<const u8> rom, const Inputs& inputs,
                  Machine::BoardOutputs& outputs)
        : DriveBoard(game, rom, inputs, outputs)
    {
        m_dip1 = 0xCF;
        m_dip2 = 0xFF;
    }

    void reset() override
    {
        DriveBoard::reset();
        m_seg = {0xFF, 0xFF, 0xFF, 0xFF};
        m_adc_port = 0;
        m_adc_bit  = 0;
        m_port42 = m_port45 = m_port46 = 0;
        m_prev42 = m_prev45 = m_prev46 = 0;
        m_uncenter1 = m_uncenter2 = 0;
        stop_all();
        m_out.force_feedback = true;
    }

    void write(u8 data) override
    {
        DriveBoard::write(data);
        if (data == 0xCB) m_initialized = false;
    }

    void serialize(Archive& ar) override
    {
        DriveBoard::serialize(ar);
        ar.raw(m_seg);
        ar.raw(m_adc_port);
        ar.raw(m_adc_bit);
        ar.raw(m_port42);
        ar.raw(m_port45);
        ar.raw(m_port46);
        ar.raw(m_prev42);
        ar.raw(m_prev45);
        ar.raw(m_prev46);
        ar.raw(m_uncenter1);
        ar.raw(m_uncenter2);
        ar.raw(m_out.constant);
        ar.raw(m_out.constant_y);
        ar.raw(m_out.self_center);
        ar.raw(m_out.friction);
        ar.raw(m_out.vibrate);
    }

    [[nodiscard]] u8 io_read8(u16 port) override
    {
        port &= 0xFF;
        switch (port) {
            case 0x20: return m_dip1;
            case 0x21: return m_dip2;
            case 0x24:
            case 0x25:
            case 0x26:
            case 0x27:
                if (port == m_adc_port && m_adc_bit-- > 0) return (adc(port - 0x24) >> m_adc_bit) & 1;
                return 0xFF;
            case 0x28: return m_data_sent;
            case 0x2C: return 0x00;  // encoder OK
            default: return 0xFF;
        }
    }

    void io_write8(u16 port, u8 data) override
    {
        DriveBoard::io_write8(port, data);
        port &= 0xFF;
        switch (port) {
            case 0x20:
            case 0x21:
            case 0x22:
            case 0x23: m_seg[port - 0x20] = data; break;
            case 0x24:
            case 0x25:
            case 0x26:
            case 0x27:
                m_adc_port = static_cast<u8>(port);
                m_adc_bit  = 8;
                break;
            case 0x29:
                m_data_received = data;
                if (data == init_reply()) m_initialized = true;
                break;
            case 0x2A:
                m_port42 = data;
                encoder();
                break;
            case 0x2D:
                m_port45 = data;
                if (joystick()) encoder();
                break;
            case 0x2E: m_port46 = data; break;
            default: break;
        }
    }

protected:
    [[nodiscard]] virtual bool joystick() const { return false; }
    [[nodiscard]] virtual u8   init_reply() const = 0;
    [[nodiscard]] virtual u8   adc(unsigned channel) const = 0;
    virtual void encoder() = 0;

    void stop_all()
    {
        m_out.constant = m_out.constant_y = 0.0f;
        m_out.self_center = m_out.friction = m_out.vibrate = 0.0f;
    }
    static float signed_force(s8 v) { return static_cast<float>(v) / (v >= 0 ? 127.0f : 128.0f); }

    std::array<u8, 4> m_seg{};
    u8 m_adc_port = 0;
    u8 m_adc_bit  = 0;
    u8 m_port42 = 0, m_port45 = 0, m_port46 = 0;
    u8 m_prev42 = 0, m_prev45 = 0, m_prev46 = 0;
    u8 m_uncenter1 = 0, m_uncenter2 = 0;
};

// -- WheelBoard ------------------------------------------------------------------

class WheelBoard final : public FeedbackBoard {
public:
    using FeedbackBoard::FeedbackBoard;

private:
    [[nodiscard]] u8 init_reply() const override { return 0xCC; }

    [[nodiscard]] u8 adc(unsigned channel) const override
    {
        switch (channel) {
            case 1: return m_initialized ? analog(rom::AnalogControl::Steer) : 0x80;
            case 2: return 0x80;
            default: return 0x00;
        }
    }

    void encoder() override
    {
        if (m_prev42 == m_port42 && m_prev46 == m_port46) return;
        switch (m_port46) {
            case 0xFC:
                if (m_port42 & 0x04) {
                    if (m_port42 & 0x80) {
                        m_out.self_center = 0.0f;
                    } else {
                        const u8 strength = static_cast<u8>(((m_port42 & 0x78) >> 3) * 0x10 + 0xF);
                        m_out.self_center = strength / 255.0f;
                    }
                } else {
                    const u8 seq  = m_port42 & 0x03;
                    const u8 data = (m_port42 & 0xF0) >> 4;
                    switch (seq) {
                        case 0: m_uncenter1 = static_cast<u8>(data << 4); break;
                        case 1: m_uncenter1 |= data; break;
                        case 2: m_uncenter2 = static_cast<u8>(data << 4); break;
                        case 3: m_uncenter2 |= data; break;
                    }
                    if (seq == 0 && m_uncenter1 == 0) {
                        m_out.vibrate = 0.0f;
                    } else if (seq == 3 && m_uncenter1 > 0) {
                        const u8 strength = static_cast<u8>(((m_uncenter1 >> 1) - 7) * 0x50
                                                            + ((m_uncenter2 >> 1) - 5) * 0x10 + 0xF);
                        m_out.vibrate = strength / 255.0f;
                    }
                }
                break;
            case 0xFE:
                // 0x80 stop, 0x81-0xC0 left, 0x40-0x7F right; 0x7F and 0x81 often mean stop.
                if (m_port42 > 0x81) {
                    m_out.constant = signed_force(m_port42 <= 0xC0 ? static_cast<s8>(2 * (0x81 - m_port42)) : s8{-0x80});
                } else if (m_port42 < 0x7F) {
                    m_out.constant = signed_force(m_port42 >= 0x40 ? static_cast<s8>(2 * (0x7F - m_port42)) : s8{0x7F});
                } else {
                    m_out.constant = 0.0f;
                }
                break;
            case 0xFF:
                if (m_port42 == 0xFF) stop_all();
                break;
            default: break;
        }
        m_prev42 = m_port42;
        m_prev46 = m_port46;
    }
};

// -- JoystickBoard ----------------------------------------------------------------

class JoystickBoard final : public FeedbackBoard {
public:
    using FeedbackBoard::FeedbackBoard;

private:
    [[nodiscard]] bool joystick() const override { return true; }
    [[nodiscard]] u8   init_reply() const override { return 0xCB; }

    [[nodiscard]] u8 adc(unsigned channel) const override
    {
        switch (channel) {
            case 0: return m_initialized ? analog(rom::AnalogControl::StickY) : 0x80;
            case 1: return m_initialized ? analog(rom::AnalogControl::StickX) : 0x80;
            default: return 0x80;
        }
    }

    void encoder() override
    {
        if (m_prev42 == m_port42 && m_prev46 == m_port46 && m_prev45 == m_port45) return;
        const bool centre = m_port42 == 0x7F && m_port45 == 0x81;
        switch (m_port46) {
            case 0xEE:
                m_out.constant    = signed_force(static_cast<s8>(2 * (m_port42 - 0x7F)));
                m_out.constant_y  = signed_force(static_cast<s8>(-2 * (m_port45 - 0x7F)));
                m_out.self_center = centre ? 1.0f : 0.0f;
                break;
            case 0xFF:
                if (m_port42 == 0xFF && m_port45 == 0xFF) stop_all();
                break;
            case 0xCE:
                if (m_port42 == 0x7F && m_port45 != 0x81) m_out.constant = signed_force(static_cast<s8>(2 * m_port45));
                if (centre) m_out.self_center = 1.0f;
                break;
            case 0xEC:
                if (m_port45 == 0x81 && m_port42 != 0x7F) m_out.constant_y = signed_force(static_cast<s8>(2 * m_port42));
                if (centre) m_out.self_center = 1.0f;
                break;
            default: break;
        }
        m_prev42 = m_port42;
        m_prev46 = m_port46;
        m_prev45 = m_port45;
    }
};

// -- BillBoard -------------------------------------------------------------------

class BillBoard final : public DriveBoard {
public:
    BillBoard(const rom::GameSpec& game, std::span<const u8> rom, const Inputs& inputs,
              Machine::BoardOutputs& outputs)
        : DriveBoard(game, rom, inputs, outputs)
    {
        m_dip1 = 0x0F;
    }

    void reset() override
    {
        DriveBoard::reset();
        m_dip1 = 0x0F;
        m_out.billboard = true;
        m_out.bill.fill(0xFF);
    }

    [[nodiscard]] bool is_billboard() const override { return true; }

    void serialize(Archive& ar) override
    {
        DriveBoard::serialize(ar);
        ar.raw(m_out.bill);
    }

    [[nodiscard]] u8 io_read8(u16 port) override
    {
        switch (port & 0xFF) {
            case 0x20: return m_dip1;  // 0x80: test all segments
            case 0x21: return m_data_sent;
            default: return 0xFF;
        }
    }

    void io_write8(u16 port, u8 data) override
    {
        port &= 0xFF;
        switch (port) {
            case 0x22:
            case 0x23:
            case 0x24:
            case 0x25:
            case 0x26:
                m_out.bill[port - 0x22] = data;
                m_data_received = data;
                break;
            case 0x28:
                if (data == 0x03) m_allow_int = true;
                break;
            case 0x2E:
                if (data == 0x00) m_initialized = true;
                break;
            default: break;
        }
    }
};

// -- the joystick board without its program (none of the dumped sets has it) -------

// Start-up handshake only (Star Wars Trilogy waits for CE, CD, CB); no forces.
class SimJoystickBoard final : public DriveBoard {
public:
    SimJoystickBoard(const rom::GameSpec& game, const Inputs& inputs, Machine::BoardOutputs& outputs)
        : DriveBoard(game, {}, inputs, outputs)
    {
    }

    void reset() override
    {
        DriveBoard::reset();
        m_init_state = 0;
    }

    [[nodiscard]] u8 read() override
    {
        if (m_initialized) return 0xCB;
        switch (m_init_state / 5) {
            case 0: return 0xCF;
            case 1: return 0xCE;
            case 2: return 0xCD;
            default:
                m_initialized = true;
                return 0xCB;
        }
    }

    void write(u8 data) override
    {
        m_data_sent = data;
        if (data == 0xCB) {
            m_initialized = false;
            m_init_state  = 0;
        }
    }

    void run_frame() override
    {
        if (!m_initialized && m_init_state < 0xFF) ++m_init_state;
    }

    void serialize(Archive& ar) override
    {
        ar.raw(m_initialized);
        ar.raw(m_data_sent);
        ar.raw(m_init_state);
    }

private:
    u8 m_init_state = 0;
};

// -- SkiBoard (simulated) -----------------------------------------------------------

class SkiBoard final : public DriveBoard {
public:
    SkiBoard(const rom::GameSpec& game, const Inputs& inputs, Machine::BoardOutputs& outputs)
        : DriveBoard(game, {}, inputs, outputs)
    {
        m_dip1 = 0xCF;
    }

    void reset() override
    {
        DriveBoard::reset();
        m_init_state = 0;
        m_read_mode  = 0;
        m_status     = 0;
        m_out.force_feedback = true;
        m_out.vibrate = 0.0f;
    }

    [[nodiscard]] u8 read() override
    {
        if (m_initialized) {
            switch (m_read_mode) {
                case 0x0: return m_status++;  // error code; changing it passes the check
                case 0x1: return m_dip1;
                default: return 0xFF;          // 0x4: foot sensors, both
            }
        }
        switch (m_init_state / 5) {
            case 0: return 0xCF;
            case 1: return 0xCE;
            case 2: return 0xCD;
            case 3: return 0xCC;
            default:
                m_initialized = true;
                return 0x80;
        }
    }

    void write(u8 cmd) override
    {
        const u8 val = cmd & 0xF;
        switch (cmd >> 4) {
            case 0x1:
                if (val == 0) m_out.vibrate = 0.0f;
                break;
            case 0x4: m_out.vibrate = val * 0x11 / 255.0f; break;
            case 0xA:
                if (val == 5) m_out.vibrate = val * 0x11 / 255.0f;
                if (val == 6) m_out.vibrate = 0.0f;
                break;
            case 0xD: m_read_mode = val & 7; break;
            default: break;
        }
    }

    void run_frame() override
    {
        if (!m_initialized) ++m_init_state;
    }

    void serialize(Archive& ar) override
    {
        ar.raw(m_initialized);
        ar.raw(m_init_state);
        ar.raw(m_read_mode);
        ar.raw(m_status);
        ar.raw(m_out.vibrate);
    }

    [[nodiscard]] u8 read8(u16) override { return 0xFF; }
    void write8(u16, u8) override {}

private:
    u8 m_init_state = 0;
    u8 m_read_mode  = 0;
    u8 m_status     = 0;
};

}  // namespace

std::unique_ptr<DriveBoardPort> create_drive_board(const rom::GameSpec& game, const rom::RomSet& roms,
                                                   const Inputs& inputs, Machine::BoardOutputs& outputs)
{
    using Type = rom::Model3DriveBoard;
    const std::span<const u8> program = roms.region("driveboard_program");
    switch (game.model3.drive_board) {
        case Type::Ski: return std::make_unique<SkiBoard>(game, inputs, outputs);
        case Type::Wheel:
            if (!program.empty()) return std::make_unique<WheelBoard>(game, program, inputs, outputs);
            break;
        case Type::Joystick:
            if (!program.empty()) return std::make_unique<JoystickBoard>(game, program, inputs, outputs);
            return std::make_unique<SimJoystickBoard>(game, inputs, outputs);
        case Type::Billboard:
            if (!program.empty()) return std::make_unique<BillBoard>(game, program, inputs, outputs);
            break;
        case Type::None: break;
    }
    return nullptr;
}

}  // namespace sm2::hw::model3
