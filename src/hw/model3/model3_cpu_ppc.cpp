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
// MainCpu over the real PowerPC core (SM2_MODEL3_HAVE_PPC builds only).

#include "cpu/ppc/ppc.h"
#include "hw/model3/ports.h"

namespace sm2::hw::model3 {
namespace {

class PpcCpu final : public MainCpu {
public:
    void init(const cpu::ppc::PpcConfig& config, cpu::PageMap& pages,
              cpu::ppc::PpcBus& bus) override
    {
        m_ppc.init(config, pages, bus);
        m_ppc.set_exec_mode(cpu::ppc::ExecMode::Jit);
    }
    void reset() override { m_ppc.reset(); }
    s64  run(s64 cycles) override { return m_ppc.run(cycles); }
    void end_timeslice() override { m_ppc.end_timeslice(); }
    void set_irq(bool asserted) override { m_ppc.set_irq(asserted); }
    void set_exec_mode(cpu::ppc::ExecMode mode) override { m_ppc.set_exec_mode(mode); }
    [[nodiscard]] u64       total_cycles() const override { return m_ppc.total_cycles(); }
    [[nodiscard]] CpuStatus status() const override { return m_ppc.status(); }
    void serialize(Archive& ar) override { m_ppc.serialize(ar); }
    void invalidate(u32 address, u32 size) override { m_ppc.invalidate(address, size); }

private:
    cpu::ppc::Ppc m_ppc;
};

}  // namespace

std::unique_ptr<MainCpu> create_ppc_cpu()
{
    return std::make_unique<PpcCpu>();
}

}  // namespace sm2::hw::model3
