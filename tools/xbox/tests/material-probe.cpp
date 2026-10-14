// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Native GPU copy gate: pixel history must preserve bytes, layers, mip and edge tiles.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 failure"); }
int main() try {
    ComPtr<ID3D12Debug> debug;
    Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> messages; Check(device.As(&messages));
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands;
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands)));
    const auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = heap;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> result;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&result)));
        return result;
    };
    constexpr UINT SLOT = 32 * 256, CASES = 5;
    auto readback = buffer(CASES * SLOT, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    constexpr UINT FULL_SLOT = 65536;
    auto full = buffer(CASES * FULL_SLOT, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, CASES> full_footprints{};
    std::vector<ComPtr<ID3D12Resource>> keep_alive;
    struct Expected { UINT width, height, x, y; };
    std::array<Expected, CASES> expected{};
    for (UINT test = 0; test < CASES; ++test) {
        D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = 150; desc.Height = 142; desc.DepthOrArraySize = 2; desc.MipLevels = 2;
        desc.Format = std::array{DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}[test];
        desc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        Check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
        // Mip 1 of array layer 1: 75x71, subresource 3.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 size{};
        device->GetCopyableFootprints(&desc, 3, 1, 0, &footprint, nullptr, nullptr, &size);
        auto upload = buffer(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped{}; D3D12_RANGE empty{}; Check(upload->Map(0, &empty, &mapped));
        for (UINT y = 0; y < 71; ++y) for (UINT x = 0; x < 75; ++x) {
            const UINT value = (test << 24) | (y << 12) | x;
            std::memcpy(static_cast<char*>(mapped) + y * footprint.Footprint.RowPitch + x * 4, &value, 4);
        }
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; src.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}; dst.SubresourceIndex = 3;
        commands->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {texture.Get(), 3, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE};
        commands->ResourceBarrier(1, &barrier);
        full_footprints[test] = footprint;
        auto full_placed = footprint; full_placed.Offset = test * FULL_SLOT;
        D3D12_TEXTURE_COPY_LOCATION whole{full.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        whole.PlacedFootprint = full_placed;
        commands->CopyTextureRegion(&whole, 0, 0, 0, &dst, nullptr);
        const UINT x = test == 4 ? 74 : test * 3, y = test == 4 ? 70 : test * 2;
        const UINT width = std::min(64U, 75 - x), height = std::min(32U, 71 - y);
        expected[test] = {width, height, x, y};
        footprint.Offset = test * SLOT; footprint.Footprint.Width = width;
        footprint.Footprint.Height = height; footprint.Footprint.RowPitch = 256;
        D3D12_TEXTURE_COPY_LOCATION tile{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}; tile.PlacedFootprint = footprint;
        D3D12_BOX box{x, y, 0, x + width, y + height, 1};
        commands->CopyTextureRegion(&tile, 0, 0, 0, &dst, &box);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        commands->ResourceBarrier(1, &barrier);
        keep_alive.push_back(texture); keep_alive.push_back(upload);
    }
    // Hold the queue behind a GPU-side wait. A deferred consumer must leave the mapped
    // readback untouched until its completion fence advances; no CPU wait between copies.
    ComPtr<ID3D12Fence> gate;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
    Check(queue->Wait(gate.Get(), 1));
    Check(commands->Close()); ID3D12CommandList* lists[]{commands.Get()}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    Check(queue->Signal(fence.Get(), 1));
    if (fence->GetCompletedValue() != 0) throw std::runtime_error("readback retired before last copy");
    Check(gate->Signal(1));
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("event");
    Check(fence->SetEventOnCompletion(1, event)); const auto wait = WaitForSingleObject(event, 30000); CloseHandle(event);
    if (wait != WAIT_OBJECT_0) throw std::runtime_error("GPU timeout");
    void* mapped{}; Check(readback->Map(0, nullptr, &mapped));
    UINT texels = 0;
    for (UINT test = 0; test < CASES; ++test) {
        const auto e = expected[test];
        for (UINT y = 0; y < e.height; ++y) for (UINT x = 0; x < e.width; ++x) {
            UINT actual{}; std::memcpy(&actual, static_cast<char*>(mapped) + test * SLOT + y * 256 + x * 4, 4);
            if (actual != ((test << 24) | ((y + e.y) << 12) | (x + e.x))) throw std::runtime_error("pixel mismatch");
            ++texels;
        }
    }
    void* full_mapped{}; Check(full->Map(0, nullptr, &full_mapped));
    for (UINT test = 0; test < CASES; ++test) {
        const auto& fp = full_footprints[test];
        std::vector<unsigned char> packed(75 * 71 * 4);
        for (UINT y = 0; y < 71; ++y)
            std::memcpy(packed.data() + y * 75 * 4,
                static_cast<char*>(full_mapped) + test * FULL_SLOT + y * fp.Footprint.RowPitch, 75 * 4);
        for (UINT y = 0; y < 71; ++y) for (UINT x = 0; x < 75; ++x) {
            UINT actual{}; std::memcpy(&actual, packed.data() + (y * 75 + x) * 4, 4);
            if (actual != ((test << 24) | (y << 12) | x)) throw std::runtime_error("native full copy/alpha mismatch");
        }
    }
    full->Unmap(0, nullptr); readback->Unmap(0, nullptr);
    for (UINT64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{}; messages->GetMessage(i, nullptr, &size); std::vector<char> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); Check(messages->GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            std::cerr << message->pDescription << '\n'; throw std::runtime_error("debug layer message");
        }
    }
    Check(device->GetDeviceRemovedReason());
    std::cout << "PASS GPU tiles: " << texels << " exact texels, 5 formats, array mip, clipped edge, restored state; native full-frame/alpha and deferred fence gate; debug clean\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
