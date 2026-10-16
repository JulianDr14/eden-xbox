// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>
#include <d3d12sdklayers.h>
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_stage_bindings.h"

#include "video_core/renderer_d3d12/d3d12_uniform_stream.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_draw_metrics.h"

using namespace D3D12;

static void CheckMetrics() {
    assert(DrawMetrics::Bin(0) == 0 && DrawMetrics::Bin(1) == 1);
    for (u32 i = 0; i < 63; ++i) {
        const u64 value = u64{1} << i;
        assert(DrawMetrics::Bin(value) == (std::min)(size_t{i + 1}, size_t{63}));
        assert(DrawMetrics::Bin(value - 1) == i);
    }
    assert(DrawMetrics::Bin(~u64{0}) == 63);
    VideoCore::Perf::SetDetailedGpuProfile(false);
    const auto binding_samples = DrawMetrics::stage_binding_samples.load();
    for (u32 i = 0; i < 128; ++i) {
        assert(!DrawMetrics::SampleStageBindings());
        const DrawMetrics::SampledStageBindingTimer timer{false};
    }
    assert(DrawMetrics::stage_binding_samples.load() == binding_samples);
    const auto locks = DrawMetrics::cache_lock_samples.load();
    { const DrawMetrics::CacheLockTimer timer; timer.Locked(); }
    assert(DrawMetrics::cache_lock_samples.load() == locks);
    const auto before = VideoCore::Perf::ReadCounter(VideoCore::Perf::Counter::DrawNs);
    { DrawMetrics::ScopedDrawTimer timer; }
    assert(VideoCore::Perf::ReadCounter(VideoCore::Perf::Counter::DrawNs) == before);
    VideoCore::Perf::SetDetailedGpuProfile(true);
    for (u32 i = 0; i < 64; ++i) {
        const DrawMetrics::SampledStageBindingTimer timer{DrawMetrics::SampleStageBindings()};
    }
    assert(DrawMetrics::stage_binding_samples.load() == binding_samples + 1);
    { DrawMetrics::ScopedDrawTimer timer; }
    { const DrawMetrics::CacheLockTimer timer; timer.Locked(); }
    assert(DrawMetrics::cache_lock_samples.load() == locks + 1);
    u64 count{};
    for (const auto& bin : DrawMetrics::draw_bins) count += bin.load();
    assert(count == 1);
    VideoCore::Perf::SetDetailedGpuProfile(false);
    std::cout << "PASS draw metrics: bin boundaries, disabled counters unchanged, enabled sample counted\n";
}

static void CheckMetadata() {
    for (u32 variant = 0; variant < 512; ++variant) {
        Shader::Info info{};
        for (u32 i = 0; i < variant % 17; ++i) {
            info.constant_buffer_descriptors.push_back({.index = i, .count = 1 + i % 3});
            info.storage_buffers_descriptors.push_back({.count = 1 + i % 2});
            info.texture_buffer_descriptors.push_back({.count = 1 + i % 4});
            info.image_buffer_descriptors.push_back({.count = 1 + i % 5});
            info.texture_descriptors.push_back({.count = 1 + i % 7});
            info.image_descriptors.push_back({.count = 1 + i % 6});
        }
        info.uses_render_area = variant % 2;
        const auto stage = InternStage(info);
        assert(stage == InternStage(info));
        assert(stage->num_texture_views == Shader::NumDescriptors(info.texture_descriptors) +
                                           Shader::NumDescriptors(info.image_descriptors));
        assert(stage->num_buffer_views == Shader::NumDescriptors(info.texture_buffer_descriptors) +
                                          Shader::NumDescriptors(info.image_buffer_descriptors));
        assert(stage->first_texture_binding == FirstTextureBinding(info, 0));
        assert(stage->num_bindings == NumStageBindings(info));
        for (u32 base : {0u, 12u, 512u})
            assert(base + stage->first_texture_binding == FirstTextureBinding(info, base));
    }
    std::cout << "PASS metadata: 512 shader layouts, arrays/empty stages/interner/base offsets\n";
}

__declspec(noinline) static u32 OldMetadata(const StageBindings& info) {
    return Shader::NumDescriptors(info.texture_descriptors) +
           Shader::NumDescriptors(info.image_descriptors) +
           Shader::NumDescriptors(info.texture_buffer_descriptors) +
           Shader::NumDescriptors(info.image_buffer_descriptors) +
           FirstTextureBinding(info, 0) + NumStageBindings(info);
}

__declspec(noinline) static u32 NewMetadata(const StageBindings& info) {
    return info.num_texture_views + info.num_buffer_views +
           info.first_texture_binding + info.num_bindings;
}

