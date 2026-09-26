// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// ExecuteIndirect test payload for the D3D12 backend (not one of the official deko3d examples).
// Example 09 with its dispatch and draw made indirect: a one-thread compute shader
// (indirect_args.glsl) writes the arguments, so they are GPU-written memory, then
// dispatchComputeIndirect runs the sine wave generator (sinewave.glsl, which reads
// gl_NumWorkGroups) and drawIndirect draws the line strip. Built by
// tools/xbox/build-deko3d-examples.ps1 as example 11.
//
// Expected picture (1280x720): a static sine wave over the whole width on black, magenta on the
// left fading to green on the right, as example 09 at its first frame. A wrong group count moves
// or shortens the wave (sinewave.glsl spreads it with gl_NumWorkGroups); no wave means the
// dispatch or the draw was lost.

#include "SampleFramework/CApplication.h"
#include "SampleFramework/CMemPool.h"
#include "SampleFramework/CShader.h"

#include <array>
#include <cstring>
#include <optional>

namespace
{
    struct Vertex
    {
        float position[4];
        float color[4];
    };

    constexpr std::array VertexAttribState =
    {
        DkVtxAttribState{ 0, 0, offsetof(Vertex, position), DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0 },
        DkVtxAttribState{ 0, 0, offsetof(Vertex, color),    DkVtxAttribSize_4x32, DkVtxAttribType_Float, 0 },
    };

    constexpr std::array VertexBufferState =
    {
        DkVtxBufferState{ sizeof(Vertex), 0 },
    };

    // sinewave.glsl's uniform block
    struct GeneratorParams
    {
        float colorA[4];
        float colorB[4];
        float offset;
        float scale;
        float padding[2];
    };

    // indirect_args.glsl's uniform block
    struct ArgsParams
    {
        uint32_t counts[4];
    };
}

class CExample11 final : public CApplication
{
    static constexpr unsigned NumFramebuffers = 2;
    static constexpr uint32_t FramebufferWidth = 1280;
    static constexpr uint32_t FramebufferHeight = 720;
    static constexpr unsigned StaticCmdSize = 0x10000;
    static constexpr unsigned NumVertices = 256;

    dk::UniqueDevice device;
    dk::UniqueQueue queue;

    std::optional<CMemPool> pool_images;
    std::optional<CMemPool> pool_code;
    std::optional<CMemPool> pool_data;

    dk::UniqueCmdBuf cmdbuf;

    CShader argsShader;
    CShader computeShader;
    CShader vertexShader;
    CShader fragmentShader;

    CMemPool::Handle argsUniformBuffer;
    CMemPool::Handle paramsUniformBuffer;
    CMemPool::Handle argsBuffer;
    CMemPool::Handle vertexBuffer;

    CMemPool::Handle framebuffers_mem[NumFramebuffers];
    dk::Image framebuffers[NumFramebuffers];
    DkCmdList framebuffer_cmdlists[NumFramebuffers];
    dk::UniqueSwapchain swapchain;

    DkCmdList compute_cmdlist, render_cmdlist;

public:
    CExample11()
    {
        device = dk::DeviceMaker{}.create();
        queue = dk::QueueMaker{device}.setFlags(DkQueueFlags_Graphics | DkQueueFlags_Compute).create();

        pool_images.emplace(device, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 16*1024*1024);
        pool_code.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code, 128*1024);
        pool_data.emplace(device, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 1*1024*1024);

        cmdbuf = dk::CmdBufMaker{device}.create();
        CMemPool::Handle cmdmem = pool_data->allocate(StaticCmdSize);
        cmdbuf.addMemory(cmdmem.getMemBlock(), cmdmem.getOffset(), cmdmem.getSize());

        argsShader.load(*pool_code, "romfs:/shaders/indirect_args.dksh");
        computeShader.load(*pool_code, "romfs:/shaders/sinewave.dksh");
        vertexShader.load(*pool_code, "romfs:/shaders/basic_vsh.dksh");
        fragmentShader.load(*pool_code, "romfs:/shaders/color_fsh.dksh");

        // Constant inputs, written once by the CPU.
        const ArgsParams args{ { NumVertices/32, NumVertices, 0, 0 } };
        argsUniformBuffer = pool_data->allocate(sizeof(args), DK_UNIFORM_BUF_ALIGNMENT);
        std::memcpy(argsUniformBuffer.getCpuAddr(), &args, sizeof(args));

        const GeneratorParams params{ { 1.0f, 0.0f, 1.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f }, 0.0f, 1.0f, { 0.0f, 0.0f } };
        paramsUniformBuffer = pool_data->allocate(sizeof(params), DK_UNIFORM_BUF_ALIGNMENT);
        std::memcpy(paramsUniformBuffer.getCpuAddr(), &params, sizeof(params));

