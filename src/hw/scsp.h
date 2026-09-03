// SPDX-License-Identifier: BSD-3-Clause
//
// Yamaha YMF292-F SCSP: 32 PCM slots, an effects DSP, three timers and a MIDI
// port.
//
// Ported from MAME's src/devices/sound/scsp.h and scsp.cpp (BSD-3-Clause,
// copyright-holders ElSemi, R. Belmont).
//
// Changes from upstream, all of them at the edges rather than in the synthesis:
//
//   device_rom_interface       ScspMemory, the sound RAM the 68000 also uses
//   sound_stream               generate(), which fills a caller's buffer
//   emu_timer x3               deadlines counted in samples; see kick_timer
//   device_serial_interface    MIDI moved from bit level to byte level, because
//                              the only thing on the other end is an i8251 whose
//                              bit timing nothing observes. See midi_in.
//   machine().rand()           a fixed-seed generator, so runs are reproducible
//   set_output_gain            m_master_gain, applied in DoMasterSamples
//   save_item / device_post_load  dropped; no save states yet
//
// The slot update, envelope, LFO and DSP feed are upstream's, so fixes there stay
// diffable.

#pragma once

#include "core/types.h"
#include "hw/scsp_dsp.h"

#include <functional>

namespace sm2::hw {

class Scsp {
public:
    /// 45.1584 MHz / 2 on the Model 2A video board.
    static constexpr u32 kDefaultClock = 22'579'200;

    /// The sample rate is the clock over this, which is 44100 Hz exactly.
    static constexpr u32 kClockDivider = 512;

    Scsp(ScspMemory& memory, u32 clock);

    void reset();

    /// Called when a timer or the MIDI FIFO wants the sound 68000's attention.
    /// `level` is the 68000 interrupt level the SCSP has been programmed to use.
    using IrqHandler = std::function<void(int level, bool assert)>;

    /// Called when the SCSP wants the *host* CPU board's attention, over the
    /// MCIEB/MCIPD pair. Wired to the CPU board's interrupt latch.
    using MainIrqHandler = std::function<void(bool assert)>;

    /// A byte the sound program sent out of the MIDI port, bound for the host.
    using MidiOutHandler = std::function<void(u8 value)>;

    void set_irq_handler(IrqHandler handler) { m_irq_cb = std::move(handler); }
    void set_main_irq_handler(MainIrqHandler handler) { m_main_irq_cb = std::move(handler); }
    void set_midi_out_handler(MidiOutHandler handler) { m_midi_out_cb = std::move(handler); }

    // -- register access, from the sound 68000 ------------------------------
    // `offset` is a word index, as MAME's read/write take it.

    [[nodiscard]] u16 read(u32 offset);
    void write(u32 offset, u16 data, u16 mem_mask = 0xffff);

    // -- audio --------------------------------------------------------------

    /// Produce `frames` interleaved stereo samples at 44100 Hz.
    ///
    /// This is also where time passes for the SCSP: the timers, the envelopes,
    /// the LFOs and the MIDI transmitter all advance one step per frame, so the
    /// caller has to keep calling it whether or not anything is listening.
    void generate(s16* output, u32 frames);

    [[nodiscard]] u32 sample_rate() const { return m_clock / kClockDivider; }

    /// A byte arrived from the host's UART.
    void midi_in(u8 value);

    // -- inspection, for the headless bring-up test --------------------------

    struct Stats {
        u64 samples          = 0;
        u64 slot_starts      = 0;
        u64 timer_interrupts = 0;
        u64 midi_in_bytes    = 0;
        u64 midi_out_bytes   = 0;
        u64 dma_transfers    = 0;
        /// Largest absolute value either output channel has reached, so silence
        /// can be told from clipping.
        s32 peak_output      = 0;
    };
    [[nodiscard]] const Stats& stats() const { return m_stats; }

    /// How many of the 32 slots are currently sounding.
    [[nodiscard]] u32 active_slots() const;

private:
    enum SCSP_STATE { SCSP_ATTACK, SCSP_DECAY1, SCSP_DECAY2, SCSP_RELEASE };

    struct SCSP_EG_t {
        int volume;
        SCSP_STATE state;
        int step;
        // step vals
        int AR;   // Attack
        int D1R;  // Decay1
        int D2R;  // Decay2
        int RR;   // Release
        int DL;   // Decay level
        u8 EGHOLD;
        u8 LPLINK;
    };

    struct SCSP_LFO_t {
        u16 phase;
        u32 phase_step;
        int *table;
        int *scale;
    };

    struct SCSP_SLOT {
        union {
            u16 data[0x10];  // only 0x1a bytes used
            u8 datab[0x20];
        } udata;
        u8 Backwards;  // the wave is playing backwards
        u8 active;     // this slot is currently playing
        u32 cur_addr;  // current play address (24.8)
        u32 nxt_addr;  // next play address
        u32 step;      // pitch step (24.8)
        SCSP_EG_t EG;      // Envelope
        SCSP_LFO_t PLFO;   // Phase LFO
        SCSP_LFO_t ALFO;   // Amplitude LFO
        int slot;
        s16 Prev;  // Previous sample (for interpolation)
    };

    // -- upstream's methods, names unchanged --------------------------------

