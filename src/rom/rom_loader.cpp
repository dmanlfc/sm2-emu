//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// sm2-emu — A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
#include "rom/rom_loader.h"

#include "core/log.h"
#include "rom/game_db.h"

#include <miniz.h>

extern "C" {
#include <7z.h>
#include <7zAlloc.h>
#include <7zBuf.h>
#include <7zCrc.h>
#include <7zFile.h>
}

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <unordered_map>

namespace sm2::rom {
namespace {

/// One entry inside an open archive.
struct ZipEntry {
    std::string name;
    u32         crc32 = 0;
    usize       size  = 0;
    u32         index = 0;
    usize       archive = 0;  ///< Which open archive holds it.
};

[[nodiscard]] std::string to_lower(std::string_view text)
{
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// Signature bytes identifying an archive's actual format, read from the
/// file's own contents rather than trusted from its extension: a renamed
/// file (7z content with a `.zip` name, or the reverse, both observed in
/// real-world Model 2 dumps) still has to load correctly, matching the
/// loader's existing CRC-first philosophy that identity comes from content.
enum class ArchiveFormat {
    Unknown,
    Zip,
    SevenZip,
};

[[nodiscard]] ArchiveFormat sniff_archive_format(const std::string& path)
{
    std::FILE* handle = std::fopen(path.c_str(), "rb");
    if (handle == nullptr) {
        return ArchiveFormat::Unknown;
    }
    u8   header[8] = {};
    const usize read = std::fread(header, 1, sizeof(header), handle);
    std::fclose(handle);

    static constexpr u8 kZipSignature[4]      = {0x50, 0x4b, 0x03, 0x04};  // "PK\x03\x04"
    static constexpr u8 kSevenZipSignature[6] = {0x37, 0x7a, 0xbc, 0xaf, 0x27, 0x1c};  // "7z\xBC\xAF\x27\x1C"

    if (read >= sizeof(kZipSignature)
        && std::memcmp(header, kZipSignature, sizeof(kZipSignature)) == 0) {
        return ArchiveFormat::Zip;
    }
    if (read >= sizeof(kSevenZipSignature)
        && std::memcmp(header, kSevenZipSignature, sizeof(kSevenZipSignature)) == 0) {
        return ArchiveFormat::SevenZip;
    }
    return ArchiveFormat::Unknown;
}

/// Owns one open 7z archive via the vendored LZMA SDK reader, plus the
/// solid-block decompression cache `SzArEx_Extract` uses to avoid
/// re-inflating a shared block for every file inside it.
struct SevenZipArchive {
    CFileInStream archive_stream{};
    CLookToRead2  look_stream{};
    CSzArEx       db{};
    bool          db_open = false;

    // SzArEx_Extract's solid-block cache: must persist across calls for the
    // same archive to get the reuse benefit, and must be zeroed before the
    // first call.
    UInt32 cached_block_index  = 0;
    Byte*  cached_out_buffer   = nullptr;
    size_t cached_out_buffer_size = 0;

    static constexpr size_t kLookAheadBufferSize = 1 << 18;

    SevenZipArchive() { SzArEx_Init(&db); }

    SevenZipArchive(const SevenZipArchive&)            = delete;
    SevenZipArchive& operator=(const SevenZipArchive&) = delete;
    // CFileInStream/CLookToRead2 hold self-pointers into `look_stream.vt`
    // (installed by LookToRead2_CreateVTable to point back at this object's
    // fields), so an instance must never be relocated once opened. As with
    // mz_zip_archive above, callers keep these in a std::deque.
    SevenZipArchive(SevenZipArchive&&)            = delete;
    SevenZipArchive& operator=(SevenZipArchive&&) = delete;

    ~SevenZipArchive()
    {
        static const ISzAlloc alloc{SzAlloc, SzFree};
        if (cached_out_buffer != nullptr) {
            ISzAlloc_Free(&alloc, cached_out_buffer);
        }
        if (db_open) {
            SzArEx_Free(&db, &alloc);
        }
        if (look_stream.buf != nullptr) {
            ISzAlloc_Free(&alloc, look_stream.buf);
        }
        File_Close(&archive_stream.file);
    }
};

/// File names inside a 7z are UTF-16LE; ROM chip names are always plain
/// ASCII in practice, so a byte-at-a-time truncation (dropping the high byte
/// of anything outside the ASCII range) is sufficient and matches what every
/// chip name actually contains. Lowercasing happens separately in
/// `index_entry`/`find`, same as the zip backend.
[[nodiscard]] std::string utf16_to_utf8(const std::vector<u16>& utf16)
{
    std::string out;
    out.reserve(utf16.size());
    for (const u16 unit : utf16) {
        if (unit == 0) {
            break;
        }
        out.push_back(static_cast<char>(unit & 0xff));
    }
    return out;
}

/// A set of open zip *or* 7z archives, indexed for lookup by CRC and by
/// name. Both backends feed the same `ZipEntry` records so the rest of
/// `RomLoader` does not need to know which format a given archive is.
///
/// More than one archive is needed for split sets, where a clone's own
/// archive holds only the chips that differ and the rest come from the
/// parent's -- and a zip and a 7z can be mixed in the same set.
class ArchiveSet {
public:
    ~ArchiveSet()
    {
        for (mz_zip_archive& archive : m_zip_archives) {
            mz_zip_reader_end(&archive);
        }
    }

    ArchiveSet()                             = default;
    ArchiveSet(const ArchiveSet&)            = delete;
    ArchiveSet& operator=(const ArchiveSet&) = delete;

    [[nodiscard]] bool open(const std::string& path)
    {
        switch (sniff_archive_format(path)) {
            case ArchiveFormat::Zip:
                return open_zip(path);
            case ArchiveFormat::SevenZip:
                return open_seven_zip(path);
            case ArchiveFormat::Unknown:
                SM2_ERROR("'%s' is not a zip or 7z archive (unrecognised signature)",
                          path.c_str());
                return false;
        }
        return false;
    }

    /// Find an entry, by CRC when the database declares one and by name
    /// otherwise. Matching on CRC *is* the verification: a corrupted chip
    /// simply is not found.
    [[nodiscard]] const ZipEntry* find(const FileSpec& file) const
    {
        if (file.has_crc) {
            const auto entry = m_by_crc.find(file.crc32);
            if (entry != m_by_crc.end()) {
                return &entry->second;
            }
            return nullptr;
        }
        const auto entry = m_by_name.find(to_lower(file.name));
        return entry != m_by_name.end() ? &entry->second : nullptr;
    }

    [[nodiscard]] bool extract(const ZipEntry& entry, std::vector<u8>* out)
    {
        return entry.archive < kSevenZipArchiveBase
                 ? extract_zip(entry, out)
                 : extract_seven_zip(entry, out);
    }

private:
    // Backend archive indices are disjoint: zip archives are indexed
    // 0..N-1 directly into `m_zip_archives`, and 7z archives are indexed
    // starting at this base into `m_seven_zip_archives`, so `ZipEntry::archive`
    // alone says which backend and slot an entry came from without a
    // separate tag field.
    static constexpr usize kSevenZipArchiveBase = 1u << 20;

    [[nodiscard]] bool open_zip(const std::string& path)
    {
        m_zip_archives.emplace_back();
        mz_zip_archive& archive = m_zip_archives.back();
        std::memset(&archive, 0, sizeof(archive));

        if (mz_zip_reader_init_file(&archive, path.c_str(), 0) == MZ_FALSE) {
            SM2_ERROR("could not open '%s' as a zip archive: %s", path.c_str(),
                      mz_zip_get_error_string(mz_zip_get_last_error(&archive)));
            m_zip_archives.pop_back();
            return false;
        }

        const usize archive_index = m_zip_archives.size() - 1;
        const u32   count         = mz_zip_reader_get_num_files(&archive);
        u32         indexed       = 0;

        for (u32 index = 0; index < count; ++index) {
            mz_zip_archive_file_stat stat{};
            if (mz_zip_reader_file_stat(&archive, index, &stat) == MZ_FALSE) {
                continue;
            }
            if (mz_zip_reader_is_file_a_directory(&archive, index) == MZ_TRUE) {
                continue;
            }

            ZipEntry entry;
            entry.name    = stat.m_filename;
            entry.crc32   = stat.m_crc32;
            entry.size    = static_cast<usize>(stat.m_uncomp_size);
            entry.index   = index;
            entry.archive = archive_index;

            index_entry(entry);
            ++indexed;
        }

        SM2_DEBUG("indexed %u file(s) from %s (zip)", indexed, path.c_str());
        return true;
    }

    [[nodiscard]] bool open_seven_zip(const std::string& path)
    {
        static const ISzAlloc alloc{SzAlloc, SzFree};
        static const ISzAlloc alloc_temp{SzAllocTemp, SzFreeTemp};
        static bool           crc_table_ready = false;
        if (!crc_table_ready) {
            CrcGenerateTable();
            crc_table_ready = true;
        }

        m_seven_zip_archives.emplace_back();
        SevenZipArchive& archive = m_seven_zip_archives.back();

        if (InFile_Open(&archive.archive_stream.file, path.c_str()) != 0) {
            SM2_ERROR("could not open '%s' as a 7z archive", path.c_str());
            m_seven_zip_archives.pop_back();
            return false;
        }
        FileInStream_CreateVTable(&archive.archive_stream);

        LookToRead2_CreateVTable(&archive.look_stream, /*lookahead=*/False);
        archive.look_stream.buf =
            static_cast<Byte*>(ISzAlloc_Alloc(&alloc, SevenZipArchive::kLookAheadBufferSize));
        archive.look_stream.bufSize   = SevenZipArchive::kLookAheadBufferSize;
        archive.look_stream.realStream = &archive.archive_stream.vt;
        LookToRead2_INIT(&archive.look_stream)

        const SRes result =
            SzArEx_Open(&archive.db, &archive.look_stream.vt, &alloc, &alloc_temp);
        if (result != SZ_OK) {
            SM2_ERROR("could not open '%s' as a 7z archive: SzArEx_Open returned %d "
                      "(corrupted archive or unsupported compression method)",
                      path.c_str(), result);
            m_seven_zip_archives.pop_back();
            return false;
        }
        archive.db_open = true;

        const usize archive_index = kSevenZipArchiveBase + m_seven_zip_archives.size() - 1;
        u32         indexed       = 0;

        for (u32 index = 0; index < archive.db.NumFiles; ++index) {
            if (SzArEx_IsDir(&archive.db, index)) {
                continue;
            }

            const size_t      name_length = SzArEx_GetFileNameUtf16(&archive.db, index, nullptr);
            std::vector<u16> name_utf16(name_length);
            SzArEx_GetFileNameUtf16(&archive.db, index, name_utf16.data());

            ZipEntry entry;
            entry.name    = utf16_to_utf8(name_utf16);
            entry.crc32   = SzBitWithVals_Check(&archive.db.CRCs, index) ? archive.db.CRCs.Vals[index] : 0;
            entry.size    = static_cast<usize>(SzArEx_GetFileSize(&archive.db, index));
            entry.index   = index;
            entry.archive = archive_index;

            index_entry(entry);
            ++indexed;
        }

        SM2_DEBUG("indexed %u file(s) from %s (7z)", indexed, path.c_str());
        return true;
    }

    void index_entry(const ZipEntry& entry)
    {
        // Several chips in a merged set can share a CRC when a revision
        // respins only some of them, so keep the first and let name lookup
        // disambiguate. Content is identical by definition.
        m_by_crc.emplace(entry.crc32, entry);
        m_by_name.emplace(to_lower(entry.name), entry);
    }

    [[nodiscard]] bool extract_zip(const ZipEntry& entry, std::vector<u8>* out)
    {
        out->resize(entry.size);
        if (entry.size == 0) {
            return true;
        }

        mz_zip_archive& archive = m_zip_archives[entry.archive];
        if (mz_zip_reader_extract_to_mem(&archive, entry.index, out->data(), out->size(), 0)
            == MZ_FALSE) {
            // miniz verifies the CRC while inflating, so this also catches a
            // file whose stored CRC matched but whose data is damaged.
            SM2_ERROR("could not extract '%s': %s", entry.name.c_str(),
                      mz_zip_get_error_string(mz_zip_get_last_error(&archive)));
            return false;
        }
        return true;
    }

    [[nodiscard]] bool extract_seven_zip(const ZipEntry& entry, std::vector<u8>* out)
    {
        static const ISzAlloc alloc{SzAlloc, SzFree};
        static const ISzAlloc alloc_temp{SzAllocTemp, SzFreeTemp};

        SevenZipArchive& archive = m_seven_zip_archives[entry.archive - kSevenZipArchiveBase];

        size_t offset            = 0;
        size_t out_size_processed = 0;
        const SRes result = SzArEx_Extract(
            &archive.db, &archive.look_stream.vt, entry.index, &archive.cached_block_index,
            &archive.cached_out_buffer, &archive.cached_out_buffer_size, &offset,
            &out_size_processed, &alloc, &alloc_temp);

        if (result != SZ_OK) {
            // SzArEx_Extract verifies the folder's CRC while inflating (7z
            // stores CRCs per solid block, same guarantee as miniz's per-file
            // CRC check), so this also catches damaged data with a correct
            // header.
            SM2_ERROR("could not extract '%s': SzArEx_Extract returned %d", entry.name.c_str(),
                      result);
            return false;
        }

        out->assign(archive.cached_out_buffer + offset,
                    archive.cached_out_buffer + offset + out_size_processed);
        return true;
    }

    // Deques, not vectors: both mz_zip_archive and SevenZipArchive hold
    // internal/self pointers once opened, so neither must ever be relocated,
    // and entries reference their archive by index. A deque never moves an
    // existing element.
    std::deque<mz_zip_archive>                m_zip_archives;
    std::deque<SevenZipArchive>                m_seven_zip_archives;
    std::unordered_map<u32, ZipEntry>         m_by_crc;
    std::unordered_map<std::string, ZipEntry> m_by_name;
};

/// How completely an archive satisfies a game definition.
struct MatchScore {
    u32 found   = 0;
    u32 missing = 0;

    [[nodiscard]] bool complete() const { return missing == 0 && found > 0; }
};

[[nodiscard]] MatchScore score_game(const ArchiveSet& archives, const GameSpec& game)
{
    MatchScore score;
    for (const RegionSpec& region : game.regions) {
        for (const FileSpec& file : region.files) {
            if (archives.find(file) != nullptr) {
                ++score.found;
            } else if (region.required) {
                ++score.missing;
            }
        }
    }
    return score;
}

}  // namespace

// ---------------------------------------------------------------------------
// Region assembly
// ---------------------------------------------------------------------------

usize RomLoader::computed_region_size(const RegionSpec&         region,
                                     const std::vector<usize>& file_sizes)
{
    usize required = 0;
    for (usize index = 0; index < region.files.size() && index < file_sizes.size(); ++index) {
        const usize size = file_sizes[index];
        if (size == 0) {
            continue;
        }
        const usize chunks = size / region.chunk;
        if (chunks == 0) {
            continue;
        }
        // Last chunk begins at offset + stride * (chunks - 1) and is chunk bytes
        // long, so the region must reach one past its end.
        const usize end = region.files[index].offset
                        + region.stride * (chunks - 1)
                        + region.chunk;
        required = std::max(required, end);
    }
    // A mirror can reach past the last chip, which is the whole point of it, so
    // a region declaring no explicit size still has to be big enough to hold
    // its own copies.
    for (const RegionCopy& copy : region.copies) {
        required = std::max<usize>(required, usize{copy.to} + copy.size);
    }
    return required;
}

std::optional<std::vector<u8>> RomLoader::assemble_region(
    const RegionSpec&                   region,
    const std::vector<std::vector<u8>>& file_contents)
{
    if (file_contents.size() != region.files.size()) {
        SM2_ERROR("region '%s': %zu file(s) declared but %zu provided",
                  region.name.c_str(), region.files.size(), file_contents.size());
        return std::nullopt;
    }

    std::vector<usize> sizes;
    sizes.reserve(file_contents.size());
    for (const std::vector<u8>& contents : file_contents) {
        sizes.push_back(contents.size());
    }

    const usize computed = computed_region_size(region, sizes);
    const usize size     = region.size != 0 ? region.size : computed;

    if (size == 0) {
        SM2_ERROR("region '%s': size is zero and cannot be derived",
                  region.name.c_str());
        return std::nullopt;
    }
    if (computed > size) {
        SM2_ERROR("region '%s': declared size 0x%zx is too small for its files, "
                  "which need 0x%zx", region.name.c_str(), size, computed);
        return std::nullopt;
    }

    std::vector<u8> data(size, region.fill);

    for (usize index = 0; index < region.files.size(); ++index) {
        const FileSpec&        file     = region.files[index];
        const std::vector<u8>& contents = file_contents[index];
        if (contents.empty()) {
            continue;
        }

        if ((contents.size() % region.chunk) != 0) {
            SM2_ERROR("region '%s': file '%s' is %zu bytes, which is not a "
                      "multiple of the %u-byte chunk size",
                      region.name.c_str(), file.name.c_str(), contents.size(),
                      region.chunk);
            return std::nullopt;
        }

        if (region.stride == region.chunk) {
            // Contiguous: no interleaving to do.
            if (file.offset + contents.size() > data.size()) {
                SM2_ERROR("region '%s': file '%s' at 0x%x overruns the region",
                          region.name.c_str(), file.name.c_str(), file.offset);
                return std::nullopt;
            }
            std::memcpy(data.data() + file.offset, contents.data(), contents.size());
            continue;
        }

        const usize chunks = contents.size() / region.chunk;
        usize       destination = file.offset;
        usize       source      = 0;

        // Bounds-check once rather than per chunk: the inner loop runs up to two
        // million times per file.
        const usize last_end = file.offset + region.stride * (chunks - 1) + region.chunk;
        if (last_end > data.size()) {
            SM2_ERROR("region '%s': file '%s' at 0x%x with stride %u overruns "
                      "the region (needs 0x%zx, have 0x%zx)",
                      region.name.c_str(), file.name.c_str(), file.offset,
                      region.stride, last_end, data.size());
            return std::nullopt;
        }

        for (usize chunk = 0; chunk < chunks; ++chunk) {
            std::memcpy(data.data() + destination, contents.data() + source, region.chunk);
            destination += region.stride;
            source      += region.chunk;
        }
    }

    // MAME's ROM_COPY, applied in declaration order once every chip is in
    // place. Before the byte swap rather than after, which is equivalent
    // (swapping a copy of a block gives the same bytes as copying the swapped
    // block) but keeps the swap as the single last step over the whole region.
    for (const RegionCopy& copy : region.copies) {
        const usize from = copy.from;
        const usize to   = copy.to;
        const usize span = copy.size;
        if (from + span > data.size() || to + span > data.size()) {
            SM2_ERROR("region '%s': copy of 0x%zx bytes from 0x%zx to 0x%zx "
                      "does not fit in 0x%zx", region.name.c_str(), span, from, to,
                      data.size());
            return std::nullopt;
        }
        // memmove, not memcpy: nothing stops a set from declaring an
        // overlapping mirror.
        std::memmove(data.data() + to, data.data() + from, span);
    }

    // MAME's driver init patches, applied after the mirrors so a patched word
    // cannot be overwritten by one.
    for (const RegionPatch& patch : region.patches) {
        if (usize{patch.offset} + 4 > data.size()) {
            SM2_ERROR("region '%s': patch at 0x%x is outside the region's 0x%zx bytes",
                      region.name.c_str(), patch.offset, data.size());
            return std::nullopt;
        }
        data[patch.offset + 0] = static_cast<u8>(patch.value);
        data[patch.offset + 1] = static_cast<u8>(patch.value >> 8);
        data[patch.offset + 2] = static_cast<u8>(patch.value >> 16);
        data[patch.offset + 3] = static_cast<u8>(patch.value >> 24);
        SM2_DEBUG("region '%s': patched 0x%x = %08x", region.name.c_str(),
                  patch.offset, patch.value);
    }

    if (region.byte_swap) {
        // Reproduces MAME's ROM_LOAD16_WORD_SWAP: the big-endian 68000 sound
        // program and its samples are stored with each 16-bit word reversed.
        for (usize offset = 0; offset + 1 < data.size(); offset += 2) {
            std::swap(data[offset], data[offset + 1]);
        }
    }

    return data;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

std::optional<LoadResult> RomLoader::load(const GameDatabase& database,
                                          const std::string&  archive_path,
                                          const std::string&  preferred_game)
{
    std::error_code error;
    if (!std::filesystem::exists(archive_path, error)) {
        SM2_ERROR("'%s' does not exist", archive_path.c_str());
        return std::nullopt;
    }

    ArchiveSet archives;
    if (!archives.open(archive_path)) {
        return std::nullopt;
    }

    // -- pick a game -------------------------------------------------------
    const GameSpec* chosen = nullptr;

    if (!preferred_game.empty()) {
        chosen = database.find(preferred_game);
        if (chosen == nullptr) {
            SM2_ERROR("'%s' is not in the ROM database", preferred_game.c_str());
            return std::nullopt;
        }
    } else {
        const GameSpec* best       = nullptr;
        MatchScore      best_score;

        for (const GameSpec& candidate : database.games()) {
            const MatchScore score = score_game(archives, candidate);
            if (score.found == 0) {
                continue;
            }
            SM2_DEBUG("candidate '%s': %u found, %u missing", candidate.name.c_str(),
                      score.found, score.missing);

            // A complete match always beats an incomplete one; between two of
            // the same completeness, more matched chips wins.
            const bool better = best == nullptr
                             || (score.complete() && !best_score.complete())
                             || (score.complete() == best_score.complete()
                                 && score.found > best_score.found);
            if (better) {
                best       = &candidate;
                best_score = score;
            }
        }

        if (best == nullptr) {
            SM2_ERROR("'%s' does not contain any game sm2-emu recognises",
                      archive_path.c_str());
            SM2_ERROR("Identification is by CRC32, so this means none of the "
                      "expected chips were found, not that the name is wrong.");
            return std::nullopt;
        }
        if (!best_score.complete()) {
            SM2_WARN("'%s' looks like '%s' but %u required file(s) are missing",
                     archive_path.c_str(), best->name.c_str(), best_score.missing);
        }
        chosen = best;
    }

    // A sibling archive named `stem`, preferring zip but accepting 7z, because
    // either format turns up in the wild for both parent sets and device sets.
    const auto find_sibling_archive =
        [&archive_path](const std::string& stem) -> std::optional<std::filesystem::path> {
        const std::filesystem::path directory =
            std::filesystem::path(archive_path).parent_path();
        for (const char* extension : {".zip", ".7z"}) {
            std::error_code exists_error;
            const std::filesystem::path candidate = directory / (stem + extension);
            if (std::filesystem::exists(candidate, exists_error)) {
                return candidate;
            }
        }
        return std::nullopt;
    };

    // -- open the parent archive if needed ---------------------------------
    if (!chosen->parent.empty()) {
        const MatchScore score = score_game(archives, *chosen);
        if (!score.complete()) {
            if (const auto parent_path = find_sibling_archive(chosen->parent)) {
                SM2_INFO("'%s' is a clone; also reading %s", chosen->name.c_str(),
                         parent_path->string().c_str());
                if (!archives.open(parent_path->string())) {
                    return std::nullopt;
                }
            } else {
                SM2_WARN("'%s' is a clone of '%s' and some chips are missing, but "
                         "no %s.zip or %s.7z was found beside the archive",
                         chosen->name.c_str(), chosen->parent.c_str(),
                         chosen->parent.c_str(), chosen->parent.c_str());
            }
        }
    }

    // -- open any device ROM sets ------------------------------------------
    // Unlike the parent archive these are opened unconditionally: the firmware
    // in them belongs to a device the board carries, so it is never present in
    // the game's own archive and scoring the game would not reveal the need.
    for (const std::string& device_set : chosen->device_sets) {
        if (const auto device_path = find_sibling_archive(device_set)) {
            SM2_INFO("'%s' needs the '%s' device set; also reading %s",
                     chosen->name.c_str(), device_set.c_str(),
                     device_path->string().c_str());
            if (!archives.open(device_path->string())) {
                return std::nullopt;
            }
        } else {
            SM2_WARN("'%s' needs the '%s' device ROM set, but no %s.zip or %s.7z "
                     "was found beside the archive", chosen->name.c_str(),
                     device_set.c_str(), device_set.c_str(), device_set.c_str());
        }
    }

    if (!board_implemented(chosen->board)) {
        SM2_ERROR("'%s' is a %s board, which is not implemented yet. The original "
                  "Model 2 and the CRX boards (2A, 2B and 2C) are supported.",
                  chosen->name.c_str(), board_name(chosen->board));
        return std::nullopt;
    }
    if (chosen->preliminary) {
        SM2_WARN("'%s' is marked preliminary and is not expected to run correctly",
                 chosen->name.c_str());
    }

    // -- assemble ----------------------------------------------------------
    LoadResult result;
    result.game = *chosen;

    for (const RegionSpec& region : chosen->regions) {
        std::vector<std::vector<u8>> contents(region.files.size());
        u32                          missing = 0;

        for (usize index = 0; index < region.files.size(); ++index) {
            const FileSpec& file  = region.files[index];
            const ZipEntry* entry = archives.find(file);

            if (entry == nullptr) {
                if (region.required) {
                    SM2_ERROR("region '%s': '%s' (CRC32 0x%08x) was not found in %s",
                              region.name.c_str(), file.name.c_str(), file.crc32,
                              archive_path.c_str());
                    return std::nullopt;
                }
                ++missing;
                continue;
            }
            if (!archives.extract(*entry, &contents[index])) {
                return std::nullopt;
            }
        }

        if (missing != 0) {
            SM2_WARN("region '%s': %u optional file(s) absent; filling with 0x%02x",
                     region.name.c_str(), missing, region.fill);
        }

        std::optional<std::vector<u8>> assembled = assemble_region(region, contents);
        if (!assembled.has_value()) {
            return std::nullopt;
        }

        SM2_DEBUG("region '%-18s' 0x%08zx bytes from %zu file(s)",
                  region.name.c_str(), assembled->size(), region.files.size());
        result.roms.add(region.name, std::move(*assembled));
    }

    SM2_INFO("loaded '%s' (%s, %s %u) - %s, %.1f MiB across %zu region(s)",
             result.game.name.c_str(),
             result.game.title.c_str(),
             result.game.manufacturer.c_str(),
             result.game.year,
             board_name(result.game.board),
             static_cast<double>(result.roms.total_bytes()) / (1024.0 * 1024.0),
             result.roms.region_names().size());

    return result;
}

bool RomLoader::dump_regions(const RomSet& roms, const std::string& directory)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        SM2_ERROR("could not create '%s': %s", directory.c_str(), error.message().c_str());
        return false;
    }

    for (const std::string& name : roms.region_names()) {
        const std::span<const u8>   data = roms.region(name);
        const std::filesystem::path path =
            std::filesystem::path(directory) / (name + ".bin");

        std::FILE* handle = std::fopen(path.string().c_str(), "wb");
        if (handle == nullptr) {
            SM2_ERROR("could not write '%s'", path.string().c_str());
            return false;
        }
        const usize written = std::fwrite(data.data(), 1, data.size(), handle);
        std::fclose(handle);

        if (written != data.size()) {
            SM2_ERROR("short write to '%s': %zu of %zu bytes", path.string().c_str(),
                      written, data.size());
            return false;
        }
        SM2_INFO("wrote %s (0x%zx bytes)", path.string().c_str(), data.size());
    }
    return true;
}

}  // namespace sm2::rom
