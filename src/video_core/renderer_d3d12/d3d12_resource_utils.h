// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

/// Description of a row-major buffer of size bytes.
[[nodiscard]] constexpr D3D12_RESOURCE_DESC BufferDesc(
    u64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) noexcept {
    return {.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Alignment = 0, .Width = size,
            .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .Format = DXGI_FORMAT_UNKNOWN,
            .SampleDesc = {.Count = 1, .Quality = 0}, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            .Flags = flags};
}

[[nodiscard]] constexpr D3D12_RESOURCE_BARRIER TransitionBarrier(
    ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after,
    UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) noexcept {
    return {.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
            .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
            .Transition = {.pResource = resource, .Subresource = subresource,
                           .StateBefore = before, .StateAfter = after}};
}

[[nodiscard]] constexpr D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource) noexcept {
    return {.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV,
            .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
            .UAV = {.pResource = resource}};
}

/// Records one transition of every subresource of resource.
inline void TransitionResource(ID3D12GraphicsCommandList* commands, ID3D12Resource* resource,
                               D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    const D3D12_RESOURCE_BARRIER barrier = TransitionBarrier(resource, before, after);
    commands->ResourceBarrier(1, &barrier);
}

/// A committed buffer, counted in the resource creation perf counters. Throws (naming what) on
/// failure; E_OUTOFMEMORY is returned through hr instead when hr is given.
[[nodiscard]] ComPtr<ID3D12Resource> CreateCommittedBuffer(
    ID3D12Device* device, u64 size, D3D12_HEAP_TYPE heap_type,
    D3D12_RESOURCE_STATES initial_state, D3D12_RESOURCE_FLAGS flags, const char* what,
    HRESULT* hr = nullptr);

/// Serializes desc (root signature 1.0) and creates it; throws with the serializer's message.
[[nodiscard]] ComPtr<ID3D12RootSignature> CreateRootSignature(
    ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc, const char* what);

/// The heap type a resource lives in (DEFAULT when the query fails). Enters the runtime: cache the
/// result instead of calling this on every use.
[[nodiscard]] D3D12_HEAP_TYPE HeapType(ID3D12Resource* resource);

} // namespace D3D12
