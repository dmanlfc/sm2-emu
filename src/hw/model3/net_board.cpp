// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Model 3 network board, simulated: the handshake the games' net code
// expects from the board's 68000 firmware, and the per-frame exchange of each
// cabinet's comm RAM segment around a ring of cabinets.
//
// Derived from Supermodel's Src/Network/SimNetBoard.cpp and SimNetBoard.h
// (https://github.com/trzy/Supermodel), GPL-3.0-or-later. Copyright (c)
// 2011-2020 Bart Trzynadlowski, Nik Henson, Ian Curtis, Harry Tuttle, and
// Spindizzi.
// Keisho changes Copyright (c) 2025+ Daniel Martin (dmanlfc).
//
// NOTICE's section 7 additional terms apply to Keisho's changes only.
//
// Cabinets talk over a hw::CommTransport, and a LoopbackTransport makes a ring
// of one. Non-blocking transports get a short bounded poll. The board is always
// attached when the spec has one. RAM is accessed as little-endian.

#include "hw/model3/ports.h"

#include "core/archive.h"
#include "core/log.h"
#include <miniz.h>
#include "hw/comm_board.h"
#include "hw/comm_transport.h"
#include "rom/game.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace sm2::hw::model3 {
namespace {

constexpr u64 kNetGuid = 0x5bf177da34872;

class SimNetBoard final : public NetBoardPort, public CommBoard {
public:
    enum class State : u8 { Start, Init, Testing, Ready, Error };
    enum class GameType : u8 { One, Two };

    SimNetBoard(const rom::GameSpec& game, std::span<u8> net_ram, GameType type)
        : m_ram(net_ram), m_buffer(0x20000, 0), m_type(type), m_dirtdvls(is(game, "dirtdvls")),
          m_lockstep(is(game, "skichamp"))
    {
        set_transport(nullptr);
        if (m_lockstep) m_delay = 0;
    }

    ~SimNetBoard() override
    {
        if (m_state == State::Ready && !m_loopback) {
            send_tagged(kLeave, nullptr, 0);
            report_stalls();
        }
    }

    static bool is(const rom::GameSpec& game, const char* name) { return game.name == name || game.parent == name; }

    // -- NetBoardPort --
    void reset() override
    {
        if (m_state == State::Ready) {
            if (!m_loopback) SM2_INFO("net board: reset by the game while linked");
            send_tagged(kLeave, nullptr, 0);
        }
        m_running = false;
        m_state   = State::Start;
    }

    [[nodiscard]] u8 read_comm_ram8(u32 addr) override { return ext()[addr & 0xFFFF]; }
    [[nodiscard]] u16 read_comm_ram16(u32 addr) override { return get16(ext(), addr & 0xFFFE); }
    [[nodiscard]] u32 read_comm_ram32(u32 addr) override
    {
        const u8* p = ext() + (addr & 0xFFFC);
        return u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
    }
    void write_comm_ram8(u32 addr, u8 data) override { ext()[addr & 0xFFFF] = data; }
    void write_comm_ram16(u32 addr, u16 data) override { put16(ext(), addr & 0xFFFE, data); }
    void write_comm_ram32(u32 addr, u32 data) override
    {
        u8* p = ext() + (addr & 0xFFFC);
        for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(data >> (8 * i));
    }

    [[nodiscard]] u16 read_io_register(u32 reg) override
    {
        if (!m_running) return 0;
        switch (reg) {
            case 0x00: return m_irq2_ack;
            case 0x88: return m_status0;
            case 0x8A: return m_status1;
            default: return 0;
        }
    }

    void write_io_register(u32 reg, u16 data) override
    {
        switch (reg) {
            case 0x00: m_irq2_ack = data; break;
            case 0x88: m_status0 = data; break;
            case 0x8A: m_status1 = data; break;
            case 0xC0:
                if (data == 0) reset();
                m_running = data != 0;
                break;
            default: break;
        }
    }

    void run_frame() override
    {
        if (!m_running) return;
        switch (m_state) {
            case State::Start:
                m_status0 = 0;
                m_status1 = m_dirtdvls ? 0x4004 : 0xE000;
                m_state   = State::Init;
                break;
            case State::Init: init(); break;
            case State::Testing:
                if (m_type == GameType::One) testing_one();
                else testing_two();
                break;
            case State::Ready: ready(); break;
            case State::Error: break;
        }
    }

