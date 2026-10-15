// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>

#include "common/bug_tracker.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/diagnostics/material_varyings_ps.h"
#include "video_core/renderer_d3d12/diagnostics/material_samples_ps.h"
#include "video_core/renderer_d3d12/diagnostics/material_identity_gs.h"
#include "video_core/renderer_d3d12/diagnostics/material_linear_gs.h"
#include "video_core/renderer_d3d12/diagnostics/material_linear_varyings_ps.h"
#include "video_core/renderer_d3d12/diagnostics/material_linear_samples_ps.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_maxwell_to_d3d12.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/util.h"

// Draw tracing (RasterizerD3D12::SetDrawTrace): per-draw logs, target dumps and GPU
// readback checks against guest memory. Diagnostics only; draws do not depend on it.

namespace D3D12 {

namespace { std::atomic_bool material_gpu_probe_enabled{false}; }
void SetMaterialGpuProbeEnabled(bool enabled) {
    material_gpu_probe_enabled.store(enabled, std::memory_order_relaxed);
}

namespace {

/// Small unsigned float of the packed formats (R11G11B10: 5-bit exponent, 6 or 5 mantissa bits).
float UnpackSmallFloat(u32 bits, u32 mantissa_bits) {
    const u32 exponent = bits >> mantissa_bits;
    const u32 mantissa = bits & ((1U << mantissa_bits) - 1);
    const float scale = static_cast<float>(1U << mantissa_bits);
    if (exponent == 0) {
        return std::ldexp(static_cast<float>(mantissa) / scale, -14);
    }
    if (exponent == 31) {
        return mantissa != 0 ? std::numeric_limits<float>::quiet_NaN()
                             : std::numeric_limits<float>::infinity();
    }
    return std::ldexp(1.0f + static_cast<float>(mantissa) / scale, static_cast<int>(exponent) - 15);
}

float UnpackHalf(u16 bits) {
    const float magnitude = UnpackSmallFloat(bits & 0x7FFF, 10);
    return (bits & 0x8000) != 0 ? -magnitude : magnitude;
}

/// One texel of a render target dump as RGBA floats; false for formats the dump cannot read.
u32 DumpTexelBytes(DXGI_FORMAT format);

bool ReadTexel(DXGI_FORMAT format, const u8* src, std::array<float, 4>& rgba) {
    // Only this texel's bytes: the last texel of a 1- or 2-byte format ends the readback.
    u32 word = 0;
    std::memcpy(&word, src, std::min<u32>(DumpTexelBytes(format), 4));
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        for (size_t i = 0; i < 4; ++i) {
            rgba[i] = static_cast<float>(src[i]) / 255.0f;
        }
        return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        rgba = {src[2] / 255.0f, src[1] / 255.0f, src[0] / 255.0f, src[3] / 255.0f};
        return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        rgba = {(word & 0x3FF) / 1023.0f, ((word >> 10) & 0x3FF) / 1023.0f,
                ((word >> 20) & 0x3FF) / 1023.0f, (word >> 30) / 3.0f};
        return true;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        rgba = {UnpackSmallFloat(word & 0x7FF, 6), UnpackSmallFloat((word >> 11) & 0x7FF, 6),
                UnpackSmallFloat(word >> 22, 5), 1.0f};
        return true;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (size_t i = 0; i < 4; ++i) {
            u16 half;
            std::memcpy(&half, src + i * 2, 2);
            rgba[i] = UnpackHalf(half);
        }
        return true;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
        rgba = {UnpackHalf(static_cast<u16>(word)), UnpackHalf(static_cast<u16>(word >> 16)), 0.0f,
                1.0f};
        return true;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT: {
        const float value = UnpackHalf(static_cast<u16>(word));
        rgba = {value, value, value, 1.0f};
        return true;
    }
    case DXGI_FORMAT_R16_UNORM: {
        const float value = static_cast<float>(word & 0xFFFF) / 65535.0f;
        rgba = {value, value, value, 1.0f};
        return true;
    }
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        std::memcpy(rgba.data(), src, sizeof(float) * 4);
        return true;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT: {
        const float value = std::bit_cast<float>(word);
        rgba = {value, value, value, 1.0f};
        return true;
    }
    case DXGI_FORMAT_R24G8_TYPELESS: {
        // Depth plane of D24S8: depth in the low 24 bits.
        const float value = static_cast<float>(word & 0xFFFFFF) / 16777215.0f;
        rgba = {value, value, value, 1.0f};
        return true;
    }
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
        rgba = {src[0] / 255.0f, src[0] / 255.0f, src[0] / 255.0f, 1.0f};
        return true;
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
        rgba = {src[0] / 255.0f, src[1] / 255.0f, 0.0f, 1.0f};
        return true;
    default:
        return false;
    }
}

u32 DumpTexelBytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R8G8_UNORM:
        return 2;
    case DXGI_FORMAT_R8_TYPELESS:
    case DXGI_FORMAT_R8_UNORM:
        return 1;
    default:
        return 4;
    }
}
} // Anonymous namespace

void RasterizerD3D12::SetDrawTrace(bool enabled, bool dump_targets) {
    if (enabled && material_trace.Active()) {
        EndMaterialTrace(false);
    }
    if (!enabled && trace_dumps) {
        DumpTargets(traced_targets);
        LOG_INFO(Render, "D3D12 trace buffers: {} of {} bound buffers differ from guest memory",
                 traced_buffers_differing, traced_buffers_checked);
        LOG_INFO(Render, "D3D12 trace textures: {} of {} sampled images differ from guest memory",
                 traced_textures_differing, traced_textures_checked);
    }
    traced_buffers_checked = 0;
    traced_buffers_differing = 0;
    traced_textures_checked = 0;
    traced_textures_differing = 0;
    if (enabled && dump_targets) {
        std::error_code ec;
        const std::filesystem::path directory =
            Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace";
        std::filesystem::remove_all(directory, ec);
        std::filesystem::create_directories(directory, ec);
    }
    trace_draws = enabled;
    trace_dumps = enabled && dump_targets;
    trace_index = 0;
    traced_targets = {};
    traced_textures.clear();
    traced_non_finite.clear();
    traced_depth.clear();
    trace_non_finite_grew = false;
}

bool RasterizerD3D12::BeginMaterialTrace(u64 pixel_shader, s32 probe_x, s32 probe_y,
                                        std::array<u64, 2> snapshot_shaders,
                                        std::array<u32, 2> snapshot_vertices) {
    if (trace_draws || !material_readbacks.empty() || pending_material_probe ||
        !material_buffer_readbacks.empty()) return false;
    EndMaterialTrace(false);
    material_trace.Begin(pixel_shader);
    material_trace_records.clear();
    material_targets = {};
    material_images = nullptr;
    ++material_capture_id;
    material_capture_stamp = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    trace_index = 0;
    material_probe_x = probe_x;
    material_probe_y = probe_y;
    material_probe_omitted = 0;
    material_probe_records.clear();
    material_snapshots.Begin(snapshot_shaders, snapshot_vertices);
    material_gpu_probe_done = false;
    material_input_pairs.clear();
    material_input_bytes = 0;
    material_sampled_depth.clear();
    material_sampled_depth_bytes = 0;
    return true;
}

void RasterizerD3D12::DumpMaterialShaderTarget(const GraphicsPipeline& pipeline,
                                             const Framebuffer& framebuffer, u32 vertices) {
    const u64 shader = pipeline.Key().unique_hashes[5];
    if (!material_snapshots.Consume(shader, vertices)) return;
    // Capture the active MRTs of one exact selected draw, within the existing budget.
    // Keeping all outputs avoids attributing a shading defect from RT0 alone.
    for (u32 target = 0; target < 8; ++target) {
        if (!(pipeline.BoundColorTargets() & (1U << target)) || framebuffer.ColorOnCopy(target))
            continue;
        const Image* image = framebuffer.ColorImage(target);
        if (!image || !image->Handle() || image->Handle()->GetDesc().SampleDesc.Count != 1)
            continue;
        try {
            const auto base = framebuffer.ColorBase(target);
            const u32 subresource = image->Subresource(base.level, base.layer);
            const auto desc = image->Handle()->GetDesc();
            ComPtr<ID3D12Device> capture_device;
            ThrowIfFailed(image->Handle()->GetDevice(IID_PPV_ARGS(&capture_device)), "PS snapshot device");
            UINT64 bytes{};
            capture_device->GetCopyableFootprints(&desc, subresource, 1, 0, nullptr, nullptr, nullptr, &bytes);
            if (!material_snapshots.AllowBytes(bytes)) {
                LOG_WARNING(Render, "D3D12 material PS snapshot RT{} skipped: {} bytes exceeds remaining budget", target, bytes);
                continue;
            }
            const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace";
            std::filesystem::create_directories(directory);
            const std::string name = fmt::format("material_{}_{}_after_draw_{}_ps_{:016x}_rt{}",
                material_capture_stamp, material_capture_id, trace_index - 1, shader, target);
            const auto dumped = DumpTarget(*image, &name, subresource);
            LOG_INFO(Render, "D3D12 material post-draw snapshot: draw={} PS={:016x} RT{} @{:x} mip={} layer={} "
                             "bytes={} result={} (first matching draw, not complete pass)",
                     trace_index - 1, shader, target, image->gpu_addr, base.level, base.layer, bytes,
                     dumped ? name : "unsupported format");
        } catch (const std::exception& e) {
            LOG_WARNING(Render, "D3D12 material PS snapshot RT{} failed: {}", target, e.what());
        }
    }
}

void RasterizerD3D12::EndMaterialTrace(bool intervals_complete) {
    if (!material_trace.Active()) return;
    try {
        DumpMaterialProbe(intervals_complete);
    } catch (const std::exception& e) {
        LOG_WARNING(Render, "D3D12 material probe export failed: {}", e.what());
    }
    if (intervals_complete) {
        try {
            DumpMaterialTargets();
        } catch (const std::exception& e) {
            LOG_WARNING(Render, "D3D12 material target dump failed: {}", e.what());
        }
    }
    LOG_INFO(Render, "D3D12 material complete: matched={} selected={} bytes={} "
                    "omitted_draws={} dropped_lines={} intervals_complete={} "
                    "(target snapshots are diagnostic; not draw replay)",
             material_trace.matched, material_trace.selected, material_trace.bytes,
             material_trace.matched - material_trace.selected, material_trace.dropped_lines,
             intervals_complete ? 1 : 0);
    material_trace.End();
    material_trace_records.clear();
    material_targets = {};
    material_images = nullptr;
}

