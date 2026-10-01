// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/renderer_d3d12/d3d12_cache_policy.h"

#include <atomic>
#include <string>
#include <utility>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include "common/common_types.h"

namespace D3D12 {

using Microsoft::WRL::ComPtr;

/// Throws std::runtime_error naming the call and the HRESULT when hr is a failure.
void ThrowIfFailed(HRESULT hr, const char* what);

namespace removal_tripwire {
extern std::atomic_bool removal_tripped;
extern std::atomic_bool check_descriptors;
void ReportRemovedAfter(HRESULT reason, const std::string& what);
} // namespace removal_tripwire

/// Also checks after every descriptor a draw writes ("descriptor_checks=1"). Off by default:
/// GetDeviceRemovedReason enters the kernel, and once per descriptor it was 40% of the draws' CPU
/// time (0.2.59 profile). The check after each submission still catches the removal.
void SetDescriptorRemovalChecks(bool enabled);

/// Removal tripwire: a view, resource or pipeline created with parameters the driver rejects
/// removes the device (DXGI_ERROR_INVALID_CALL) without any other error, and the Xbox driver
/// rejects things the PC one accepts. Called right after such calls, it logs the first one after
/// which the device is gone; describe() (the call and its parameters) only runs then.
template <typename Describe>
void CheckRemovedAfter(ID3D12Device* device, Describe&& describe) {
    if (removal_tripwire::removal_tripped.load(std::memory_order_relaxed)) {
        return;
    }
    if (const HRESULT reason = device->GetDeviceRemovedReason(); FAILED(reason)) {
        removal_tripwire::ReportRemovedAfter(reason, describe());
    }
}

/// CheckRemovedAfter for the descriptors written on every draw, only with SetDescriptorRemovalChecks.
template <typename Describe>
void CheckRemovedAfterDescriptor(ID3D12Device* device, Describe&& describe) {
    if (removal_tripwire::check_descriptors.load(std::memory_order_relaxed)) {
        CheckRemovedAfter(device, std::forward<Describe>(describe));
    }
}

/// The D3D12 device, its direct queue and a fence to track queue progress.
///
/// Xbox Series UWP (Dev Mode) exposes feature level 11.0 and shader model <= 6.4 with no Agility SDK,
/// and no public document lists the rest of its caps, so the constructor logs everything the
/// renderer will later depend on (LogCapabilities). That report is the ground truth for the backend.
/// Enables GPU-based validation along with the debug layer (renderer_debug) for devices created
/// from now on. PC only.
void SetGpuBasedValidation(bool enabled);

/// Enables DRED auto-breadcrumbs and page-fault tracking for subsequently created devices.
/// Disabled by default because both facilities add measurable runtime overhead; use boot.cfg
/// "dred=1" only while diagnosing a device removal.
void SetDredEnabled(bool enabled);

/// The app's memory in use and its limit, false when unknown. On the Xbox the GPU allocates from
/// the same 5 GiB budget as the rest of the process, which DXGI does not show.
using AppMemoryQuery = bool (*)(u64& used, u64& limit);
/// Set by the frontend before the renderer is created; makes the caches' memory usage count the
/// whole app (see Device::CacheMemoryUsage).
void SetAppMemoryQuery(AppMemoryQuery query);

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

    /// Budget exposed to the generic caches. Xbox GPU allocations share the UWP process limit, so
    /// leave room for guest DRAM, JIT code, staging and transient allocations instead of treating
    /// DXGI's whole local-memory budget as cache space.
    [[nodiscard]] u64 CacheMemoryBudget() const;

    /// Memory usage for the texture and buffer caches' garbage collection, against the budget
    /// they read at startup. With an app memory query it is that budget minus what the app has
    /// left, so the caches start evicting as the process nears its limit whoever allocates (the
    /// Series ran out at 4.8 of 5 GiB with DXGI still reporting room, 0.2.56). Otherwise the GPU
    /// usage DXGI reports.
    [[nodiscard]] u64 CacheMemoryUsage() const;
    [[nodiscard]] u64 CacheMemoryUsage(const CacheMemorySnapshot& snapshot) const;
    [[nodiscard]] CacheMemorySnapshot QueryCacheMemoryPressure() const;

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
    u64 initial_budget{}; ///< local video memory budget when the device was created
};

} // namespace D3D12
