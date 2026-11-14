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

#include "render/model3/texture_dump.h"

#include "core/log.h"
#include "render/image_write.h"
#include "render/texture_dump.h"
#include "render/texture_replace.h"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace sm2::render::model3 {
namespace {

constexpr std::array<const char*, 12> kFormatNames = {
    "T1RGB5",      "A4L4 (low byte)", "L4A4 (low byte)", "A4L4 (high byte)", "L4A4 (high byte)", "L8 (low byte)",
    "L8 (high byte)", "RGBA4",        "L4 (bits 0-3)",   "L4 (bits 4-7)",    "L4 (bits 8-11)",   "L4 (bits 12-15)"};
constexpr std::array<const char*, 4> kWrapNames = {"repeat", "clamp", "mirror", "mirror, clamp"};

}  // namespace

TextureDumper::TextureDumper(std::string directory, std::string game)
    : m_directory(std::move(directory))
    , m_game(std::move(game))
    , m_writer([this] { write_loop(); })
{
    std::error_code error;
    std::filesystem::create_directories(m_directory + "/frames", error);
    if (error) {
        SM2_ERROR("texture dump: could not create '%s'", m_directory.c_str());
    } else {
        SM2_INFO("texture dump: collecting into '%s'", m_directory.c_str());
    }
}

TextureDumper::~TextureDumper()
{
    finish();
    {
        const std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_stopping = true;
    }
    m_queue_ready.notify_one();
    m_writer.join();
}

void TextureDumper::queue_png(std::string path, u32 width, u32 height, std::vector<u8> pixels)
{
    {
        const std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_queue.push_back(PendingPng{std::move(path), width, height, std::move(pixels)});
    }
    m_queue_ready.notify_one();
}

void TextureDumper::write_loop()
{
    for (;;) {
        PendingPng job;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_ready.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_queue.empty()) {
                return;
            }
            job = std::move(m_queue.front());
            m_queue.pop_front();
        }
        (void)write_png(job.path, job.width, job.height, 4, job.pixels.data());
    }
}

void TextureDumper::scan(const FramePlan& plan, const TextureSheets& sheets, TextureHasher& hasher)
{
    ++m_frame;
    bool fresh = false;
    u32  last_block = ~0u;
    Texture* texture = nullptr;
    for (const R3DDraw& d : plan.draws) {
        const R3DUniforms& u = plan.uniforms[d.uniforms];
        if (u.textureEnabled == 0) {
            continue;
        }
        if (d.uniforms != last_block) {
            last_block = d.uniforms;
            const TextureRef ref  = texture_ref(u);
            const u64        hash = hasher.lookup(sheets, ref);
            auto [entry, inserted] = m_textures.try_emplace(hash);
            texture = &entry->second;
            if (inserted) {
                texture->ref         = ref;
                texture->first_frame = m_frame;
                texture->wrap[0]     = u.textureWrapMode[0];
                texture->wrap[1]     = u.textureWrapMode[1];
                decode_rgba(sheets, ref, true, &texture->rgba);
                m_order.push_back(hash);
                fresh = true;
            }
            texture->cut = texture->cut || u.alphaTest != 0 || u.textureAlpha != 0;
            if (texture->last_frame != m_frame) {
                texture->last_frame = m_frame;
                ++texture->frames_seen;
            }
        }
        ++texture->uses;
    }
    if (fresh) {
        char name[40];
        std::snprintf(name, sizeof(name), "/frames/frame_%06u.png", m_frame);
        m_frame_request = m_directory + name;
    }
}

std::string TextureDumper::take_frame_request()
{
    return std::exchange(m_frame_request, std::string());
}

void TextureDumper::finish()
{
    if (m_finished) {
        return;
    }
    m_finished = true;

    std::ostringstream json;
    json << "{\"game\":\"" << m_game << "\",\"unit\":\"draws\",\"textures\":[";
    bool first = true;
    for (const u64 hash : m_order) {
        Texture& t = m_textures.at(hash);
        if (!t.cut) {
            for (usize i = 3; i < t.rgba.size(); i += 4) {
                t.rgba[i] = 0xff;
            }
        }
        const std::string file = replacement_file_name(hash);
        queue_png(m_directory + "/" + file, t.ref.width, t.ref.height, std::move(t.rgba));

        char info[160];
        std::snprintf(info, sizeof(info), "%s, page %u at %u,%u, wrap %s / %s",
                      t.ref.format < kFormatNames.size() ? kFormatNames[t.ref.format] : "?", t.ref.page, t.ref.x,
                      t.ref.y, kWrapNames[static_cast<usize>(t.wrap[0] & 3)], kWrapNames[static_cast<usize>(t.wrap[1] & 3)]);
        json << (first ? "" : ",") << "{\"file\":\"" << file << "\",\"w\":" << t.ref.width << ",\"h\":" << t.ref.height
             << ",\"orient\":\"\",\"cut\":" << (t.cut ? "true" : "false") << ",\"area\":" << t.uses
             << ",\"frames\":" << t.frames_seen << ",\"first\":" << t.first_frame << ",\"info\":\"" << info
             << "\",\"variants\":[],\"outlines\":[]}";
        first = false;
    }
    json << "]}";
    std::ofstream(m_directory + "/index.html") << texture_index_html(json.str());

    SM2_INFO("texture dump: %zu texture(s) over %u frame(s); open '%s/index.html'", m_order.size(), m_frame,
             m_directory.c_str());
}

}  // namespace sm2::render::model3
