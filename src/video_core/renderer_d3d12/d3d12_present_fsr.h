// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "video_core/renderer_d3d12/d3d12_device.h"
#include "video_core/renderer_d3d12/d3d12_fsr_plan.h"

namespace D3D12 {

class CommandStream;
class CpuDescriptorAllocator;
class Scheduler;
class ShaderCompiler;

/// AMD FidelityFX Super Resolution 1 for the present: EASU enlarges the guest image into a
/// screen-sized target, then RCAS sharpens that target into the present frame. Design and
/// sources: docs/xbox/xbox_fsr.md.
///
/// - Eden's FSR shaders, the ones Vulkan uses, translated SPIR-V -> DXIL like every other shader.
///   Always their 32-bit variants: the console reports no native 16-bit shader ops and its driver
///   rejects pixel shaders that require them (docs/xbox/xbox_d3d12_phase4.md, 0.2.24.0).
/// - Pixel shaders, not compute: they need no typed UAV loads, which the console does not have
///   for the formats involved.
/// - One intermediate target (EASU output, 32 bpp as ffx_fsr1.h recommends); RCAS writes straight
///   into the present frame. Vulkan uses two targets and a final blit.
/// - Built on first use: a session that never selects FSR never compiles or allocates it.
class PresentFsr {
public:
    explicit PresentFsr(const Device& device, const ShaderCompiler& shader_compiler,
                        Scheduler& scheduler, CpuDescriptorAllocator& view_descriptors,
                        CpuDescriptorAllocator& rtv_descriptors);
    ~PresentFsr();

    PresentFsr(const PresentFsr&) = delete;
    PresentFsr& operator=(const PresentFsr&) = delete;

    /// Builds the pipelines the first time it is called. False when they cannot be built (logged
    /// once): FSR then stays off for the session and the present keeps the bilinear blit.
    [[nodiscard]] bool EnsurePipelines();

    /// (Re)creates the EASU target at the plan's size. Before any descriptor of the frame is
    /// allocated: its SRV is copied into the ring with the source's. False when it cannot be
    /// created (logged; out of memory, for one): FSR then stays off, as for EnsurePipelines.
    [[nodiscard]] bool PrepareTarget(const Fsr::Plan& plan);

    /// Gives the EASU target's memory back once frames in flight are done with it, while FSR is
    /// off. The next PrepareTarget creates it again.
    void ReleaseTarget();

    /// Offline SRV of the EASU target, valid after PrepareTarget.
    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE TargetSrv() const noexcept {
        return target_srv;
    }

    /// Binds the root signature and the descriptor tables both passes share, then runs EASU from
    /// source_table (sampled with sampler_table) into the target, which ends ready to sample.
    /// The caller has set the descriptor heaps.
    void RecordEasu(CommandStream* cmd, const Fsr::Plan& plan,
                    D3D12_GPU_DESCRIPTOR_HANDLE source_table,
                    D3D12_GPU_DESCRIPTOR_HANDLE sampler_table);

    /// Runs RCAS from target_table into the bound render target, through the bound viewport (the
    /// letterboxed screen rectangle, the plan's size). After RecordEasu, with no other root
    /// signature bound in between.
    void RecordRcas(CommandStream* cmd, const Fsr::Plan& plan,
                    D3D12_GPU_DESCRIPTOR_HANDLE target_table);

private:
    void CreatePipelines();
    void CreateTarget(u32 width, u32 height);

    static constexpr DXGI_FORMAT TARGET_FORMAT = DXGI_FORMAT_R10G10B10A2_UNORM;

    const Device& device;
    const ShaderCompiler& shader_compiler;
    Scheduler& scheduler;
    CpuDescriptorAllocator& view_descriptors;
    CpuDescriptorAllocator& rtv_descriptors;

    enum class State { NotBuilt, Ready, Failed };
    State state{State::NotBuilt};
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> easu_pipeline;
    ComPtr<ID3D12PipelineState> rcas_pipeline;
    bool needs_runtime_data{}; ///< the translated stages read spirv_to_dxil's runtime data CBV

    ComPtr<ID3D12Resource> target; ///< EASU output, kept in PIXEL_SHADER_RESOURCE between frames
    D3D12_CPU_DESCRIPTOR_HANDLE target_srv{};
    D3D12_CPU_DESCRIPTOR_HANDLE target_rtv{};
    u32 target_width{};
    u32 target_height{};
};

} // namespace D3D12