void RasterizerD3D12::ProbeMaterialDraw(const GraphicsPipeline& pipeline,
                                      const Framebuffer& framebuffer) {
    const bool center = material_probe_x == -2 && material_probe_y == -2;
    if (!center && (material_probe_x < 0 || material_probe_y < 0)) return;
    // Include preliminary MRT passes: RT0 may exist but be inactive while slot 3 is written.
    for (u32 rt = 0; rt < VideoCommon::NUM_RT; ++rt) {
        if (!(pipeline.BoundColorTargets() & (1U << rt)) || framebuffer.ColorOnCopy(rt)) continue;
        Image* image = const_cast<Image*>(framebuffer.ColorImage(rt));
        if (!image || !image->Handle()) continue;
        const auto desc = image->Handle()->GetDesc();
        // CopyTextureRegion requires full-subresource copies for depth/stencil and MSAA.
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
            (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) continue;
        const auto base = framebuffer.ColorBase(rt);
        const u32 mip_width = static_cast<u32>(std::max<u64>(1, desc.Width >> base.level));
        const u32 mip_height = std::max(1U, desc.Height >> base.level);
        const u32 probe_x = center ? mip_width / 2 : static_cast<u32>(material_probe_x);
        const u32 probe_y = center ? mip_height / 2 : static_cast<u32>(material_probe_y);
        // Coordinates refer to the native target, not the window or a scaled screenshot.
        if (probe_x >= mip_width || probe_y >= mip_height) continue;
        if (material_probe_records.size() == MATERIAL_PROBE_LIMIT) {
            ++material_probe_omitted;
            return;
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        ComPtr<ID3D12Device> capture_device;
        if (FAILED(image->Handle()->GetDevice(IID_PPV_ARGS(&capture_device)))) continue;
        const u32 subresource = image->Subresource(base.level, base.layer);
        capture_device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint,
                                             nullptr, nullptr, nullptr);
        std::array<float, 4> rgba{};
        const std::array<u8, 16> zero{};
        if (DumpTexelBytes(footprint.Footprint.Format) != 4 ||
            !ReadTexel(footprint.Footprint.Format, zero.data(), rgba)) continue;
        const u32 x = probe_x > 32 ? probe_x - 32 : 0;
        const u32 y = probe_y > 16 ? probe_y - 16 : 0;
        const u32 width = std::min(64U, mip_width - x);
        const u32 height = std::min(32U, mip_height - y);
        if (!material_probe_buffer.buffer) {
            try {
                material_probe_records.reserve(MATERIAL_PROBE_LIMIT);
                material_probe_buffer = staging.Request(
                    u64{MATERIAL_PROBE_LIMIT} * MATERIAL_PROBE_SLOT_BYTES, MemoryUsage::Download, true);
            } catch (const std::exception& e) {
                material_probe_x = -1;
                LOG_WARNING(Render, "D3D12 material probe allocation failed: {}", e.what());
                return;
            }
            if (material_probe_buffer.mapped_span.size() <
                u64{MATERIAL_PROBE_LIMIT} * MATERIAL_PROBE_SLOT_BYTES) {
                staging.FreeDeferred(material_probe_buffer); // No copies have been recorded.
                material_probe_buffer = {};
                material_probe_x = -1;
                LOG_WARNING(Render, "D3D12 material probe buffer too small");
                return;
            }
        }
        footprint.Offset = material_probe_buffer.offset +
            material_probe_records.size() * MATERIAL_PROBE_SLOT_BYTES;
        footprint.Footprint.Width = width;
        footprint.Footprint.Height = height;
        footprint.Footprint.Depth = 1;
        footprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        const D3D12_TEXTURE_COPY_LOCATION src{
            .pResource = image->Handle(), .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
            .SubresourceIndex = subresource};
        const D3D12_TEXTURE_COPY_LOCATION dst{
            .pResource = material_probe_buffer.buffer, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
            .PlacedFootprint = footprint};
        const D3D12_BOX box{x, y, 0, x + width, y + height, 1};
        const auto previous = image->State();
        image->Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
        scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        image->Transition(previous);
        material_probe_records.push_back({trace_index, rt, static_cast<u32>(footprint.Footprint.Format),
            x, y, width, height, subresource, image->gpu_addr,
            pipeline.Key().unique_hashes[1], pipeline.Key().unique_hashes[5]});
    }
}

void RasterizerD3D12::DumpMaterialProbe(bool complete) {
    if (!material_probe_buffer.buffer) return;
    // Pin until the LAST copy tick completes, not the allocation's first tick.
    pending_material_probe.emplace(PendingMaterialProbe{
        material_probe_buffer, std::move(material_probe_records), scheduler.CurrentTick(),
        complete ? fmt::format("material_{}_{}_probe", material_capture_stamp, material_capture_id)
                 : std::string{}, material_probe_omitted});
    material_probe_buffer = {};
}

void RasterizerD3D12::ExportMaterialProbe(PendingMaterialProbe& probe) {
    auto& readback = probe.buffer;
    const auto& records = probe.records;
    if (probe.name.empty()) return;
    const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace";
    std::filesystem::create_directories(directory);
    const auto& name = probe.name;
    std::ofstream data{directory / (name + ".bin"), std::ios::binary};
    std::ofstream index{directory / (name + ".csv")};
    index << "draw,target,dxgi,x,y,width,height,address,vs,ps,offset,subresource\n";
    u64 offset = 0;
    for (size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        index << fmt::format("{},{},{},{},{},{},{},{:x},{:016x},{:016x},{},{}\n", r.draw, r.target, r.format,
            r.x, r.y, r.width, r.height, r.address, r.vertex_shader, r.pixel_shader, offset, r.subresource);
        for (u32 row = 0; row < r.height; ++row) {
            const auto* bytes = readback.mapped_span.data() + i * MATERIAL_PROBE_SLOT_BYTES + row * 256;
            data.write(reinterpret_cast<const char*>(bytes), r.width * 4);
        }
        offset += u64{r.width} * r.height * 4;
    }
    data.flush();
    index.flush();
    if (!data || !index) {
        LOG_WARNING(Render, "D3D12 material probe write failed: {}", name);
        return;
    }
    LOG_INFO(Render, "D3D12 material probe: {} records, {} omitted, {} bytes, {}",
             records.size(), probe.omitted, offset, name);
}

void RasterizerD3D12::DumpMaterialTargets() {
    if (!material_images) return;
    std::scoped_lock lock{texture_cache.mutex};
    const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace";
    std::filesystem::create_directories(directory);
    u64 budget = 32ULL << 20;
    for (size_t index = 0; index < material_targets.size(); ++index) {
        const auto& target = material_targets[index];
        if (!target.resource) continue;
        // Slot IDs can be recycled. Retaining the resource and checking identity avoids
        // dereferencing a deleted image or exporting a replacement from a reused slot.
        Image* image = nullptr;
        for (const auto [id, candidate] : *material_images) {
            if (id == target.id && candidate->Handle() == target.resource.Get()) {
                image = candidate;
                break;
            }
        }
        if (!image) {
            LOG_WARNING(Render, "D3D12 material capture {} target {} expired", material_capture_id, index);
            continue;
        }
        const u32 subresource = image->Subresource(target.base.level, target.base.layer);
        const auto desc = target.resource->GetDesc();
        UINT64 bytes{};
        ComPtr<ID3D12Device> capture_device;
        ThrowIfFailed(target.resource->GetDevice(IID_PPV_ARGS(&capture_device)), "material capture device");
        capture_device->GetCopyableFootprints(&desc, subresource, 1, 0, nullptr, nullptr, nullptr, &bytes);
        if (bytes == 0 || bytes > budget) {
            LOG_WARNING(Render, "D3D12 material capture {} target {} skipped: {} bytes, {} budget",
                        material_capture_id, index, bytes, budget);
            continue;
        }
        budget -= bytes;
        const std::string name = fmt::format("material_{}_{}_{}_{}_{:x}_m{}_l{}", material_capture_stamp, material_capture_id,
            index == 0 ? "color" : index == 1 ? "aux" : "depth", image->info.format,
            image->gpu_addr, target.base.level, target.base.layer);
        const auto dumped = DumpTarget(*image, &name, subresource);
        LOG_INFO(Render, "D3D12 material capture {} target {}: {} bytes, {}", material_capture_id,
                 index, bytes, dumped ? name : "unsupported dump format/samples");
    }
}

void RasterizerD3D12::LogDrawTrace(std::string_view line) {
    if (material_trace.Active()) {
        // Identical state/bindings are printed once per capture. Full string equality (not a
        // hash alone) prevents collisions from hiding changed formats, samplers or registers.
        const size_t state_at = line.find(" | VS ");
        const size_t samplers_at = line.find(" samplers: ");
        const size_t payload_at = state_at != std::string_view::npos ? state_at : samplers_at;
        if (payload_at != std::string_view::npos) {
            std::string payload{line.substr(payload_at)};
            if (const auto found = material_trace_records.find(payload);
                found != material_trace_records.end()) {
                const auto reference = fmt::format("{} {}_ref=#{}", line.substr(0, payload_at),
                    state_at != std::string_view::npos ? "state" : "samplers", found->second);
                if (material_trace.AllowBytes(reference.size() + 256)) {
                    LOG_INFO(Render, "{}", reference);
                }
                return;
            }
            if (material_trace.AllowBytes(line.size() + 256)) {
                // Stored payload bytes cannot exceed the emitted-text budget; selected draw
                // count also bounds the dictionary to at most two entries per selected draw.
                material_trace_records.emplace(std::move(payload),
                    state_at != std::string_view::npos ? trace_index - 1 : trace_index);
                LOG_INFO(Render, "{}", line);
            }
            return;
        }
    }
    // Include room for the logger's timestamp/source prefix in the fixed text budget.
    if (material_trace.Active() && !material_trace.AllowBytes(line.size() + 256)) return;
    LOG_INFO(Render, "{}", line);
}

