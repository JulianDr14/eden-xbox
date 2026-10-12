// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/logging.h"
#include "common/scratch_buffer.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_bc_copy.h"
#include "video_core/renderer_d3d12/d3d12_blit_image.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "video_core/renderer_d3d12/d3d12_texture_cache.h"
#include "video_core/texture_cache/util.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <d3d12sdklayers.h>
#include <iostream>
#include <vector>

using namespace D3D12;
using namespace VideoCommon;
using VideoCore::Surface::PixelFormat;

static_assert(requires(Image &image, Common::ScratchBuffer<u8> &data,
                       std::vector<BufferImageCopy> &copies) {
    image.PreserveGuestBlocks(data, copies);
});

static std::vector<BufferImageCopy> Layout(const ImageInfo &info, size_t &bytes) {
    std::vector<BufferImageCopy> copies;
    bytes = 0;
    const u32 block = VideoCore::Surface::DefaultBlockWidth(info.format);
    const u32 bpp = VideoCore::Surface::BytesPerBlock(info.format);
    for (s32 level = 0; level < info.resources.levels; ++level) {
        const u32 w = std::max(1U, info.size.width >> level);
        const u32 h = std::max(1U, info.size.height >> level);
        const u32 bw = (w + block - 1) / block, bh = (h + block - 1) / block;
        const size_t size = size_t{bw} * bh * bpp * info.resources.layers;
        copies.push_back({.buffer_offset = bytes,
                          .buffer_size = size,
                          .buffer_row_length = bw * block,
                          .buffer_image_height = bh * block,
                          .image_subresource = {level, 0, info.resources.layers},
                          .image_offset = {},
                          .image_extent = {w, h, 1}});
        bytes += size;
    }
    return copies;
}

