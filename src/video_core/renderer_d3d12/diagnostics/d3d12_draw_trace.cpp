// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>

#include "common/bug_tracker.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/util.h"

// Draw tracing (RasterizerD3D12::SetDrawTrace): per-draw logs, target dumps and GPU
// readback checks against guest memory. Diagnostics only; draws do not depend on it.

namespace D3D12 {

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
    staging.FreeDeferred(readback);
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
    }
    return non_finite;
}

void RasterizerD3D12::TraceDraw(std::string_view what, const GraphicsPipeline* pipeline,
                                const Framebuffer* framebuffer,
                                std::span<const VideoCommon::ImageViewId> views, u32 vertices,
                                u32 instances) {
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
        line += fmt::format(" depth test {} write {} func {} stencil {} cull {}",
                            dynamic.depth_test_enable.Value(), dynamic.depth_write_enable.Value(),
                            static_cast<u32>(dynamic.DepthTestFunc()),
                            dynamic.stencil_enable.Value(), dynamic.cull_enable.Value());
        line += fmt::format(" face {} front {}", static_cast<u32>(dynamic.CullFace()),
                            static_cast<u32>(dynamic.FrontFace()));
        // Vertex attributes the vertex stage reads: buffer, offset, size and type.
        for (u32 i = 0; i < 4; ++i) {
            const auto& attrib = maxwell3d->regs.vertex_attrib_format[i];
            line += fmt::format(" attr{} {:08x}(b{} +{} s{:x} t{}{})", i, attrib.hex,
                                attrib.buffer.Value(), attrib.offset.Value(),
                                static_cast<u32>(attrib.size.Value()),
                                static_cast<u32>(attrib.type.Value()),
                                attrib.constant ? " const" : "");
        }
        line += fmt::format(" a2c {} alpha test {} ref {:.4g} early z {} msaa {}",
                            key.state.alpha_to_coverage_enabled.Value(),
                            key.state.alpha_test_func.Value(),
                            std::bit_cast<float>(key.state.alpha_test_ref),
                            key.state.early_z.Value(),
                            static_cast<u32>(key.state.msaa_mode.Value()));
        // Clip distances the vertex stage writes, and which the guest enables.
        u32 clip_written = 0;
        const StageBindings& vs_info = pipeline->StageInfo(0);
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
        if (const Image* const image = view.SourceImage();
            image && trace_dumps && traced_textures.insert(image->gpu_addr).second) {
            DumpTextureNonFinite(*image);
            CheckTracedTexture(*image);
        }
        line += fmt::format(" | tex {} {}x{} type {} layers {}+{} of [{}]", view.format,
                            view.size.width, view.size.height, static_cast<u32>(view.type),
                            view.range.base.layer, view.range.extent.layers,
                            describe(view.SourceImage()));
    }
    LOG_INFO(Render, "{}", line);
}

} // namespace D3D12