void RasterizerD3D12::DumpTextureNonFinite(const Image& image) {
    // Every level and layer (cube faces included): lighting inputs made on the GPU earlier.
    const u32 levels = static_cast<u32>(image.info.resources.levels);
    const u32 layers = image.info.type == VideoCommon::ImageType::e3D
                           ? 1U
                           : static_cast<u32>(image.info.resources.layers);
    u64 total = 0;
    std::string bad;
    for (u32 layer = 0; layer < std::min(layers, 128U); ++layer) {
        for (u32 level = 0; level < levels; ++level) {
            const std::optional<u64> count =
                DumpTarget(image, nullptr, image.Subresource(static_cast<s32>(level),
                                                             static_cast<s32>(layer)));
            if (!count) {
                return; // not decoded (compressed formats)
            }
            total += *count;
            if (*count != 0) {
                bad += fmt::format(" L{}/{}:{}", level, layer, *count);
            }
        }
    }
    if (total != 0) {
        BUG_TRACK_KEY(NonFinite, image.gpu_addr,
                      "{} NaN/Inf texels in texture {} {}x{} @{:x}:{}", total, image.info.format,
                      image.info.size.width, image.info.size.height, image.gpu_addr, bad);
    }
    LOG_INFO(Render, "D3D12 trace texture {} {}x{}x{} L{} @{:x} flags {:x}: non-finite {}{}",
             image.info.format, image.info.size.width, image.info.size.height,
             image.info.type == VideoCommon::ImageType::e3D ? image.info.size.depth : layers,
             levels, image.gpu_addr, static_cast<u32>(image.flags), total, bad);
}
void RasterizerD3D12::CheckTracedBuffers(std::span<const TracedBuffer> buffers,
                                         const DrawParams& params, bool verbose) {
    using Kind = TracedBuffer::Kind;
    constexpr u32 max_checked = 8U << 20;
    // The first 64 words of a cbuf as floats, to follow the matrices a draw is given.
    const auto LogUniformFloats = [this](u32 slot, const u8* data, u32 size) {
        std::string values;
        for (u32 at = 0; at + 4 <= std::min(size, 256U); at += 4) {
            float value;
            std::memcpy(&value, data + at, 4);
            values += fmt::format("{}{:.5g}", at % 64 == 0 ? " |" : " ", value);
        }
        LOG_INFO(Render, "D3D12 trace cbuf #{}: slot {} size {}:{}", trace_index - 1, slot, size,
                 values);
    };
    std::optional<std::pair<u32, u32>> index_range; // vertex numbers, base vertex included
    if (!params.is_indexed && params.num_vertices != 0) {
        index_range.emplace(params.base_vertex, params.base_vertex + params.num_vertices - 1);
    }
    for (const TracedBuffer& traced : buffers) {
        const char* const name = [&] {
            switch (traced.kind) {
            case Kind::Uniform:
                return "cbuf";
            case Kind::NullUniform:
                return "cbuf NULL";
            case Kind::StreamedUniform:
                return "cbuf streamed";
            case Kind::Vertex:
                return "vertex";
            case Kind::Index:
                return "index";
            case Kind::RewrittenIndex:
                return "index rewritten";
            }
            return "?";
        }();
        if (traced.kind == Kind::StreamedUniform && traced.mapped) {
            LogUniformFloats(traced.slot, traced.mapped, traced.size);
        }
        if (!traced.buffer || traced.size == 0) {
            if (!verbose && traced.kind != Kind::NullUniform) {
                continue;
            }
            LOG_INFO(Render,
                     "D3D12 trace buffers #{}: {} {} offset {} size {} (buffer {} bytes)",
                     trace_index - 1, name, traced.slot, traced.offset, traced.size,
                     traced.stride);
            continue;
        }
        const u32 size = std::min(traced.size, max_checked);
        std::vector<u8> guest(size);
        device_memory.ReadBlockUnsafe(traced.device_addr, guest.data(), size);
        if (traced.kind == Kind::Index && params.is_indexed && traced.stride != 0) {
            u32 low = UINT32_MAX;
            u32 high = 0;
            for (u32 i = 0; i < params.num_vertices; ++i) {
                const size_t at = static_cast<size_t>(params.first_index + i) * traced.stride;
                if (at + traced.stride > guest.size()) {
                    break;
                }
                u32 value = 0;
                std::memcpy(&value, guest.data() + at, traced.stride);
                low = std::min(low, value);
                high = std::max(high, value);
            }
            if (low <= high) {
                index_range.emplace(low + params.base_vertex, high + params.base_vertex);
            }
        }
        StagingBufferRef readback = staging.Request(size, MemoryUsage::Download, true);
        if (readback.mapped_span.size() < size) {
            staging.FreeDeferred(readback);
            continue;
        }
        traced.buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
        scheduler.CommandList()->CopyBufferRegion(readback.buffer, readback.offset,
                                                  traced.buffer->Handle(), traced.offset, size);
        scheduler.Finish();
        const u8* const on_gpu = readback.mapped_span.data();
        u32 mismatched = 0;
        u32 first_mismatch = UINT32_MAX;
        u32 gpu_zero = 0;
        u32 guest_zero = 0;
        u32 gpu_non_finite = 0;
        u32 guest_non_finite = 0;
        for (u32 at = 0; at + 4 <= size; at += 4) {
            u32 a;
            u32 b;
            std::memcpy(&a, on_gpu + at, 4);
            std::memcpy(&b, guest.data() + at, 4);
            if (a != b) {
                ++mismatched;
                first_mismatch = std::min(first_mismatch, at);
            }
            gpu_zero += a == 0 ? 1 : 0;
            guest_zero += b == 0 ? 1 : 0;
            gpu_non_finite += (a & 0x7f800000U) == 0x7f800000U ? 1 : 0;
            guest_non_finite += (b & 0x7f800000U) == 0x7f800000U ? 1 : 0;
        }
        staging.FreeDeferred(readback);
        if (traced.kind == Kind::Uniform) {
            LogUniformFloats(traced.slot, guest.data(), size);
        }
        ++traced_buffers_checked;
        if (mismatched != 0) {
            ++traced_buffers_differing;
        } else if (!verbose && traced.kind != Kind::Vertex) {
            continue;
        }
        std::string line = fmt::format(
            "D3D12 trace buffers #{}: {} {} @{:x}+{} size {} stride {}: {} of {} words differ "
            "from guest",
            trace_index - 1, name, traced.slot, traced.device_addr - traced.offset, traced.offset,
            traced.size, traced.stride, mismatched, size / 4);
        if (mismatched != 0) {
            u32 a = 0;
            u32 b = 0;
            std::memcpy(&a, on_gpu + first_mismatch, 4);
            std::memcpy(&b, guest.data() + first_mismatch, 4);
            line += fmt::format(" (first at {}: gpu {:08x} guest {:08x})", first_mismatch, a, b);
        }
        line += fmt::format(", zero words gpu {} guest {}, NaN/Inf words gpu {} guest {}", gpu_zero,
                            guest_zero, gpu_non_finite, guest_non_finite);
        if (traced.kind == Kind::Vertex && index_range && traced.stride != 0) {
            line += fmt::format(", holds {} vertices, draw reads {}..{}",
                                traced.size / traced.stride, index_range->first,
                                index_range->second);
            if (traced.stride % 4 == 0 && traced.stride <= 64) {
                // Range of each float column over the vertices the draw reads.
                const u32 columns = traced.stride / 4;
                std::array<float, 16> low;
                std::array<float, 16> high;
                low.fill(std::numeric_limits<float>::infinity());
                high.fill(-std::numeric_limits<float>::infinity());
                for (u32 vertex = index_range->first; vertex <= index_range->second; ++vertex) {
                    const size_t at = static_cast<size_t>(vertex) * traced.stride;
                    if (at + traced.stride > guest.size()) {
                        break;
                    }
                    for (u32 column = 0; column < columns; ++column) {
                        float value;
                        std::memcpy(&value, guest.data() + at + column * 4, 4);
                        low[column] = std::min(low[column], value);
                        high[column] = std::max(high[column], value);
                    }
                }
                for (u32 column = 0; column < columns; ++column) {
                    line += fmt::format(" | c{} {:.6g}..{:.6g}", column, low[column], high[column]);
                }
            }
        }
        LOG_INFO(Render, "{}", line);
    }
}

void RasterizerD3D12::DumpTargets(std::span<const VideoCommon::ImageId> targets) {
    for (size_t index = 0; index < targets.size(); ++index) {
        // The last slot is the depth buffer (its depth plane).
        const bool is_depth = index == VideoCommon::NUM_RT;
        if (targets[index] == VideoCommon::ImageId{} || !traced_images) {
            continue;
        }
        const Image* const target = &(*traced_images)[targets[index]];
        if (!target->Handle()) {
            continue; // deleted since
        }
        const std::string name = fmt::format(
            "{:03}_{}_{}_{}x{}_{:x}", trace_index == 0 ? 0 : trace_index - 1,
            is_depth ? std::string("ds") : fmt::format("rt{}", index), target->info.format,
            target->info.size.width, target->info.size.height, target->gpu_addr);
        DumpTarget(*target, &name);
    }
}

void RasterizerD3D12::CheckTracedTexture(const Image& target) {
    using VideoCommon::ImageFlagBits;
    using VideoCommon::ImageType;
    const VideoCommon::ImageInfo& info = target.info;
    const FormatInfo& transfer = target.TransferFormat();
    // What the GPU wrote, or the CPU wrote since the upload, differs from the upload for good.
    if (!target.Handle() || !transfer.supported || transfer.stencil_srv != DXGI_FORMAT_UNKNOWN ||
        info.num_samples > 1 || info.type == ImageType::e3D || info.type == ImageType::Linear ||
        info.type == ImageType::Buffer ||
        True(target.flags & (ImageFlagBits::GpuModified | ImageFlagBits::CpuModified))) {
        return;
    }
    // The texture cache's own upload path: unswizzle, then decode (ASTC) when converted.
    std::vector<u8> swizzled(target.guest_size_bytes);
    gpu_memory->ReadBlockUnsafe(target.gpu_addr, swizzled.data(), swizzled.size());
    std::vector<u8> unswizzled(target.unswizzled_size_bytes);
    auto copies = VideoCommon::UnswizzleImage(*gpu_memory, target.gpu_addr, info, swizzled,
                                              unswizzled);
    std::vector<u8> converted;
    std::span<const u8> expected = unswizzled;
    if (transfer.converted) {
        converted.resize(target.converted_size_bytes);
        VideoCommon::ConvertImage(unswizzled, info, converted, copies);
        expected = converted;
    }
    const VideoCore::Surface::PixelFormat copy_format = transfer.copy_format;
    const u32 block_w = VideoCore::Surface::DefaultBlockWidth(copy_format);
    const u32 block_h = VideoCore::Surface::DefaultBlockHeight(copy_format);
    const u32 block_bytes = VideoCore::Surface::BytesPerBlock(copy_format);
    const bool rgba8 = copy_format == VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;

    Image& image = const_cast<Image&>(target);
    ++traced_textures_checked;
    std::string detail;
    u32 differing_subresources = 0;
    for (const VideoCommon::BufferImageCopy& copy : copies) {
        const u32 level = static_cast<u32>(copy.image_subresource.base_level);
        const u32 row_length =
            copy.buffer_row_length != 0 ? copy.buffer_row_length : copy.image_extent.width;
        const u32 image_height =
            copy.buffer_image_height != 0 ? copy.buffer_image_height : copy.image_extent.height;
        const u32 blocks_x = std::max(1U, Common::DivCeil(copy.image_extent.width, block_w));
        const u32 rows = std::max(1U, Common::DivCeil(copy.image_extent.height, block_h));
        const u64 row_bytes =
            static_cast<u64>(std::max(1U, Common::DivCeil(row_length, block_w))) * block_bytes;
        const u64 slice = row_bytes * std::max(rows, Common::DivCeil(image_height, block_h));
        const u32 layers = static_cast<u32>(std::max(1, copy.image_subresource.num_layers));
        for (u32 layer = 0; layer < layers; ++layer) {
            const u32 subresource =
                image.Subresource(static_cast<s32>(level), static_cast<s32>(layer));
            const D3D12_RESOURCE_DESC desc = image.Handle()->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT num_rows = 0;
            UINT64 total_bytes = 0;
            ID3D12Device* device = nullptr;
            image.Handle()->GetDevice(IID_PPV_ARGS(&device));
            device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, &num_rows,
                                          nullptr, &total_bytes);
            device->Release();
            if (total_bytes == 0 || total_bytes == UINT64_MAX || num_rows < rows) {
                return;
            }
            StagingBufferRef readback =
                staging.Request(static_cast<size_t>(total_bytes), MemoryUsage::Download, true);
            const D3D12_RESOURCE_STATES previous = image.State();
            image.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
            const D3D12_TEXTURE_COPY_LOCATION src{
                .pResource = image.Handle(),
                .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                .SubresourceIndex = subresource,
            };
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = footprint;
            placed.Offset = readback.offset;
            const D3D12_TEXTURE_COPY_LOCATION dst{
                .pResource = readback.buffer,
                .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                .PlacedFootprint = placed,
            };
            scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            image.Transition(previous);
            scheduler.Finish();

            const u64 base = copy.buffer_offset + layer * slice;
            u64 differing = 0;
            u64 gpu_clear = 0;
            u64 guest_clear = 0;
            std::string first;
            for (u32 y = 0; y < rows; ++y) {
                const u8* const on_gpu = readback.mapped_span.data() +
                                         static_cast<size_t>(y) * footprint.Footprint.RowPitch;
                const u64 row_at = base + y * row_bytes;
                if (row_at + static_cast<u64>(blocks_x) * block_bytes > expected.size()) {
                    break;
                }
                const u8* const guest = expected.data() + row_at;
                for (u32 x = 0; x < blocks_x; ++x) {
                    const u8* const a = on_gpu + static_cast<size_t>(x) * block_bytes;
                    const u8* const b = guest + static_cast<size_t>(x) * block_bytes;
                    if (rgba8) {
                        gpu_clear += a[3] == 0 ? 1 : 0;
                        guest_clear += b[3] == 0 ? 1 : 0;
                    }
                    if (std::memcmp(a, b, block_bytes) == 0) {
                        continue;
                    }
                    if (differing++ == 0) {
                        u32 gpu_word = 0;
                        u32 guest_word = 0;
                        std::memcpy(&gpu_word, a, std::min(4U, block_bytes));
                        std::memcpy(&guest_word, b, std::min(4U, block_bytes));
                        first = fmt::format("first at {},{}: gpu {:08x} guest {:08x}", x, y,
                                            gpu_word, guest_word);
                    }
                }
            }
            staging.FreeDeferred(readback);
            if (differing == 0) {
                continue;
            }
            if (differing_subresources++ < 8) {
                detail += fmt::format(" | L{}/{}: {} of {} blocks differ ({})", level, layer,
                                      differing, static_cast<u64>(blocks_x) * rows, first);
                if (rgba8) {
                    detail += fmt::format(", alpha 0 gpu {} guest {}", gpu_clear, guest_clear);
                }
            }
        }
    }
    if (differing_subresources != 0) {
        ++traced_textures_differing;
    }
    {
        // The host layout the driver chose, to compare arrays that sample wrong with ones that do not.
        const D3D12_RESOURCE_DESC desc = image.Handle()->GetDesc();
        ID3D12Device* device = nullptr;
        image.Handle()->GetDevice(IID_PPV_ARGS(&device));
        const D3D12_RESOURCE_ALLOCATION_INFO allocation = device->GetResourceAllocationInfo(0, 1, &desc);
        u64 layer_bytes = 0;
        device->GetCopyableFootprints(&desc, 0, desc.MipLevels, 0, nullptr, nullptr, nullptr,
                                      &layer_bytes);
        device->Release();
        detail += fmt::format(" | host format {} {}x{}x{} mips {} flags 0x{:x}: allocation {} bytes "
                              "align {}, one layer's mip chain {} bytes",
                              static_cast<u32>(desc.Format), desc.Width, desc.Height,
                              desc.DepthOrArraySize, desc.MipLevels, static_cast<u32>(desc.Flags),
                              allocation.SizeInBytes, allocation.Alignment, layer_bytes);
    }
    LOG_INFO(Render, "D3D12 trace texture check {} {}x{}x{} L{} @{:x}{}: {}{}", info.format,
             info.size.width, info.size.height, info.resources.layers, info.resources.levels,
             target.gpu_addr, transfer.converted ? " (decoded on the CPU)" : "",
             differing_subresources == 0
                 ? std::string("matches guest memory")
                 : fmt::format("{} subresources differ", differing_subresources),
             detail);
}