    void serialize(Archive& ar) override
    {
        ar.bytes(m_buffer.data(), m_buffer.size());
        ar.raw(m_state);
        ar.raw(m_num_machines);
        ar.raw(m_machine_index);
        ar.raw(m_counter);
        ar.raw(m_segment_size);
        ar.raw(m_running);
        ar.raw(m_irq2_ack);
        ar.raw(m_status0);
        ar.raw(m_status1);
        ar.raw(m_commbank);
        if (ar.loading()) m_transport->reset();
    }

    [[nodiscard]] CommBoard* comm() override { return this; }

    // -- CommBoard --
    void set_transport(std::unique_ptr<CommTransport> transport) override
    {
        m_loopback  = transport == nullptr;
        m_transport = transport ? std::move(transport) : std::make_unique<LoopbackTransport>();
    }
    // Ski Champ's cabinets check each other's IDs within the frame; any
    // delay reads as a duplicate cabinet, so it reads with no delay.
    void set_link_delay(u32 frames) override { m_delay = m_lockstep ? 0 : std::max(frames, 1u); }
    [[nodiscard]] bool link_alive() const override { return m_state == State::Ready; }
    [[nodiscard]] u8   link_id() const override { return m_machine_index; }
    [[nodiscard]] u8   link_count() const override { return m_num_machines; }
    [[nodiscard]] bool enabled() const override { return m_running; }

private:
    static u16 get16(const u8* p, u32 off) { return static_cast<u16>(p[off] | (p[off + 1] << 8)); }
    static void put16(u8* p, u32 off, u16 v)
    {
        p[off]     = static_cast<u8>(v);
        p[off + 1] = static_cast<u8>(v >> 8);
    }
    static u16 flip(u16 v) { return static_cast<u16>((v << 8) | (v >> 8)); }

    [[nodiscard]] u8* comm_ram() { return m_buffer.data() + (m_commbank ? 0x10000 : 0); }
    [[nodiscard]] u8* ext() { return m_buffer.data() + (m_commbank ? 0 : 0x10000); }
    // Byte offsets into the board RAM and comm RAM.
    [[nodiscard]] u16 ram16(u32 off) const { return get16(m_ram.data(), off); }
    void comm16(u32 off, u16 v) { put16(comm_ram(), off, v); }

    static constexpr usize kHeader      = 6;    ///< origin, sequence, packing
    static constexpr usize kHistory     = 300;  ///< own segments kept for resends

    enum Tag : u8 { kGuid = 1, kIndex = 2, kCount = 3, kData = 4, kResend = 5, kLeave = 6 };

    struct Pair {
        u8 total    = 0;
        u8 playable = 0;
    };

    void send_tagged(u8 tag, const void* data, usize size)
    {
        std::vector<u8> frame(1 + size);
        frame[0] = tag;
        if (size != 0) std::memcpy(frame.data() + 1, data, size);
        m_transport->send(frame);
    }

    // The ring handshake, one step per frame and never blocking, so cabinets
    // that boot seconds apart still meet. The master sends its GUID, then
    // index 0, then the count, each until it comes back round the ring; the
    // others forward every message (again on a repeat) and number themselves.
    struct Handshake {
        bool master    = false;
        u8   phase     = 0;
        u32  retry     = 0;
        bool guid_ok   = false;
        bool has_index = false;
        bool done      = false;
        Pair index;
        Pair count;
        Pair (*next)(Pair) = nullptr;
    };

