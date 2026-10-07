// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/perf_counters.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_staging_buffer_pool.h"
#include "video_core/renderer_d3d12/d3d12_swapchain.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_present_manager.h"

namespace D3D12 {

/// Presented frame whose draws go to the log (the draw trace), and how many presented frames the
/// trace spans from it (a game may present more than once per frame it draws). Frame 0 keeps the
/// default: the frame that ends in frame_1.bmp. Set before boot (boot.cfg "trace_frame=",
/// "trace_frames=").
void SetTracedFrame(u32 frame, u32 count = 1);

/// Whether presents write frame*.bmp and trace a frame's draws (the default). Off for a session
/// played by hand (boot.cfg "play=1"). Set before boot.
void SetFrameDiagnostics(bool enabled);

/// Shows the progress of the disk shader cache build on screen, if renderer is the D3D12 one
/// (see RendererD3D12::ShowLoadProgress). A LoadDiskResources callback for the frontend.
void ShowLoadProgress(VideoCore::RendererBase& renderer, size_t done, size_t total);

/// Frontend menu over the last guest frame, if renderer is the D3D12 one. GPU thread only
/// (Tegra::GPU::RunOnGpuThread), with the guest paused; presents at once. Upper-case text.
void ShowGameMenu(VideoCore::RendererBase& renderer, std::string_view title,
                  std::span<const std::string> items, size_t selected, std::string_view hint);
/// Removes the menu and presents the guest frame again. GPU thread only.
void HideGameMenu(VideoCore::RendererBase& renderer);

/// Direct3D 12 renderer: a device and swapchain on the UWP CoreWindow, presenting the guest's
/// display framebuffer. The framebuffer is deswizzled on the CPU and drawn to the window with
/// Eden's own blit shaders, translated SPIR-V -> DXIL at runtime (phase 2). If that shader path is
/// unavailable, the phase-1 CPU scale + copy is used instead. Guest GPU work is routed through the
/// D3D12 rasterizer and its phase-3 caches, queries, DMA and fence manager.
///
/// Presentation already runs on the rasterizer infrastructure (phase 3a): the scheduler's command
/// list and ticks, staging memory from the stream buffer, and descriptors from the shader-visible
/// ring and the sampler heap.
///
/// Frames are composited into the present manager's frames, which copies them to the swapchain
/// and presents them on its own thread (d3d12_present_manager.h).
///
/// Implementation files: renderer_d3d12.cpp (setup, Composite, Present), d3d12_present_blit.cpp
/// (GPU path), d3d12_present_cpu.cpp (CPU fallback), d3d12_overlay.cpp, and in diagnostics/
/// d3d12_perf_report.cpp and d3d12_frame_dump.cpp.
class RendererD3D12 final : public VideoCore::RendererBase {
public:
    explicit RendererD3D12(Core::Frontend::EmuWindow& emu_window,
                           Tegra::MaxwellDeviceMemoryManager& device_memory, Tegra::GPU& gpu,
                           std::unique_ptr<Core::Frontend::GraphicsContext> context);
    ~RendererD3D12() override;
    /// Explicit diagnostic gate only, never called by normal gameplay.
    void RemoveDeviceForProbe();

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffers) override;

    std::vector<u8> GetAppletCaptureBuffer() override;

    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return device.AdapterName();
    }

    /// Presents a progress bar and "done/total" on a black screen while the disk shader cache
    /// builds its pipelines. For the boot thread, before the guest runs (nothing else presents
    /// then); updates at most every 33 ms.
    void ShowLoadProgress(size_t done, size_t total, std::string_view phase = {});

    struct GameMenuOverlay {
        std::string title;
        std::vector<std::string> items;
        size_t selected{};
        std::string hint;
    };
    /// Shows (or with nullopt removes) the frontend menu and presents the last guest frame with
    /// it. GPU thread only, while the guest is paused.
    void ShowGameMenu(std::optional<GameMenuOverlay> menu);

