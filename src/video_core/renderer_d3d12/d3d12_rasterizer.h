// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "video_core/control/channel_state_cache.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_buffer_cache.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_fence_manager.h"
#include "video_core/renderer_d3d12/d3d12_indirect_buffer.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
#include "video_core/renderer_d3d12/d3d12_conditional_rendering.h"
#include "video_core/renderer_d3d12/d3d12_query_cache.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/renderer_d3d12/diagnostics/d3d12_material_trace.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"

namespace Tegra {
struct FramebufferConfig;
}

namespace D3D12 {

void SetMaterialGpuProbeEnabled(bool enabled);

class AccelerateDMA final : public Tegra::Engines::AccelerateDMAInterface {
public:
    AccelerateDMA(BufferCache& buffer_cache, TextureCache& texture_cache);
    bool BufferCopy(GPUVAddr src, GPUVAddr dst, u64 amount) override;
    bool BufferClear(GPUVAddr address, u64 amount, u32 value) override;
    bool ImageToBuffer(const Tegra::DMA::ImageCopy&, const Tegra::DMA::ImageOperand&,
                       const Tegra::DMA::BufferOperand&) override;
    bool BufferToImage(const Tegra::DMA::ImageCopy&, const Tegra::DMA::BufferOperand&,
                       const Tegra::DMA::ImageOperand&) override;

private:
    template <bool IS_UPLOAD>
    bool BufferImageCopy(const Tegra::DMA::ImageCopy&, const Tegra::DMA::BufferOperand&,
                         const Tegra::DMA::ImageOperand&);
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
};

class RasterizerD3D12 final : public VideoCore::RasterizerInterface,
                              protected VideoCommon::ChannelSetupCaches<VideoCommon::ChannelInfo> {
public:
    RasterizerD3D12(Tegra::GPU& gpu, Tegra::MaxwellDeviceMemoryManager& device_memory,
                    const Device& device, Scheduler& scheduler, const ShaderCompiler& compiler,
                    BufferCacheRuntime& buffer_runtime, TextureCacheRuntime& texture_runtime,
                    DescriptorRing& descriptor_ring, SamplerHeap& sampler_heap,
                    BlitImageHelper& blit_helper, StagingBufferPool& staging);
    ~RasterizerD3D12() override;

    void Draw(bool, u32) override;
    void DrawIndirect() override;
    void DrawTexture() override;
    void Clear(u32) override;
    void DispatchCompute() override;
    void ResetCounter(VideoCommon::QueryType type) override;
    void Query(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32,
               u32) override;
    void BindGraphicsUniformBuffer(size_t, u32, GPUVAddr, u32) override;
    void DisableGraphicsUniformBuffer(size_t, u32) override;
    void FlushAll() override;
    // Memory materialization only, NOT a complete GPU checkpoint. The coordinator must already
    // have parked CPU/services and drained guest command producers, and call on the GPU thread.
    bool MaterializeCheckpointMemory() override;
    void FlushCheckpointRegion(DAddr address, u64 size) override;
    void FlushRegion(DAddr, u64, VideoCommon::CacheType = VideoCommon::CacheType::All) override;
    bool MustFlushRegion(DAddr, u64,
                         VideoCommon::CacheType = VideoCommon::CacheType::All) override;
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr, u64) override;
    void InvalidateRegion(DAddr, u64,
                          VideoCommon::CacheType = VideoCommon::CacheType::All) override;
    void InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) override;
    void OnCacheInvalidation(PAddr, u64) override;
    bool OnCPUWrite(PAddr, u64) override;
    void InvalidateGPUCache() override;
    void UnmapMemory(DAddr, u64) override;
    void ModifyGPUMemory(size_t, GPUVAddr, u64) override;
    void SignalFence(std::function<void()>&&) override;
    void SyncOperation(std::function<void()>&&) override;
    void SignalSyncPoint(u32) override;
    void SignalReference() override;
    void ReleaseFences(bool = true) override;
    void FlushAndInvalidateRegion(DAddr, u64,
                                  VideoCommon::CacheType = VideoCommon::CacheType::All) override;
    void WaitForIdle() override;
    void FragmentBarrier() override;
    void TiledCacheBarrier() override;
    void FlushCommands() override;
    /// Submits between draws once the list being recorded holds many uploads.
    void FlushIfUploadHeavy();
    void TickFrame() override;
    bool AccelerateConditionalRendering() override;
    bool AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface&,
                               const Tegra::Engines::Fermi2D::Surface&,
                               const Tegra::Engines::Fermi2D::Config&) override;
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override;
    void AccelerateInlineToMemory(GPUVAddr, size_t, std::span<const u8>) override;
    void LoadDiskResources(u64, std::stop_token,
                           const VideoCore::DiskResourceLoadCallback&) override;
    void InitializeChannel(Tegra::Control::ChannelState&) override;
    void BindChannel(Tegra::Control::ChannelState&) override;
    void ReleaseChannel(s32) override;

    [[nodiscard]] bool AnyCommandQueued() const noexcept { return true; }

    /// A guest image the display can sample directly (see RendererD3D12::Composite).
    struct DisplayTexture {
        D3D12_CPU_DESCRIPTOR_HANDLE srv;
        u32 width;
        u32 height;
    };

    /// The texture cache image holding the framebuffer at framebuffer_addr, transitioned for
    /// sampling, or nullopt when the guest wrote it from the CPU (as RasterizerVulkan).
    std::optional<DisplayTexture> AccelerateDisplay(const Tegra::FramebufferConfig& config,
                                                    DAddr framebuffer_addr);

    /// Logs every draw, clear and skipped draw until turned off: one frame of it, taken on two
    /// machines, shows where their output starts to differ.
    /// With dump_targets, each framebuffer is also written next to the log (trace\*.bmp) when
    /// the draws move to another one, and rt0's NaN/Inf count is logged after every draw (slow:
    /// every draw waits for the GPU).
    void SetDrawTrace(bool enabled, bool dump_targets = false);
    /// Explicit bounded capture with target snapshots and an optional native pixel probe.
    /// Refuses an intensive trace; the caller ends it after the selected present intervals.
    bool BeginMaterialTrace(u64 pixel_shader = 0, s32 probe_x = -1, s32 probe_y = -1,
                            std::array<u64, 2> snapshot_shaders = {},
                            std::array<u32, 2> snapshot_vertices = {});
    void EndMaterialTrace(bool intervals_complete = true);
    /// Copies the exact composited image handed to presentation, with capture/frame identity.
    void CaptureMaterialPresent(ID3D12Resource* resource, u64 frame);
    void PollMaterialReadbacks();

