// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <stdexcept>
#include <utility>

#include "common/logging.h"
#include "common/settings.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "video_core/capture.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"
#include "video_core/renderer_d3d12/renderer_d3d12_internal.h"

namespace D3D12 {

using namespace PresentDetail;

namespace {

/// Shader-visible CBV/SRV/UAV descriptors in the ring. Far below the 1,000,000 tier limit; enough
/// for many frames of draws with room for the ring never to stall in practice.
constexpr u32 DESCRIPTOR_RING_SIZE = 256 * 1024;

std::atomic<u32> traced_frame_override{};
std::atomic<u32> traced_frame_count{1};
std::atomic<bool> frame_diagnostics{true};
std::atomic<u32> trace_requested{};
std::atomic<bool> material_trace_requested{};
std::atomic<u32> material_capture_frame{};
std::atomic<u64> material_trace_shader{};
std::atomic<s32> material_probe_x{-1}, material_probe_y{-1};

IUnknown* CoreWindowOf(const Core::Frontend::EmuWindow& emu_window) {
    const auto& info = emu_window.GetWindowInfo();
    if (info.type != Core::Frontend::WindowSystemType::CoreWindow ||
        info.render_surface == nullptr) {
        throw std::runtime_error("D3D12: the frontend did not provide a CoreWindow");
    }
    return static_cast<IUnknown*>(info.render_surface);
}

} // Anonymous namespace

void SetTracedFrame(u32 frame, u32 count) {
    traced_frame_override.store(frame, std::memory_order_relaxed);
    traced_frame_count.store(std::max<u32>(count, 1), std::memory_order_relaxed);
}

void TraceNextFrames(u32 count) {
    trace_requested.store(std::max<u32>(count, 1), std::memory_order_relaxed);
}

void SetFrameDiagnostics(bool enabled) {
    frame_diagnostics.store(enabled, std::memory_order_relaxed);
}

void TraceNextMaterialFrame(u64 pixel_shader) {
    material_trace_shader.store(pixel_shader, std::memory_order_relaxed);
    material_trace_requested.store(true, std::memory_order_release);
}

void SetMaterialProbe(s32 x, s32 y) {
    material_probe_x.store(x, std::memory_order_relaxed);
    material_probe_y.store(y, std::memory_order_relaxed);
}

void SetMaterialCaptureFrame(u32 frame) {
    material_capture_frame.store(frame, std::memory_order_relaxed);
}

namespace {
std::atomic<u64> material_snapshot_first{}, material_snapshot_second{};
std::atomic<u32> material_snapshot_vertices_first{}, material_snapshot_vertices_second{};
}

void SetMaterialSnapshotShaders(u64 first, u64 second) {
    material_snapshot_first.store(first, std::memory_order_relaxed);
    material_snapshot_second.store(second, std::memory_order_relaxed);
}

void SetMaterialSnapshotVertexCounts(u32 first, u32 second) {
    material_snapshot_vertices_first.store(first, std::memory_order_relaxed);
    material_snapshot_vertices_second.store(second, std::memory_order_relaxed);
}

RendererD3D12::RendererD3D12(Core::Frontend::EmuWindow& emu_window,
                             Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                             std::unique_ptr<Core::Frontend::GraphicsContext> context_)
    : RendererBase(emu_window, std::move(context_)), device_memory{device_memory_}, gpu{gpu_},
      device{}, swapchain{device, CoreWindowOf(emu_window),
                          emu_window.GetFramebufferLayout().width,
                          emu_window.GetFramebufferLayout().height},
      shader_compiler{}, scheduler{device}, staging_pool{device, scheduler},
      transfer_buffers{device, scheduler},
      buffer_cache_runtime{device, scheduler, staging_pool, transfer_buffers, device_memory_},
      view_descriptors{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV},
      sampler_descriptors{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 256},
      rtv_descriptors{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 256, &scheduler},
      dsv_descriptors{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 256, &scheduler},
      texture_cache_runtime{device, scheduler, staging_pool, transfer_buffers, view_descriptors,
                            sampler_descriptors, rtv_descriptors, dsv_descriptors},
      descriptor_ring{device.Get(), scheduler, DESCRIPTOR_RING_SIZE},
      sampler_heap{device.Get(), scheduler},
      blit_helper{device, scheduler, shader_compiler, descriptor_ring, sampler_heap,
                  sampler_descriptors},
      present_fsr{device, shader_compiler, scheduler, view_descriptors, rtv_descriptors},
      rasterizer{gpu_, device_memory_, device, scheduler, shader_compiler, buffer_cache_runtime,
                 texture_cache_runtime, descriptor_ring, sampler_heap, blit_helper,
                 staging_pool},
      present_manager{device, scheduler, swapchain, rtv_descriptors,
                      Settings::values.async_presentation.GetValue()} {
    ID3D12Device* const dev = device.Get();
    texture_cache_runtime.SetBlitHelper(&blit_helper);

    const D3D12_RESOURCE_DESC image_desc =
        Texture2DDesc(swapchain.Width(), swapchain.Height(), Swapchain::FORMAT);
    dev->GetCopyableFootprints(&image_desc, 0, 1, 0, &footprint, nullptr, nullptr, &upload_size);

    blit_ready = CreateBlitPipeline();

    // Present one dark-blue frame straight away: on-console, a blue screen before the guest draws
    // anything proves the device and swapchain work independently of the emulation.
    FrameLease frame = present_manager.AcquireFrame();
    // Force this one-time visible copy through the dedicated path. Guest frames below use the
    // stream ring, so the phase-3a.2 gate exercises both allocation paths.
    StagingBufferRef upload = staging_pool.Request(upload_size, MemoryUsage::Upload, true);
    for (u32 y = 0; y < swapchain.Height(); ++y) {
        auto* row = reinterpret_cast<u32*>(upload.mapped_span.data() +
                                           y * footprint.Footprint.RowPitch);
        std::fill_n(row, swapchain.Width(), 0xFF402010u);
    }
    RecordCopy(upload, *frame);
    staging_pool.FreeDeferred(upload);
    Present(std::move(frame));
    LOG_INFO(Render, "D3D12: presenting through the scheduler (tick {})", scheduler.CurrentTick());
}

RendererD3D12::~RendererD3D12() {
    rasterizer.EndMaterialTrace(false);
    try {
        present_manager.WaitIdle();
        scheduler.DrainForShutdown();
    } catch (const std::exception& e) {
        LOG_ERROR(Render, "{}", e.what());
    }
}

void RendererD3D12::RemoveDeviceForProbe() {
    ComPtr<ID3D12Device5> removable;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&removable)),
                  "device removal probe requires ID3D12Device5");
    TraceGpuOperation("explicit device removal probe");
    removable->RemoveDevice();
    device.ReportDeviceRemoved();
    throw std::runtime_error("injected real D3D12 device removal for recovery gate");
}

