// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdexcept>
#include <string>

#include <fmt/format.h>

#include "video_core/perf_counters.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"

namespace D3D12 {

ComPtr<ID3D12Resource> CreateCommittedBuffer(ID3D12Device* device, u64 size,
                                             D3D12_HEAP_TYPE heap_type,
                                             D3D12_RESOURCE_STATES initial_state,
                                             D3D12_RESOURCE_FLAGS flags, const char* what,
                                             HRESULT* hr) {
    const D3D12_HEAP_PROPERTIES heap{.Type = heap_type};
    const D3D12_RESOURCE_DESC desc = BufferDesc(size, flags);
    ComPtr<ID3D12Resource> buffer;
    VideoCore::Perf::ScopedTimer timer{VideoCore::Perf::Counter::ResourceCreateUs,
                                       VideoCore::Perf::Counter::ResourcesCreated};
    const HRESULT result = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           initial_state, nullptr,
                                                           IID_PPV_ARGS(&buffer));
    if (hr) {
        *hr = result;
        if (result == E_OUTOFMEMORY) {
            return {};
        }
    }
    ThrowIfFailed(result, what);
    CheckRemovedAfter(device, [&] {
        return fmt::format("{} ({} bytes, heap {})", what, size, static_cast<u32>(heap_type));
    });
    return buffer;
}

ComPtr<ID3D12RootSignature> CreateRootSignature(ID3D12Device* device,
                                                const D3D12_ROOT_SIGNATURE_DESC& desc,
                                                const char* what) {
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    const HRESULT hr =
        D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &error);
    if (FAILED(hr)) {
        std::string message = "(no details)";
        if (error && error->GetBufferSize() > 0) {
            message.assign(static_cast<const char*>(error->GetBufferPointer()),
                           error->GetBufferSize());
        }
        throw std::runtime_error(fmt::format("D3D12: serializing the {} failed (0x{:08X}): {}",
                                             what, static_cast<u32>(hr), message));
    }
    ComPtr<ID3D12RootSignature> root_signature;
    ThrowIfFailed(device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                              serialized->GetBufferSize(),
                                              IID_PPV_ARGS(&root_signature)),
                  what);
    return root_signature;
}

D3D12_HEAP_TYPE HeapType(ID3D12Resource* resource) {
    D3D12_HEAP_PROPERTIES properties{};
    D3D12_HEAP_FLAGS flags{};
    if (FAILED(resource->GetHeapProperties(&properties, &flags))) {
        return D3D12_HEAP_TYPE_DEFAULT;
    }
    return properties.Type;
}

} // namespace D3D12