    void handshake_message(const std::vector<u8>& frame)
    {
        if (frame.empty()) return;
        const u8   tag = frame[0];
        if (tag == kData || tag == kResend) {
            // A cabinet that finished first is already exchanging data.
            if (m_pending.size() < 64) m_pending.push_back(frame);
            return;
        }
        const usize n  = frame.size() - 1;
        const u8*  p   = frame.data() + 1;
        Handshake& h   = m_hs;
        Pair value;
        if ((tag == kIndex || tag == kCount) && n == sizeof(Pair)) std::memcpy(&value, p, sizeof(Pair));
        if (h.master) {
            if (tag == kGuid && h.phase == 0) {
                u64 guid = 0;
                if (n == sizeof(guid)) std::memcpy(&guid, p, sizeof(guid));
                if (guid != kNetGuid) {
                    fail("unable to verify connection; make sure all cabinets run the same build");
                    return;
                }
                h.phase = 1;
                h.retry = 0;
            } else if (tag == kIndex && h.phase == 1 && n == sizeof(Pair)) {
                h.count = value;
                h.phase = 2;
                h.retry = 0;
            } else if (tag == kCount && h.phase == 2) {
                h.index = Pair{};
                h.done  = true;
            }
            return;
        }
        if (tag == kGuid) {
            u64 guid = 0;
            if (n == sizeof(guid)) std::memcpy(&guid, p, sizeof(guid));
            if (guid != kNetGuid) {
                fail("unable to verify connection; make sure all cabinets run the same build");
                return;
            }
            h.guid_ok = true;
            send_tagged(kGuid, &guid, sizeof(guid));
        } else if (tag == kIndex && h.guid_ok && n == sizeof(Pair)) {
            if (!h.has_index) {
                h.index     = h.next(value);
                h.has_index = true;
            }
            send_tagged(kIndex, &h.index, sizeof(Pair));
        } else if (tag == kCount && h.has_index && n == sizeof(Pair)) {
            h.count = value;
            h.done  = true;
            send_tagged(kCount, &value, sizeof(Pair));
        }
    }

    /// True once this cabinet knows its index and the ring's size.
    bool handshake(bool master, Pair (*next)(Pair), Pair& index, Pair& count)
    {
        constexpr u32 kRetryFrames = 30;
        Handshake& h = m_hs;
        h.master     = master;
        h.next       = next;
        for (int guard = 0; guard < 64 && !h.done && m_state != State::Error; ++guard) {
            auto frame = m_transport->recv();
            if (!frame) break;
            handshake_message(*frame);
        }
        if (m_state == State::Error) return false;
        if (master && !h.done) {
            if (h.retry == 0) {
                h.retry = kRetryFrames;
                if (h.phase == 0) send_tagged(kGuid, &kNetGuid, sizeof(kNetGuid));
                else if (h.phase == 1) {
                    const Pair zero{};
                    send_tagged(kIndex, &zero, sizeof(Pair));
                } else send_tagged(kCount, &h.count, sizeof(Pair));
                if (m_loopback) return handshake(master, next, index, count);
            } else {
                --h.retry;
            }
        }
        if (!h.done) return false;
        index = h.index;
        count = h.count;
        return true;
    }

    void fail(const char* what)
    {
        SM2_WARN("net board: %s", what);
        m_state = State::Error;
    }

    void init()
    {
        std::fill(m_buffer.begin(), m_buffer.end(), u8{0});
        if (m_type == GameType::One) {
            if (m_status0 & 0x8000) {
                m_irq2_ack |= 0x01;
                if (m_status0 == 0xF000) {
                    m_status1 = 0;
                    comm16(0x72, flip(0x1));
                    m_state = State::Testing;
                    m_hs    = Handshake{};
                    m_pending.clear();
                }
                m_status0 = 0;
            }
        } else {
            m_status1 = 0;
            m_state   = State::Testing;
            m_counter = 0;
            m_hs      = Handshake{};
            m_pending.clear();
        }
    }

    void testing_one()
    {
        m_status0 += 1;  // type 1 games need it incremented every frame
        Pair pindex;
        Pair pcount;
        if (!handshake(m_loopback || ram16(0x400) == 0, +[](Pair p) { return Pair{static_cast<u8>(p.total + 1), 0}; },
                       pindex, pcount)) {
            return;
        }
        const u8 index = pindex.total;
        const u8 count = pcount.total;
        if (count == 0 && !m_loopback) {
            fail("no other cabinets detected; make sure only one is set to master");
            return;
        }
        m_num_machines  = static_cast<u8>(count + 1);
        m_machine_index = index;
        m_status0       = 0;
        m_status1       = static_cast<u16>(0x2021 + count * 0x20 + index);
        comm16(0x0, ram16(0x400));
        comm16(0x2, count);
        comm16(0x4, index);
        m_counter = 0;
        comm16(0x6, 0);
        m_segment_size = ram16(0x404);
        comm16(0x8, flip(static_cast<u16>(0x100 + m_segment_size)));
        comm16(0xA, flip(static_cast<u16>(ram16(0x402) - m_segment_size - 1)));
        comm16(0xC, flip(0x100));
        comm16(0xE, flip(static_cast<u16>(ram16(0x402) - m_segment_size + 0x200)));
        m_state       = State::Ready;
        m_link_frames = 0;
        m_inbox.clear();
        m_expect.clear();
        m_sent.clear();
        m_sent_seq = 0;
        if (!m_loopback) {
            SM2_INFO("net board: linked as cabinet %u of %u (%u-byte segments)", m_machine_index + 1u,
                     u32{m_num_machines}, u32{m_segment_size});
        }
    }

