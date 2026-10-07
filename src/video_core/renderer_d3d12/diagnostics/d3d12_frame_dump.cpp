// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/renderer_d3d12.h"
#include "video_core/renderer_d3d12/renderer_d3d12_internal.h"

// Presented frames read back and written to disk (frame dumps).

namespace D3D12 {

using namespace PresentDetail;

StagingBufferRef RendererD3D12::RecordFrameReadback(ID3D12Resource* image) {
    StagingBufferRef readback = staging_pool.Request(upload_size, MemoryUsage::Download, true);
    ID3D12GraphicsCommandList* const cmd = scheduler.CommandList();
    D3D12_RESOURCE_BARRIER barrier =
        TransitionBarrier(image, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->ResourceBarrier(1, &barrier);
    const D3D12_TEXTURE_COPY_LOCATION src{
        .pResource = image,
        .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
        .SubresourceIndex = 0,
    };
    const D3D12_TEXTURE_COPY_LOCATION dst = StagingSource(readback, footprint);
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    cmd->ResourceBarrier(1, &barrier);
    return readback;
}

void RendererD3D12::WriteFrameDump(StagingBufferRef& readback, u32 dump_index) {
    scheduler.Finish();
    const u32 width = swapchain.Width();
    const u32 height = swapchain.Height();
    const u32 row_bytes = width * 4;
    const u32 image_bytes = row_bytes * height;
    // 32-bit BGRA bottom-up BMP: BITMAPFILEHEADER + BITMAPINFOHEADER, written by hand.
    std::vector<u8> file(54 + static_cast<size_t>(image_bytes));
    const auto put32 = [&file](size_t offset, u32 value) { std::memcpy(&file[offset], &value, 4); };
    const auto put16 = [&file](size_t offset, u16 value) { std::memcpy(&file[offset], &value, 2); };
    file[0] = 'B';
    file[1] = 'M';
    put32(2, static_cast<u32>(file.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, height);
    put16(26, 1);
    put16(28, 32);
    put32(34, image_bytes);
    for (u32 y = 0; y < height; ++y) {
        const u8* const src = readback.mapped_span.data() +
                              static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        u8* const dst = file.data() + 54 + static_cast<size_t>(height - 1 - y) * row_bytes;
        for (u32 x = 0; x < width; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2];
            dst[x * 4 + 1] = src[x * 4 + 1];
            dst[x * 4 + 2] = src[x * 4 + 0];
            dst[x * 4 + 3] = src[x * 4 + 3];
        }
    }
    staging_pool.FreeDeferred(readback);
    const std::filesystem::path path =
        Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) /
        (dump_index == 0 ? std::string("frame.bmp") : fmt::format("frame_{}.bmp", dump_index));
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    if (out) {
        LOG_INFO(Render, "D3D12: presented frame dumped to {} ({}x{})", path.string(), width,
                 height);
    } else {
        LOG_WARNING(Render, "D3D12: could not write the frame dump {}", path.string());
    }
}

} // namespace D3D12
