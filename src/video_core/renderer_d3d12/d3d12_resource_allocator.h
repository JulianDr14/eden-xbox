// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

class Scheduler;

/// Suballocates texture memory from reusable DEFAULT heaps. The allocation token returns its
/// range only after the scheduler has released the placed resource at the same fence tick.
class TextureResourceAllocator {
public:
    struct Allocation;
    struct Resource {
        ComPtr<ID3D12Resource> resource;
        std::shared_ptr<Allocation> allocation;
        bool placed{};
    };

    TextureResourceAllocator(const Device& device, Scheduler& scheduler);
    ~TextureResourceAllocator();

    TextureResourceAllocator(const TextureResourceAllocator&) = delete;
    TextureResourceAllocator& operator=(const TextureResourceAllocator&) = delete;

    [[nodiscard]] Resource Create(const D3D12_RESOURCE_DESC& desc,
                                  D3D12_RESOURCE_STATES initial_state,
                                  const D3D12_CLEAR_VALUE* clear_value = nullptr);
    void DeferRelease(Resource&& resource);

    [[nodiscard]] std::string Report() const;

private:
    struct State;
    const Device& device;
    Scheduler& scheduler;
    std::shared_ptr<State> state;
};

} // namespace D3D12
