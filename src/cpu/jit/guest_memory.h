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

#include "cpu/jit/arm64_emitter.h"
#include "cpu/jit/x64_emitter.h"

#include "core/types.h"

#include <vector>

// Inline big-endian guest accesses against cpu::PageMap:
//   page = table[addr >> 12]; null -> slow
//   off = addr & 0xFFF; off > 4096 - size -> slow (page crossing)
//   host access at page + off, byte-reversed
// Slow paths are queued and emitted out of line by finish() (call once at
// the end of the block). They call fn(ctx, addr[, value]) with values in
// guest numeric order, zero-extended; the helper sign-extends loads.
// live_* masks: caller-saved registers to preserve across the slow call (the
// load destination never needs to be listed).
// Scratch: X16, X17, X30 (arm64) / R10, R11, RAX (x86-64).

namespace sm2::cpu::jit {

/// Slow-path signatures shared by both hosts. `size` is implied by which
/// function is called (index 0..3 = 8, 16, 32, 64 bits).
using GuestReadFn  = u64 (*)(void* ctx, u32 address);
using GuestWriteFn = void (*)(void* ctx, u32 address, u64 value);

struct GuestMemoryFns {
    GuestReadFn  read[4]  = {};
    GuestWriteFn write[4] = {};
};

namespace a64 {

struct GuestMemoryConfig {
    Reg            ctx         = X19;  ///< Passed as the slow path's first argument.
    Reg            read_table  = X21;  ///< Holds PageMap::read_table().
    Reg            write_table = X22;  ///< Holds PageMap::write_table().
    GuestMemoryFns fns{};
};

class GuestMemory {
public:
    GuestMemory(Emitter& a, const GuestMemoryConfig& cfg) : m_a(a), m_cfg(cfg) {}

    /// `size` in bytes (1, 2, 4, 8). `dst` W or X (X for size 8 or for a
    /// sign-extended 32-bit load); `addr` a W register with the guest address.
    void load(u32 size, Reg dst, Reg addr, bool sign_extend = false, u32 live_gprs = 0, u32 live_fprs = 0);
    void store(u32 size, Reg value, Reg addr, u32 live_gprs = 0, u32 live_fprs = 0);
    /// Raw bits of a 4- or 8-byte value straight into / out of an S or D
    /// register (lfs/lfd/stfs/stfd before any precision conversion).
    void load_fp(VReg dst, Reg addr, u32 live_gprs = 0, u32 live_fprs = 0);
    void store_fp(VReg value, Reg addr, u32 live_gprs = 0, u32 live_fprs = 0);

    /// Emit every queued slow path.
    void finish();
    [[nodiscard]] usize pending() const { return m_slow.size(); }

private:
    struct Slow {
        Label entry, resume;
        bool  store;
        bool  fp;
        bool  sign;
        u32   size;
        Reg   data;
        VReg  vdata;
        Reg   addr;
        u32   live_gprs, live_fprs;
    };

    Label lookup(u32 size, Reg addr, Reg table);
    void  queue(Slow s) { m_slow.push_back(s); }

    Emitter&          m_a;
    GuestMemoryConfig m_cfg;
    std::vector<Slow> m_slow;
};

}  // namespace a64

namespace x64 {

struct GuestMemoryConfig {
    Gp             ctx         = RBX;
    Gp             read_table  = R12;
    Gp             write_table = R13;
    GuestMemoryFns fns{};
    bool           use_movbe = false;  ///< Set from host_features().movbe.
    Abi            abi       = kHostAbi;
};

class GuestMemory {
public:
    GuestMemory(Emitter& a, const GuestMemoryConfig& cfg) : m_a(a), m_cfg(cfg) {}

    /// `dst` 32-bit (or 64-bit for size 8 / sign-extended 32-bit), `addr` a
    /// 32-bit register.
    void load(u32 size, Gp dst, Gp addr, bool sign_extend = false, u32 live_gprs = 0, u32 live_xmms = 0);
    void store(u32 size, Gp value, Gp addr, u32 live_gprs = 0, u32 live_xmms = 0);
    /// Raw 4- or 8-byte loads / stores to the low lane of an XMM register.
    void load_fp(u32 size, Xmm dst, Gp addr, u32 live_gprs = 0, u32 live_xmms = 0);
    void store_fp(u32 size, Xmm value, Gp addr, u32 live_gprs = 0, u32 live_xmms = 0);

    void finish();
    [[nodiscard]] usize pending() const { return m_slow.size(); }

private:
    struct Slow {
        Label entry, resume;
        bool  store;
        bool  fp;
        bool  sign;
        u32   size;
        Gp    data;
        Xmm   xdata;
        Gp    addr;
        u32   live_gprs, live_xmms;
    };

    Label lookup(u32 size, Gp addr, Gp table);
    void  load_swapped(u32 size, Gp dst, bool sign);

    Emitter&          m_a;
    GuestMemoryConfig m_cfg;
    std::vector<Slow> m_slow;
};

}  // namespace x64

}  // namespace sm2::cpu::jit
