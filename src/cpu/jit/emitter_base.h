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
#pragma once

#include "core/types.h"

#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

// Shared emitter state: code span, labels with forward fixups, sticky errors.
// Writes past the span are dropped but the cursor still advances; check ok()
// once per block (overflowed() means flush the cache and retry).

namespace sm2::cpu::jit {

static_assert(std::endian::native == std::endian::little,
              "the emitters write instruction words in host order; every JIT host is little-endian");

/// Code space: written through `rw`, run (and branched relative to) at `exec`.
struct CodeSpan {
    u8*   rw   = nullptr;
    u8*   exec = nullptr;
    usize size = 0;
};

/// A branch target that may be bound after the branches to it are emitted.
/// Only meaningful to the emitter that created it.
struct Label {
    u32 id = ~0U;
    [[nodiscard]] bool valid() const { return id != ~0U; }
};

class EmitterBase {
public:
    EmitterBase() = default;
    explicit EmitterBase(CodeSpan span) { reset(span); }
    virtual ~EmitterBase() = default;

    EmitterBase(const EmitterBase&)            = delete;
    EmitterBase& operator=(const EmitterBase&) = delete;

    /// Start emitting at the beginning of `span`, forgetting all labels.
    void reset(CodeSpan span)
    {
        m_span     = span;
        m_pos      = 0;
        m_overflow = false;
        m_error    = false;
        m_labels.clear();
        m_fixups.clear();
    }

    /// Bytes emitted so far (including any that did not fit).
    [[nodiscard]] usize size() const { return m_pos; }
    [[nodiscard]] usize capacity() const { return m_span.size; }
    [[nodiscard]] const CodeSpan& span() const { return m_span; }

    /// The span ran out. Not a bug: flush the code cache and emit again.
    [[nodiscard]] bool overflowed() const { return m_overflow; }
    /// A branch or literal was out of range or an encoding was impossible.
    [[nodiscard]] bool has_error() const { return m_error; }
    /// Neither of the above and every referenced label is bound.
    [[nodiscard]] bool ok() const { return !m_overflow && !m_error && m_fixups.empty(); }

    /// Execution address of the next byte to be emitted, or of offset `pos`.
    [[nodiscard]] u8* cursor() const { return m_span.exec + m_pos; }
    [[nodiscard]] u8* exec_at(usize pos) const { return m_span.exec + pos; }
    /// Writable alias of offset `pos` (for patching already-emitted code).
    [[nodiscard]] u8* rw_at(usize pos) const { return m_span.rw + pos; }

    [[nodiscard]] Label new_label()
    {
        m_labels.push_back(kUnbound);
        return Label{static_cast<u32>(m_labels.size() - 1)};
    }
    [[nodiscard]] bool is_bound(Label l) const { return l.valid() && l.id < m_labels.size() && m_labels[l.id] != kUnbound; }
    /// Offset from the start of the span; only valid once bound.
    [[nodiscard]] usize label_offset(Label l) const { return static_cast<usize>(m_labels[l.id]); }
    [[nodiscard]] u8* label_address(Label l) const { return exec_at(label_offset(l)); }

    /// Bind `l` to the current position and resolve every branch already
    /// emitted towards it.
    void bind(Label l)
    {
        if (!l.valid() || l.id >= m_labels.size() || m_labels[l.id] != kUnbound) {
            m_error = true;
            return;
        }
        m_labels[l.id] = static_cast<s64>(m_pos);
        for (usize i = 0; i < m_fixups.size();) {
            if (m_fixups[i].label == l.id) {
                if (!patch_fixup(m_fixups[i].pos, m_fixups[i].kind, m_pos)) m_error = true;
                m_fixups[i] = m_fixups.back();
                m_fixups.pop_back();
            } else {
                ++i;
            }
        }
    }

    // -- Raw data --------------------------------------------------------------

    void emit8(u8 v) { put(&v, 1); }
    void emit16(u16 v) { put(&v, 2); }
    void emit32(u32 v) { put(&v, 4); }
    void emit64(u64 v) { put(&v, 8); }
    void emit_bytes(const void* p, usize n) { put(p, n); }

    /// Pad with `fill` bytes to a multiple of `alignment` (relative to the
    /// execution address, which is what alignment means to the CPU).
    void align_bytes(usize alignment, u8 fill)
    {
        while ((reinterpret_cast<uintptr_t>(cursor()) & (alignment - 1)) != 0) emit8(fill);
    }

    /// Mark the emitter as failed (used by encoders that are handed operands
    /// they cannot represent).
    void set_error() { m_error = true; }

protected:
    static constexpr s64 kUnbound = -1;

    struct Fixup {
        usize pos;   ///< Offset of the instruction (or field) to patch.
        u32   label;
        u32   kind;  ///< Emitter-specific.
    };

    /// Forward reference from `pos` to unbound `l`.
    void add_fixup(usize pos, Label l, u32 kind)
    {
        if (!l.valid() || l.id >= m_labels.size()) {
            m_error = true;
            return;
        }
        m_fixups.push_back(Fixup{pos, l.id, kind});
    }

    /// Re-encode the reference at `pos` to point at `target` (both offsets
    /// within the span). Returns false when out of range.
    virtual bool patch_fixup(usize pos, u32 kind, usize target) = 0;

    void put(const void* p, usize n)
    {
        if (m_pos + n <= m_span.size) {
            std::memcpy(m_span.rw + m_pos, p, n);
        } else {
            m_overflow = true;
        }
        m_pos += n;
    }

    [[nodiscard]] bool in_span(usize pos, usize n) const { return pos + n <= m_span.size; }

    u32 read32_at(usize pos) const
    {
        u32 v = 0;
        if (in_span(pos, 4)) std::memcpy(&v, m_span.rw + pos, 4);
        return v;
    }
    void write32_at(usize pos, u32 v)
    {
        if (in_span(pos, 4)) std::memcpy(m_span.rw + pos, &v, 4);
    }

    CodeSpan           m_span{};
    usize              m_pos      = 0;
    bool               m_overflow = false;
    bool               m_error    = false;
    std::vector<s64>   m_labels;
    std::vector<Fixup> m_fixups;
};

}  // namespace sm2::cpu::jit