    void testing_two()
    {
        Pair index;
        Pair count;
        const u16 role = ram16(0x200);
        const bool relay = role >= 0x8000;
        Pair (*next)(Pair) = relay ? +[](Pair p) { return Pair{static_cast<u8>(p.total + 1), p.playable}; }
                                   : +[](Pair p) { return Pair{static_cast<u8>(p.total + 1), static_cast<u8>(p.playable + 1)}; };
        if (!handshake(m_loopback || role == 0, next, index, count)) return;
        if (relay && !m_dirtdvls) index.playable |= 0x80;
        if (count.total == 0 && !m_loopback) {
            if (m_dirtdvls) m_status1 = 0x8085;
            fail("no other cabinets detected; make sure only one is set to master");
            return;
        }
        m_num_machines  = static_cast<u8>(count.total + 1);
        m_machine_index = index.total;
        m_status0       = 5;
        m_status1 = m_dirtdvls ? static_cast<u16>((count.playable << 4) | index.playable | 0x7400)
                               : static_cast<u16>((count.playable << 8) | index.playable);
        comm16(0x0, role);
        comm16(0x2, static_cast<u16>((count.playable << 8) | count.total));
        comm16(0x4, static_cast<u16>((index.playable << 8) | index.total));
        m_counter = 0;
        comm16(0x6, 0);
        m_segment_size = ram16(0x204);
        comm16(0x8, flip(static_cast<u16>(0x100 + m_segment_size)));
        comm16(0xA, flip(ram16(0x206)));
        comm16(0xC, flip(0x100));
        comm16(0xE, flip(static_cast<u16>(ram16(0x206) + 0x80)));
        m_state       = State::Ready;
        m_link_frames = 0;
        m_inbox.clear();
        m_expect.clear();
        m_sent.clear();
        m_sent_seq = 0;
        if (!m_loopback) {
            SM2_INFO("net board: linked as cabinet %u of %u (%u-byte segments)", m_machine_index + 1u,
                     u32{m_num_machines}, u32{m_segment_size});
        }
    }

    // Every frame each cabinet sends its segment, which travels the ring once,
    // and takes exactly one segment per other cabinet, as the real board's
    // lockstep relay does. Reading a few frames behind hides the network
    // round trip and frame-time jitter; it only waits when a cabinet is truly
    // behind. Segments are numbered, so one lost on the way is asked for again
    // from the cabinet that sent it. Slot k holds the segment from k cabinets
    // back; slot n our own after a lap.
    void ready()
    {
        ++m_counter;
        comm16(0x6, flip(m_counter));
        const u32 n   = m_num_machines;
        const u32 seg = m_segment_size;
        if (n != 0 && 0x100 + (n + 1) * seg <= 0x10000) {
            if (m_inbox.size() != n) {
                m_inbox.assign(n, {});
                m_expect.assign(n, 0);
            }
            const u8* mine = comm_ram() + 0x100;
            std::vector<u8> own = pack(mine, seg);
            send_tagged(kData, own.data(), own.size());
            std::memcpy(comm_ram() + 0x100 + n * seg, mine, seg);
            if (m_loopback) {
                m_transport->recv();
            } else {
                m_sent.push_back(std::move(own));
                if (m_sent.size() > kHistory) m_sent.pop_front();
                if (!pump()) {
                    link_broken("another cabinet left the ring");
                    return;
                }
                if (m_link_frames++ >= m_delay && !take_segments(n, seg)) return;
            }
        }
        m_commbank = !m_commbank;
    }