void RemoveDeviceForProbe(VideoCore::RendererBase& renderer) {
    if (Settings::values.renderer_backend.GetValue() != Settings::RendererBackend::Direct3D12) {
        throw std::runtime_error("device removal probe requires D3D12 renderer");
    }
    static_cast<RendererD3D12&>(renderer).RemoveDeviceForProbe();
}

void RendererD3D12::Composite(std::span<const Tegra::FramebufferConfig> framebuffers) {
    if (framebuffers.empty()) {
        return;
    }
    last_framebuffer = framebuffers.front();
    if (!present_failed) {
        try {
            if (std::optional<std::string> error = present_manager.TakeError()) {
                throw std::runtime_error(*error);
            }
            const Tegra::FramebufferConfig& framebuffer = framebuffers.front();
            // Frame pacing: waits while every present frame is queued, at most FRAME_COUNT
            // frames ahead of the GPU.
            const auto wait_start = std::chrono::steady_clock::now();
            FrameLease frame = present_manager.AcquireFrame();
            const auto wait_end = std::chrono::steady_clock::now();
            if (blit_ready && CompositeAccelerated(framebuffer, *frame)) {
                // frame.bmp once the guest has had two seconds to settle, then frame_<n>.bmp
                // every ten seconds or so, to follow a game's progress in a headless run.
                constexpr u32 DUMP_FRAME = 120;
                constexpr u32 DUMP_INTERVAL = 600;
                constexpr u32 MAX_DUMPS = 12;
                ++accelerated_frames;
                if (material_trace_presents_remaining != 0) {
                    // This is the exact composited image copied by PresentManager to DXGI,
                    // including crop/scale/overlays. Copy before handing its lease over.
                    frame->Transition(scheduler.CommandList(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                    rasterizer.CaptureMaterialPresent(frame->image.Get(), accelerated_frames);
                }
                if (material_trace_presents_remaining != 0 &&
                    --material_trace_presents_remaining == 0) {
                    rasterizer.EndMaterialTrace();
                }
                const bool diagnostics = frame_diagnostics.load(std::memory_order_relaxed);
                const bool dump = diagnostics && accelerated_frames >= DUMP_FRAME &&
                                  (accelerated_frames - DUMP_FRAME) % DUMP_INTERVAL == 0 &&
                                  (accelerated_frames - DUMP_FRAME) / DUMP_INTERVAL < MAX_DUMPS;
                std::optional<StagingBufferRef> readback;
                if (dump) {
                    readback = RecordFrameReadback(*frame);
                }
                Present(std::move(frame));
                if (readback) {
                    WriteFrameDump(*readback, (accelerated_frames - DUMP_FRAME) / DUMP_INTERVAL);
                }
                // A trace asked for at run time (a developer hotkey) starts with the next frame,
                // also in a session played by hand.
                static bool requested_trace = false;
                if (const u32 requested = trace_requested.exchange(0, std::memory_order_relaxed)) {
                    traced_frame_override.store(accelerated_frames + 2, std::memory_order_relaxed);
                    traced_frame_count.store(requested, std::memory_order_relaxed);
                    requested_trace = true;
                }
                // Every draw of the frame that ends in frame_1.bmp goes to the log, to compare
                // a console run with a PC run draw by draw.
                const u32 override_frame = traced_frame_override.load(std::memory_order_relaxed);
                const u32 traced_frame =
                    override_frame != 0 ? override_frame : DUMP_FRAME + DUMP_INTERVAL;
                // Played by hand (boot.cfg "play=1"): no frame dumps, no draw trace.
                const bool trace = diagnostics || requested_trace;
                if (trace && accelerated_frames == traced_frame - 1) {
                    LOG_INFO(Render, "D3D12: tracing the draws of frame {}", traced_frame);
                    // An explicit trace_frame also writes its render targets (trace\*.bmp).
                    rasterizer.SetDrawTrace(true, override_frame != 0);
                } else if (trace &&
                           accelerated_frames ==
                               traced_frame - 1 +
                                   traced_frame_count.load(std::memory_order_relaxed)) {
                    rasterizer.SetDrawTrace(false);
                    requested_trace = false;
                    LOG_INFO(Render, "D3D12: draw trace of frame {} complete", traced_frame);
                }
                const u32 automatic_capture = material_capture_frame.load(std::memory_order_relaxed);
                const bool capture_requested = material_trace_requested.exchange(false, std::memory_order_acquire);
                if (capture_requested || (automatic_capture != 0 && accelerated_frames == automatic_capture)) {
                    const bool started = rasterizer.BeginMaterialTrace(
                        material_trace_shader.load(std::memory_order_relaxed),
                        material_probe_x.load(std::memory_order_relaxed),
                        material_probe_y.load(std::memory_order_relaxed),
                        {material_snapshot_first.load(std::memory_order_relaxed),
                         material_snapshot_second.load(std::memory_order_relaxed)},
                        {material_snapshot_vertices_first.load(std::memory_order_relaxed),
                         material_snapshot_vertices_second.load(std::memory_order_relaxed)});
                    // MK8 may present while recording a guest frame's preliminary passes.
                    // Include the next interval too; draw/text budgets span both intervals.
                    material_trace_presents_remaining = started ? 2 : 0;
                    LOG_INFO(Render, "D3D12 material frame {}: {}", accelerated_frames + 1,
                             started ? "begin (2 present intervals; up to 3 target snapshots)" : "busy: draw trace active");
                }
            } else if (const bool has_image = ReadGuestLayer(framebuffer);
                       blit_ready && has_image) {
                PrepareGuestImage(static_cast<u32>(crop_width), static_cast<u32>(crop_height));
                // Staging memory lives until the scheduler tick it was requested in. The copy is
                // recorded before RecordComposite allocates descriptors (which may flush), so it
                // always lands in that tick's list.
                const StagingBufferRef upload =
                    staging_pool.Request(guest_upload_size, MemoryUsage::Upload);
                CopyGuestImage(framebuffer, upload.mapped_span.data(),
                               guest_footprint.Footprint.RowPitch);
                RecordUpload(upload);
                // CopyGuestImage already cropped and flipped the image; the blit has always shown
                // its first row at the bottom of the screen.
                RecordComposite(*frame, {.srv = guest_srv,
                                         .width = guest_width,
                                         .height = guest_height,
                                         .crop = {0.0f, 1.0f, 1.0f, 0.0f}});
                Present(std::move(frame));
            } else {
                const StagingBufferRef upload =
                    staging_pool.Request(upload_size, MemoryUsage::Upload);
                ScaleGuestImage(framebuffer, upload.mapped_span.data(),
                                footprint.Footprint.RowPitch);
                RecordCopy(upload, *frame);
                Present(std::move(frame));
            }
            const auto present_end = std::chrono::steady_clock::now();
            using Ms = std::chrono::duration<double, std::milli>;
            RecordPacing(Ms(wait_end - wait_start).count(), Ms(present_end - wait_end).count());
        } catch (const std::exception& e) {
            // Keep the emulation running headless rather than taking the GPU thread down.
            LOG_CRITICAL(Render, "{} - presentation disabled", e.what());
            present_failed = true;
            // A removed device never comes back: let a headless frontend recover now, rather
            // than only once a later draw happens to fail (0.2.74 froze waiting for that).
            if (FAILED(device.Get()->GetDeviceRemovedReason())) {
                VideoCommon::GPUThread::ReportException(e.what());
            }
        }
    }
    gpu.RendererFrameEndNotify();
    // As RendererVulkan::Composite: advances the caches' garbage collection and the fence
    // manager's frame, which otherwise never run.
    try {
        rasterizer.TickFrame();
    } catch (const std::exception& e) {
        LOG_CRITICAL(Render, "{} - rasterizer frame tick failed", e.what());
    }
    render_window.OnFrameDisplayed();
}

void RendererD3D12::Present(FrameLease&& frame) {
    present_manager.Present(std::move(frame));
    staging_pool.TickFrame();
}

std::vector<u8> RendererD3D12::GetAppletCaptureBuffer() {
    return std::vector<u8>(VideoCore::Capture::TiledSize);
}

} // namespace D3D12