private:
    /// The frontend menu, centered over back buffer rtv (in RENDER_TARGET state), if open.
    void DrawGameMenu(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    /// While pipelines build (VideoCore::ShaderNotify), a small panel in the bottom-right corner
    /// of back buffer rtv (in RENDER_TARGET state): three dots that cycle and the shader count.
    void DrawShaderIndicator(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    /// Cached performance panel; samples process CPU and completed queue timestamps at 2 Hz.
    void DrawPerformanceOverlay(ID3D12GraphicsCommandList* cmd,
                                D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    void DrawTraceIndicator(ID3D12GraphicsCommandList* cmd, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    /// Builds the blit root signature and PSO from translated SPIR-V; false (logged) on failure.
    bool CreateBlitPipeline();
    /// (Re)creates the guest texture and its SRV for a new size.
    void PrepareGuestImage(u32 width, u32 height);

    /// Deswizzles the guest layer into `linear`; false if there is nothing to show.
    bool ReadGuestLayer(const Tegra::FramebufferConfig& framebuffer);
    /// Converts the cropped, flipped guest image to RGBA8 at its own size (shader path).
    void CopyGuestImage(const Tegra::FramebufferConfig& framebuffer, u8* dst, u32 row_pitch);
    /// Converts and letterboxes the guest image into the window-sized upload image (CPU path).
    void ScaleGuestImage(const Tegra::FramebufferConfig& framebuffer, u8* dst, u32 row_pitch);

    /// Records the shader-path upload of the CPU-read guest image into guest_texture.
    void RecordUpload(const StagingBufferRef& upload);
    /// Draws a texture into frame, letterboxed, then the overlays; tex_scale_offset maps the
    /// screen (x right, y up in D3D12 clip space) to texture coordinates.
    void RecordBlit(PresentFrame& frame, D3D12_GPU_DESCRIPTOR_HANDLE srv_table,
                    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table,
                    const std::array<float, 4>& tex_scale_offset);
    /// Composites the guest image the GPU rendered (texture cache) into frame, without reading
    /// it back. False when the framebuffer is not a cached image.
    bool CompositeAccelerated(const Tegra::FramebufferConfig& framebuffer, PresentFrame& frame);
    /// Records the CPU-path copy of the upload image into frame.
    void RecordCopy(const StagingBufferRef& upload, PresentFrame& frame);
    /// Submits the recorded frame and hands it to the present manager.
    void Present(FrameLease&& frame);
    /// Records a copy of frame into readback memory (before Present).
    StagingBufferRef RecordFrameReadback(PresentFrame& frame);
    /// Waits for the copy and writes it as frame.bmp next to the log: the one way to see what
    /// the console presented without a capture card.
    void WriteFrameDump(StagingBufferRef& readback, u32 dump_index);

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;
    Device device;
    Swapchain swapchain;
    ShaderCompiler shader_compiler;
    Scheduler scheduler;
    StagingBufferPool staging_pool;
    TransferBufferPool transfer_buffers; ///< GPU scratch buffers of both caches' copies
    BufferCacheRuntime buffer_cache_runtime;
    CpuDescriptorAllocator view_descriptors;    ///< offline CBV/SRV/UAV
    CpuDescriptorAllocator sampler_descriptors; ///< offline samplers
    CpuDescriptorAllocator rtv_descriptors;
    CpuDescriptorAllocator dsv_descriptors;
    TextureCacheRuntime texture_cache_runtime;
    DescriptorRing descriptor_ring;
    SamplerHeap sampler_heap;
    BlitImageHelper blit_helper;
    RasterizerD3D12 rasterizer;
    /// After everything it uses: its present thread stops before they go away.
    PresentManager present_manager;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; ///< window-sized image (CPU path)
    u64 upload_size{};

    // Shader path
    bool blit_ready{};
    ComPtr<ID3D12RootSignature> blit_root_signature;
    ComPtr<ID3D12PipelineState> blit_pipeline;
    D3D12_CPU_DESCRIPTOR_HANDLE linear_sampler{};
    ComPtr<ID3D12Resource> guest_texture;
    D3D12_CPU_DESCRIPTOR_HANDLE guest_srv{};
    u32 guest_width{};
    u32 guest_height{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT guest_footprint{};
    u64 guest_upload_size{};

    std::vector<u8> linear; ///< deswizzled guest framebuffer
    int crop_left{};
    int crop_top{};
    int crop_width{};
    int crop_height{};
    bool present_failed{};
    /// The frontend menu while open, and the last framebuffer composited, to redraw it paused.
    std::optional<GameMenuOverlay> game_menu;
    std::optional<Tegra::FramebufferConfig> last_framebuffer;
    bool logged_accelerated{};
    u32 accelerated_frames{};
    bool logged_fallback{};
    u32 indicator_frames{}; ///< frames the shader indicator has been drawn, for its animation
    std::chrono::steady_clock::time_point last_load_present{};
    struct PerformanceOverlay {
        std::chrono::steady_clock::time_point sample_time{};
        u64 cpu_ticks{};
        u64 gpu_us{};
        u32 frames{};
        bool cpu_valid{};
        double last_frame_ms{};
        double max_frame_ms{};
        std::vector<D3D12_RECT> text;
        D3D12_RECT panel{};
        u64 trace_key{};
        LONG trace_top{};
        std::vector<D3D12_RECT> trace_text;
        D3D12_RECT trace_panel{};
    } performance_overlay;

    /// Frame pacing statistics, logged every PACING_WINDOW frames: the interval between
    /// Composite calls and the time the GPU thread spends blocked on frame pacing and Present.
    struct PacingStats {
        static constexpr u32 WINDOW = 300;
        std::chrono::steady_clock::time_point last_composite{};
        std::array<double, WINDOW> intervals_ms{};
        u32 frames{};
        u32 hitches{}; ///< intervals over 1.5 vblanks
        double total_ms{};
        double max_interval_ms{};
        double max_wait_ms{};
        double max_present_ms{};
    } pacing;
    void RecordPacing(double wait_ms, double present_ms);
    /// Where a slow frame (or a pacing window) spent its time, from VideoCore::Perf.
    void ReportPerf(double interval_ms);
    void ReportPerfWindow(u32 frames, double total_ms);
    VideoCore::Perf::Snapshot perf_frame{};  ///< counters at the previous present
    VideoCore::Perf::Snapshot perf_window{}; ///< counters at the start of the pacing window
    std::chrono::steady_clock::time_point last_hitch_report{};
};

} // namespace D3D12
