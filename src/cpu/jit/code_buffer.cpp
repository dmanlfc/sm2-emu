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
#include "cpu/jit/code_buffer.h"

#include "core/log.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <fcntl.h>
#    include <sys/syscall.h>
#  endif
#  if defined(__APPLE__)
#    include <libkern/OSCacheControl.h>
#    include <pthread.h>
#  endif
#endif

#if defined(__APPLE__) && defined(__aarch64__)
#  define SM2_JIT_MAP_JIT 1
#else
#  define SM2_JIT_MAP_JIT 0
#endif
#if defined(__x86_64__) || defined(_M_X64)
#  define SM2_JIT_RWX 1
#else
#  define SM2_JIT_RWX 0
#endif

namespace sm2::cpu::jit {

namespace {

#if SM2_JIT_MAP_JIT
// pthread_jit_write_protect_np() is per thread and covers every MAP_JIT
// region at once, so the nesting depth is per thread too, not per buffer.
thread_local int t_map_jit_depth = 0;
#endif

usize host_page_size()
{
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwAllocationGranularity;
#else
    const long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? static_cast<usize>(p) : 4096;
#endif
}

// Somewhere in the program's own code, used to place the buffer close to the
// C++ helpers generated code calls.
uintptr_t program_text_address()
{
    return reinterpret_cast<uintptr_t>(&CodeBuffer::flush_icache);
}

// Is all of [p, p + size) within direct-call range of the program's code?
// The margin leaves room for the size of the program's text itself.
bool near_program(const void* p, usize size)
{
    constexpr uintptr_t kReach = uintptr_t{112} << 20;
    const uintptr_t     text   = program_text_address();
    const auto          lo     = reinterpret_cast<uintptr_t>(p);
    const uintptr_t     hi     = lo + size;
    const uintptr_t     d_lo   = lo > text ? lo - text : text - lo;
    const uintptr_t     d_hi   = hi > text ? hi - text : text - hi;
    return d_lo < kReach && d_hi < kReach;
}

// Candidate addresses for the buffer, nearest the program first: just above
// and just below its text, in 16 MiB steps.
template <typename Fn>
void for_each_hint(usize size, Fn&& fn)
{
    constexpr uintptr_t kStep = uintptr_t{16} << 20;
    const uintptr_t     text  = program_text_address() & ~(kStep - 1);
    for (uintptr_t k = 1; k <= 5; ++k) {
        if (fn(reinterpret_cast<void*>(text + k * kStep))) return;
        if (text > k * kStep + size && fn(reinterpret_cast<void*>(text - k * kStep - size))) return;
    }
}

}  // namespace

bool CodeBuffer::init(usize size, bool force_toggle)
{
    release();
    const usize page = host_page_size();
    size             = (size + page - 1) & ~(page - 1);

    bool mapped = false;
#if defined(__linux__) && defined(__aarch64__)
    if (!force_toggle) mapped = map_dual(size);
#else
    (void)force_toggle;
#endif
#if SM2_JIT_RWX
    if (!mapped && !force_toggle) mapped = map_rwx(size);
#endif
    if (!mapped) mapped = map_single(size);
    if (!mapped) {
        SM2_ERROR("jit: could not reserve %zu bytes of executable memory", size);
        return false;
    }

    m_size       = size;
    m_used       = 0;
    m_persistent = 0;
    m_rw_delta   = m_rw - m_exec;
    SM2_DEBUG("jit: %zu MiB code buffer at %p (%s, %s the program's code)", size >> 20,
              static_cast<void*>(m_exec),
              m_protection == Protection::MapJit        ? "MAP_JIT"
              : m_protection == Protection::DualMapping ? "dual-mapped"
              : m_protection == Protection::Rwx         ? "RWX"
                                                        : "toggled",
              near_program(m_exec, m_size) ? "near" : "far from");
    return true;
}

bool CodeBuffer::map_dual(usize size)
{
#if defined(__linux__) && defined(SYS_memfd_create)
    const int fd = static_cast<int>(syscall(SYS_memfd_create, "sm2-jit", 1U /* MFD_CLOEXEC */));
    if (fd < 0) return false;
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        close(fd);
        return false;
    }
    void* rx = nullptr;
    for_each_hint(size, [&](void* hint) {
        void* p = mmap(hint, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) return false;
        if (near_program(p, size)) {
            rx = p;
            return true;
        }
        munmap(p, size);
        return false;
    });
    if (rx == nullptr) {
        void* p = mmap(nullptr, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) rx = p;
    }
    void* rw = rx != nullptr ? mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (rx == nullptr || rw == MAP_FAILED) {
        if (rx != nullptr) munmap(rx, size);
        close(fd);
        return false;
    }
    m_exec       = static_cast<u8*>(rx);
    m_rw         = static_cast<u8*>(rw);
    m_fd         = fd;
    m_protection = Protection::DualMapping;
    return true;
#else
    (void)size;
    return false;
#endif
}

