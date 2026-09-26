// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <string>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "common/common_types.h"

namespace D3D12 {

using Microsoft::WRL::ComPtr;

/// Throws std::runtime_error naming the call and the HRESULT when hr is a failure.
void ThrowIfFailed(HRESULT hr, const char* what);

/// The D3D12 device, its direct queue and a fence to track queue progress.
///
/// Xbox Series UWP (Dev Mode) exposes feature level 11.0 and shader model <= 6.4 with no Agility SDK,
/// and no public document lists the rest of its caps, so the constructor logs everything the
/// renderer will later depend on (LogCapabilities). That report is the ground truth for the backend.
class Device {
public:
    Device();
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    ID3D12Device* Get() const {
        return device.Get();
    }
    ID3D12CommandQueue* Queue() const {
        return queue.Get();
    }
    IDXGIFactory4* Factory() const {
        return factory.Get();
    }
    const std::string& AdapterName() const {
        return adapter_name;
    }

    /// Local (device) memory budget and usage; zeroes if the adapter cannot report them.
    [[nodiscard]] DXGI_QUERY_VIDEO_MEMORY_INFO QueryVideoMemory() const;

    /// Signals the queue and returns the value that marks this point.
    u64 Signal();
    /// Blocks until the queue has passed value.
    void Wait(u64 value);
    /// Blocks until all submitted work is done.
    void WaitIdle();

    /// With the debug layer on (renderer_debug), moves its stored errors and warnings to the log.
    /// Called once per frame; cheap when the layer is off.
    void LogDebugMessages();

    /// Logs what is known about a device removal, once: the removal reason, the debug layer's
    /// pending messages and, with renderer_debug, DRED's breadcrumbs and page fault.
    void ReportDeviceRemoved();

private:
    void LogCapabilities() const;

    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter3> adapter3; ///< same adapter; null if it lacks IDXGIAdapter3
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12InfoQueue> info_queue; ///< debug layer only
    u32 debug_messages_logged{};
    std::atomic_flag removal_reported;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event{};
    u64 next_fence_value{1};
    std::string adapter_name;
};

} // namespace D3D12
