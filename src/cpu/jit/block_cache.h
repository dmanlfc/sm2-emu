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

#include "cpu/jit/code_buffer.h"

#include "core/types.h"

#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

// Block cache: (pc, mode) -> host code, per-page block lists for
// invalidation, and patchable block-to-block links.
//
// Backend contract (one CodeBuffer + BlockCache + Dispatcher per guest CPU):
//
//   pinned regs   arm64  x86-64
//     context     X19    RBX     guest CPU state, from run()'s argument
//     budget      X20    R14     s64 cycles left
//     extra       X21-X28 / R12, R13, R15: DispatcherConfig::pinned
//   Blocks are jumped to inside the dispatcher's frame: SP is 16-aligned
//   (x86-64: shadow space reserved) and must be unchanged on exit.
//
//   Compile: emit into code.tail() in a WriteScope. Direct exits use
//   emit_linked_exit() (dispatcher.h):
//       subs budget, #cycles ; b.le fallback ; b fallback  <- link site
//     fallback:
//       ctx->pc = target ; b dispatch
//   Indirect exits store the pc and go to dispatch (emit_indirect_exit);
//   emit_exit_now() leaves run() at once. Then commit() and insert() with the
//   exits. Link sites are patched to target blocks as they appear and back
//   to their fallback when the target is invalidated.
//
//   run(n) executes whole blocks until >= n cycles are charged and returns
//   the remaining budget (cycles run = n - result).
//
//   SMC: insert() marks covered pages with PageMap::set_code_page(); the
//   machine's slow-path store calls invalidate_range(). Invalidated code is
//   not reused until clear(), so a block invalidated while running finishes
//   safely (a backend that needs to stop at once checks a flag after
//   slow-path stores). When the buffer fills, clear() everything.

namespace sm2::cpu {
class PageMap;
}

namespace sm2::cpu::jit {

enum class HostArch : u8 { Arm64, X64 };
#if defined(__aarch64__) || defined(_M_ARM64)
inline constexpr HostArch kHostArch = HostArch::Arm64;
#else
inline constexpr HostArch kHostArch = HostArch::X64;
#endif

/// (mode << 32) | pc. The mode key is whatever else, besides the address,
/// changes how guest code translates (PowerPC: MSR[IR/DR/PR/FP/LE]...).
using BlockKey = u64;
[[nodiscard]] constexpr BlockKey make_key(u32 pc, u32 mode) { return (u64{mode} << 32) | pc; }

using BlockId = u32;
inline constexpr BlockId kNoBlock = ~0U;

/// A direct exit of a block, as emitted by emit_linked_exit().
struct BlockExit {
    u8* site     = nullptr;  ///< Execution address of the patchable jump.
    u8* fallback = nullptr;  ///< Where it jumps while unlinked.
    u32 target_pc   = 0;
    u32 target_mode = 0;
};

struct BlockDesc {
    u32       pc         = 0;
    u32       mode       = 0;
    u32       guest_size = 0;  ///< Bytes of guest code translated, from pc.
    u8*       host       = nullptr;
    u32       host_size  = 0;
    std::span<const BlockExit> exits{};
};

class BlockCache {
public:
    /// One slot of the direct-mapped table generated code probes inline.
    struct FastEntry {
        BlockKey    key  = kEmptyKey;
        const void* host = nullptr;
    };
    static_assert(sizeof(FastEntry) == 16, "dispatchers index the table with a shift of 4");
    static constexpr BlockKey kEmptyKey = ~BlockKey{0};

    /// Fast table: 2^fast_bits entries indexed by pc >> fast_shift.
    explicit BlockCache(CodeBuffer& code, HostArch arch = kHostArch, u32 fast_bits = 16, u32 fast_shift = 2);
    ~BlockCache();

    BlockCache(const BlockCache&)            = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    /// Mark covered pages as code (and unmark them on invalidation).
    void set_page_map(PageMap* map) { m_page_map = map; }

    // -- Lookup ------------------------------------------------------------------

    [[nodiscard]] const void* find(u32 pc, u32 mode) const;
    [[nodiscard]] BlockId     find_id(u32 pc, u32 mode) const;
    /// Re-fill the fast-table slot for (pc, mode) if the block exists, so the
    /// next dispatch hits. find() does this too.
    void touch(u32 pc, u32 mode) const { (void)find(pc, mode); }

    [[nodiscard]] FastEntry* fast_table() const { return m_fast.get(); }
    [[nodiscard]] u32        fast_bits() const { return m_fast_bits; }
    [[nodiscard]] u32        fast_shift() const { return m_fast_shift; }
    [[nodiscard]] u32        fast_index(u32 pc) const { return (pc >> m_fast_shift) & ((1U << m_fast_bits) - 1); }

    // -- Insertion ---------------------------------------------------------------

    /// Replaces any block with the same key; links exits both ways.
    BlockId insert(const BlockDesc& desc);

    // -- Invalidation ------------------------------------------------------------

    void invalidate(BlockId id);
    /// Every block whose guest code overlaps [address, address + size).
    void invalidate_range(u32 address, u32 size);
    /// Every block touching the 4 KiB page containing `address`.
    void invalidate_page(u32 address);
    /// Forget everything and rewind the code buffer to its persistent mark.
    void clear();

    // -- Introspection -----------------------------------------------------------

    struct Stats {
        usize blocks       = 0;
        usize links        = 0;  ///< Exits currently patched to a block.
        usize code_pages   = 0;
        u64   inserted     = 0;
        u64   invalidated  = 0;
    };
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] bool  is_linked(const u8* site) const;
    [[nodiscard]] CodeBuffer& code() const { return m_code; }

private:
    struct Block {
        BlockKey         key        = 0;
        u32              pc         = 0;
        u32              guest_size = 0;
        u8*              host       = nullptr;
        u32              host_size  = 0;
        bool             live       = false;
        std::vector<u32> exits;  ///< Indices into m_links.
    };
    struct Link {
        u8*      site     = nullptr;
        u8*      fallback = nullptr;
        BlockKey target   = 0;
        BlockId  owner    = kNoBlock;
        bool     linked   = false;
        bool     live     = false;
    };

    void patch(Link& link, const void* to);
    void unlink_all_to(BlockKey key);
    void link_all_to(BlockKey key, const void* entry);
    void set_page(u32 page, bool code);

    CodeBuffer& m_code;
    HostArch    m_arch;
    PageMap*    m_page_map = nullptr;
    u32         m_fast_bits;
    u32         m_fast_shift;
    std::unique_ptr<FastEntry[]> m_fast;

    std::vector<Block>                                 m_blocks;
    std::vector<BlockId>                               m_free_blocks;
    std::vector<Link>                                  m_links;
    std::vector<u32>                                   m_free_links;
    std::unordered_map<BlockKey, BlockId>              m_by_key;
    std::unordered_map<BlockKey, std::vector<u32>>     m_waiting;  ///< target key -> link indices
    std::unordered_map<u32, std::vector<BlockId>>      m_pages;    ///< guest page -> blocks
    u64 m_inserted    = 0;
    u64 m_invalidated = 0;
};

}  // namespace sm2::cpu::jit