std::string RasterizerD3D12::TraceDepthChanges(const Image& target) {
    ID3D12Resource* const resource = target.Handle();
    if (!resource) {
        return {};
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.SampleDesc.Count > 1) {
        return {};
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT num_rows = 0;
    UINT64 row_size = 0;
    UINT64 total_bytes = 0;
    ID3D12Device* device = nullptr;
    resource->GetDevice(IID_PPV_ARGS(&device));
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &num_rows, &row_size, &total_bytes);
    device->Release();
    const u32 width = footprint.Footprint.Width;
    if (total_bytes == 0 || total_bytes == UINT64_MAX || width == 0 || row_size < width) {
        return {};
    }
    StagingBufferRef readback =
        staging.Request(static_cast<size_t>(total_bytes), MemoryUsage::Download, true);
    Image& image = const_cast<Image&>(target);
    const D3D12_RESOURCE_STATES previous = image.State();
    image.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_TEXTURE_COPY_LOCATION src{
        .pResource = resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = footprint;
    placed.Offset = readback.offset;
    const D3D12_TEXTURE_COPY_LOCATION dst{
        .pResource = readback.buffer,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = placed,
    };
    scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    image.Transition(previous);
    scheduler.Finish();

    const size_t row_bytes = static_cast<size_t>(row_size);
    const u32 height = num_rows;
    std::vector<u8> now(row_bytes * height);
    for (u32 y = 0; y < height; ++y) {
        std::memcpy(now.data() + y * row_bytes,
                    readback.mapped_span.data() + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
                    row_bytes);
    }
    staging.FreeDeferred(readback);

    std::vector<u8>& before = traced_depth[target.gpu_addr];
    if (before.size() != now.size()) {
        before = std::move(now);
        return " | depth first seen";
    }
    const size_t texel = row_bytes / width;
    constexpr u32 SCALE = 4;
    const u32 mask_width = Common::DivCeil(width, SCALE);
    const u32 mask_height = Common::DivCeil(height, SCALE);
    std::vector<u8> mask(static_cast<size_t>(mask_width) * mask_height);
    u64 changed = 0;
    u32 min_x = UINT32_MAX;
    u32 min_y = UINT32_MAX;
    u32 max_x = 0;
    u32 max_y = 0;
    // Depth values of the changed texels, before and after: 32-bit float depth, or the 24-bit
    // unorm depth of D24S8 (the low 24 bits).
    const bool float_depth = desc.Format == DXGI_FORMAT_R32_TYPELESS ||
                             desc.Format == DXGI_FORMAT_D32_FLOAT ||
                             desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS ||
                             desc.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    const auto depth_value = [&](const u8* p) {
        u32 raw{};
        std::memcpy(&raw, p, sizeof(raw));
        return float_depth ? std::bit_cast<float>(raw)
                           : static_cast<float>(raw & 0xFFFFFF) / 16777215.0f;
    };
    float before_min = std::numeric_limits<float>::max();
    float before_max = std::numeric_limits<float>::lowest();
    float after_min = before_min;
    float after_max = before_max;
    for (u32 y = 0; y < height; ++y) {
        const u8* const a = before.data() + y * row_bytes;
        const u8* const b = now.data() + y * row_bytes;
        if (std::memcmp(a, b, row_bytes) == 0) {
            continue;
        }
        for (u32 x = 0; x < width; ++x) {
            if (std::memcmp(a + x * texel, b + x * texel, texel) == 0) {
                continue;
            }
            if (texel >= 4) {
                const float old_depth = depth_value(a + x * texel);
                const float new_depth = depth_value(b + x * texel);
                before_min = std::min(before_min, old_depth);
                before_max = std::max(before_max, old_depth);
                after_min = std::min(after_min, new_depth);
                after_max = std::max(after_max, new_depth);
            }
            ++changed;
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
            mask[static_cast<size_t>(y / SCALE) * mask_width + x / SCALE] = 1;
        }
    }
    before.swap(now);
    if (changed == 0) {
        return " | depth unchanged";
    }
    // Bottom-up 32-bit BMP: white where the draw changed depth, black elsewhere.
    const u32 bmp_row = mask_width * 4;
    std::vector<u8> file(54 + static_cast<size_t>(bmp_row) * mask_height);
    const auto put32 = [&file](size_t offset, u32 value) { std::memcpy(&file[offset], &value, 4); };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, static_cast<u32>(file.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, mask_width);
    put32(22, mask_height);
    file[26] = 1;
    file[28] = 32;
    put32(34, bmp_row * mask_height);
    for (u32 y = 0; y < mask_height; ++y) {
        u8* const out = file.data() + 54 + static_cast<size_t>(mask_height - 1 - y) * bmp_row;
        for (u32 x = 0; x < mask_width; ++x) {
            const u8 value = mask[static_cast<size_t>(y) * mask_width + x] ? 255 : 0;
            out[x * 4 + 0] = value;
            out[x * 4 + 1] = value;
            out[x * 4 + 2] = value;
            out[x * 4 + 3] = 255;
        }
    }
    const std::filesystem::path path = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
                                       "trace" / fmt::format("{:03}_dz.bmp", trace_index - 1);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(file.data()),
                 static_cast<std::streamsize>(file.size()));
    std::string values;
    if (texel >= 4) {
        values = fmt::format(" from {:.6g}..{:.6g} to {:.6g}..{:.6g} (format {})", before_min,
                             before_max, after_min, after_max, static_cast<u32>(desc.Format));
    }
    return fmt::format(" | depth changed {} texels in {},{}..{},{}{}", changed, min_x, min_y,
                       max_x, max_y, values);
}

std::optional<u64> RasterizerD3D12::DumpTarget(const Image& target, const std::string* bmp_name,
                                               u32 subresource) {
    ID3D12Resource* const resource = target.Handle();
    if (!resource) {
        return std::nullopt;
    }
    const D3D12_RESOURCE_DESC desc = resource->GetDesc();
    if (desc.SampleDesc.Count > 1) {
        return std::nullopt; // MSAA cannot be copied to a buffer
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    u64 total_bytes = 0;
    ID3D12Device* device = nullptr;
    resource->GetDevice(IID_PPV_ARGS(&device));
    device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr,
                                  &total_bytes);
    device->Release();
    // The depth plane of D24S8 copies as R32 with the depth in its low 24 bits. R16_TYPELESS
    // holds halves or, for D16 and R16 images, normalized integers.
    const bool unorm16 = target.info.format == VideoCore::Surface::PixelFormat::D16_UNORM ||
                         target.info.format == VideoCore::Surface::PixelFormat::R16_UNORM;
    DXGI_FORMAT format = footprint.Footprint.Format;
    if (desc.Format == DXGI_FORMAT_R24G8_TYPELESS) {
        format = DXGI_FORMAT_R24G8_TYPELESS;
    } else if (unorm16) {
        format = DXGI_FORMAT_R16_UNORM;
    }
    std::array<float, 4> probe{};
    const std::array<u8, 8> zeros{};
    if (!ReadTexel(format, zeros.data(), probe)) {
        if (bmp_name) {
            LOG_INFO(Render, "D3D12 trace dump {}: DXGI format {} not decoded", *bmp_name,
                     static_cast<u32>(format));
        }
        return std::nullopt;
    }

    if (bmp_name && bmp_name->starts_with("material_")) {
        Image& image = const_cast<Image&>(target);
        const auto previous = image.State();
        image.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
        SCOPE_EXIT { image.Transition(previous); };
        const bool queued = QueueMaterialReadback(resource, subresource, format, *bmp_name);
        return queued ? std::optional<u64>{0} : std::nullopt;
    }

    // Diagnostics only: the copy needs the image in COPY_SOURCE; its next use transitions it
    // back. Waiting here keeps one frame of dumps from holding a readback each.
    Image& image = const_cast<Image&>(target);
    const D3D12_RESOURCE_STATES previous = image.State();
    const u64 needed = static_cast<u64>(footprint.Footprint.RowPitch) *
                           (footprint.Footprint.Height - 1) +
                       static_cast<u64>(footprint.Footprint.Width) * DumpTexelBytes(format);
    if (total_bytes == 0 || total_bytes == UINT64_MAX || needed > total_bytes) {
        LOG_WARNING(Render, "D3D12 trace: cannot dump subresource {} of {} (footprint {} bytes)",
                    subresource, target.info.format, total_bytes);
        return std::nullopt;
    }
    StagingBufferRef readback = staging.Request(total_bytes, MemoryUsage::Download, true);
    if (readback.mapped_span.size() < needed) {
        LOG_WARNING(Render, "D3D12 trace: readback of {} bytes is smaller than {}",
                    readback.mapped_span.size(), needed);
        staging.FreeDeferred(readback);
        return std::nullopt;
    }
    image.Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
    const D3D12_TEXTURE_COPY_LOCATION src{
        .pResource = resource,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = subresource,
    };
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = footprint;
    placed.Offset = readback.offset;
    const D3D12_TEXTURE_COPY_LOCATION dst{
        .pResource = readback.buffer,
        .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
        .PlacedFootprint = placed,
    };
    scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    image.Transition(previous);
    scheduler.Finish();

    SCOPE_EXIT { staging.FreeDeferred(readback); };
    return ExportTargetReadback(readback, footprint, format, bmp_name, false);
}