        // Written by indirect_args.glsl only: DkDispatchIndirectData at 0, DkDrawIndirectData at 16.
        argsBuffer = pool_data->allocate(32, DK_UNIFORM_BUF_ALIGNMENT);
        std::memset(argsBuffer.getCpuAddr(), 0, 32);

        vertexBuffer = pool_data->allocate(sizeof(Vertex)*NumVertices, alignof(Vertex));

        createFramebufferResources();
    }

    ~CExample11()
    {
        queue.waitIdle();
        cmdbuf.clear();
        swapchain.destroy();
        for (unsigned i = 0; i < NumFramebuffers; i ++)
            framebuffers_mem[i].destroy();
        vertexBuffer.destroy();
        argsBuffer.destroy();
        paramsUniformBuffer.destroy();
        argsUniformBuffer.destroy();
    }

    void createFramebufferResources()
    {
        dk::ImageLayout layout_framebuffer;
        dk::ImageLayoutMaker{device}
            .setFlags(DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression)
            .setFormat(DkImageFormat_RGBA8_Unorm)
            .setDimensions(FramebufferWidth, FramebufferHeight)
            .initialize(layout_framebuffer);

        std::array<DkImage const*, NumFramebuffers> fb_array;
        for (unsigned i = 0; i < NumFramebuffers; i ++)
        {
            framebuffers_mem[i] = pool_images->allocate(layout_framebuffer.getSize(), layout_framebuffer.getAlignment());
            framebuffers[i].initialize(layout_framebuffer, framebuffers_mem[i].getMemBlock(), framebuffers_mem[i].getOffset());

            dk::ImageView colorTarget{ framebuffers[i] };
            cmdbuf.bindRenderTargets(&colorTarget);
            framebuffer_cmdlists[i] = cmdbuf.finishList();

            fb_array[i] = &framebuffers[i];
        }
        swapchain = dk::SwapchainMaker{device, nwindowGetDefault(), fb_array}.create();

        recordStaticCommands();
    }

    void recordStaticCommands()
    {
        // 1. The GPU writes the indirect arguments.
        cmdbuf.bindShaders(DkStageFlag_Compute, { argsShader });
        cmdbuf.bindUniformBuffer(DkStage_Compute, 0, argsUniformBuffer.getGpuAddr(), argsUniformBuffer.getSize());
        cmdbuf.bindStorageBuffer(DkStage_Compute, 0, argsBuffer.getGpuAddr(), argsBuffer.getSize());
        cmdbuf.dispatchCompute(1, 1, 1);
        cmdbuf.barrier(DkBarrier_Full, DkInvalidateFlags_L2Cache);

        // 2. Indirect dispatch of the vertex generator.
        cmdbuf.bindShaders(DkStageFlag_Compute, { computeShader });
        cmdbuf.bindUniformBuffer(DkStage_Compute, 0, paramsUniformBuffer.getGpuAddr(), paramsUniformBuffer.getSize());
        cmdbuf.bindStorageBuffer(DkStage_Compute, 0, vertexBuffer.getGpuAddr(), vertexBuffer.getSize());
        cmdbuf.dispatchComputeIndirect(argsBuffer.getGpuAddr());
        cmdbuf.barrier(DkBarrier_Primitives, 0);
        compute_cmdlist = cmdbuf.finishList();

        // 3. Indirect draw of the generated line strip.
        dk::RasterizerState rasterizerState;
        dk::ColorState colorState;
        dk::ColorWriteState colorWriteState;
        dk::BlendState blendState;
        cmdbuf.setViewports(0, { { 0.0f, 0.0f, FramebufferWidth, FramebufferHeight, 0.0f, 1.0f } });
        cmdbuf.setScissors(0, { { 0, 0, FramebufferWidth, FramebufferHeight } });
        cmdbuf.clearColor(0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 0.0f);
        cmdbuf.bindShaders(DkStageFlag_GraphicsMask, { vertexShader, fragmentShader });
        cmdbuf.bindRasterizerState(rasterizerState);
        cmdbuf.bindColorState(colorState);
        cmdbuf.bindColorWriteState(colorWriteState);
        cmdbuf.bindBlendStates(0, blendState);
        cmdbuf.bindVtxBuffer(0, vertexBuffer.getGpuAddr(), vertexBuffer.getSize());
        cmdbuf.bindVtxAttribState(VertexAttribState);
        cmdbuf.bindVtxBufferState(VertexBufferState);
        cmdbuf.drawIndirect(DkPrimitive_LineStrip, argsBuffer.getGpuAddr() + 16);
        render_cmdlist = cmdbuf.finishList();
    }

    bool onFrame(u64 ns) override
    {
        queue.submitCommands(compute_cmdlist);
        int slot = queue.acquireImage(swapchain);
        queue.submitCommands(framebuffer_cmdlists[slot]);
        queue.submitCommands(render_cmdlist);
        queue.presentImage(swapchain, slot);
        return true;
    }
};

void Example11(void)
{
    CExample11 app;
    app.run();
}
