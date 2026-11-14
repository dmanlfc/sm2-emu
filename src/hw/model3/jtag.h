// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Model 3's JTAG test access port, daisy-chained through the Real3D ASICs
// and 3D-RAM chips and reached through the system registers.
//
// Derived from Supermodel's Src/Model3/JTAG.cpp and JTAG.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2003-2024 The Supermodel Team.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// The shift register is a u64, since no register exceeds 32 bits. Modewords
// reach the Real3D through a callback interface.
#pragma once

#include "core/types.h"

#include <array>

namespace sm2 {
class Archive;
}

namespace sm2::hw::model3 {

/// The ASIC types whose modeword the Real3D tracks. Dummy is a second copy of
/// an ASIC whose modeword is not forwarded.
enum class Asic : s8 {
    Mercury = 0,
    Venus   = 1,
    Earth   = 2,
    Mars    = 3,
    Jupiter = 4,
    Dummy   = -1,
};

class JtagModewordSink {
public:
    virtual ~JtagModewordSink() = default;
    virtual void write_jtag_modeword(Asic device, u32 data) = 0;
};

class Jtag {
public:
    explicit Jtag(JtagModewordSink* sink = nullptr);

    void set_sink(JtagModewordSink* sink) { m_sink = sink; }

    /// 0x10, 0x15, 0x20 or 0x21: sets the ID codes and the chain length.
    void set_stepping(int stepping);

    [[nodiscard]] bool read() const { return m_tdo; }
    void write(bool tck, bool tms, bool tdi, bool trst);
    void reset();

    void serialize(Archive& ar);

private:
    enum class State : u8 {
        TestLogicReset, RunTestIdle, SelectDRScan, CaptureDR, ShiftDR, Exit1DR, PauseDR,
        Exit2DR, UpdateDR, SelectIRScan, CaptureIR, ShiftIR, Exit1IR, PauseIR, Exit2IR,
        UpdateIR,
    };

    struct Device {
        bool is_asic       = true;  ///< false: a 3D-RAM chip
        Asic name          = Asic::Dummy;
        u64  shift_reg     = 0;
        u32  shift_size    = 0;
        u8   instruction   = 0;
        u32  id_code       = 0;
        u32  modeword      = 0;

        bool shift(bool tdi);
        void capture_dr();
        void capture_ir();
        void update_ir();
        void reset();
    };

    void update_dr(Device& device);

    JtagModewordSink* m_sink = nullptr;
    State m_state    = State::TestLogicReset;
    bool  m_last_tck = false;
    bool  m_tdo      = false;
    u32   m_num_devices = 10;
    /// In chain order (Step 2.x); Step 1.x uses the first ten.
    std::array<Device, 17> m_chain{};
};

}  // namespace sm2::hw::model3