u64 RasterizerD3D12::ExportTargetReadback(StagingBufferRef& readback,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint, DXGI_FORMAT format,
    const std::string* bmp_name, bool native) {
    const u32 width = static_cast<u32>(footprint.Footprint.Width);
    const u32 height = footprint.Footprint.Height;
    const u32 texel_bytes = DumpTexelBytes(format);
    const u32 row_bytes = width * 4;
    std::vector<u8> file;
    if (bmp_name) {
        file.resize(54 + static_cast<size_t>(row_bytes) * height);
        const auto put32 = [&file](size_t offset, u32 value) {
            std::memcpy(&file[offset], &value, 4);
        };
        file[0] = 'B';
        file[1] = 'M';
        put32(2, static_cast<u32>(file.size()));
        put32(10, 54);
        put32(14, 40);
        put32(18, width);
        put32(22, height);
        file[26] = 1;
        file[28] = 32;
        put32(34, row_bytes * height);
    }
    std::array<float, 4> min_value;
    std::array<float, 4> max_value;
    min_value.fill(std::numeric_limits<float>::infinity());
    max_value.fill(-std::numeric_limits<float>::infinity());
    std::array<double, 4> sum{};
    u64 non_finite = 0;
    for (u32 y = 0; y < height; ++y) {
        const u8* const row = readback.mapped_span.data() +
                              static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        u8* const out = bmp_name ? file.data() + 54 + static_cast<size_t>(height - 1 - y) * row_bytes
                                 : nullptr;
        for (u32 x = 0; x < width; ++x) {
            std::array<float, 4> rgba{};
            ReadTexel(format, row + static_cast<size_t>(x) * texel_bytes, rgba);
            bool finite = true;
            for (size_t c = 0; c < 4; ++c) {
                if (!std::isfinite(rgba[c])) {
                    finite = false;
                    rgba[c] = 1.0f;
                }
                min_value[c] = std::min(min_value[c], rgba[c]);
                max_value[c] = std::max(max_value[c], rgba[c]);
                sum[c] += rgba[c];
            }
            non_finite += finite ? 0 : 1;
            if (out) {
                // As stored, clamped to [0, 1]; NaN/Inf texels in magenta.
                const auto to_byte = [](float value) {
                    return static_cast<u8>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                };
                out[x * 4 + 0] = finite ? to_byte(rgba[2]) : 255;
                out[x * 4 + 1] = finite ? to_byte(rgba[1]) : 0;
                out[x * 4 + 2] = finite ? to_byte(rgba[0]) : 255;
                out[x * 4 + 3] = 255;
            }
        }
    }
    if (native && bmp_name) {
        const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace";
        std::filesystem::create_directories(directory);
        std::ofstream raw(directory / (*bmp_name + ".raw"), std::ios::binary);
        for (u32 y = 0; y < height; ++y) {
            raw.write(reinterpret_cast<const char*>(readback.mapped_span.data() +
                size_t{y} * footprint.Footprint.RowPitch), size_t{width} * texel_bytes);
        }
        std::ofstream info(directory / (*bmp_name + ".json"));
        info << fmt::format("{{\"width\":{},\"height\":{},\"dxgi_format\":{},"
                            "\"row_bytes\":{},\"gpu_row_pitch\":{},\"origin\":\"top-left\","
                            "\"native_bytes\":true,\"preview_clamped\":true}}\n",
            width, height, static_cast<u32>(format), width * texel_bytes,
            footprint.Footprint.RowPitch);
        raw.flush();
        info.flush();
        if (!raw || !info) throw std::runtime_error("material native export failed");
    }
    if (bmp_name) {
        const double count = static_cast<double>(width) * height;
        LOG_INFO(Render,
                 "D3D12 trace dump {}: min {:.4g} {:.4g} {:.4g} {:.4g} max {:.4g} {:.4g} {:.4g} "
                 "{:.4g} mean {:.4g} {:.4g} {:.4g} {:.4g} non-finite texels {}",
                 *bmp_name, min_value[0], min_value[1], min_value[2], min_value[3], max_value[0],
                 max_value[1], max_value[2], max_value[3], sum[0] / count, sum[1] / count,
                 sum[2] / count, sum[3] / count, non_finite);
        const std::filesystem::path path =
            Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace" / (*bmp_name + ".bmp");
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(file.data()),
                     static_cast<std::streamsize>(file.size()));
        if (!stream) {
            LOG_WARNING(Render, "D3D12: could not write target dump {}", path.string());
        }
    }
    return non_finite;
}

bool RasterizerD3D12::QueueMaterialReadback(ID3D12Resource* resource, u32 subresource,
                                          DXGI_FORMAT format, const std::string& name, const D3D12_BOX* region) {
    auto desc = resource->GetDesc();
    if ((desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
         desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D) || desc.SampleDesc.Count != 1)
        return false;
    ComPtr<ID3D12Device> capture_device;
    ThrowIfFailed(resource->GetDevice(IID_PPV_ARGS(&capture_device)), "capture device");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes{};
    UINT rows{};
    UINT64 row_bytes{};
    if (region) {
        // Probe ROIs allow float4 and packed float RGB, never compressed blocks.
        if (subresource || !MaterialProbeRegionBytes(static_cast<u32>(format)) ||
            desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
            region->left >= region->right || region->top >= region->bottom ||
            region->right > desc.Width || region->bottom > desc.Height ||
            region->front != 0 || region->back != 1) return false;
        desc.Width = region->right - region->left;
        desc.Height = region->bottom - region->top;
    }
    capture_device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint,
                                         &rows, &row_bytes, &bytes);
    const u32 texel_bytes = DumpTexelBytes(format);
    std::array<float, 4> decoded{};
    const std::array<u8, 16> zero{};
    if (!ReadTexel(format, zero.data(), decoded) && !MaterialBlockBytes(static_cast<u32>(format)))
        return false;
    // Reserve 16 MiB for the two 1080p presentation copies on Series. Intermediate
    // diagnostics may be omitted; they must not crowd out the visible reference image.
    u64 present_bytes = 0;
    for (const auto& pending : material_readbacks)
        if (pending.name.find("_present_") != std::string::npos) present_bytes += pending.bytes;
    const u64 reserve = (16ULL << 20) - std::min<u64>(16ULL << 20, present_bytes);
    const u64 limit = name.find("_present_") != std::string::npos
        ? MATERIAL_READBACK_LIMIT : MATERIAL_READBACK_LIMIT - reserve;
    if (!bytes || !texel_bytes || material_readback_bytes > limit ||
        bytes > limit - material_readback_bytes ||
        footprint.Footprint.Depth != 1 || !rows || row_bytes > footprint.Footprint.RowPitch ||
        u64{rows - 1} * footprint.Footprint.RowPitch + row_bytes > bytes) {
        LOG_INFO(Render, "D3D12 material deferred copy omitted: {} bytes={} pending={}",
                    name, bytes, material_readback_bytes);
        return false;
    }
    // Reserve metadata before recording a GPU copy; no throwing container growth afterwards.
    material_readbacks.reserve(material_readbacks.size() + 1);
    MaterialReadback pending{{}, resource, footprint, format, scheduler.CurrentTick(), bytes, name};
    pending.rows = rows;
    pending.row_bytes = row_bytes;
    pending.subresource = subresource;
    pending.buffer = staging.Request(bytes, MemoryUsage::Download, true);
    SCOPE_EXIT { if (pending.buffer.buffer) staging.FreeDeferred(pending.buffer); };
    if (pending.buffer.mapped_span.size() < bytes) {
        return false;
    }
    auto placed = footprint;
    placed.Offset = pending.buffer.offset;
    const D3D12_TEXTURE_COPY_LOCATION src{resource,
        D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = subresource}};
    const D3D12_TEXTURE_COPY_LOCATION dst{pending.buffer.buffer,
        D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = placed}};
    scheduler.CommandList()->CopyTextureRegion(&dst, 0, 0, 0, &src, region);
    material_readback_bytes += bytes;
    material_readbacks.push_back(std::move(pending));
    pending.buffer = {};
    LOG_INFO(Render, "D3D12 material deferred copy: {} subresource={} dxgi={} bytes={} tick={}",
             name, subresource, static_cast<u32>(format), bytes, scheduler.CurrentTick());
    return true;
}

