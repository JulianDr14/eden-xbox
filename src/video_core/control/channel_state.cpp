// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <stdexcept>
#include <type_traits>

#include "common/assert.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/engines/puller.h"
#include "video_core/memory_manager.h"

namespace Tegra::Control {
namespace {

// Match NVK/Nouveau's initial pushbuffer subchannel layout.
constexpr u32 Nvk3DSubchannel = 0;
constexpr u32 NvkComputeSubchannel = 1;
constexpr u32 Nvk2DSubchannel = 3;
constexpr u32 NvkCopySubchannel = 4;

void BindNvkDefaultSubchannels(ChannelState::Payload& payload) {
    auto& dma_pusher = payload.dma_pusher;
    dma_pusher.BindSubchannel(&payload.maxwell_3d, Nvk3DSubchannel, Engines::EngineTypes::Maxwell3D);
    dma_pusher.BindSubchannel(&payload.kepler_compute, NvkComputeSubchannel,
                              Engines::EngineTypes::KeplerCompute);
    // Subchannel 2 is M2MF there; Eden does not expose a 0x9039 engine yet.
    dma_pusher.BindSubchannel(&payload.fermi_2d, Nvk2DSubchannel, Engines::EngineTypes::Fermi2D);
    dma_pusher.BindSubchannel(&payload.maxwell_dma, NvkCopySubchannel,
                              Engines::EngineTypes::MaxwellDMA);
}

constexpr u32 PulseStateVersion = 1;

class PulseOut {
public:
    explicit PulseOut(std::vector<u8>& bytes_) : bytes{bytes_} {}
    template <typename T>
    void Pod(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* data = reinterpret_cast<const u8*>(&value);
        bytes.insert(bytes.end(), data, data + sizeof(T));
    }

private:
    std::vector<u8>& bytes;
};

class PulseIn {
public:
    explicit PulseIn(std::span<const u8> bytes_) : bytes{bytes_} {}
    template <typename T>
    void Pod(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (sizeof(T) > bytes.size() - offset) {
            throw std::invalid_argument("Pulse: truncated GPU channel state");
        }
        std::memcpy(&value, bytes.data() + offset, sizeof(T));
        offset += sizeof(T);
    }
    template <typename T>
    T Get() {
        T value;
        Pod(value);
        return value;
    }
    // A count of following records of at least min_record bytes each, bounded by what is left.
    u32 Count(size_t min_record) {
        const auto count = Get<u32>();
        if (count > (bytes.size() - offset) / min_record) {
            throw std::invalid_argument("Pulse: GPU channel state count");
        }
        return count;
    }
    void End() const {
        if (offset != bytes.size()) throw std::invalid_argument("Pulse: trailing GPU channel state");
    }

private:
    std::span<const u8> bytes;
    size_t offset{};
};

} // Anonymous namespace

ChannelState::Payload::Payload(Core::System& system, MemoryManager& memory_manager, ChannelState& channel_state)
    : maxwell_3d(memory_manager)
    , fermi_2d(memory_manager)
    , kepler_compute(memory_manager)
    , maxwell_dma(memory_manager)
    , kepler_memory(memory_manager)
    , nv01_timer(memory_manager)
    , dma_pusher(system, memory_manager, channel_state)
{}

ChannelState::ChannelState(s32 bind_id_)
    : bind_id{bind_id_}
{}

void ChannelState::Init(Core::System& system, u64 program_id_) {
    ASSERT(memory_manager);
    program_id = program_id_;
    payload.emplace(system, *memory_manager, *this);
    BindNvkDefaultSubchannels(*payload);
    initialized = true;
}

void ChannelState::BindRasterizer(VideoCore::RasterizerInterface* rasterizer) {
    payload->dma_pusher.BindRasterizer(rasterizer);
    memory_manager->BindRasterizer(rasterizer);
    payload->maxwell_3d.BindRasterizer(rasterizer);
    payload->fermi_2d.BindRasterizer(rasterizer);
    payload->kepler_memory.BindRasterizer(rasterizer);
    payload->kepler_compute.BindRasterizer(rasterizer);
    payload->maxwell_dma.BindRasterizer(rasterizer);
    //payload->nv01_timer.BindRasterizer(rasterizer);
}

std::vector<u8> ChannelState::PulseSave() const {
    if (!initialized || !payload) throw std::logic_error("Pulse: GPU channel not initialized");
    const auto& p = *payload;
    const auto& pusher = p.dma_pusher;
    // With the GPU idle the pusher is between command lists; a method or a macro still fed by
    // the next list is mid-command and gets another try.
    if (!pusher.dma_pushbuffer.empty() || pusher.dma_state.method_count != 0 ||
        pusher.dma_state.length_pending != 0) {
        throw std::runtime_error("a GPU channel is mid-command");
    }
    if (p.maxwell_3d.executing_macro != 0 || !p.maxwell_3d.macro_params.empty()) {
        throw std::runtime_error("a GPU macro is half fed");
    }

    std::vector<u8> bytes;
    PulseOut out{bytes};
    out.Pod(PulseStateVersion);

    const auto& m3d = p.maxwell_3d;
    out.Pod(m3d.regs);
    out.Pod(m3d.shadow_state);
    out.Pod(m3d.macro_positions);
    out.Pod(m3d.state);
    out.Pod(m3d.execute_on);
    out.Pod(m3d.engine_state);
    out.Pod(static_cast<u32>(m3d.replace_table.size()));
    for (const auto& [key, value] : m3d.replace_table) {
        out.Pod(key);
        out.Pod(value);
    }
    out.Pod(static_cast<u32>(m3d.macro_engine.uploaded_macro_code.size()));
    for (const auto& [method, code] : m3d.macro_engine.uploaded_macro_code) {
        out.Pod(method);
        out.Pod(static_cast<u32>(code.size()));
        for (const u32 word : code) out.Pod(word);
    }

    out.Pod(p.fermi_2d.regs);
    out.Pod(p.kepler_compute.regs);
    out.Pod(p.maxwell_dma.regs);
    out.Pod(p.kepler_memory.regs);

    out.Pod(pusher.puller.regs);
    out.Pod(pusher.puller.bound_engines);
    for (size_t i = 0; i < pusher.subchannels.size(); ++i) {
        out.Pod(static_cast<u8>(pusher.subchannels[i] != nullptr));
        out.Pod(pusher.subchannel_type[i]);
    }
    out.Pod(pusher.dma_state.method);
    out.Pod(pusher.dma_state.subchannel);
    out.Pod(static_cast<u8>(pusher.dma_state.non_incrementing));
    out.Pod(static_cast<u8>(pusher.dma_increment_once));
    return bytes;
}

void ChannelState::PulseLoad(std::span<const u8> state) {
    if (!initialized || !payload) throw std::logic_error("Pulse: GPU channel not initialized");
    auto& p = *payload;
    PulseIn in{state};
    if (in.Get<u32>() != PulseStateVersion) {
        throw std::invalid_argument("Pulse: GPU channel state version");
    }

    auto& m3d = p.maxwell_3d;
    in.Pod(m3d.regs);
    in.Pod(m3d.shadow_state);
    in.Pod(m3d.macro_positions);
    in.Pod(m3d.state);
    in.Pod(m3d.execute_on);
    in.Pod(m3d.engine_state);
    m3d.replace_table.clear();
    const auto replacements = in.Count(sizeof(u64) + sizeof(u32));
    for (u32 i = 0; i < replacements; ++i) {
        const auto key = in.Get<u64>();
        m3d.replace_table[key] = in.Get<Engines::Maxwell3D::HLEReplacementAttributeType>();
    }
    m3d.macro_engine.macro_cache.clear();
    m3d.macro_engine.uploaded_macro_code.clear();
    const auto macros = in.Count(2 * sizeof(u32));
    for (u32 i = 0; i < macros; ++i) {
        const auto method = in.Get<u32>();
        std::vector<u32> code(in.Count(sizeof(u32)));
        for (auto& word : code) in.Pod(word);
        m3d.macro_engine.uploaded_macro_code[method] = std::move(code);
    }
    m3d.executing_macro = 0;
    m3d.macro_params.clear();
    m3d.macro_segments.clear();
    // Every register is new to the rasterizer: it rebuilds its view of them on the next draw.
    m3d.dirty.flags.set();

    in.Pod(p.fermi_2d.regs);
    in.Pod(p.kepler_compute.regs);
    in.Pod(p.maxwell_dma.regs);
    in.Pod(p.kepler_memory.regs);

    auto& pusher = p.dma_pusher;
    in.Pod(pusher.puller.regs);
    in.Pod(pusher.puller.bound_engines);
    for (u32 i = 0; i < pusher.subchannels.size(); ++i) {
        const auto bound = in.Get<u8>();
        const auto type = in.Get<Engines::EngineTypes>();
        Engines::EngineInterface* engine = nullptr;
        if (bound) {
            switch (type) {
            case Engines::EngineTypes::Nv01Timer: engine = &p.nv01_timer; break;
            case Engines::EngineTypes::KeplerCompute: engine = &p.kepler_compute; break;
            case Engines::EngineTypes::Maxwell3D: engine = &p.maxwell_3d; break;
            case Engines::EngineTypes::Fermi2D: engine = &p.fermi_2d; break;
            case Engines::EngineTypes::MaxwellDMA: engine = &p.maxwell_dma; break;
            case Engines::EngineTypes::KeplerMemory: engine = &p.kepler_memory; break;
            default: throw std::invalid_argument("Pulse: GPU subchannel engine");
            }
        }
        pusher.subchannels[i] = engine;
        pusher.subchannel_type[i] = type;
    }
    in.Pod(pusher.dma_state.method);
    in.Pod(pusher.dma_state.subchannel);
    pusher.dma_state.non_incrementing = in.Get<u8>() != 0;
    pusher.dma_increment_once = in.Get<u8>() != 0;
    in.End();
}

} // namespace Tegra::Control