int main() try {
    std::cerr << "BC copy gate: CPU bounds\n";
    for (u32 width = 1; width <= 65; ++width) {
        for (s32 x = -1; x <= static_cast<s32>(width); ++x) {
            for (u32 extent = 0; extent <= width + 1; ++extent) {
                const auto plan = PlanBcCopyAxis(x, extent, width, 1, 20);
                const bool expected = x >= 0 && u32(x) < width && x % 4 == 0 && extent > 0 &&
                                      extent <= width - u32(x) &&
                                      (extent % 4 == 0 || extent == width - u32(x)) &&
                                      (extent + 3) / 4 <= 19;
                assert(bool(plan) == expected);
                if (plan)
                    assert(plan->blocks == (extent + 3) / 4 && plan->source_end == x + extent);
            }
        }
    }
    assert(!PlanBcCopyAxis(0, UINT32_MAX, 64, 0, 64));
    assert(!PlanBcCopyAxis(0, 4, 64, -1, 64));
    assert(!PlanBcCopyAxis(0, 8, 64, 3, 4));
    const auto check_failure = [](s32 src, u32 extent, u32 size, s32 dst, u32 target,
                                  u32 block, const char* expected) {
        const char* failure{};
        assert(!PlanBcCopyAxis(src, extent, size, dst, target, block, &failure));
        assert(failure && std::strcmp(failure, expected) == 0);
    };
    check_failure(-1, 4, 8, 0, 8, 4, "negative source offset");
    check_failure(0, 4, 8, -1, 8, 4, "negative destination offset");
    check_failure(0, 0, 8, 0, 8, 4, "zero extent");
    check_failure(0, 4, 8, 0, 8, 0, "zero block size");
    check_failure(8, 4, 8, 0, 8, 4, "source offset outside mip");
    check_failure(0, 4, 8, 8, 8, 4, "destination offset outside mip");
    check_failure(1, 4, 8, 0, 8, 4, "source offset not block aligned");
    check_failure(0, 9, 8, 0, 8, 4, "extent exceeds source mip");
    check_failure(0, 2, 8, 0, 2, 4, "partial block does not end at source mip edge");
    check_failure(0, 1, 4, 0, 1, 4, "partial block does not end at source mip edge");
    check_failure(0, 8, 8, 0, 1, 4, "block count exceeds destination mip");
    const char* success_reason = "old failure";
    assert(PlanBcCopyAxis(0, 2, 2, 0, 1, 4, &success_reason) && !success_reason);
    assert(AliasBlockExtent(2, 0, 8, 4) == 8);
    assert(AliasBlockExtent(1, 0, 4, 4) == 4);
    assert(AliasBlockExtent(1, 0, 2, 4) == 2);
    assert(AliasBlockExtent(1, 0, 1, 4) == 1);
    assert(!AliasBlockExtent(3, 0, 8, 4));
    assert(!AliasBlockExtent(UINT32_MAX, 0, 8, 4));
    assert(!AliasBlockExtent(1, 1, 8, 4));
    BcBlockCoverage coverage;
    coverage.Reset(16, 16, 5, 3);
    assert(!coverage.Contains(0, 0, 0, 1, 1));
    coverage.Include(0, 1, 1, 2, 1);
    assert(coverage.Contains(0, 1, 1, 2, 1));
    assert(!coverage.Contains(0, 0, 1, 2, 1));
    assert(!coverage.Contains(5, 1, 1, 2, 1));
    assert(!coverage.Contains(1, 1, 1, 2, 1));
    coverage.Include(0, 0, 1, 1, 1);
    assert(coverage.Contains(0, 0, 1, 3, 1));
    coverage.Fill();
    assert(coverage.Contains(14, 0, 0, 1, 1));
    assert(!coverage.Contains(15, 0, 0, 1, 1));
    coverage.Clear();
    assert(!coverage.Contains(14, 0, 0, 1, 1));
    std::cerr << "BC copy gate: initialize runtime\n";
    Common::Log::Initialize();
    Settings::values.renderer_debug = true;
    Device device;
    std::cerr << "BC copy gate: device ready\n";
    ComPtr<ID3D12InfoQueue> messages;
    ThrowIfFailed(device.Get()->QueryInterface(IID_PPV_ARGS(&messages)), "debug info queue");
    messages->ClearStoredMessages();
    Scheduler scheduler{device};
    StagingBufferPool staging{device, scheduler};
    TransferBufferPool transfers{device, scheduler};
    CpuDescriptorAllocator views{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    CpuDescriptorAllocator samplers{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER};
    CpuDescriptorAllocator rtvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV};
    CpuDescriptorAllocator dsvs{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV};
    TextureCacheRuntime runtime{device, scheduler, staging, transfers, views, samplers, rtvs, dsvs};
    ImageInfo source_info;
    source_info.format = PixelFormat::BC3_UNORM;
    source_info.type = ImageType::e2D;
    source_info.size = {16, 16, 1};
    source_info.resources = {5, 3};
    Image source{runtime, source_info, 0, 0};
    assert(source.IsBcDecoded());
    size_t raw_size{};
    const auto raw_copies = Layout(source_info, raw_size);
    std::vector<u8> raw(raw_size);
    for (size_t i = 0; i < raw.size(); ++i)
        raw[i] = u8(i * 73 + i / 16 * 19 + 7);
    auto converted_copies = raw_copies;
    auto decoded = runtime.UploadStagingBuffer(source.converted_size_bytes);
    source.PreserveGuestBlocks(raw, raw_copies);
    ConvertImage(raw, source_info, decoded.mapped_span, converted_copies);
    source.UploadMemory(decoded, converted_copies);
    assert(source.GuestBlocks());

    ImageInfo target_info = source_info;
    target_info.format = PixelFormat::R32G32B32A32_UINT;
    Image target{runtime, target_info, 0, 0};
    size_t target_size{};
    const auto target_copies = Layout(target_info, target_size);
    auto initial = runtime.UploadStagingBuffer(target_size);
    std::memset(initial.mapped_span.data(), 0xcd, target_size);
    target.UploadMemory(initial, target_copies);
    std::vector<u8> expected(target_size, 0xcd);
    std::vector<ImageCopy> copies;
    for (s32 level = 0; level < 5; ++level) {
        const u32 offset = level < 4 ? 1 : 0;
        copies.push_back({.src_subresource = {level, 0, 3},
                          .dst_subresource = {level, 0, 3},
                          .src_offset = {},
                          .dst_offset = {s32(offset), s32(offset), 0},
                          .extent = raw_copies[level].image_extent});
        const u32 blocks = (raw_copies[level].image_extent.width + 3) / 4;
        const u32 target_w = target_copies[level].image_extent.width;
        for (u32 layer = 0; layer < 3; ++layer)
            for (u32 y = 0; y < blocks; ++y)
                std::memcpy(expected.data() + target_copies[level].buffer_offset +
                                ((size_t{layer} * target_w + y + offset) * target_w + offset) * 16,
                            raw.data() + raw_copies[level].buffer_offset +
                                (size_t{layer} * blocks + y) * blocks * 16,
                            blocks * 16);
    }
    // Exercise the actual dispatcher, representation preservation and resource
    // state tracking.
    runtime.CopyImage(target, source, copies);
    auto readback = runtime.DownloadStagingBuffer(target_size);
    target.DownloadMemory(readback, target_copies);
    for (u64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        messages->GetMessage(i, nullptr, &size);
        std::vector<u8> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        messages->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            std::cerr << message->pDescription << '\n';
    }
    scheduler.Finish();
    assert(std::memcmp(readback.mapped_span.data(), expected.data(), target_size) == 0);

    // Partial copy between decoded arrays must update the original blocks on the
    // GPU too.
    Image second{runtime, source_info, 0, 0};
    std::vector<u8> second_raw(raw_size, 0x31);
    auto second_decoded = runtime.UploadStagingBuffer(second.converted_size_bytes);
    auto second_copies = raw_copies;
    second.PreserveGuestBlocks(second_raw, raw_copies);
    ConvertImage(second_raw, source_info, second_decoded.mapped_span, second_copies);
    second.UploadMemory(second_decoded, second_copies);
    const ImageCopy partial{.src_subresource = {0, 1, 1},
                            .dst_subresource = {0, 2, 1},
                            .src_offset = {4, 8, 0},
                            .dst_offset = {8, 4, 0},
                            .extent = {8, 4, 1}};
    runtime.CopyImage(second, source, std::span{&partial, 1});
    assert(second.GuestBlocks());
    const ImageCopy export_partial{.src_subresource = {0, 2, 1},
                                   .dst_subresource = {0, 0, 1},
                                   .src_offset = {8, 4, 0},
                                   .dst_offset = {7, 6, 0},
                                   .extent = {8, 4, 1}};
    runtime.CopyImage(target, second, std::span{&export_partial, 1});
    std::memcpy(expected.data() + (6 * 16 + 7) * 16, raw.data() + ((1 * 4 + 2) * 4 + 1) * 16, 32);
    auto second_readback = runtime.DownloadStagingBuffer(target_size);
    target.DownloadMemory(second_readback, target_copies);
    scheduler.Finish();
    assert(std::memcmp(second_readback.mapped_span.data(), expected.data(), target_size) == 0);

    // Logged Game A alias geometry: 128x128 BC3, 48 layers, mips 4/5/6 exported to
    // UINT128 4x2/4x1 images. Extents are 2x2/1x1 BLOCKS, not decoded texels.
    auto alias_info = source_info;
    alias_info.size = {128, 128, 1};
    alias_info.resources = {7, 48};
    Image alias_source{runtime, alias_info, 0, 0};
    size_t alias_bytes{};
    const auto alias_layout = Layout(alias_info, alias_bytes);
    std::vector<u8> alias_raw(alias_bytes);
    for (size_t i = 0; i < alias_bytes; ++i) alias_raw[i] = u8(i * 37 + i / 16);
    alias_source.PreserveGuestBlocks(alias_raw, alias_layout);
    auto alias_upload = runtime.UploadStagingBuffer(alias_source.converted_size_bytes);
    auto alias_converted = alias_layout;
    ConvertImage(alias_raw, alias_info, alias_upload.mapped_span, alias_converted);
    alias_source.UploadMemory(alias_upload, alias_converted);
    for (s32 level = 4; level <= 6; ++level) {
        const u32 count = level == 4 ? 2 : 1;
        auto alias_target_info = target_info;
        alias_target_info.size = {4, count, 1};
        alias_target_info.resources = {1, 1};
        Image alias_target{runtime, alias_target_info, 0, 0};
        size_t output_bytes{};
        const auto output_layout = Layout(alias_target_info, output_bytes);
        auto sentinel = runtime.UploadStagingBuffer(output_bytes);
        std::memset(sentinel.mapped_span.data(), 0xcd, output_bytes);
        for (s32 layer = 6; layer < 30; ++layer) {
            alias_target.UploadMemory(sentinel, output_layout);
            const ImageCopy alias_copy{.src_subresource = {level, layer, 1},
                                       .dst_subresource = {0, 0, 1},
                                       .src_offset = {}, .dst_offset = {},
                                       .extent = {count, count, 1},
                                       .extent_in_blocks = true};
            runtime.CopyImage(alias_target, alias_source, std::span{&alias_copy, 1});
            auto output = runtime.DownloadStagingBuffer(output_bytes);
            alias_target.DownloadMemory(output, output_layout);
            scheduler.Finish();
            std::vector<u8> expected_alias(output_bytes, 0xcd);
            const size_t input = alias_layout[level].buffer_offset + size_t(layer) * count * count * 16;
            for (u32 row = 0; row < count; ++row)
                std::memcpy(expected_alias.data() + row * 4 * 16,
                            alias_raw.data() + input + row * count * 16, count * 16);
            assert(std::memcmp(output.mapped_span.data(), expected_alias.data(), output_bytes) == 0);
        }
    }

    ShaderCompiler compiler;
    assert(compiler.IsAvailable());
    DescriptorRing ring{device.Get(), scheduler, 4096};
    SamplerHeap sampler_heap{device.Get(), scheduler};
    BlitImageHelper helper{device, scheduler, compiler, ring, sampler_heap, samplers};
    assert(helper.IsAvailable());
    runtime.SetBlitHelper(&helper);
    // Endpoint-only blocks make the visual comparison exact across hardware BC
    // interpolation precision. Arbitrary original blocks were already checked
    // bit-for-bit above.
    std::array<u8, 32> incoming_blocks{};
    for (unsigned block = 0; block < 2; ++block) {
        incoming_blocks[block * 16] = incoming_blocks[block * 16 + 1] = 255;
        incoming_blocks[block * 16 + 8] = incoming_blocks[block * 16 + 10] = 0xe0;
        incoming_blocks[block * 16 + 9] = incoming_blocks[block * 16 + 11] = 0x07;
    }
    const BufferImageCopy incoming_upload{.buffer_offset = 0,
                                          .buffer_size = 32,
                                          .buffer_row_length = 2,
                                          .buffer_image_height = 1,
                                          .image_subresource = {0, 0, 1},
                                          .image_offset = {7, 6, 0},
                                          .image_extent = {2, 1, 1}};
    auto incoming_staging = runtime.UploadStagingBuffer(32);
    std::memcpy(incoming_staging.mapped_span.data(), incoming_blocks.data(), 32);
    target.UploadMemory(incoming_staging, std::span{&incoming_upload, 1});
    std::memcpy(expected.data() + (6 * 16 + 7) * 16, incoming_blocks.data(), 32);
    const ImageCopy incoming{.src_subresource = {0, 0, 1},
                             .dst_subresource = {1, 0, 1},
                             .src_offset = {7, 6, 0},
                             .dst_offset = {},
                             .extent = {2, 1, 1}};
    runtime.CopyImage(second, target, std::span{&incoming, 1});
    assert(second.GuestBlocks());
    const ImageCopy outgoing{.src_subresource = {1, 0, 1},
                             .dst_subresource = {0, 0, 1},
                             .src_offset = {},
                             .dst_offset = {10, 10, 0},
                             .extent = {8, 4, 1}};
    runtime.CopyImage(target, second, std::span{&outgoing, 1});
    std::memcpy(expected.data() + (10 * 16 + 10) * 16, incoming_blocks.data(), 32);
    auto inverse_readback = runtime.DownloadStagingBuffer(target_size);
    target.DownloadMemory(inverse_readback, target_copies);
    scheduler.Finish();
    assert(std::memcmp(inverse_readback.mapped_span.data(), expected.data(), target_size) == 0);
    // Compare the decoded representation as well, including every untouched
    // mip/layer/texel.
    std::memcpy(second_raw.data() + ((2 * 4 + 1) * 4 + 2) * 16,
                raw.data() + ((1 * 4 + 2) * 4 + 1) * 16, 32);
    std::memcpy(second_raw.data() + raw_copies[1].buffer_offset, incoming_blocks.data(), 32);
    std::vector<u8> expected_decoded(second.converted_size_bytes);
    auto reference_copies = raw_copies;
    ConvertImage(second_raw, source_info, expected_decoded, reference_copies);
    auto color_readback = runtime.DownloadStagingBuffer(expected_decoded.size());
    second.DownloadMemory(color_readback, reference_copies);
    scheduler.Finish();
    assert(std::memcmp(color_readback.mapped_span.data(), expected_decoded.data(),
                       expected_decoded.size()) == 0);
    // Regression from Game A: an array born entirely from GPU-written blocks, with
    // no CPU upload.
    Image generated{runtime, source_info, 0, 0};
    auto generated_incoming = incoming;
    generated_incoming.dst_subresource = {0, 1, 1};
    generated_incoming.dst_offset = {4, 8, 0};
    runtime.CopyImage(generated, target, std::span{&generated_incoming, 1});
    assert(generated.GuestBlocks());
    const ImageCopy generated_outgoing{.src_subresource = {0, 1, 1},
                                       .dst_subresource = {0, 0, 1},
                                       .src_offset = {4, 8, 0},
                                       .dst_offset = {12, 12, 0},
                                       .extent = {8, 4, 1}};
    assert(runtime.CopyDecodedBlocks(target, generated, std::span{&generated_outgoing, 1}));
    auto uncovered = generated_outgoing;
    uncovered.src_offset = {0, 0, 0};
    assert(!runtime.CopyDecodedBlocks(target, generated, std::span{&uncovered, 1}));
    uncovered = generated_outgoing;
    uncovered.src_subresource.base_layer = 2;
    assert(!runtime.CopyDecodedBlocks(target, generated, std::span{&uncovered, 1}));
    uncovered = generated_outgoing;
    uncovered.src_subresource.base_level = 1;
    uncovered.src_offset = {0, 0, 0};
    assert(!runtime.CopyDecodedBlocks(target, generated, std::span{&uncovered, 1}));
    std::memcpy(expected.data() + (12 * 16 + 12) * 16, incoming_blocks.data(), 32);
    auto generated_readback = runtime.DownloadStagingBuffer(target_size);
    target.DownloadMemory(generated_readback, target_copies);
    scheduler.Finish();
    assert(std::memcmp(generated_readback.mapped_span.data(), expected.data(), target_size) == 0);
    ImageInfo mk_blocks_info;
    mk_blocks_info.format = PixelFormat::R32G32B32A32_UINT;
    mk_blocks_info.type = ImageType::e2D;
    mk_blocks_info.size = {32, 32, 1};
    mk_blocks_info.resources = {1, 6};
    Image mk_blocks{runtime, mk_blocks_info, 0, 0};
    Image mk_result{runtime, mk_blocks_info, 0, 0};
    auto mk_info = mk_blocks_info;
    mk_info.format = PixelFormat::BC3_UNORM;
    mk_info.size = {128, 128, 1};
    Image mk_array{runtime, mk_info, 0, 0};
    size_t mk_size{};
    const auto mk_uploads = Layout(mk_blocks_info, mk_size);
    auto mk_data = runtime.UploadStagingBuffer(mk_size);
    for (size_t i = 0; i < mk_size; ++i)
        mk_data.mapped_span[i] = u8(i * 71 + i / 16 * 29);
    std::vector<u8> mk_expected(mk_data.mapped_span.begin(), mk_data.mapped_span.begin() + mk_size);
    mk_blocks.UploadMemory(mk_data, mk_uploads);
    const ImageCopy mk_input{.src_subresource = {0, 0, 6},
                             .dst_subresource = {0, 0, 6},
                             .src_offset = {},
                             .dst_offset = {},
                             .extent = {32, 32, 1}};
    runtime.CopyImage(mk_array, mk_blocks, std::span{&mk_input, 1});
    assert(mk_array.GuestBlocks());
    const auto *const original_resource = mk_array.GuestBlocks()->Handle();
    auto mk_output = mk_input;
    mk_output.extent = {128, 128, 1};
    for (unsigned repeat = 0; repeat < 192; ++repeat) {
        runtime.CopyImage(mk_result, mk_array, std::span{&mk_output, 1});
        assert(mk_array.GuestBlocks()->Handle() == original_resource);
    }
    auto mk_readback = runtime.DownloadStagingBuffer(mk_size);
    mk_result.DownloadMemory(mk_readback, mk_uploads);
    scheduler.Finish();
    assert(std::memcmp(mk_readback.mapped_span.data(), mk_expected.data(), mk_size) == 0);
    // Previously supported native-to-native BC/int copies and plain copies still work.
    Image native{runtime, source_info, 0, 0, true};
    auto native_data = runtime.UploadStagingBuffer(raw_size);
    std::memcpy(native_data.mapped_span.data(), raw.data(), raw_size);
    native.UploadMemory(native_data, raw_copies);
    runtime.CopyImage(target, native, copies);
    Image clone{runtime, target_info, 0, 0};
    std::vector<ImageCopy> plain_copies;
    for (const auto& upload : target_copies) {
        plain_copies.push_back({.src_subresource = upload.image_subresource,
            .dst_subresource = upload.image_subresource, .src_offset = {}, .dst_offset = {},
            .extent = upload.image_extent});
    }
    runtime.CopyImage(clone, target, plain_copies);
    auto clone_readback = runtime.DownloadStagingBuffer(target_size);
    clone.DownloadMemory(clone_readback, target_copies);
    scheduler.Finish();
    assert(std::memcmp(clone_readback.mapped_span.data(), expected.data(), target_size) == 0);
    runtime.SetBlitHelper(nullptr);

    // A sub-block decoded copy has no exact compressed encoding; invalidate that
    // representation.
    auto unaligned = partial;
    unaligned.src_offset.x = 1;
    unaligned.extent.width = 1;
    runtime.CopyImage(second, source, std::span{&unaligned, 1});
    assert(!second.GuestBlocks());

    auto bad = copies.front();
    bad.src_offset.x = 1;
    assert(!runtime.CopyDecodedBlocks(target, source, std::span{&bad, 1}));
    bad = copies.front();
    bad.dst_subresource.base_layer = 3;
    assert(!runtime.CopyDecodedBlocks(target, source, std::span{&bad, 1}));
    bad = copies.front();
    bad.src_subresource.base_level = 5;
    assert(!runtime.CopyDecodedBlocks(target, source, std::span{&bad, 1}));
    auto incompatible_info = target_info;
    incompatible_info.format = PixelFormat::R16G16B16A16_FLOAT;
    Image incompatible{runtime, incompatible_info, 0, 0};
    assert(!runtime.CopyDecodedBlocks(incompatible, source, copies));
    // A GPU write without original blocks must invalidate, never reuse stale
    // compressed bytes.
    source.Transition(D3D12_RESOURCE_STATE_RENDER_TARGET);
    assert(!source.GuestBlocks());
    assert(!runtime.CopyDecodedBlocks(target, source, copies));
    scheduler.Finish();
    ThrowIfFailed(device.Get()->GetDeviceRemovedReason(), "test device removed");
    for (u64 i = 0; i < messages->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{};
        messages->GetMessage(i, nullptr, &size);
        std::vector<u8> storage(size);
        auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
        messages->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::cerr << message->pDescription << '\n';
            return 1;
        }
    }
    std::cout << "PASS: BC axis bounds, exact GPU BC3 blocks/arrays/offsets/5 mips, "
                 "inverse GPU decode and block preservation, sentinel preservation, "
                 "invalid copies and stale representation; D3D12 debug clean\n";
} catch (const std::exception &error) {
    std::cerr << "BC copy gate exception: " << error.what() << '\n';
    return 1;
}