void RasterizerD3D12::ProbeMaterialIntermediates(const GraphicsPipeline& pipeline,
    const PipelineBindings& bindings, const Framebuffer& framebuffer, const DrawParams& params,
    Maxwell::PrimitiveTopology topology) {
    if (!material_gpu_probe_enabled.load(std::memory_order_relaxed) || material_gpu_probe_done ||
        !material_snapshots.Targeted(pipeline.Key().unique_hashes[5], params.num_vertices)) return;
    material_gpu_probe_done = true; // A rejected interface must not retry/allocate per draw.
    const Image* image = framebuffer.ColorImage(0);
    if (!image || !image->Handle() || framebuffer.ColorBase(0).level != 0 ||
        material_probe_x < 0 || material_probe_y < 0) return;
    const auto source_desc = image->Handle()->GetDesc();
    const u64 pixels = source_desc.Width * source_desc.Height;
    if (!pixels || pixels > (32ULL << 20) / 16 || source_desc.SampleDesc.Count != 1 ||
        source_desc.Width > INT_MAX || source_desc.Height > INT_MAX) return;
    const LONG width = static_cast<LONG>(source_desc.Width), height = static_cast<LONG>(source_desc.Height);
    if (material_probe_x >= width || material_probe_y >= height) return;
    D3D12_RECT roi{std::max(0, material_probe_x - 128), std::max(0, material_probe_y - 64), 0, 0};
    roi.right = std::min(width, roi.left + 256);
    roi.bottom = std::min(height, roi.top + 128);
    const auto original_scissor = ScissorRect(0);
    roi.left = std::max(roi.left, original_scissor.left);
    roi.top = std::max(roi.top, original_scissor.top);
    roi.right = std::min(roi.right, original_scissor.right);
    roi.bottom = std::min(roi.bottom, original_scissor.bottom);
    if (roi.left >= roi.right || roi.top >= roi.bottom) return;

    ComPtr<ID3D12Device> capture_device;
    ThrowIfFailed(image->Handle()->GetDevice(IID_PPV_ARGS(&capture_device)), "material GPU probe device");
    const bool reflection = pipeline.Key().unique_hashes[1] == 0x55e0d63ec3a46b1fULL &&
                            pipeline.Key().unique_hashes[5] == 0xba9295f0d82e7ae1ULL;
    const bool lowered = pipeline.LoweredNoPerspectiveMask() != 0;
    if (!reflection && lowered && pipeline.LoweredNoPerspectiveMask() != 3) {
        throw std::runtime_error("material GPU probe: unsupported partial NoPerspective lowering");
    }
    std::vector<ComPtr<ID3D12PipelineState>> probes;
    std::vector<std::string_view> names;
    std::vector<std::string_view> channels;
    if (reflection) {
        names = {"reflection_direction_lod", "reflection_implicit", "reflection_level_zero",
                 "reflection_ambient_lut", "reflection_gradient_x", "reflection_gradient_y"};
        channels = {"reflection.xyz/LOD_unclamped", "cube.Sample.rgba", "cube.SampleLevel0.rgba",
                    "Generic4.rgb/LUT.r", "ddx_fine(reflection).xyz/ddx_fine(LUTcoord)",
                    "ddy_fine(reflection).xyz/ddy_fine(LUTcoord)"};
        if (std::filesystem::exists(Common::FS::GetExeDirectory() / "material_reflection_original_cfg.flag")) {
            names = {"reflection_cfg_cube", "reflection_cfg_lut_power", "reflection_cfg_direction",
                     "reflection_cfg_material", "reflection_cfg_ambient", "reflection_cfg_scaled"};
            channels = {"original CFG:cube.Sample.rgba", "original CFG:LUT.r/abs(cube.a)/log2/exp2(log2*4)",
                        "original CFG:cube.direction.xyz/LUTcoord", "original CFG:mask.r/albedo.rgb",
                        "original CFG:Generic4.rgb/material weight", "original CFG:scaled reflection.rgb/CB scalar"};
        }
        if (std::filesystem::exists(Common::FS::GetExeDirectory() / "material_reflection_branch_controls.flag")) {
            names = {"reflection_branch_bypass_first", "reflection_branch_bypass_second", "reflection_branch_bypass_both",
                     "reflection_branch_predicate_first", "reflection_branch_predicate_second", "reflection_branch_predicate_both"};
            channels.assign(6, "diagnostic branch control:original final RT3.rgba to float4 RT0");
        }
        if (std::filesystem::exists(Common::FS::GetExeDirectory() / "material_reflection_6.dxil")) {
            names.insert(names.end(), {"reflection_original_final", "reflection_original_r11"});
            channels.insert(channels.end(), {"original CFG/math:RT3.rgba redirected to float4 RT0",
                "same redirected DXIL:RT3.rgb to R11G11B10 RT0"});
        }
        for (size_t pass = 0; pass < names.size(); ++pass) {
            // Private diagnostic sidecars are packaged locally, never embedded in production.
            // Bounded, opt-in loading happens once per capture.
            const auto path = Common::FS::GetExeDirectory() /
                              fmt::format("material_reflection_{}.dxil", pass);
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) throw std::runtime_error("material reflection probe: missing sidecar");
            const auto bytes = file.tellg();
            if (bytes < 32 || bytes > (1 << 20))
                throw std::runtime_error("material reflection probe: invalid sidecar size");
            std::vector<u8> code(static_cast<size_t>(bytes));
            file.seekg(0);
            file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size()));
            if (!file || std::memcmp(code.data(), "DXBC", 4) != 0)
                throw std::runtime_error("material reflection probe: invalid DXIL container");
            probes.push_back(pipeline.CreateMaterialProbePipeline(code, {}, true,
                pass == 7 ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R32G32B32A32_FLOAT));
        }
    } else {
        names = {"varyings", "samples", "identity_varyings", "linear_varyings", "linear_samples"};
        channels = {"Generic0.x/Generic0.y/Generic1.x/Generic1.y",
            "depth.r/depth.g/mask(screenUV,LOD0)/mask(screenUV,LOD9)",
            "identityGS:Generic0.x/Generic0.y/Generic1.x/Generic1.y",
            "smooth(a*clipW)/SVPosition.w:Generic0.x/Generic0.y/Generic1.x/Generic1.y",
            "smooth(a*clipW)/SVPosition.w:depth.r/depth.g/mask(screenUV,LOD0)/mask(screenUV,LOD9)"};
        if (lowered) {
            probes = {pipeline.CreateMaterialProbePipeline(material_linear_varyings_ps),
                      pipeline.CreateMaterialProbePipeline(material_linear_samples_ps)};
        } else {
            probes = {pipeline.CreateMaterialProbePipeline(material_varyings_ps),
                      pipeline.CreateMaterialProbePipeline(material_samples_ps),
                      pipeline.CreateMaterialProbePipeline(material_varyings_ps, material_identity_gs),
                      pipeline.CreateMaterialProbePipeline(material_linear_varyings_ps, material_linear_gs),
                      pipeline.CreateMaterialProbePipeline(material_linear_samples_ps, material_linear_gs)};
        }
    }
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width; desc.Height = height;
    desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_CLEAR_VALUE clear{}; clear.Format = desc.Format;
    for (auto& channel : clear.Color) channel = -99999.0f;
    ComPtr<ID3D12Resource> target;
    ThrowIfFailed(capture_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&target)), "material GPU probe RT");
    ComPtr<ID3D12Resource> packed_target;
    const bool compare_packed = reflection && probes.size() == 8;
    if (compare_packed) {
        auto packed_desc = desc;
        packed_desc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
        D3D12_CLEAR_VALUE packed_clear{}; packed_clear.Format = packed_desc.Format;
        ThrowIfFailed(capture_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
            &packed_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &packed_clear,
            IID_PPV_ARGS(&packed_target)), "material packed probe RT");
        scheduler.DeferRelease(packed_target);
    }
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, compare_packed ? 2U : 1U};
    ComPtr<ID3D12DescriptorHeap> rtvs;
    ThrowIfFailed(capture_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtvs)),
                  "material GPU probe RTV heap");
    const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    capture_device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    auto packed_rtv = rtv;
    if (compare_packed) {
        packed_rtv.ptr += capture_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        capture_device->CreateRenderTargetView(packed_target.Get(), nullptr, packed_rtv);
    }

    // Lifetime covers the async recorder and GPU. No flush/wait and no live guest target writes.
    scheduler.DeferRelease(target); scheduler.DeferRelease(rtvs);
    for (const auto& probe : probes) scheduler.DeferRelease(probe);
    auto* cmd = scheduler.CommandList();
    SCOPE_EXIT {
        command_state.valid = false;
        try {
            // Internal draws must not contribute samples/primitives to guest query reports.
            query_cache.EnableCounters();
            BindDrawState(pipeline, bindings, framebuffer, params, topology);
        } catch (const std::exception& e) {
            // Leave the cache invalid so the next normal draw rebinds all state.
            LOG_WARNING(Render, "D3D12 material GPU probe state restore deferred: {}", e.what());
        }
    };
    query_cache.DisableStreams();
    const D3D12_BOX box{static_cast<UINT>(roi.left), static_cast<UINT>(roi.top), 0,
        static_cast<UINT>(roi.right), static_cast<UINT>(roi.bottom), 1};
    for (size_t pass = 0; pass < probes.size(); ++pass) {
        const bool packed = reflection && pass == 7;
        auto* active_target = packed ? packed_target.Get() : target.Get();
        const auto active_rtv = packed ? packed_rtv : rtv;
        const auto active_format = packed ? DXGI_FORMAT_R11G11B10_FLOAT : desc.Format;
        const std::array<float, 4> packed_clear{};
        if (pass && !packed) {
            D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {active_target, 0, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  D3D12_RESOURCE_STATE_RENDER_TARGET};
            cmd->ResourceBarrier(1, &barrier);
        }
        cmd->ClearRenderTargetView(active_rtv, packed ? packed_clear.data() : clear.Color, 1, &roi);
        cmd->OMSetRenderTargets(1, &active_rtv, FALSE, nullptr);
        cmd->RSSetScissorRects(1, &roi);
        scheduler.SetPipelineState(probes[pass].Get());
        {
            const ConditionalRendering::Scope predicate{conditional_rendering, cmd};
            if (params.is_indexed) {
                cmd->DrawIndexedInstanced(params.num_vertices, params.num_instances, params.first_index,
                                          static_cast<INT>(params.base_vertex), params.base_instance);
            } else {
                cmd->DrawInstanced(params.num_vertices, params.num_instances, params.base_vertex,
                                   params.base_instance);
            }
        }
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {active_target, 0, D3D12_RESOURCE_STATE_RENDER_TARGET,
                              D3D12_RESOURCE_STATE_COPY_SOURCE};
        cmd->ResourceBarrier(1, &barrier);
        const auto name = fmt::format("material_{}_{}_draw_{}_gpu_{}_x{}_y{}",
            material_capture_stamp, material_capture_id, trace_index,
            names[pass], roi.left, roi.top);
        const bool queued = QueueMaterialReadback(active_target, 0, active_format, name, &box);
        LOG_INFO(Render, "D3D12 material GPU probe: {} queued={} ROI={}x{} full={}x{} "
                        "VS={:016x} PS={:016x} pipeline={:016x} lowered_noperspective={:08x} channels={}",
            name, queued, roi.right - roi.left, roi.bottom - roi.top, width, height,
            pipeline.Key().unique_hashes[1], pipeline.Key().unique_hashes[5], pipeline.KeyHash(),
            pipeline.LoweredNoPerspectiveMask(), channels[pass]);
    }
}

