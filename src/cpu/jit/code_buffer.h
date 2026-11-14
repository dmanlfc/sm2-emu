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

#include "cpu/jit/emitter_base.h"

#include "core/types.h"

// Executable memory: one reserved region, bump-allocated, freed only as a
// whole by clear() (back to the persistent mark, where the dispatcher lives).
//
// W^X: bracket every write in a WriteScope.
//   macOS arm64  MAP_JIT + pthread_jit_write_protect_np (per thread)
//   Linux arm64  memfd dual mapping (RW + RX views), mprotect fallback
//   x86-64       one RWX mapping (per-compile toggling is too slow);
//                toggle fallback
//   others       mprotect / VirtualProtect toggling of the whole region; no
//                thread may run code from the buffer while a scope is open
// Emitters write through CodeSpan::rw and branch relative to CodeSpan::exec.
// commit() and patch*() flush the icache; anything else calls flush_icache().
// Keep the buffer <= 128 MiB so arm64 blocks can B to each other; init()
// tries to place it near the program's code so calls to C++ can be BL/CALL.

namespace sm2::cpu::jit {

class CodeBuffer {
public:
    static constexpr usize kDefaultSize = usize{64} << 20;
    /// Largest size that still lets every arm64 block reach every other.
    static constexpr usize kMaxDirectBranchSize = usize{128} << 20;
    static constexpr usize kAlign = 16;

    enum class Protection : u8 {
        None,         ///< Not initialised.
        MapJit,       ///< macOS arm64: per-thread write protection.
        DualMapping,  ///< Separate RW and RX views.
        Toggle,       ///< One view, flipped between RW and RX.
        Rwx,          ///< x86-64: one view, always writable and executable.
    };

    CodeBuffer() = default;
    ~CodeBuffer() { release(); }

    CodeBuffer(const CodeBuffer&)            = delete;
    CodeBuffer& operator=(const CodeBuffer&) = delete;

    /// `force_toggle` skips dual mapping.
    [[nodiscard]] bool init(usize size = kDefaultSize, bool force_toggle = false);
    void release();

    [[nodiscard]] bool       valid() const { return m_exec != nullptr; }
    [[nodiscard]] Protection protection() const { return m_protection; }
    [[nodiscard]] u8*        exec_base() const { return m_exec; }
    [[nodiscard]] usize      capacity() const { return m_size; }
    [[nodiscard]] usize      used() const { return m_used; }
    [[nodiscard]] usize      remaining() const { return m_size - m_used; }

    /// Does [exec_base, exec_base + capacity) contain `p`?
    [[nodiscard]] bool contains(const void* p) const
    {
        const auto* b = static_cast<const u8*>(p);
        return b >= m_exec && b < m_exec + m_size;
    }

    /// Writable alias of an execution address inside the buffer.
    [[nodiscard]] u8* to_rw(const void* exec) const
    {
        return const_cast<u8*>(static_cast<const u8*>(exec)) + m_rw_delta;
    }

    // -- Handing out space -------------------------------------------------------

    /// All free space from the (aligned) cursor. Emit into it, then commit()
    /// the bytes actually used. Nothing else may be allocated in between.
    [[nodiscard]] CodeSpan tail();
    /// Finish code emitted into tail(); returns its address (nullptr if too big).
    u8* commit(usize bytes);

    /// A fixed-size piece of space, already committed (cursor advanced). The
    /// caller writes it and calls flush_icache().
    [[nodiscard]] CodeSpan allocate(usize bytes);

    /// Everything allocated so far survives clear().
    void set_persistent_mark() { m_persistent = m_used; }
    /// Forget every allocation after the persistent mark.
    void clear() { m_used = m_persistent; }

    // -- Writing -------------------------------------------------------------------

    /// Make the buffer writable for the calling thread. Nestable; must be
    /// balanced by end_write() on the same thread.
    void begin_write();
    void end_write();

    class WriteScope {
    public:
        explicit WriteScope(CodeBuffer& b) : m_b(b) { m_b.begin_write(); }
        ~WriteScope() { m_b.end_write(); }
        WriteScope(const WriteScope&)            = delete;
        WriteScope& operator=(const WriteScope&) = delete;

    private:
        CodeBuffer& m_b;
    };

    /// Make freshly written code at [exec, exec + size) visible to
    /// instruction fetch on this core and every other.
    static void flush_icache(const void* exec, usize size);

    /// Atomic aligned 32-bit code patch + flush. Inside a WriteScope.
    void patch32(void* exec, u32 value);
    /// Overwrite `size` bytes of code and flush them. Inside a WriteScope.
    void patch_bytes(void* exec, const void* data, usize size);

private:
    bool map_dual(usize size);
    bool map_rwx(usize size);
    bool map_single(usize size);
    void set_executable(bool executable);

    u8*        m_exec       = nullptr;
    u8*        m_rw         = nullptr;  ///< Same as m_exec unless dual-mapped.
    ptrdiff_t  m_rw_delta   = 0;
    usize      m_size       = 0;
    usize      m_used       = 0;
    usize      m_persistent = 0;
    Protection m_protection = Protection::None;
    int        m_write_depth = 0;  ///< Toggle mode only (MapJit is per thread).
    int        m_fd          = -1;
};

}  // namespace sm2::cpu::jit