    bool take_segments(u32 n, u32 seg)
    {
        using clock          = std::chrono::steady_clock;
        const auto start     = clock::now();
        const auto deadline  = start + std::chrono::seconds(5);
        auto       nack_gap  = std::chrono::milliseconds(100);
        auto       next_nack = start + nack_gap;
        for (u32 k = 1; k < n; ++k) {
            const u32 origin = (m_machine_index + n - k) % n;
            auto&     inbox  = m_inbox[origin];
            const auto origin_start = clock::now();
            for (;;) {
                if (!pump()) {
                    link_broken("another cabinet left the ring");
                    return false;
                }
                if (inbox.contains(m_expect[origin])) break;
                const auto now = clock::now();
                if (now >= deadline) {
                    link_broken("nothing heard for 5 s");
                    return false;
                }
                if (now >= next_nack) {
                    u8 request[6] = {static_cast<u8>(origin), m_machine_index};
                    put32(request + 2, m_expect[origin]);
                    send_tagged(kResend, request, sizeof(request));
                    next_nack = now + nack_gap;
                    nack_gap  = std::min(nack_gap * 2, std::chrono::milliseconds(400));
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
            if (m_waited.size() != n) m_waited.assign(n, 0);
            m_waited[origin] += std::chrono::duration<double, std::milli>(clock::now() - origin_start).count();
            if (k == n - 1) {
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - start).count();
                if (waited > 50) ++m_stalls;
                m_longest = std::max(m_longest, static_cast<u32>(waited));
            }
            auto it = inbox.find(m_expect[origin]++);
            std::memcpy(comm_ram() + 0x100 + k * seg, it->second.data(), std::min<usize>(it->second.size(), seg));
            inbox.erase(it);
        }
        return true;
    }

    /// Files every frame that has arrived; false on a "link broken" frame.
    bool pump()
    {
        const u32 n    = m_num_machines;
        const u32 next = (m_machine_index + 1) % n;
        for (;;) {
            std::vector<u8> frame;
            if (!m_pending.empty()) {
                frame = std::move(m_pending.front());
                m_pending.pop_front();
            } else if (auto f = m_transport->recv()) {
                if (f->empty()) return false;
                frame = std::move(*f);
            } else {
                return true;
            }
            if (frame[0] == kLeave) return false;
            if (frame[0] == kResend && frame.size() == 7) {
                const u8 origin = frame[1];
                if (origin == m_machine_index) {
                    resend_from(get32(frame.data() + 3));
                } else if (origin < n && next != frame[2]) {
                    m_transport->send(frame);
                }
                continue;
            }
            if (frame[0] != kData) {
                handshake_message(frame);
                continue;
            }
            if (frame.size() < 1 + kHeader) continue;
            const u8 origin = frame[1];
            if (origin >= n || origin == m_machine_index) continue;
            if (next != origin) m_transport->send(frame);
            const u32 seq   = get32(frame.data() + 2);
            auto&     inbox = m_inbox[origin];
            if (seq >= m_expect[origin] && inbox.size() < 600 && !inbox.contains(seq)) {
                std::vector<u8> data(m_segment_size);
                if (unpack(frame, data)) inbox.emplace(seq, std::move(data));
            }
        }
    }

    /// A data frame body: origin, sequence, packing, then the segment, deflated
    /// when that is smaller. Most of a segment is zero, and a whole one is
    /// several datagram fragments over UDP.
    std::vector<u8> pack(const u8* seg, u32 size)
    {
        std::vector<u8> out(kHeader + mz_compressBound(size));
        out[0] = m_machine_index;
        put32(out.data() + 1, m_sent_seq++);
        mz_ulong packed = out.size() - kHeader;
        if (!m_loopback && mz_compress2(out.data() + kHeader, &packed, seg, size, 1) == MZ_OK && packed < size) {
            out[5] = 1;
            out.resize(kHeader + packed);
        } else {
            out[5] = 0;
            out.resize(kHeader + size);
            std::memcpy(out.data() + kHeader, seg, size);
        }
        return out;
    }

    static bool unpack(const std::vector<u8>& frame, std::vector<u8>& seg)
    {
        const u8*   body = frame.data() + 1 + kHeader;
        const usize size = frame.size() - 1 - kHeader;
        if (frame[6] == 0) {
            std::memcpy(seg.data(), body, std::min(size, seg.size()));
            return true;
        }
        mz_ulong len = seg.size();
        return mz_uncompress(seg.data(), &len, body, size) == MZ_OK;
    }

    /// Sends a few of our segments again from `seq` on, after one went missing;
    /// a repeat request for the same one is ignored while the resend travels.
    void resend_from(u32 seq)
    {
        const auto now = std::chrono::steady_clock::now();
        if (seq == m_resent_seq && now - m_resent_at < std::chrono::milliseconds(80)) return;
        m_resent_seq = seq;
        m_resent_at  = now;
        for (const auto& own : m_sent) {
            const u32 s = get32(own.data() + 1);
            if (s >= seq && s < seq + 4) send_tagged(kData, own.data(), own.size());
        }
    }

    static u32 get32(const u8* p) { return u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24); }
    static void put32(u8* p, u32 v)
    {
        for (int i = 0; i < 4; ++i) p[i] = static_cast<u8>(v >> (8 * i));
    }