void RasterizerD3D12::CaptureMaterialSampledInputs(const GraphicsPipeline& pipeline,
    std::span<const VideoCommon::ImageViewId> views, u32 vertices, const Framebuffer& framebuffer) {
    const auto shader = pipeline.Key().unique_hashes[5];
    // Count zero preserves the existing first-draw capture without extra texture copies.
    if (!material_snapshots.Targeted(shader, vertices)) return;
    // A blended output cannot be replayed from shader inputs alone: preserve the
    // destination before this selected draw, with the same bounded deferred path.
    if (pipeline.Key().state.attachments[0].enable &&
        (pipeline.BoundColorTargets() & 1) && !framebuffer.ColorOnCopy(0)) {
        if (const Image* image = framebuffer.ColorImage(0); image && image->Handle() &&
            image->Handle()->GetDesc().SampleDesc.Count == 1) {
            const auto base = framebuffer.ColorBase(0);
            const auto name = fmt::format("material_{}_{}_before_draw_{}_ps_{:016x}_rt0",
                material_capture_stamp, material_capture_id, trace_index, shader);
            const auto saved = DumpTarget(*image, &name, image->Subresource(base.level, base.layer));
            LOG_INFO(Render, "D3D12 material pre-blend snapshot: draw={} PS={:016x} result={}",
                     trace_index, shader, saved ? name : "omitted/unsupported");
        }
    }
    MaterialSampledInputBudget selected_budget;
    std::unordered_set<std::string> copied;
    for (const auto id : views) {
        const auto& view = texture_cache.GetImageView(id);
        Image* image = view.SourceImage();
        if (!image || !image->Handle()) continue;
        const auto desc = image->Handle()->GetDesc();
        if (view.ImageHandle() != image->Handle() ||
            (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
             desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE1D) || desc.SampleDesc.Count != 1 ||
            (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
            // Depth sampling may bind a feedback copy rather than this source. The
            // separate sampled-depth diagnostic records those candidates explicitly.
            LOG_INFO(Render, "D3D12 material selected input unsupported: draw={} @{:x} dimension={} view_copy={} depth={}",
                     trace_index, image->gpu_addr, static_cast<u32>(desc.Dimension),
                     view.ImageHandle() != image->Handle(),
                     (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0);
            continue;
        }
        ComPtr<ID3D12Device> capture_device;
        ThrowIfFailed(image->Handle()->GetDevice(IID_PPV_ARGS(&capture_device)), "selected input device");
        for (s32 layer = view.range.base.layer;
             layer < view.range.base.layer + view.range.extent.layers; ++layer) {
            for (s32 mip = view.range.base.level;
                 mip < view.range.base.level + view.range.extent.levels; ++mip) {
                const auto key = fmt::format("{:x}_m{}_l{}", image->gpu_addr, mip, layer);
                if (copied.size() == 256 && !copied.contains(key)) {
                    LOG_INFO(Render, "D3D12 material selected inputs capped: draw={} subresources=256", trace_index);
                    return;
                }
                if (!copied.insert(key).second) continue;
                const auto subresource = image->Subresource(mip, layer);
                UINT64 bytes{};
                capture_device->GetCopyableFootprints(&desc, subresource, 1, 0,
                    nullptr, nullptr, nullptr, &bytes);
                // Full 1080p inputs are required to follow discontinuities outside the ROI.
                // QueueMaterialReadback still enforces 64 MiB and the presentation reserve.
                if (!selected_budget.Fits(bytes)) {
                    LOG_INFO(Render, "D3D12 material selected input omitted: draw={} {} bytes={} limit=32MiB/resource8MiB",
                             trace_index, key, bytes);
                    continue;
                }
                const auto previous = image->State();
                image->Transition(D3D12_RESOURCE_STATE_COPY_SOURCE);
                SCOPE_EXIT { image->Transition(previous); };
                const auto name = fmt::format("material_{}_{}_before_draw_{}_ps_{:016x}_input_{}",
                    material_capture_stamp, material_capture_id, trace_index, shader, key);
                if (QueueMaterialReadback(image->Handle(), subresource, desc.Format, name))
                    selected_budget.Commit(bytes);
                else
                    LOG_INFO(Render, "D3D12 material selected input not saved: draw={} {} dxgi={}",
                             trace_index, key, static_cast<u32>(desc.Format));
            }
        }
    }
}

void RasterizerD3D12::CaptureMaterialPresent(ID3D12Resource* resource, u64 frame) {
    if (!material_trace.Active() || !resource) return;
    try {
        std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
        const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace";
        std::filesystem::create_directories(directory);
        QueueMaterialReadback(resource, 0, resource->GetDesc().Format,
            fmt::format("material_{}_{}_present_{}", material_capture_stamp,
                        material_capture_id, frame));
    } catch (const std::exception& e) {
        LOG_WARNING(Render, "D3D12 material presentation capture failed: {}", e.what());
    }
}

void RasterizerD3D12::CaptureMaterialInputs(const GraphicsPipeline& pipeline,
    std::span<const TracedBuffer> buffers, const DrawParams& params) {
    const std::string pair = fmt::format("vs_{:016x}_ps_{:016x}",
        pipeline.Key().unique_hashes[1], pipeline.Key().unique_hashes[5]);
    // All shader pairs, including opaque materials. Explicitly bounded; this is a first-draw
    // sample, not a complete replay of every object using the same shader.
    const bool targeted = material_snapshots.Targeted(pipeline.Key().unique_hashes[5], params.num_vertices);
    if (!targeted && (material_input_pairs.contains(pair) || material_input_pairs.size() == 128)) return;
    if (material_input_pairs.size() < 128) material_input_pairs.insert(pair);
    LOG_INFO(Render, "D3D12 material inputs: draw={} {} vertices={} instances={} first_index={} "
                    "base_vertex={} base_instance={} indexed={} targeted={} (first pair or explicit selector)",
        trace_index, pair, params.num_vertices, params.num_instances, params.first_index,
        params.base_vertex, params.base_instance, params.is_indexed, targeted);
    u32 binding = 0;
    for (const auto& source : buffers) {
        const u32 ordinal = binding++;
        if (!source.size || (!source.buffer && !source.mapped)) continue;
        // Uniforms contain camera/object matrices. Geometry can be large; keep a named prefix
        // and report full size, rather than silently claiming the buffer is complete.
        const u32 bytes = std::min(source.size, 256U << 10);
        if (bytes > (8ULL << 20) - material_input_bytes) {
            LOG_INFO(Render, "D3D12 material input omitted: draw={} binding={} bytes={} budget=8MiB",
                     trace_index, ordinal, bytes);
            continue;
        }
        const auto name = fmt::format("material_{}_{}_draw_{}_{}_binding_{}_kind_{}_slot_{}",
            material_capture_stamp, material_capture_id, trace_index, pair, ordinal,
            static_cast<u32>(source.kind), source.slot);
        material_buffer_readbacks.reserve(material_buffer_readbacks.size() + 1);
        MaterialBufferReadback pending{{}, {}, {}, scheduler.CurrentTick(), bytes, name};
        SCOPE_EXIT { if (pending.buffer.buffer) staging.FreeDeferred(pending.buffer); };
        if (source.kind == TracedBuffer::Kind::StreamedUniform && source.mapped) {
            pending.streamed.assign(source.mapped, source.mapped + bytes);
        } else if (source.buffer && source.buffer->Handle()) {
            pending.source = source.buffer->Handle();
            pending.buffer = staging.Request(bytes, MemoryUsage::Download, true);
            source.buffer->Transition(D3D12_RESOURCE_STATE_GENERIC_READ);
            scheduler.CommandList()->CopyBufferRegion(pending.buffer.buffer, pending.buffer.offset,
                source.buffer->Handle(), source.offset, bytes);
        } else continue;
        material_input_bytes += bytes;
        material_buffer_readbacks.push_back(std::move(pending));
        pending.buffer = {};
        LOG_INFO(Render, "D3D12 material input {}: address={:x} offset={} stride={} full_size={} "
                        "saved={} truncated={}", name, source.device_addr, source.offset,
                        source.stride, source.size, bytes, bytes != source.size);
    }
}

void RasterizerD3D12::PollMaterialReadbacks() {
    // Called on the GPU thread under both cache mutexes. Never flush/wait to export.
    if (material_trace.Active()) return; // Disk work only after the captured intervals.
    for (auto it = material_buffer_readbacks.begin(); it != material_buffer_readbacks.end();) {
        if (!scheduler.IsFree(it->tick)) { ++it; continue; }
        try {
            const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace";
            std::filesystem::create_directories(directory);
            std::ofstream out(directory / (it->name + ".bin"), std::ios::binary);
            const u8* data = it->streamed.empty() ? it->buffer.mapped_span.data() : it->streamed.data();
            out.write(reinterpret_cast<const char*>(data), it->size);
            if (!out) throw std::runtime_error("input buffer export failed");
        } catch (const std::exception& e) {
            LOG_WARNING(Render, "D3D12 material input export failed: {}", e.what());
        }
        if (it->buffer.buffer) staging.FreeDeferred(it->buffer);
        it = material_buffer_readbacks.erase(it);
    }
    if (pending_material_probe && scheduler.IsFree(pending_material_probe->tick)) {
        try {
            ExportMaterialProbe(*pending_material_probe);
        } catch (const std::exception& e) {
            LOG_WARNING(Render, "D3D12 material probe export failed: {}", e.what());
        }
        staging.FreeDeferred(pending_material_probe->buffer);
        pending_material_probe.reset();
    }
    for (auto it = material_readbacks.begin(); it != material_readbacks.end();) {
        if (!scheduler.IsFree(it->tick)) { ++it; continue; }
        try {
            if (MaterialBlockBytes(static_cast<u32>(it->format))) {
                const auto desc = it->source->GetDesc();
                const auto mip = it->subresource % desc.MipLevels;
                const u64 logical_width = std::max<u64>(1, desc.Width >> mip);
                const u32 logical_height = std::max(1U, desc.Height >> mip);
                const auto directory = Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "trace";
                std::filesystem::create_directories(directory);
                std::ofstream raw(directory / (it->name + ".raw"), std::ios::binary);
                for (u32 row = 0; row < it->rows; ++row)
                    raw.write(reinterpret_cast<const char*>(it->buffer.mapped_span.data() +
                        size_t{row} * it->footprint.Footprint.RowPitch),
                        static_cast<std::streamsize>(it->row_bytes));
                std::ofstream info(directory / (it->name + ".json"));
                info << fmt::format("{{\"width\":{},\"height\":{},\"dxgi_format\":{},"
                    "\"row_bytes\":{},\"block_rows\":{},\"gpu_row_pitch\":{},"
                    "\"native_bytes\":true,\"block_compressed\":true,\"origin\":\"top-left\"}}\n",
                    logical_width, logical_height,
                    static_cast<u32>(it->format), it->row_bytes, it->rows,
                    it->footprint.Footprint.RowPitch);
                raw.flush();
                info.flush();
                if (!raw || !info) throw std::runtime_error("material block export failed");
            } else {
                ExportTargetReadback(it->buffer, it->footprint, it->format, &it->name, true);
            }
        } catch (const std::exception& e) {
            LOG_WARNING(Render, "D3D12 material deferred export failed: {}: {}", it->name, e.what());
        }
        material_readback_bytes -= it->bytes;
        staging.FreeDeferred(it->buffer);
        it = material_readbacks.erase(it);
    }
}

void RasterizerD3D12::TraceDraw(std::string_view what, const GraphicsPipeline* pipeline,
                                const Framebuffer* framebuffer,
                                std::span<const VideoCommon::ImageViewId> views, u32 vertices,
                                u32 instances) {
    if (material_trace.Active() && framebuffer && pipeline && framebuffer->Images()) {
        material_images = framebuffer->Images();
        const auto extent = framebuffer->Extent();
        const u64 area = u64{extent.width} * extent.height;
        const auto remember = [&](size_t slot, VideoCommon::ImageId id,
                                  const Image* image, VideoCommon::SubresourceBase base) {
            if (image && image->Handle() && area >= material_targets[slot].area) {
                const auto& previous = material_targets[slot];
                // Keep the first auxiliary at this resolution (the scene's preliminary
                // surface), rather than replacing it with later same-size lighting/bloom.
                if (slot == 1 && previous.resource && previous.area == area) return;
                if (previous.id == id && previous.resource.Get() == image->Handle() &&
                    previous.base.level == base.level && previous.base.layer == base.layer &&
                    previous.area == area) return;
                material_targets[slot] = {id, image->Handle(), base, area};
            }
        };
        for (size_t rt = 0; rt < VideoCommon::NUM_RT; ++rt) {
            if (!(pipeline->BoundColorTargets() & (1U << rt)) || framebuffer->ColorOnCopy(rt)) continue;
            remember(rt == 0 ? 0 : 1, framebuffer->ColorImageId(rt), framebuffer->ColorImage(rt),
                     framebuffer->ColorBase(rt));
        }
        remember(2, framebuffer->DepthImageId(), framebuffer->DepthImage(), framebuffer->DepthBase());
    }
    const auto describe = [](const Image* image) {
        if (!image) {
            return std::string("none");
        }
        const auto& info = image->info;
        return fmt::format("{} {}x{}x{} L{} @{:x} flags {:x}", info.format, info.size.width,
                           info.size.height,
                           info.type == VideoCommon::ImageType::e3D ? info.size.depth
                                                                     : info.resources.layers,
                           info.resources.levels, image->gpu_addr,
                           static_cast<u32>(image->flags));
    };
    // The previous framebuffer is done (for now): keep what it holds. The copy lands after the
    // draw being traced, which only matters when both framebuffers share an image.
    if (framebuffer && trace_dumps) {
        std::array<VideoCommon::ImageId, VideoCommon::NUM_RT + 1> targets{};
        for (size_t index = 0; index < VideoCommon::NUM_RT; ++index) {
            targets[index] = framebuffer->ColorImageId(index);
        }
        targets[VideoCommon::NUM_RT] = framebuffer->DepthImageId();
        if (targets != traced_targets) {
            DumpTargets(traced_targets);
            traced_targets = targets;
            if (framebuffer->Images()) {
                traced_images = framebuffer->Images();
            }
        }
    }
    trace_non_finite_grew = false;
    std::string line = fmt::format("D3D12 trace #{}: {}", trace_index++, what);
    if (vertices != 0 || instances != 0) {
        line += fmt::format(" ({} vertices x{})", vertices, instances);
    }
    if (pipeline) {
        const GraphicsPipelineCacheKey& key = pipeline->Key();
        line += fmt::format(" | VS {:016x} PS {:016x} blend0 {:08x}", key.unique_hashes[1],
                            key.unique_hashes[5], key.state.attachments[0].raw);
        const auto& dynamic = key.state.dynamic_state;
        line += fmt::format(" PSO={} key={:016x} targets={:x} topology={} bias enable={} "
                            "guest={:.6g} host={} clamp={:.6g} slope={:.6g}",
                            static_cast<void*>(pipeline->Handle()), pipeline->KeyHash(),
                            pipeline->BoundColorTargets(),
                            static_cast<u32>(key.state.topology.Value()),
                            dynamic.depth_bias_enable.Value(), std::bit_cast<float>(key.depth_bias),
                            static_cast<INT>(std::bit_cast<float>(key.depth_bias) / 2.0f),
                            std::bit_cast<float>(key.depth_bias_clamp),
                            std::bit_cast<float>(key.slope_scaled_depth_bias));
        for (size_t rt = 0; rt < key.state.attachments.size(); ++rt) {
            const auto& blend = key.state.attachments[rt];
            if (!(pipeline->BoundColorTargets() & (1U << rt))) continue;
            const auto append_guest = [&](const auto& source) {
                line += fmt::format(" | guestBlend{} perTarget={} separateA={} "
                                    "RGB={}/{}/{} A={}/{}/{}", rt,
                                    maxwell3d->regs.blend_per_target_enabled, source.separate_alpha,
                                    static_cast<u32>(source.color_source),
                                    static_cast<u32>(source.color_dest), static_cast<u32>(source.color_op),
                                    static_cast<u32>(source.alpha_source),
                                    static_cast<u32>(source.alpha_dest), static_cast<u32>(source.alpha_op));
            };
            if (maxwell3d->regs.blend_per_target_enabled) {
                append_guest(maxwell3d->regs.blend_per_target[rt]);
            } else {
                append_guest(maxwell3d->regs.blend);
            }
            if (!blend.enable) {
                line += fmt::format(" | blend{} disabled mask={:x}", rt, blend.raw & 0xf);
                continue;
            }
            line += fmt::format(" | blend{} enable={} packed={:08x} "
                                "keyRGB={}/{}/{} keyA={}/{}/{} "
                                "translatedRGB={}/{}/{} translatedA={}/{}/{}",
                                rt, blend.enable.Value(), blend.raw,
                                static_cast<u32>(blend.SourceRGBFactor()),
                                static_cast<u32>(blend.DestRGBFactor()),
                                static_cast<u32>(blend.EquationRGB()),
                                static_cast<u32>(blend.SourceAlphaFactor()),
                                static_cast<u32>(blend.DestAlphaFactor()),
                                static_cast<u32>(blend.EquationAlpha()),
                                static_cast<u32>(MaxwellToD3D12::BlendFactor(blend.SourceRGBFactor(), false)),
                                static_cast<u32>(MaxwellToD3D12::BlendFactor(blend.DestRGBFactor(), false)),
                                static_cast<u32>(MaxwellToD3D12::BlendOp(blend.EquationRGB())),
                                static_cast<u32>(MaxwellToD3D12::BlendFactor(blend.SourceAlphaFactor(), true)),
                                static_cast<u32>(MaxwellToD3D12::BlendFactor(blend.DestAlphaFactor(), true)),
                                static_cast<u32>(MaxwellToD3D12::BlendOp(blend.EquationAlpha())));
        }
        const auto& blend_color = maxwell3d->regs.blend_color;
        line += fmt::format(" constants=[{:.6g},{:.6g},{:.6g},{:.6g}]",
                            blend_color.r, blend_color.g, blend_color.b, blend_color.a);
        line += fmt::format(" depth test {} write {} func {} stencil {} cull {}",
                            dynamic.depth_test_enable.Value(), dynamic.depth_write_enable.Value(),
                            static_cast<u32>(dynamic.DepthTestFunc()),
                            dynamic.stencil_enable.Value(), dynamic.cull_enable.Value());
        line += fmt::format(" face {} front {}", static_cast<u32>(dynamic.CullFace()),
                            static_cast<u32>(dynamic.FrontFace()));
        // Trace the same enabled/read attributes and fetch policy as the PSO, including
        // normals/tangents beyond slot 3. No vertex readback or per-frame logging.
        const StageBindings& vs_info = pipeline->StageInfo(0);
        for (u32 i = 0; i < key.state.attributes.size(); ++i) {
            const auto& baked = key.state.attributes[i];
            if (!baked.enabled || !vs_info.loads.Generic(i)) continue;
            const auto& attrib = maxwell3d->regs.vertex_attrib_format[i];
            line += fmt::format(" attr{} {:08x}(b{} +{} s{:x} t{}{})", i, attrib.hex,
                                attrib.buffer.Value(), attrib.offset.Value(),
                                static_cast<u32>(attrib.size.Value()),
                                static_cast<u32>(attrib.type.Value()),
                                attrib.constant ? " const" : "");
            const u32 stride = key.state.vertex_strides[baked.buffer];
            const auto fetch = MaxwellToD3D12::SplitAttributeFetch(
                baked.Type(), baked.Size(), baked.offset, stride);
            const auto requested_format = fetch.parts > 1 ? fetch.part_format
                : MaxwellToD3D12::VertexFormat(baked.Type(), baked.Size());
            const auto format = requested_format == DXGI_FORMAT_UNKNOWN
                ? DXGI_FORMAT_R32G32B32A32_FLOAT : requested_format;
            line += fmt::format(" fetch{}[dxgi={} fallback={} offset={} stride={} divisor={} parts={} bytes={}]",
                                i, static_cast<u32>(format), requested_format == DXGI_FORMAT_UNKNOWN,
                                baked.offset, stride,
                                key.state.binding_divisors[baked.buffer], fetch.parts, fetch.part_bytes);
        }
        const auto& ps_info = pipeline->StageInfo(4);
        line += " interp[";
        for (u32 i = 0; i < 32; ++i) {
            if (ps_info.loads.Generic(i)) {
                line += fmt::format("{}:{};", i, (ps_info.interpolation >> (2 * i)) & 3);
            }
        }
        line += "]"; // 0=smooth/perspective, 1=flat, 2=no-perspective (guest IR).
        const bool has_depth = framebuffer && framebuffer->DepthFormat() != DXGI_FORMAT_UNKNOWN;
        line += fmt::format(" depthHost[format={} test={} write={} func={}] bounds[guest={} min={:.9g} "
                            "max={:.9g} host=0]",
                            framebuffer ? static_cast<u32>(framebuffer->DepthFormat()) : 0,
                            has_depth && dynamic.depth_test_enable.Value(),
                            has_depth && dynamic.depth_write_enable.Value(),
                            static_cast<u32>(dynamic.depth_test_enable.Value()
                                ? MaxwellToD3D12::ComparisonFunc(dynamic.DepthTestFunc())
                                : D3D12_COMPARISON_FUNC_ALWAYS),
                            dynamic.depth_bounds_enable.Value(), maxwell3d->regs.depth_bounds[0],
                            maxwell3d->regs.depth_bounds[1]);
        line += fmt::format(" a2c {} alpha test {} ref {:.4g} early z {} msaa {}",
                            key.state.alpha_to_coverage_enabled.Value(),
                            key.state.alpha_test_func.Value(),
                            std::bit_cast<float>(key.state.alpha_test_ref),
                            key.state.early_z.Value(),
                            static_cast<u32>(key.state.msaa_mode.Value()));
        // Clip distances the vertex stage writes, and which the guest enables.
        u32 clip_written = 0;
        for (u32 i = 0; i < 8; ++i) {
            const auto attribute = static_cast<Shader::IR::Attribute>(
                static_cast<u32>(Shader::IR::Attribute::ClipDistance0) + i);
            if (vs_info.stores[attribute]) {
                clip_written |= 1u << i;
            }
        }
        line += fmt::format(" clip written {:x} enabled {:x}", clip_written,
                            maxwell3d->regs.user_clip_enable.raw);
        // Depth range: the guest's viewport 0 transform and what D3D12 gets from it.
        const auto& regs = maxwell3d->regs;
        const auto& vp = regs.viewport_transform[0];
        std::array<D3D12_VIEWPORT, Maxwell::NumViewports> viewports{};
        const ViewportState vp_state = ComputeViewports(viewports);
        line += fmt::format(" | z mode {} scale {:.6g} translate {:.6g} -> D3D {:.6g}..{:.6g}"
                            " flip {:x} clamp_disabled {}",
                            regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? "-1..1" : "0..1",
                            vp.scale_z, vp.translate_z, viewports[0].MinDepth,
                            viewports[0].MaxDepth, vp_state.yz_flip_mask,
                            dynamic.depth_clamp_disabled.Value());
        // Viewport 0 and scissor 0, guest and D3D12.
        const auto& sc = regs.scissor_test[0];
        const D3D12_RECT scissor = ScissorRect(0);
        line += fmt::format(" | vp xy {:.6g},{:.6g} scale {:.6g},{:.6g} swizzle_y {} -> D3D {:.6g},{:.6g} "
                            "{:.6g}x{:.6g} | origin {} flip_y {} clip {}x{} | scissor {} {},{}-{},{} "
                            "-> D3D {},{}-{},{}",
                            vp.translate_x, vp.translate_y, vp.scale_x, vp.scale_y,
                            static_cast<u32>(vp.swizzle.y.Value()), viewports[0].TopLeftX,
                            viewports[0].TopLeftY, viewports[0].Width, viewports[0].Height,
                            regs.window_origin.mode == Maxwell::WindowOrigin::Mode::UpperLeft
                                ? "upper-left"
                                : "lower-left",
                            regs.window_origin.flip_y.Value(), regs.surface_clip.width,
                            regs.surface_clip.height, sc.enable, sc.min_x, sc.min_y, sc.max_x,
                            sc.max_y, scissor.left, scissor.top, scissor.right, scissor.bottom);
    }
    if (framebuffer) {
        const VideoCommon::Extent2D extent = framebuffer->Extent();
        line += fmt::format(" | fb {}x{}", extent.width, extent.height);
        for (size_t index = 0; index < VideoCommon::NUM_RT; ++index) {
            if (const Image* const image = framebuffer->ColorImage(index)) {
                line += fmt::format(" rt{} [{}]", index, describe(image));
            }
        }
        if (const u32 missing = framebuffer->MissingColorMask()) {
            line += fmt::format(" MISSING RTV mask {:x}", missing);
        }
        if (const Image* const image = framebuffer->DepthImage()) {
            line += fmt::format(" ds [{}]", describe(image));
        }
        // Where NaN/Inf enter the frame: rt0 after every traced draw.
        if (const Image* const image = framebuffer->ColorImage(0); image && pipeline && trace_dumps) {
            const VideoCommon::SubresourceBase base = framebuffer->ColorBase(0);
            if (const std::optional<u64> non_finite =
                    DumpTarget(*image, nullptr, image->Subresource(base.level, base.layer))) {
                line += fmt::format(" | rt0 L{}/{} non-finite {}", base.level, base.layer,
                                    *non_finite);
                u64& previous = traced_non_finite[image->gpu_addr];
                trace_non_finite_grew = *non_finite > previous;
                previous = *non_finite;
            }
        }
        if (const Image* const depth = framebuffer->DepthImage(); depth && pipeline && trace_dumps) {
            line += TraceDepthChanges(*depth);
        }
    }
    for (const VideoCommon::ImageViewId id : views) {
        const ImageView& view = texture_cache.GetImageView(id);
        if (material_trace.Active() && view.SourceImage() && view.SourceImage()->Handle() &&
            (view.SourceImage()->Handle()->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) {
            // Snapshot a sampled depth surface at its first use, before later clears/reuse.
            // These are candidates for shadow maps, not an assertion that all are shadows.
            const Image& image = *view.SourceImage();
            const auto base = view.range.base;
            const std::string key = fmt::format("{:x}_m{}_l{}", image.gpu_addr, base.level, base.layer);
            if (material_sampled_depth.insert(key).second) {
                try {
                    const auto desc = image.Handle()->GetDesc();
                    ComPtr<ID3D12Device> capture_device;
                    ThrowIfFailed(image.Handle()->GetDevice(IID_PPV_ARGS(&capture_device)), "depth sample capture");
                    UINT64 bytes{};
                    capture_device->GetCopyableFootprints(&desc, image.Subresource(base.level, base.layer),
                        1, 0, nullptr, nullptr, nullptr, &bytes);
                    if (bytes && bytes <= (16ULL << 20) - material_sampled_depth_bytes) {
                        const auto name = fmt::format("material_{}_{}_draw_{}_sampled_depth_{}",
                            material_capture_stamp, material_capture_id, trace_index - 1, key);
                        if (DumpTarget(image, &name, image.Subresource(base.level, base.layer)))
                            material_sampled_depth_bytes += bytes;
                    } else {
                        LOG_INFO(Render, "D3D12 material sampled depth omitted: {} bytes={}", key, bytes);
                    }
                } catch (const std::exception& e) {
                    LOG_WARNING(Render, "D3D12 material sampled depth capture failed: {}", e.what());
                }
            }
        }
        if (const Image* const image = view.SourceImage();
            image && trace_dumps && traced_textures.insert(image->gpu_addr).second) {
            DumpTextureNonFinite(*image);
            CheckTracedTexture(*image);
        }
        line += fmt::format(" | tex {} {}x{} type {} mips {}+{} layers {}+{} of [{}]", view.format,
                            view.size.width, view.size.height, static_cast<u32>(view.type),
                            view.range.base.level, view.range.extent.levels,
                            view.range.base.layer, view.range.extent.layers,
                            describe(view.SourceImage()));
    }
    LogDrawTrace(line);
    if (material_trace.Active() && pipeline && framebuffer && vertices && instances) {
        DumpMaterialShaderTarget(*pipeline, *framebuffer, vertices);
    }
}

} // namespace D3D12