bool CodeBuffer::map_rwx(usize size)
{
#if defined(_WIN32)
    void* mem = nullptr;
    for_each_hint(size, [&](void* hint) {
        void* p = VirtualAlloc(hint, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p == nullptr) return false;
        if (near_program(p, size)) {
            mem = p;
            return true;
        }
        VirtualFree(p, 0, MEM_RELEASE);
        return false;
    });
    if (mem == nullptr) mem = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (mem == nullptr) return false;
#else
    constexpr int kProt  = PROT_READ | PROT_WRITE | PROT_EXEC;
    constexpr int kFlags = MAP_PRIVATE | MAP_ANON;
    void* mem = nullptr;
    for_each_hint(size, [&](void* hint) {
        void* p = mmap(hint, size, kProt, kFlags, -1, 0);
        if (p == MAP_FAILED) return false;
        if (near_program(p, size)) {
            mem = p;
            return true;
        }
        munmap(p, size);
        return false;
    });
    if (mem == nullptr) {
        void* p = mmap(nullptr, size, kProt, kFlags, -1, 0);
        if (p == MAP_FAILED) return false;
        mem = p;
    }
#endif
    m_exec       = static_cast<u8*>(mem);
    m_rw         = m_exec;
    m_size       = size;
    m_protection = Protection::Rwx;
    return true;
}

bool CodeBuffer::map_single(usize size)
{
#if defined(_WIN32)
    void* mem = nullptr;
    for_each_hint(size, [&](void* hint) {
        void* p = VirtualAlloc(hint, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p == nullptr) return false;
        if (near_program(p, size)) {
            mem = p;
            return true;
        }
        VirtualFree(p, 0, MEM_RELEASE);
        return false;
    });
    if (mem == nullptr) mem = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem == nullptr) return false;
    m_exec       = static_cast<u8*>(mem);
    m_rw         = m_exec;
    m_protection = Protection::Toggle;
    m_size       = size;
    set_executable(true);
    return true;
#else
#  if SM2_JIT_MAP_JIT
    constexpr int kProt  = PROT_READ | PROT_WRITE | PROT_EXEC;
    constexpr int kFlags = MAP_PRIVATE | MAP_ANON | MAP_JIT;
#  else
    constexpr int kProt  = PROT_READ | PROT_WRITE;
    constexpr int kFlags = MAP_PRIVATE | MAP_ANON;
#  endif
    void* mem = nullptr;
    for_each_hint(size, [&](void* hint) {
        void* p = mmap(hint, size, kProt, kFlags, -1, 0);
        if (p == MAP_FAILED) return false;
        if (near_program(p, size)) {
            mem = p;
            return true;
        }
        munmap(p, size);
        return false;
    });
    if (mem == nullptr) {
        void* p = mmap(nullptr, size, kProt, kFlags, -1, 0);
        if (p == MAP_FAILED) return false;
        mem = p;
    }
    m_exec = static_cast<u8*>(mem);
    m_rw   = m_exec;
    m_size = size;
#  if SM2_JIT_MAP_JIT
    m_protection = Protection::MapJit;
#  else
    m_protection = Protection::Toggle;
    set_executable(true);
