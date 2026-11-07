// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the front end needs of a cabinet link board, whichever board it is.
#pragma once

#include "core/types.h"
#include "hw/comm_transport.h"

#include <memory>

namespace sm2::hw {

class CommBoard {
public:
    virtual ~CommBoard() = default;

    /// Swap the frame transport (takes ownership); nullptr restores the default
    /// loopback.
    virtual void set_transport(std::unique_ptr<CommTransport> transport) = 0;

    /// Frames by which other cabinets' data is read late (Model 3 net board).
    virtual void set_link_delay(u32 /*frames*/) {}

    [[nodiscard]] virtual bool link_alive() const = 0;
    [[nodiscard]] virtual u8   link_id() const    = 0;
    [[nodiscard]] virtual u8   link_count() const = 0;
    [[nodiscard]] virtual bool enabled() const    = 0;
};

}  // namespace sm2::hw
