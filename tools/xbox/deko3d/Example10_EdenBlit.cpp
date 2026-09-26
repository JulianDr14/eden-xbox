// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Phase 4.4 test payload for the D3D12 backend (not one of the official deko3d examples): exercises
// the 2D engine blits and the clears D3D12 has to draw, with a picture that is easy to check in
// frame.bmp. Built by tools/xbox/build-deko3d-examples.ps1 as example 10, on the examples'
// SampleFramework.
//
// Expected picture (1280x720):
//  - background: left half cyan (a clear with only the green mask over blue), right half blue
//  - box at (100,100) 256x256: a 2x2 texture scaled with nearest filtering, four quadrants:
//    top-left red, top-right green, bottom-left blue, bottom-right white
//  - box at (700,100) 256x256: the same texture with linear filtering (a smooth gradient between
//    the four colors)
//  - box at (1100,100) 128x128: nearest and flipped vertically: top-left blue, top-right white,
//    bottom-left red, bottom-right green
// The boxes are powers of two: Eden's Fermi2D truncates the source region it derives from the
// fixed-point step (2/400 * 400 gives 1.99999, so one texel), a limitation of every backend.
// The partial stencil clear has no visible effect; it only has to run without errors.

#include "SampleFramework/CApplication.h"
#include "SampleFramework/CMemPool.h"

#include <array>
#include <cstring>
#include <optional>

class CExample10 final : public CApplication
{
    static constexpr unsigned NumFramebuffers = 2;
    static constexpr uint32_t FramebufferWidth = 1280;
    static constexpr uint32_t FramebufferHeight = 720;
    static constexpr unsigned StaticCmdSize = 0x1000;

    dk::UniqueDevice device;
    dk::UniqueQueue queue;

    std::optional<CMemPool> pool_images;
    std::optional<CMemPool> pool_data;

    dk::UniqueCmdBuf cmdbuf;

    CMemPool::Handle source_mem;
    dk::Image source;

    CMemPool::Handle depth_mem;
    dk::Image depthBuffer;

    CMemPool::Handle framebuffers_mem[NumFramebuffers];
    dk::Image framebuffers[NumFramebuffers];
    DkCmdList framebuffer_cmdlists[NumFramebuffers];
    DkCmdList render_cmdlists[NumFramebuffers];
    dk::UniqueSwapchain swapchain;

public:
    CExample10()
    {
        device = dk::DeviceMaker{}.create();
        queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics).create();

