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
// Model 3's counterpart of render::TextureDumper: every distinct texture the
// frame plans sample, as tex_<hash>.png, with the same index.html.
#pragma once

#include "core/types.h"
#include "render/model3/frame_plan.h"
#include "render/model3/texture_key.h"
#include "render/model3/textures.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sm2::render::model3 {

class TextureDumper {
public:
    TextureDumper(std::string directory, std::string game);
    ~TextureDumper();

    TextureDumper(const TextureDumper&)            = delete;
    TextureDumper& operator=(const TextureDumper&) = delete;

    /// Call once per built frame.
    void scan(const FramePlan& plan, const TextureSheets& sheets, TextureHasher& hasher);

    /// Where this frame's screenshot belongs when it showed a new texture, else
    /// empty. Clears the request.
    [[nodiscard]] std::string take_frame_request();

    void finish();

private:
    struct Texture {
        TextureRef      ref;
        std::vector<u8> rgba;
        bool            cut         = false;
        u64             uses        = 0;
        u32             frames_seen = 0;
        u32             last_frame  = 0;
        u32             first_frame = 0;
        s32             wrap[2]     = {0, 0};
    };
    struct PendingPng {
        std::string     path;
        u32             width  = 0;
        u32             height = 0;
        std::vector<u8> pixels;
    };

    void queue_png(std::string path, u32 width, u32 height, std::vector<u8> pixels);
    void write_loop();

    std::string m_directory;
    std::string m_game;
    std::string m_frame_request;
    u32         m_frame    = 0;
    bool        m_finished = false;
    std::unordered_map<u64, Texture> m_textures;
    std::vector<u64>                 m_order;

    std::mutex              m_queue_mutex;
    std::condition_variable m_queue_ready;
    std::deque<PendingPng>  m_queue;
    bool                    m_stopping = false;
    std::thread             m_writer;  // last: starts once everything above exists
};

}  // namespace sm2::render::model3