static void BenchmarkMetadata() {
    std::array<std::shared_ptr<const StageBindings>, 32> stages;
    for (size_t i = 0; i < stages.size(); ++i) {
        Shader::Info info{};
        for (u32 j = 0; j < i % 16; ++j) {
            info.texture_descriptors.push_back({.count = 1 + j % 3});
            info.constant_buffer_descriptors.push_back({.count = 1});
        }
        stages[i] = InternStage(info);
    }
    std::array<double, 7> before{}, after{};
    u64 old_sum{}, new_sum{};
    for (u32 sample = 0; sample < 7; ++sample) {
        for (u32 order = 0; order < 2; ++order) {
            const bool optimized = (sample + order) % 2;
            u64 sum{};
            constexpr u32 iterations = 1000000;
            const auto start = std::chrono::steady_clock::now();
            for (u32 i = 0; i < iterations; ++i)
                sum += optimized ? NewMetadata(*stages[i % stages.size()])
                                 : OldMetadata(*stages[i % stages.size()]);
            const double ns = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - start).count() / iterations;
            (optimized ? after : before)[sample] = ns;
            (optimized ? new_sum : old_sum) += sum;
        }
    }
    assert(old_sum == new_sum && old_sum);
    std::sort(before.begin(), before.end()); std::sort(after.begin(), after.end());
    std::cout << "Metadata median ns/stage " << before[3] << " -> " << after[3]
              << " (7 samples; min/max " << before[0] << '/' << before[6] << " -> "
              << after[0] << '/' << after[6] << "; sum " << old_sum << ")\n";
}

// Real sampler descriptors: borrowed keys must survive rehash and be discarded at heap reset.
static void CheckSamplers(bool debug) {
    Settings::values.renderer_debug = debug;
    Device device;
    {
        Scheduler scheduler{device};
        CpuDescriptorAllocator offline{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER};
        SamplerHeap samplers{device.Get(), scheduler};
        const auto handle = offline.Allocate();
        const D3D12_SAMPLER_DESC desc{.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
            .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
            .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, .MaxAnisotropy = 1,
            .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS, .MaxLOD = D3D12_FLOAT32_MAX};
        device.Get()->CreateSampler(&desc, handle);
        std::array<u64, 32> a{}, b{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 32> handles;
        handles.fill(handle);
        for (u32 i = 0; i < 32; ++i) { a[i] = 10000 + i; b[i] = 20000 + i; }
        const auto first = samplers.GetTable(a, handles);
        const auto second = samplers.GetTable(b, handles);
        for (u32 i = 0; i < 1984; ++i) {
            const u64 unique = 30000 + i;
            (void)samplers.GetTable({&unique, 1}, {&handle, 1});
            // Repeated table hits after map rehashes keep the borrowed key alive.
            assert(samplers.GetTable(a, handles).ptr == first.ptr);
            assert(samplers.GetTable(b, handles).ptr == second.ptr);
        }
        const u64 overflow = 999999;
        const auto reset = samplers.GetTable({&overflow, 1}, {&handle, 1});
        assert(reset.ptr == first.ptr);
        assert(samplers.GetTable({&overflow, 1}, {&handle, 1}).ptr == reset.ptr);
        const auto rebuilt = samplers.GetTable(a, handles);
        assert(rebuilt.ptr == first.ptr + device.Get()->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER));
        assert(samplers.GetTable(a, handles).ptr == rebuilt.ptr);
        scheduler.Finish();
        offline.Free(handle);
        std::cout << "PASS sampler cache: alternating tables, 1984 insertions/rehashes, full heap/reset/rebuild\n";
    }
    if (debug) {
        ComPtr<ID3D12InfoQueue> messages;
        ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "sampler debug queue");
        for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T size{}; messages->GetMessage(i, nullptr, &size);
            std::vector<std::byte> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            ThrowIfFailed(messages->GetMessage(i, message, &size), "sampler debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
                throw std::runtime_error(message->pDescription);
        }
    }
}

