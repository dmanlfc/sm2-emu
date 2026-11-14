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
#include "cpu/jit/block_cache.h"

#include "cpu/jit/arm64_emitter.h"
#include "cpu/jit/x64_emitter.h"
#include "cpu/page_map.h"

#include <algorithm>
#include <cstring>

namespace sm2::cpu::jit {

namespace {

constexpr u32 kPageBits = PageMap::kPageBits;

// Pages [first, last] covered by `size` bytes from `pc` (at least one).
void page_span(u32 pc, u32 size, u32& first, u32& last)
{
    const u64 end = u64{pc} + (size == 0 ? 1 : size) - 1;
    first         = pc >> kPageBits;
    last          = static_cast<u32>(std::min<u64>(end, 0xFFFFFFFFULL) >> kPageBits);
}

}  // namespace

BlockCache::BlockCache(CodeBuffer& code, HostArch arch, u32 fast_bits, u32 fast_shift)
    : m_code(code)
    , m_arch(arch)
    , m_fast_bits(fast_bits)
    , m_fast_shift(fast_shift)
    , m_fast(std::make_unique<FastEntry[]>(usize{1} << fast_bits))
{
}

BlockCache::~BlockCache() = default;

const void* BlockCache::find(u32 pc, u32 mode) const
{
    const BlockKey key = make_key(pc, mode);
    FastEntry&     e   = m_fast[fast_index(pc)];
    if (e.key == key) return e.host;
    const auto it = m_by_key.find(key);
    if (it == m_by_key.end()) return nullptr;
    e.key  = key;
    e.host = m_blocks[it->second].host;
    return e.host;
}

BlockId BlockCache::find_id(u32 pc, u32 mode) const
{
    const auto it = m_by_key.find(make_key(pc, mode));
    return it == m_by_key.end() ? kNoBlock : it->second;
}

void BlockCache::set_page(u32 page, bool code)
{
    if (m_page_map != nullptr) m_page_map->set_code_page(page << kPageBits, code);
}

void BlockCache::patch(Link& link, const void* to)
{
    CodeBuffer::WriteScope scope(m_code);
    if (m_arch == HostArch::Arm64) {
        if (!a64::branch26_reachable(link.site, to)) return;  // stays on its fallback
        m_code.patch32(link.site, a64::encode_b(link.site, to));
    } else {
        u8 jmp[5];
        x64::encode_jmp_rel32(jmp, link.site, to);
        if (!x64::rel32_reachable(link.site + 5, to)) return;
        // Only the displacement changes; the E9 opcode was emitted with it.
        m_code.patch_bytes(link.site + 1, jmp + 1, 4);
    }
    link.linked = to != link.fallback;
}

void BlockCache::link_all_to(BlockKey key, const void* entry)
{
    const auto it = m_waiting.find(key);
    if (it == m_waiting.end()) return;
    for (const u32 li : it->second) {
        Link& l = m_links[li];
        if (l.live && !l.linked) patch(l, entry);
    }
}

void BlockCache::unlink_all_to(BlockKey key)
{
    const auto it = m_waiting.find(key);
    if (it == m_waiting.end()) return;
    for (const u32 li : it->second) {
        Link& l = m_links[li];
        if (l.live && l.linked) patch(l, l.fallback);
    }
}

BlockId BlockCache::insert(const BlockDesc& desc)
{
    const BlockKey key = make_key(desc.pc, desc.mode);
    if (const auto it = m_by_key.find(key); it != m_by_key.end()) invalidate(it->second);

    BlockId id;
    if (!m_free_blocks.empty()) {
        id = m_free_blocks.back();
        m_free_blocks.pop_back();
    } else {
        id = static_cast<BlockId>(m_blocks.size());
        m_blocks.emplace_back();
    }
    {
        Block& b     = m_blocks[id];
        b.key        = key;
        b.pc         = desc.pc;
        b.guest_size = desc.guest_size;
        b.host       = desc.host;
        b.host_size  = desc.host_size;
        b.live       = true;
        b.exits.clear();
    }
    m_by_key[key]              = id;
    m_fast[fast_index(desc.pc)] = FastEntry{key, desc.host};
    ++m_inserted;

    u32 first = 0, last = 0;
    page_span(desc.pc, desc.guest_size, first, last);
    for (u32 p = first;; ++p) {
        auto& list = m_pages[p];
        if (list.empty()) set_page(p, true);
        list.push_back(id);
        if (p == last) break;
    }

    for (const BlockExit& e : desc.exits) {
        u32 li;
        if (!m_free_links.empty()) {
            li = m_free_links.back();
            m_free_links.pop_back();
        } else {
            li = static_cast<u32>(m_links.size());
            m_links.emplace_back();
        }
        Link& l    = m_links[li];
        l.site     = e.site;
        l.fallback = e.fallback;
        l.target   = make_key(e.target_pc, e.target_mode);
        l.owner    = id;
        l.linked   = false;
        l.live     = true;
        m_waiting[l.target].push_back(li);
        m_blocks[id].exits.push_back(li);
        if (const auto t = m_by_key.find(l.target); t != m_by_key.end()) patch(l, m_blocks[t->second].host);
    }

    // Existing blocks (and this one's own self-loops, already handled above)
    // that were waiting for this key.
    link_all_to(key, desc.host);
    return id;
}

void BlockCache::invalidate(BlockId id)
{
    if (id >= m_blocks.size() || !m_blocks[id].live) return;
    Block& b = m_blocks[id];

    // Exits elsewhere that jump here go back to their fallbacks.
    unlink_all_to(b.key);

    // This block's own exits stop waiting for anything.
    for (const u32 li : b.exits) {
        Link& l = m_links[li];
        if (auto w = m_waiting.find(l.target); w != m_waiting.end()) {
            auto& v = w->second;
            v.erase(std::remove(v.begin(), v.end(), li), v.end());
            if (v.empty()) m_waiting.erase(w);
        }
        l.live   = false;
        l.linked = false;
        m_free_links.push_back(li);
    }
    b.exits.clear();

    m_by_key.erase(b.key);
    FastEntry& e = m_fast[fast_index(b.pc)];
    if (e.key == b.key) e = FastEntry{};

    u32 first = 0, last = 0;
    page_span(b.pc, b.guest_size, first, last);
    for (u32 p = first;; ++p) {
        if (auto it = m_pages.find(p); it != m_pages.end()) {
            auto& v = it->second;
            v.erase(std::remove(v.begin(), v.end(), id), v.end());
            if (v.empty()) {
                m_pages.erase(it);
                set_page(p, false);
            }
        }
        if (p == last) break;
    }

    b.live = false;
    m_free_blocks.push_back(id);
    ++m_invalidated;
}

void BlockCache::invalidate_range(u32 address, u32 size)
{
    if (size == 0) return;
    const u64 end = u64{address} + size;  // exclusive
    u32       first = 0, last = 0;
    page_span(address, size, first, last);
    std::vector<BlockId> doomed;
    for (u32 p = first;; ++p) {
        if (const auto it = m_pages.find(p); it != m_pages.end()) {
            for (const BlockId id : it->second) {
                const Block& b      = m_blocks[id];
                const u64    b_end  = u64{b.pc} + (b.guest_size == 0 ? 1 : b.guest_size);
                if (b.pc < end && address < b_end) doomed.push_back(id);
            }
        }
        if (p == last) break;
    }
    for (const BlockId id : doomed) invalidate(id);  // invalidate() ignores repeats
}

void BlockCache::invalidate_page(u32 address)
{
    const auto it = m_pages.find(address >> kPageBits);
    if (it == m_pages.end()) return;
    const std::vector<BlockId> doomed = it->second;
    for (const BlockId id : doomed) invalidate(id);
}

void BlockCache::clear()
{
    for (const auto& [page, list] : m_pages) set_page(page, false);
    m_pages.clear();
    m_blocks.clear();
    m_free_blocks.clear();
    m_links.clear();
    m_free_links.clear();
    m_by_key.clear();
    m_waiting.clear();
    for (usize i = 0; i < (usize{1} << m_fast_bits); ++i) m_fast[i] = FastEntry{};
    m_code.clear();
}

BlockCache::Stats BlockCache::stats() const
{
    Stats s;
    s.blocks     = m_by_key.size();
    s.code_pages = m_pages.size();
    for (const Link& l : m_links) s.links += (l.live && l.linked) ? 1 : 0;
    s.inserted    = m_inserted;
    s.invalidated = m_invalidated;
    return s;
}

bool BlockCache::is_linked(const u8* site) const
{
    for (const Link& l : m_links) {
        if (l.live && l.site == site) return l.linked;
    }
    return false;
}

}  // namespace sm2::cpu::jit