    void report_stalls()
    {
        if (m_stalls != 0) {
            SM2_INFO("net board: %u frame(s) waited over 50 ms for the link, longest %u ms%s", m_stalls, m_longest,
                     m_lockstep ? "" : "; raise link_delay if the race stutters");
        }
        std::string by;
        for (usize i = 0; i < m_waited.size(); ++i) {
            if (i != m_machine_index) by += " " + std::to_string(i + 1) + ":" + std::to_string(static_cast<u32>(m_waited[i])) + "ms";
        }
        if (!by.empty()) SM2_INFO("net board: time spent waiting on each cabinet:%s", by.c_str());
        m_stalls  = 0;
        m_longest = 0;
        m_waited.clear();
    }

    void link_broken(const char* why)
    {
        report_stalls();
        send_tagged(kLeave, nullptr, 0);
        m_state = State::Error;
        if (m_type == GameType::One) m_status1 = 0x40;
        SM2_WARN("net board: link broken (%s)", why);
    }

    std::span<u8>                  m_ram;
    std::vector<u8>                m_buffer;
    GameType                       m_type;
    bool                           m_dirtdvls;
    bool                           m_lockstep;
    std::unique_ptr<CommTransport> m_transport;
    bool                           m_loopback = true;

    State m_state         = State::Start;
    u8    m_num_machines  = 0;
    u8    m_machine_index = 0;
    u16   m_counter       = 0;
    u16   m_segment_size  = 0;
    bool  m_running       = false;
    u16   m_irq2_ack      = 0;
    u16   m_status0       = 0;
    u16   m_status1       = 0;
    bool  m_commbank      = false;
    Handshake m_hs;
    std::deque<std::vector<u8>> m_pending;
    std::vector<std::map<u32, std::vector<u8>>> m_inbox;
    std::vector<u32>                            m_expect;
    std::deque<std::vector<u8>>                 m_sent;
    u32                                         m_sent_seq    = 0;
    u32                                         m_link_frames = 0;
    u32                                         m_delay       = 6;
    u32                                         m_stalls      = 0;
    std::vector<double>                         m_waited;
    u32                                         m_resent_seq  = ~0u;
    std::chrono::steady_clock::time_point       m_resent_at;
    u32                                         m_longest     = 0;
};

}  // namespace

std::unique_ptr<NetBoardPort> create_net_board(const rom::GameSpec& game, std::span<u8> net_ram)
{
    if (!game.model3.netboard) return nullptr;
    using T = SimNetBoard;
    for (const char* name : {"daytona2", "harley", "scud", "srally2", "skichamp", "spikeout", "spikeofe"}) {
        if (T::is(game, name)) return std::make_unique<T>(game, net_ram, T::GameType::One);
    }
    for (const char* name : {"lemans24", "von2", "dirtdvls"}) {
        if (T::is(game, name)) return std::make_unique<T>(game, net_ram, T::GameType::Two);
    }
    SM2_WARN("net board: '%s' is not supported", game.name.c_str());
    return nullptr;
}

}  // namespace sm2::hw::model3