    u8 DecodeSCI(u8 irq);
    void CheckPendingIRQ();
    void MainCheckPendingIRQ(u16 irq_type);
    void ResetInterrupts();
    int Get_AR(int base, int R);
    int Get_DR(int base, int R);
    void Compute_EG(SCSP_SLOT *slot);
    int EG_Update(SCSP_SLOT *slot);
    u32 Step(SCSP_SLOT *slot);
    void Compute_LFO(SCSP_SLOT *slot);
    void StartSlot(SCSP_SLOT *slot);
    void StopSlot(SCSP_SLOT *slot, int keyoff);
    void init();
    void UpdateSlotReg(int s, int r);
    void UpdateReg(int reg);
    void UpdateSlotRegR(int slot, int reg);
    void UpdateRegR(int reg);
    void w16(u32 addr, u16 val);
    u16 r16(u32 addr);
    inline s32 UpdateSlot(SCSP_SLOT *slot);
    void DoMasterSamples(s16 *output, u32 frames);
    void exec_dma();
    void LFO_Init();
    s32 PLFO_Step(SCSP_LFO_t *LFO);
    s32 ALFO_Step(SCSP_LFO_t *LFO);
    void LFO_ComputeStep(SCSP_LFO_t *LFO, u32 LFOF, u32 LFOWS, u32 LFOS, int ALFO);

    // -- replacements for MAME's framework ----------------------------------

    /// MAME's timerA/B/C emu_timers. The delay upstream asks for is
    /// attotime::from_ticks(512, (clock/prescale) / (255 - value)), which at a
    /// sample rate of clock/512 is exactly prescale * (255 - value) samples, so
    /// the timers become deadlines on the sample counter and need no scheduler.
    void kick_timer(int which, u16 value);
    void service_timers();
    void timer_expired(int which);

    /// One sample of the MIDI transmitter's byte clock. Upstream lets
    /// device_serial_interface shift bits out; nothing here observes the bits, so
    /// the byte is delivered whole after one byte time. See the note in scsp.cpp.
    void service_midi_out();
    void begin_midi_transmit(u8 value);

    /// MAME's machine().rand(), seeded fixed so that two runs of the emulator
    /// produce the same noise. Used for the noise slot type and for the LFO's
    /// noise waveform table.
    u32 next_random();

    /// MAME's devcb objects are callable whether or not anything is bound;
    /// std::function is not, so these wrap the null check.
    void irq_cb(int level, bool assert);
    void main_irq_cb(bool assert);
    [[nodiscard]] bool has_irq_handler() const { return static_cast<bool>(m_irq_cb); }

    void warn_dma_gate();

    [[nodiscard]] u8 read_byte(u32 address) { return m_memory->scsp_read_byte(address); }
    [[nodiscard]] u16 read_word(u32 address) { return m_memory->scsp_read_word(address); }
    void write_word(u32 address, u16 value) { m_memory->scsp_write_word(address, value); }

    // -- state -------------------------------------------------------------

    ScspMemory *m_memory = nullptr;
    u32 m_clock = kDefaultClock;

    IrqHandler     m_irq_cb;
    MainIrqHandler m_main_irq_cb;
    MidiOutHandler m_midi_out_cb;

    union {
        u16 data[0x30 / 2];
        u8 datab[0x30];
    } m_udata;

    SCSP_SLOT m_Slots[32];
    s16 m_RINGBUF[128];
    u8 m_BUFPTR;

    u32 m_IrqTimA;
    u32 m_IrqTimBC;
    u32 m_IrqMidi;
    u32 m_IrqCPU;
    u32 m_IrqDMA;
    u8 m_latched_MSLC;
    u16 m_latched_MSLC_data;
    u8 m_MidiOutStack[32];
    u8 m_MidiOutW, m_MidiOutR;
    u8 m_MidiStack[32];
    u8 m_MidiW, m_MidiR;
    s32 m_EG_TABLE[0x400];
    int m_LPANTABLE[0x10000];
    int m_RPANTABLE[0x10000];
    int m_TimPris[3];
    int m_TimCnt[3];

    /// Sample counts at which timers A, B and C fire. Negative means idle.
    s64 m_TimDeadline[3];

    // DMA stuff
    struct {
        u32 dmea;
        u16 drga;
        u16 dtlg;
        u8 dgate;
        u8 ddir;
    } m_dma;

    u16 m_mcieb;
    u16 m_mcipd;
    int m_ARTABLE[64], m_DRTABLE[64];
    ScspDsp m_DSP;
    s16 *m_RBUFDST;  // this points to where the sample will be stored in the RingBuf

    // LFO
    int m_PLFO_TRI[256], m_PLFO_SQR[256], m_PLFO_SAW[256], m_PLFO_NOI[256];
    int m_ALFO_TRI[256], m_ALFO_SQR[256], m_ALFO_SAW[256], m_ALFO_NOI[256];
    int m_PSCALES[8][256];
    int m_ASCALES[8][256];

    /// MVOL, as a multiplier. MAME sets it as a stream output gain; here it is
    /// applied where the mixer writes its result.
    float m_master_gain = 1.0f;

    /// Samples produced since reset. The timers and the MIDI transmitter are
    /// scheduled against this.
    s64 m_sample_count = 0;

    /// Samples left before the MIDI transmitter finishes the byte in flight.
    /// Zero means idle, and m_midi_transmit_byte is what will be delivered.
    u32 m_midi_out_countdown = 0;
    u8  m_midi_transmit_byte = 0;

    u32 m_random_state = 0x1234'5678;

    /// Warned-once flags for conditions upstream reports through popmessage or
    /// logerror on every occurrence, which would otherwise flood the log.
    bool m_warned_invalid_ssctl = false;
    bool m_warned_scieb         = false;
    bool m_warned_cpu_irq       = false;
    bool m_warned_mcieb         = false;
    bool m_warned_dma_gate      = false;
    bool m_warned_dma_irq       = false;

    Stats m_stats;
};

}  // namespace sm2::hw