static void CheckUniformStream(bool threaded) {
    Settings::values.renderer_debug = true;
    SetAsyncRecording(threaded);
    Device device;
    ComPtr<ID3D12InfoQueue> messages;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "uniform debug queue");
    messages->ClearStoredMessages();
    Scheduler scheduler{device};
    StagingBufferPool staging{device, scheduler};
    CpuDescriptorAllocator offline{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER};
    SamplerHeap samplers{device.Get(), scheduler};
    const auto sampler_handle = offline.Allocate();
    const D3D12_SAMPLER_DESC sampler_desc{.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT,
        .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
        .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, .MaxAnisotropy = 1,
        .ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS, .MaxLOD = D3D12_FLOAT32_MAX};
    device.Get()->CreateSampler(&sampler_desc, sampler_handle);
    std::vector<u64> full_keys(2048);
    for (u32 i = 0; i < full_keys.size(); ++i) full_keys[i] = i + 1;
    const std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> full_handles(2048, sampler_handle);
    UniformStream stream;
    auto readback = CreateCommittedBuffer(device.Get(), 8 * 1024 * 1024,
        D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_FLAG_NONE, "uniform stream readback");
    std::vector<u8> expected;
    u64 allocations{};
    const auto allocate = [&](size_t size) {
        ++allocations;
        const auto ref = staging.Request(size, MemoryUsage::Upload);
        return UniformUpload{ref.mapped_span, ref.buffer->GetGPUVirtualAddress() + ref.offset};
    };
    // Use the address only to validate alignment; the staging callback also retains the resource
    // and offset of each reservation, for CopyBufferRegion of the suballocated slices.
    StagingBufferRef block{};
    u64 block_address{};
    const auto copy_allocate = [&](size_t size) {
        ++allocations;
        block = staging.Request(size, MemoryUsage::Upload);
        block_address = block.buffer->GetGPUVirtualAddress() + block.offset;
        return UniformUpload{block.mapped_span, block_address};
    };
    // Large bypasses may replace the callback's block, so find a slice's owning reservation by
    // recording each allocation (just in the test; production does not retain this list).
    std::vector<StagingBufferRef> reservations;
    const auto tracked_allocate = [&](size_t size) {
        auto upload = copy_allocate(size);
        reservations.push_back(block);
        return upload;
    };
    constexpr std::array<size_t, 10> sizes{1, 255, 256, 257, 1024, 2048, 4096, 4097, 65536, 0};
    for (u32 i = 0; i < 400; ++i) {
        const size_t size = sizes[i % sizes.size()];
        const u64 tick = scheduler.CurrentTick();
        const auto upload = stream.Request(size, tick, tracked_allocate);
        assert(upload.address % 256 == 0 && upload.mapped.size() >= size);
        const u8 value = static_cast<u8>(i * 17);
        std::fill_n(upload.mapped.data(), size, value);
        if (size) {
            bool found{};
            for (const auto& reservation : reservations) {
                const u64 base = reservation.buffer->GetGPUVirtualAddress() + reservation.offset;
                if (upload.address < base || upload.address - base + size > reservation.mapped_span.size()) continue;
                scheduler.CommandList()->CopyBufferRegion(readback.Get(), expected.size(),
                    reservation.buffer, reservation.offset + upload.address - base, size);
                found = true;
                break;
            }
            assert(found);
            expected.insert(expected.end(), size, value);
        }
        if (i % 31 == 30) {
            // Force the same Finish/reset that Configure must perform BEFORE UBO reservations.
            const u64 old_tick = scheduler.CurrentTick();
            (void)samplers.GetTable(full_keys, full_handles);
            const u64 unique = 10000 + i;
            (void)samplers.GetTable({&unique, 1}, {&sampler_handle, 1});
            assert(scheduler.CurrentTick() > old_tick);
        }
    }
    scheduler.Finish();
    void* mapped{};
    const D3D12_RANGE range{0, expected.size()};
    ThrowIfFailed(readback->Map(0, &range, &mapped), "map uniform readback");
    assert(std::equal(expected.begin(), expected.end(), static_cast<const u8*>(mapped)));
    const D3D12_RANGE no_write{};
    readback->Unmap(0, &no_write);
    for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
        SIZE_T size{}; messages->GetMessage(i, nullptr, &size);
        std::vector<std::byte> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        ThrowIfFailed(messages->GetMessage(i, message, &size), "uniform debug message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            throw std::runtime_error(message->pDescription);
    }
    std::cout << "PASS uniform stream GPU (threaded=" << threaded << "): 400 uploads/" << expected.size()
              << " exact bytes, 13 sampler rollover checkpoints, 0..65536-byte requests, " << allocations
              << " staging reservations; debug clean\n";
    if (threaded) { SetAsyncRecording(false); return; }
    // Allocation-only timing using the real staging pool, no shader/draw/GPU execution timing.
    Settings::values.renderer_debug = false;
    std::array<double, 7> before{}, after{};
    u64 checksum{};
    for (u32 sample = 0; sample < 7; ++sample) {
        for (u32 order = 0; order < 2; ++order) {
            const bool optimized = (sample + order) % 2;
            UniformStream candidate;
            constexpr u32 iterations = 10000;
            const auto start = std::chrono::steady_clock::now();
            for (u32 i = 0; i < iterations; ++i) {
                const auto upload = optimized ? candidate.Request(256, scheduler.CurrentTick(), allocate)
                                              : allocate(256);
                checksum += upload.address;
            }
            const double ns = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - start).count() / iterations;
            (optimized ? after : before)[sample] = ns;
            scheduler.Finish();
        }
    }
    std::sort(before.begin(), before.end()); std::sort(after.begin(), after.end());
    std::cout << "Uniform staging median ns/256-byte request " << before[3] << " -> " << after[3]
              << " (7 samples; min/max " << before[0] << '/' << before[6] << " -> "
              << after[0] << '/' << after[6] << "; checksum " << checksum << ")\n";
    device.LogDebugMessages();
    ThrowIfFailed(device.Get()->GetDeviceRemovedReason(), "uniform stream device status");
}

int main() try {
    Common::Log::Initialize();
    CheckMetrics(); CheckMetadata(); BenchmarkMetadata();
    CheckSamplers(false); CheckSamplers(true);
    CheckUniformStream(false); CheckUniformStream(true);
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
}