#  endif
    return true;
#endif
}

void CodeBuffer::release()
{
    if (m_exec == nullptr) return;
#if defined(_WIN32)
    VirtualFree(m_exec, 0, MEM_RELEASE);
#else
    munmap(m_exec, m_size);
    if (m_rw != m_exec) munmap(m_rw, m_size);
    if (m_fd >= 0) close(m_fd);
#endif
    m_exec        = nullptr;
    m_rw          = nullptr;
    m_rw_delta    = 0;
    m_fd          = -1;
    m_size        = 0;
    m_used        = 0;
    m_persistent  = 0;
    m_write_depth = 0;
    m_protection  = Protection::None;
}

void CodeBuffer::set_executable(bool executable)
{
#if defined(_WIN32)
    DWORD old = 0;
    if (!VirtualProtect(m_exec, m_size, executable ? PAGE_EXECUTE_READ : PAGE_READWRITE, &old)) {
        SM2_ERROR("jit: VirtualProtect failed (%lu)", GetLastError());
    }
#else
    if (mprotect(m_exec, m_size, executable ? (PROT_READ | PROT_EXEC) : (PROT_READ | PROT_WRITE)) != 0) {
        SM2_ERROR("jit: mprotect failed");
    }
#endif
}

void CodeBuffer::begin_write()
{
    switch (m_protection) {
    case Protection::MapJit:
#if SM2_JIT_MAP_JIT
        if (t_map_jit_depth++ == 0) pthread_jit_write_protect_np(0);
#endif
        break;
    case Protection::Toggle:
        if (m_write_depth++ == 0) set_executable(false);
        break;
    case Protection::DualMapping:
    case Protection::Rwx:
    case Protection::None:
        break;
    }
}

void CodeBuffer::end_write()
{
    switch (m_protection) {
    case Protection::MapJit:
#if SM2_JIT_MAP_JIT
        if (--t_map_jit_depth == 0) pthread_jit_write_protect_np(1);
#endif
        break;
    case Protection::Toggle:
        if (--m_write_depth == 0) set_executable(true);
        break;
    case Protection::DualMapping:
    case Protection::Rwx:
    case Protection::None:
        break;
    }
}

CodeSpan CodeBuffer::tail()
{
    const usize start = (m_used + kAlign - 1) & ~(kAlign - 1);
    if (start >= m_size) return CodeSpan{m_rw + m_size, m_exec + m_size, 0};
    return CodeSpan{m_rw + start, m_exec + start, m_size - start};
}

u8* CodeBuffer::commit(usize bytes)
{
    const usize start = (m_used + kAlign - 1) & ~(kAlign - 1);
    if (start > m_size || bytes > m_size - start) return nullptr;
    m_used = start + bytes;
    flush_icache(m_exec + start, bytes);
    return m_exec + start;
}

CodeSpan CodeBuffer::allocate(usize bytes)
{
    const usize start = (m_used + kAlign - 1) & ~(kAlign - 1);
    if (start > m_size || bytes > m_size - start) return CodeSpan{};
    m_used = start + bytes;
    return CodeSpan{m_rw + start, m_exec + start, bytes};
}

void CodeBuffer::flush_icache(const void* exec, usize size)
{
    if (size == 0) return;
#if defined(__APPLE__)
    sys_icache_invalidate(const_cast<void*>(exec), size);
#elif defined(_WIN32)
    FlushInstructionCache(GetCurrentProcess(), exec, size);
#else
    char* b = static_cast<char*>(const_cast<void*>(exec));
    __builtin___clear_cache(b, b + size);
#endif
}

void CodeBuffer::patch32(void* exec, u32 value)
{
    u32* rw = reinterpret_cast<u32*>(to_rw(exec));
    std::atomic_ref<u32>(*rw).store(value, std::memory_order_relaxed);
    flush_icache(exec, 4);
}

void CodeBuffer::patch_bytes(void* exec, const void* data, usize size)
{
    std::memcpy(to_rw(exec), data, size);
    flush_icache(exec, size);
}

}  // namespace sm2::cpu::jit
