// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_d3d12/d3d12_rasterizer.h"
#include "video_core/surface.h"

namespace D3D12 {

AccelerateDMA::AccelerateDMA(BufferCache& buffers, TextureCache& textures)
    : buffer_cache{buffers}, texture_cache{textures} {}
bool AccelerateDMA::BufferCopy(GPUVAddr src, GPUVAddr dst, u64 amount) {
    std::scoped_lock lock{buffer_cache.mutex}; return buffer_cache.DMACopy(src, dst, amount);
}
bool AccelerateDMA::BufferClear(GPUVAddr address, u64 amount, u32 value) {
    std::scoped_lock lock{buffer_cache.mutex}; return buffer_cache.DMAClear(address, amount, value);
}
template <bool IS_UPLOAD>
bool AccelerateDMA::BufferImageCopy(const Tegra::DMA::ImageCopy& info,
                                    const Tegra::DMA::BufferOperand& buffer_operand,
                                    const Tegra::DMA::ImageOperand& image_operand) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) return false;
    const u32 size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    constexpr auto sync = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto op = IS_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                              : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] = buffer_cache.ObtainBuffer(buffer_operand.address, size, sync, op);
    // The texture code copies to and from the raw resource and expects it in COMMON (it relies
    // on implicit promotion), so end any state a draw gave it in this command list.
    buffer->Transition(D3D12_RESOURCE_STATE_COMMON);
    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        info, buffer_operand, image_operand, image_id, IS_UPLOAD);
    const std::span copies{&copy, 1};
    if constexpr (IS_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copies);
    } else {
        if (offset % VideoCore::Surface::BytesPerBlock(image->info.format)) return false;
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copies,
                                              buffer_operand.address, size);
    }
    return true;
}
bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& i,
                                  const Tegra::DMA::ImageOperand& image,
                                  const Tegra::DMA::BufferOperand& buffer) {
    return BufferImageCopy<false>(i, buffer, image);
}
bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& i,
                                  const Tegra::DMA::BufferOperand& buffer,
                                  const Tegra::DMA::ImageOperand& image) {
    return BufferImageCopy<true>(i, buffer, image);
}

} // namespace D3D12