        pool_images.emplace(device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 16*1024*1024);
        pool_data.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 1*1024*1024);

        cmdbuf = dk::CmdBufMaker{device}.create();
        CMemPool::Handle cmdmem = pool_data->allocate(StaticCmdSize);
        cmdbuf.addMemory(cmdmem.getMemBlock(), cmdmem.getOffset(), cmdmem.getSize());

        createSource();
        createFramebufferResources();
    }

    ~CExample10()
    {
        queue.waitIdle();
        cmdbuf.clear();
        swapchain.destroy();
        for (unsigned i = 0; i < NumFramebuffers; i ++)
            framebuffers_mem[i].destroy();
        depth_mem.destroy();
        source_mem.destroy();
    }

    void createSource()
    {
        // A 2x2 RGBA8 texture: red, green / blue, white (rows top to bottom).
        dk::ImageLayout layout;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(2, 2)
            .initialize(layout);
        source_mem = pool_images->allocate(layout.getSize(), layout.getAlignment());
        source.initialize(layout, source_mem.getMemBlock(), source_mem.getOffset());

        static constexpr uint32_t Texels[4] = {
            0xFF0000FF, 0xFF00FF00, // red, green (ABGR in memory: R is the low byte)
            0xFFFF0000, 0xFFFFFFFF, // blue, white
        };
        CMemPool::Handle upload = pool_data->allocate(sizeof(Texels), DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
        std::memcpy(upload.getCpuAddr(), Texels, sizeof(Texels));

        dk::ImageView view{source};
        cmdbuf.copyBufferToImage({ upload.getGpuAddr() }, view, { 0, 0, 0, 2, 2, 1 });
        queue.submitCommands(cmdbuf.finishList());
        queue.waitIdle();
        cmdbuf.clear(); // keeps its memory for the static lists
        upload.destroy();
    }

    void createFramebufferResources()
    {
        dk::ImageLayout layout_depthbuffer;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_HwCompression)
            .setFormat(DkImageFormat_Z24S8)
            .setDimensions(FramebufferWidth, FramebufferHeight)
            .initialize(layout_depthbuffer);
        depth_mem = pool_images->allocate(layout_depthbuffer.getSize(), layout_depthbuffer.getAlignment());
        depthBuffer.initialize(layout_depthbuffer, depth_mem.getMemBlock(), depth_mem.getOffset());

        dk::ImageLayout layout_framebuffer;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(FramebufferWidth, FramebufferHeight)
            .initialize(layout_framebuffer);

        std::array<DkImage const*, NumFramebuffers> fb_array;
        for (unsigned i = 0; i < NumFramebuffers; i ++)
        {
            framebuffers_mem[i] = pool_images->allocate(layout_framebuffer.getSize(), layout_framebuffer.getAlignment());
            framebuffers[i].initialize(layout_framebuffer, framebuffers_mem[i].getMemBlock(), framebuffers_mem[i].getOffset());

            dk::ImageView colorTarget{ framebuffers[i] }, depthTarget{ depthBuffer };
            cmdbuf.bindRenderTargets(&colorTarget, &depthTarget);
            framebuffer_cmdlists[i] = cmdbuf.finishList();

            fb_array[i] = &framebuffers[i];
        }
        swapchain = dk::SwapchainMaker{device, nwindowGetDefault(), fb_array}.create();

        for (unsigned i = 0; i < NumFramebuffers; i ++)
            render_cmdlists[i] = recordFrame(framebuffers[i]);
    }

    DkCmdList recordFrame(dk::Image& framebuffer)
    {
        // Clears: blue everywhere, then only green over the left half (cyan).
        cmdbuf.setScissors(0, { { 0, 0, FramebufferWidth, FramebufferHeight } });
        cmdbuf.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 1.0f, 1.0f);
        cmdbuf.setScissors(0, { { 0, 0, FramebufferWidth/2, FramebufferHeight } });
        cmdbuf.clearColor(0, DkColorMask_G, 1.0f, 1.0f, 1.0f, 1.0f);
        cmdbuf.setScissors(0, { { 0, 0, FramebufferWidth, FramebufferHeight } });
        // Depth plus the low four stencil bits only.
        cmdbuf.clearDepthStencil(true, 1.0f, 0x0F, 0x05);

        // 2D engine blits of the 2x2 texture into the framebuffer.
        dk::ImageView src{source}, dst{framebuffer};
        cmdbuf.blitImage(src, { 0, 0, 0, 2, 2, 1 }, dst, { 100, 100, 0, 256, 256, 1 },
                         DkBlitFlag_FilterNearest);
        cmdbuf.blitImage(src, { 0, 0, 0, 2, 2, 1 }, dst, { 700, 100, 0, 256, 256, 1 },
                         DkBlitFlag_FilterLinear);
        cmdbuf.blitImage(src, { 0, 0, 0, 2, 2, 1 }, dst, { 1100, 100, 0, 128, 128, 1 },
                         DkBlitFlag_FilterNearest | DkBlitFlag_FlipY);
        return cmdbuf.finishList();
    }

    bool onFrame(u64 ns) override
    {
        int slot = queue.acquireImage(swapchain);
        queue.submitCommands(framebuffer_cmdlists[slot]);
        queue.submitCommands(render_cmdlists[slot]);
        queue.presentImage(swapchain, slot);
        return true;
    }
};

void Example10(void)
{
    CExample10 app;
    app.run();
}
