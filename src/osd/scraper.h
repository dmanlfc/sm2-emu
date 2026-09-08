//  ____  __  __  ____         _____ __  __ _   _
// / ___||  \/  ||___ \       | ____|  \/  | | | |
// \___ \| |\/| |  __) |_____ |  _| | |\/| | | | |
//  ___) | |  | | / __/|_____|| |___| |  | | |_| |
// |____/|_|  |_||_____|      |_____|_|  |_|\___/
//
// A Sega Model 2 arcade emulator.
// Copyright (c) 2025+ Daniel Martin (dmanlfc)
// SPDX-License-Identifier: BSD-3-Clause
//
// This header must not be removed. The source files in this project may not be
// used to contribute to commercial projects or for monetary gain without the
// express written permission of the author.
//
#pragma once

#include <atomic>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace sm2::osd {

/// Fetches ArcadeDB box art and descriptions for the game picker on a
/// background thread, cached under an artwork directory and keyed by MAME short
/// name. Inert when built without libcurl or disabled by the user.
///
/// Thread-safety contract (the reason this class is shaped as it is): the
/// worker touches only libcurl, the filesystem under the artwork dir, and JSON.
/// It never calls the renderer/GUI/ImGui/audio/SDL. The one cross-thread
/// channel is the mutex-guarded name queue drained by take_ready(); image
/// decode and GPU upload are the main thread's job. The owner joins the thread
/// (stop()/dtor) before tearing down anything the main thread owns.
class Scraper {
public:
    struct Entry {
        std::string name;   ///< MAME short name: the ArcadeDB and cache key.
        std::string title;  ///< games.xml title, the offline fallback label.
    };

    /// Cached text for one set; any field may be empty (offline or unknown set).
    struct Metadata {
        std::string title;
        std::string year;
        std::string manufacturer;
        std::string genre;
        std::string description;
        /// Cached image paths in display order (flyer, title, ingame, marquee),
        /// which the picker cycles through while a game is highlighted.
        std::vector<std::string> image_paths;
        bool found = false;  ///< true if ArcadeDB returned a record.
    };

    Scraper() = default;
    ~Scraper();

    Scraper(const Scraper&)            = delete;
    Scraper& operator=(const Scraper&) = delete;

    /// Start the worker. No thread is started when disabled or built without
    /// libcurl; the picker then relies on the cache and games.xml titles alone.
    void start(std::string artwork_dir, std::vector<Entry> entries, bool enabled);

    /// Stop and join the worker. Idempotent; bounded by the request timeout.
    void stop();

    /// Take the names cached since the last call. Non-blocking.
    [[nodiscard]] std::vector<std::string> take_ready();

    /// Read `name`'s cached metadata, falling back to `fallback_title` when
    /// nothing is cached. Main thread.
    [[nodiscard]] Metadata load_metadata(const std::string& name,
                                         const std::string& fallback_title) const;

    /// libcurl present AND enabled by the user; false means offline.
    [[nodiscard]] bool scraping_available() const { return m_scraping; }

private:
    void run();

    std::string        m_artwork_dir;
    std::vector<Entry> m_entries;
    bool               m_scraping = false;

    std::thread             m_thread;
    std::atomic<bool>       m_stop{false};

    mutable std::mutex      m_ready_mutex;
    std::queue<std::string> m_ready;
};

}  // namespace sm2::osd