private:
    struct DrawParams {
        u32 num_vertices;
        u32 num_instances;
        u32 first_index;
        u32 base_vertex;
        u32 base_instance;
        bool is_indexed;
        u32 runtime_first_vertex; ///< what gl_VertexIndex adds to SV_VertexID
    };
    struct ViewportState {
        u32 yz_flip_mask;
        float width;
        float height;
    };

    using IndirectParams = Tegra::Engines::Maxwell3D::DrawManager::IndirectParams;

    /// Whether the guest culls both faces of the draw's polygons, which draws nothing.
    bool CullsEveryPrimitive() const;

    // The implementation is split by concern: d3d12_rasterizer.cpp (draws, dispatches, memory
    // and sync), d3d12_rasterizer_state.cpp (command-list state), d3d12_rasterizer_indirect.cpp,
    // d3d12_rasterizer_clear.cpp and diagnostics/d3d12_draw_trace.cpp (SetDrawTrace).

    /// Records changed state and then the draw. Command-list state is invalidated on every reset.
    void RecordDraw(const GraphicsPipeline& pipeline, const PipelineBindings& bindings,
                    const Framebuffer& framebuffer, const DrawParams& params,
                    Maxwell::PrimitiveTopology topology);
    /// RecordDraw without the draw (ExecuteIndirect replaces it).
    void BindDrawState(const GraphicsPipeline& pipeline, const PipelineBindings& bindings,
                       const Framebuffer& framebuffer, const DrawParams& params,
                       Maxwell::PrimitiveTopology topology);
    /// With dred=1, traces the guest constant buffers of a draw whose vertex shader dump_shader
    /// names.
    void TraceWatchedVertexShader(u64 vertex_shader);
    void InvalidateCommandListState();
    void InvalidateGraphicsState();
    /// Sets the root constants, tables and integer sampler CBV of a draw that differ from the
    /// ones the command list holds for the bound root signature.
    void SetGraphicsRootArguments(CommandStream* cmd, const PipelineLayout& layout,
                                  const PipelineBindings& bindings, const DrawParams& params);
    void ApplyPendingStateInvalidation();
    /// Indirect draws of topologies rewritten on the CPU: reads the arguments and draws directly.
    void DrawIndirectOnCpu(const IndirectParams& params);
    [[nodiscard]] ViewportState ComputeViewports(
        std::array<D3D12_VIEWPORT, Maxwell::NumViewports>& viewports) const;
    ViewportState UpdateViewports(CommandStream* cmd);
    void UpdateScissors(CommandStream* cmd);
    [[nodiscard]] D3D12_RECT ScissorRect(size_t index) const;
    void QueryFallback(GPUVAddr, VideoCommon::QueryType, VideoCommon::QueryPropertiesFlags, u32);

    // Draw trace (diagnostics/d3d12_draw_trace.cpp).
    /// One trace line (SetDrawTrace): what was recorded or why it was skipped.
    void TraceDraw(std::string_view what, const GraphicsPipeline* pipeline,
                   const Framebuffer* framebuffer, std::span<const VideoCommon::ImageViewId> views,
                   u32 vertices, u32 instances);
    /// Writes the color targets and the depth plane of a traced framebuffer as trace\*.bmp.
    void DumpTargets(std::span<const VideoCommon::ImageId> targets);
    /// Copies level 0 / layer 0 of an image back and waits for it. Returns its NaN/Inf texel
    /// count (nullopt when the format is not decoded); with a name, also logs per-channel stats
    /// and writes trace\<name>.bmp.
    std::optional<u64> DumpTarget(const Image& image, const std::string* bmp_name,
                                  u32 subresource = 0);
    bool QueueMaterialReadback(ID3D12Resource* resource, u32 subresource,
                               DXGI_FORMAT format, const std::string& name, const D3D12_BOX* region = nullptr);
    void ProbeMaterialIntermediates(const GraphicsPipeline& pipeline, const PipelineBindings& bindings,
        const Framebuffer& framebuffer, const DrawParams& params, Maxwell::PrimitiveTopology topology);
    bool material_gpu_probe_done{};
    u64 ExportTargetReadback(StagingBufferRef& readback,
                             const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                             DXGI_FORMAT format, const std::string* bmp_name, bool native);
    /// Logs the NaN/Inf texels of every level and layer of a sampled image (once per trace).
    void DumpTextureNonFinite(const Image& image);
    /// For every traced draw: reads each bound cbuf, vertex and index buffer back from the GPU and
    /// compares it with guest memory. Logs the ones that differ, or all of them (with the index
    /// range against the vertex buffer sizes) when verbose.
    void CheckTracedBuffers(std::span<const TracedBuffer> buffers, const DrawParams& params,
                            bool verbose);
    /// Decodes a sampled image again from guest memory (as the texture cache uploads it) and
    /// compares every level and layer with what the GPU holds. Logs the levels that differ.
    void CheckTracedTexture(const Image& image);
    /// Which texels of the depth target the last traced draw changed: written as
    /// trace\<index>_dz.bmp at a quarter of the size (white cells hold a changed texel), with the
    /// count and bounding box returned for the trace line.
    std::string TraceDepthChanges(const Image& depth);

    Tegra::GPU& gpu;
    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Scheduler& scheduler;
    StagingBufferPool& staging;
    BufferCacheRuntime& buffer_runtime;
    TextureCacheRuntime& texture_runtime;
    DescriptorRing& descriptor_ring;
    SamplerHeap& sampler_heap;
    BlitImageHelper& blit_helper;
    IndirectArgumentRing indirect_args;
    std::vector<u32> indirect_words; ///< CPU part of the ExecuteIndirect records
    GuestDescriptorQueue descriptor_queue;
    BufferCache buffer_cache;
    TextureCache texture_cache;
    PipelineCache pipeline_cache;
    Vulkan::StateTracker state_tracker;
    struct CommandListState {
        bool valid{};
        bool heaps_bound{};
        ID3D12RootSignature* graphics_root{};
        std::array<SIZE_T, VideoCommon::NUM_RT> color_targets{};
        u32 num_color_targets{};
        SIZE_T depth_target{};
        std::array<float, 4> blend_factor{};
        u32 stencil_ref{};
        D3D12_PRIMITIVE_TOPOLOGY topology{D3D_PRIMITIVE_TOPOLOGY_UNDEFINED};
        ViewportState viewport{};
        /// Graphics root arguments set since graphics_root was bound: a root signature change
        /// discards them, so they are cleared with it.
        struct RootArguments {
            bool valid{};
            std::array<u32, PUSH_CONSTANT_WORDS> push_constants{};
            std::array<u32, 16> runtime_words{};
            u64 resource_table{};
            u64 sampler_table{};
            D3D12_GPU_VIRTUAL_ADDRESS integer_samplers{};
        } root_args;
    } command_state;
    bool channel_bound{};
    bool state_invalidation_pending{};
    // QueryCache owns the scheduler callback lifetime and clears every submission callback in its
    // destructor. Keep it after the state captured by our reset callback, so it is destroyed first.
    QueryCache query_cache;
    ConditionalRendering conditional_rendering;
    AccelerateDMA accelerate_dma;
    FenceManager fence_manager;
    bool logged_integer_clear{};
    bool logged_draw_texture{};
    bool logged_first_draw{};
    bool logged_stencil_ref{};
    bool logged_layer_clear{};
    bool logged_first_dispatch{};
    bool logged_indirect_dispatch{};
    bool logged_indirect_draw{};
    bool logged_cpu_indirect_draw{};
    bool logged_byte_count_draw{};
    /// Draws, clears and dispatches since the last FlushCommands submission.
    u32 draw_counter{};
    // Draw trace state (diagnostics/d3d12_draw_trace.cpp).
    bool trace_draws{};
    MaterialTraceBudget material_trace;
    std::unordered_map<std::string, u32> material_trace_records;
    struct MaterialTarget {
        VideoCommon::ImageId id{};
        ComPtr<ID3D12Resource> resource;
        VideoCommon::SubresourceBase base{};
        u64 area{};
    };
    std::array<MaterialTarget, 3> material_targets; // RT0, one auxiliary color, depth.
    SlotVector<Image>* material_images{};
    u32 material_capture_id{};
    u64 material_capture_stamp{};
    void DumpMaterialTargets();
    void DumpMaterialShaderTarget(const GraphicsPipeline& pipeline, const Framebuffer& framebuffer,
                                  u32 vertices);
    void CaptureMaterialSampledInputs(const GraphicsPipeline& pipeline,
                                     std::span<const VideoCommon::ImageViewId> views, u32 vertices,
                                     const Framebuffer& framebuffer);
    MaterialSnapshotSelection material_snapshots;
    struct MaterialReadback {
        StagingBufferRef buffer;
        ComPtr<ID3D12Resource> source;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        DXGI_FORMAT format;
        u64 tick;
        u64 bytes;
        std::string name;
        u32 rows{};
        u64 row_bytes{};
        u32 subresource{};
    };
    std::vector<MaterialReadback> material_readbacks;
    u64 material_readback_bytes{};
    static constexpr u64 MATERIAL_READBACK_LIMIT = 64ULL << 20;
    void ProbeMaterialDraw(const GraphicsPipeline& pipeline, const Framebuffer& framebuffer);
    void DumpMaterialProbe(bool complete);
    struct MaterialProbeRecord {
        u32 draw, target, format, x, y, width, height, subresource;
        u64 address, vertex_shader, pixel_shader;
    };
    static constexpr u32 MATERIAL_PROBE_LIMIT = 2048;
    static constexpr u32 MATERIAL_PROBE_SLOT_BYTES = 32 * 256;
    std::vector<MaterialProbeRecord> material_probe_records;
    StagingBufferRef material_probe_buffer{};
    struct PendingMaterialProbe {
        StagingBufferRef buffer;
        std::vector<MaterialProbeRecord> records;
        u64 tick;
        std::string name;
        u32 omitted;
    };
    std::optional<PendingMaterialProbe> pending_material_probe;
    void ExportMaterialProbe(PendingMaterialProbe& probe);
    struct MaterialBufferReadback {
        StagingBufferRef buffer;
        ComPtr<ID3D12Resource> source;
        std::vector<u8> streamed;
        u64 tick;
        u32 size;
        std::string name;
    };
    std::vector<MaterialBufferReadback> material_buffer_readbacks;
    std::unordered_set<std::string> material_input_pairs;
    u64 material_input_bytes{};
    std::unordered_set<std::string> material_sampled_depth;
    u64 material_sampled_depth_bytes{};
    void CaptureMaterialInputs(const GraphicsPipeline& pipeline,
                               std::span<const TracedBuffer> buffers, const DrawParams& params);
    s32 material_probe_x{-1}, material_probe_y{-1};
    u32 material_probe_omitted{};
    void LogDrawTrace(std::string_view line);
    bool trace_dumps{};
    u32 trace_index{};
    /// Color targets then depth of the framebuffer the traced draws go to (ids, not the
    /// Framebuffer: the cache may delete it before the draws move on).
    std::array<VideoCommon::ImageId, VideoCommon::NUM_RT + 1> traced_targets{};
    SlotVector<Image>* traced_images{};
    std::unordered_set<GPUVAddr> traced_textures;
    /// Last rt0 NaN/Inf count per target, and whether the last traced draw raised it.
    std::unordered_map<GPUVAddr, u64> traced_non_finite;
    bool trace_non_finite_grew{};
    u32 traced_buffers_checked{};
    u32 traced_buffers_differing{};
    /// Last contents of each traced depth target (plane 0, rows packed), for TraceDepthChanges.
    std::unordered_map<GPUVAddr, std::vector<u8>> traced_depth;
    u32 traced_textures_checked{};
    u32 traced_textures_differing{};
};

} // namespace D3D12
