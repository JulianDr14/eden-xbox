// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <fmt/format.h>

#include "video_core/renderer_d3d12/d3d12_command_recorder.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

namespace D3D12 {

using ListRecorder = CommandRecorder<ID3D12GraphicsCommandList*>;

/// The calls the renderer makes on its direct command list, with the list's own names and
/// parameters, recorded through a ListRecorder: on the recorder's worker they reach the list later,
/// so everything a call points to (barriers, viewports, copy locations...) is copied when it is
/// recorded. Descriptor handles are copied as handles: OMSetRenderTargets and the clears read the
/// RTV/DSV behind them when the worker records them (see CpuDescriptorAllocator).
///
/// A call missing here is added with the same signature, or recorded as is through Record.
class CommandStream {
public:
    using List = ID3D12GraphicsCommandList;

    explicit CommandStream(ListRecorder& recorder_) : recorder{recorder_} {}

    /// Any other use of the list, as func(list), in recording order.
    template <typename Func>
    void Record(Func&& func) {
        recorder.Record(std::forward<Func>(func));
    }

    // --- Barriers and pipeline state -------------------------------------------------------

    void ResourceBarrier(UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
        if (GpuDiagnosticsEnabled()) {
            for (UINT i = 0; i < count; ++i) {
                const auto& b = barriers[i];
                if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                    TraceGpuDetail(fmt::format("barrier resource={} sub={} before={:x} after={:x} flags={}",
                        static_cast<void*>(b.Transition.pResource), b.Transition.Subresource,
                        static_cast<u32>(b.Transition.StateBefore), static_cast<u32>(b.Transition.StateAfter),
                        static_cast<u32>(b.Flags)));
                } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
                    TraceGpuDetail(fmt::format("UAV barrier resource={}", static_cast<void*>(b.UAV.pResource)));
                } else if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                    TraceGpuDetail(fmt::format("alias barrier before={} after={}",
                        static_cast<void*>(b.Aliasing.pResourceBefore), static_cast<void*>(b.Aliasing.pResourceAfter)));
                }
            }
        }
        Array(barriers, count, [count](List* list, const D3D12_RESOURCE_BARRIER* copy) {
            list->ResourceBarrier(count, copy);
        });
    }
    void DiscardResource(ID3D12Resource* resource) {
        recorder.Record([resource](List* list) { list->DiscardResource(resource, nullptr); });
    }
    void SetPipelineState(ID3D12PipelineState* pipeline) {
        if (GpuDiagnosticsEnabled()) TraceGpuDetail(fmt::format("bind PSO={}", static_cast<void*>(pipeline)));
        recorder.Record([pipeline](List* list) { list->SetPipelineState(pipeline); });
    }
    void SetDescriptorHeaps(UINT count, ID3D12DescriptorHeap* const* heaps) {
        Array(heaps, count, [count](List* list, ID3D12DescriptorHeap* const* copy) {
            list->SetDescriptorHeaps(count, copy);
        });
    }
    void SetPredication(ID3D12Resource* buffer, UINT64 offset, D3D12_PREDICATION_OP op) {
        recorder.Record([=](List* list) { list->SetPredication(buffer, offset, op); });
    }
    void SetMarker(UINT metadata, const void* data, UINT size) {
        Array(static_cast<const std::byte*>(data), size,
              [metadata, size](List* list, const std::byte* copy) {
                  list->SetMarker(metadata, copy, size);
              });
    }

    // --- Root arguments ----------------------------------------------------------------------

    void SetGraphicsRootSignature(ID3D12RootSignature* signature) {
        recorder.Record([signature](List* list) { list->SetGraphicsRootSignature(signature); });
    }
    void SetComputeRootSignature(ID3D12RootSignature* signature) {
        recorder.Record([signature](List* list) { list->SetComputeRootSignature(signature); });
    }
    void SetGraphicsRootDescriptorTable(UINT param, D3D12_GPU_DESCRIPTOR_HANDLE table) {
        recorder.Record(
            [=](List* list) { list->SetGraphicsRootDescriptorTable(param, table); });
    }
    void SetComputeRootDescriptorTable(UINT param, D3D12_GPU_DESCRIPTOR_HANDLE table) {
        recorder.Record([=](List* list) { list->SetComputeRootDescriptorTable(param, table); });
    }
    void SetGraphicsRoot32BitConstants(UINT param, UINT count, const void* data, UINT offset) {
        Array(static_cast<const u32*>(data), count,
              [=](List* list, const u32* copy) {
                  list->SetGraphicsRoot32BitConstants(param, count, copy, offset);
              });
    }
    void SetComputeRoot32BitConstants(UINT param, UINT count, const void* data, UINT offset) {
        Array(static_cast<const u32*>(data), count,
              [=](List* list, const u32* copy) {
                  list->SetComputeRoot32BitConstants(param, count, copy, offset);
              });
    }
    void SetGraphicsRootConstantBufferView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
        recorder.Record(
            [=](List* list) { list->SetGraphicsRootConstantBufferView(param, address); });
    }
    void SetComputeRootConstantBufferView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
        recorder.Record(
            [=](List* list) { list->SetComputeRootConstantBufferView(param, address); });
    }
    void SetComputeRootShaderResourceView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
        recorder.Record(
            [=](List* list) { list->SetComputeRootShaderResourceView(param, address); });
    }
    void SetComputeRootUnorderedAccessView(UINT param, D3D12_GPU_VIRTUAL_ADDRESS address) {
        recorder.Record(
            [=](List* list) { list->SetComputeRootUnorderedAccessView(param, address); });
    }

    // --- Input assembler, rasterizer and output merger -------------------------------------

    void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY topology) {
        recorder.Record([topology](List* list) { list->IASetPrimitiveTopology(topology); });
    }
    void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* view) {
        if (GpuDiagnosticsEnabled()) TraceGpuDetail(fmt::format("index buffer VA={:x} bytes={} format={}",
            view ? view->BufferLocation : 0, view ? view->SizeInBytes : 0,
            view ? static_cast<u32>(view->Format) : 0));
        const std::optional<D3D12_INDEX_BUFFER_VIEW> copy =
            view ? std::optional{*view} : std::nullopt;
        recorder.Record([copy](List* list) { list->IASetIndexBuffer(copy ? &*copy : nullptr); });
    }
    void IASetVertexBuffers(UINT start, UINT count, const D3D12_VERTEX_BUFFER_VIEW* views) {
        if (GpuDiagnosticsEnabled() && views) {
            for (UINT i = 0; i < count; ++i) {
                TraceGpuDetail(fmt::format("vertex buffer slot={} VA={:x} bytes={} stride={}",
                    start + i, views[i].BufferLocation, views[i].SizeInBytes, views[i].StrideInBytes));
            }
        }
        if (views == nullptr) {
            recorder.Record(
                [=](List* list) { list->IASetVertexBuffers(start, count, nullptr); });
            return;
        }
        Array(views, count, [=](List* list, const D3D12_VERTEX_BUFFER_VIEW* copy) {
            list->IASetVertexBuffers(start, count, copy);
        });
    }
    void RSSetViewports(UINT count, const D3D12_VIEWPORT* viewports) {
        Array(viewports, count, [count](List* list, const D3D12_VIEWPORT* copy) {
            list->RSSetViewports(count, copy);
        });
    }
    void RSSetScissorRects(UINT count, const D3D12_RECT* rects) {
        Array(rects, count, [count](List* list, const D3D12_RECT* copy) {
            list->RSSetScissorRects(count, copy);
        });
    }
    void OMSetRenderTargets(UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* targets,
                            BOOL single_range, const D3D12_CPU_DESCRIPTOR_HANDLE* depth) {
        const std::optional<D3D12_CPU_DESCRIPTOR_HANDLE> depth_copy =
            depth ? std::optional{*depth} : std::nullopt;
        // A single range is one handle to the first of count consecutive descriptors.
        const UINT handles = targets == nullptr ? 0 : (single_range ? 1 : count);
        Array(targets, handles,
              [=](List* list, const D3D12_CPU_DESCRIPTOR_HANDLE* copy) {
                  list->OMSetRenderTargets(count, handles == 0 ? nullptr : copy, single_range,
                                           depth_copy ? &*depth_copy : nullptr);
              });
    }
    void OMSetBlendFactor(const FLOAT factor[4]) {
        const std::optional<std::array<FLOAT, 4>> copy =
            factor ? std::optional{std::array{factor[0], factor[1], factor[2], factor[3]}}
                   : std::nullopt;
        recorder.Record(
            [copy](List* list) { list->OMSetBlendFactor(copy ? copy->data() : nullptr); });
    }
    void OMSetStencilRef(UINT reference) {
        recorder.Record([reference](List* list) { list->OMSetStencilRef(reference); });
    }

    // --- Draws, dispatches and clears --------------------------------------------------------

    void DrawInstanced(UINT vertices, UINT instances, UINT first_vertex, UINT first_instance) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("DrawInstanced vertices={} instances={} first={} instance={}",
            vertices, instances, first_vertex, first_instance)) : 0;
        Breadcrumb(token, false);
        recorder.Record([=](List* list) {
            list->DrawInstanced(vertices, instances, first_vertex, first_instance);
        });
        Breadcrumb(token, true);
    }
    void DrawIndexedInstanced(UINT indices, UINT instances, UINT first_index, INT base_vertex,
                              UINT first_instance) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("DrawIndexed indices={} instances={} first={} base={} instance={}",
            indices, instances, first_index, base_vertex, first_instance)) : 0;
        Breadcrumb(token, false);
        recorder.Record([=](List* list) {
            list->DrawIndexedInstanced(indices, instances, first_index, base_vertex,
                                       first_instance);
        });
        Breadcrumb(token, true);
    }
    void Dispatch(UINT x, UINT y, UINT z) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("Dispatch groups={}x{}x{}", x, y, z)) : 0;
        Breadcrumb(token, false);
        recorder.Record([=](List* list) { list->Dispatch(x, y, z); });
        Breadcrumb(token, true);
    }
    void ExecuteIndirect(ID3D12CommandSignature* signature, UINT max_commands,
                         ID3D12Resource* arguments, UINT64 arguments_offset,
                         ID3D12Resource* count, UINT64 count_offset) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("ExecuteIndirect signature={} max={} args={} offset={} count={} offset={}",
            static_cast<void*>(signature), max_commands, static_cast<void*>(arguments), arguments_offset,
            static_cast<void*>(count), count_offset)) : 0;
        Breadcrumb(token, false);
        recorder.Record([=](List* list) {
            list->ExecuteIndirect(signature, max_commands, arguments, arguments_offset, count,
                                  count_offset);
        });
        Breadcrumb(token, true);
    }
    void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE target, const FLOAT color[4],
                               UINT rect_count, const D3D12_RECT* rects) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("clear RTV={:x} rgba={},{},{},{} rects={}",
            target.ptr, color[0], color[1], color[2], color[3], rect_count)) : 0;
        Breadcrumb(token, false);
        const std::array<FLOAT, 4> value{color[0], color[1], color[2], color[3]};
        Array(rects, rects ? rect_count : 0, [=](List* list, const D3D12_RECT* copy) {
            list->ClearRenderTargetView(target, value.data(), rect_count,
                                        rect_count == 0 ? nullptr : copy);
        });
        Breadcrumb(token, true);
    }
    void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE target, D3D12_CLEAR_FLAGS flags,
                               FLOAT depth, UINT8 stencil, UINT rect_count,
                               const D3D12_RECT* rects) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("clear DSV={:x} flags={} depth={} stencil={} rects={}",
            target.ptr, static_cast<u32>(flags), depth, stencil, rect_count)) : 0;
        Breadcrumb(token, false);
        Array(rects, rects ? rect_count : 0, [=](List* list, const D3D12_RECT* copy) {
            list->ClearDepthStencilView(target, flags, depth, stencil, rect_count,
                                        rect_count == 0 ? nullptr : copy);
        });
        Breadcrumb(token, true);
    }

    // --- Copies and queries ------------------------------------------------------------------

    void CopyBufferRegion(ID3D12Resource* dst, UINT64 dst_offset, ID3D12Resource* src,
                          UINT64 src_offset, UINT64 bytes) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("CopyBuffer dst={} offset={} src={} offset={} bytes={}",
            static_cast<void*>(dst), dst_offset, static_cast<void*>(src), src_offset, bytes)) : 0;
        Breadcrumb(token, false);
        recorder.Record([=](List* list) {
            list->CopyBufferRegion(dst, dst_offset, src, src_offset, bytes);
        });
        Breadcrumb(token, true);
    }
    void CopyResource(ID3D12Resource* dst, ID3D12Resource* src) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("CopyResource dst={} src={}", static_cast<void*>(dst), static_cast<void*>(src))) : 0;
        Breadcrumb(token, false);
        recorder.Record([=](List* list) { list->CopyResource(dst, src); });
        Breadcrumb(token, true);
    }
    void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                           const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box) {
        const u64 token = GpuDiagnosticsEnabled() ? TraceGpuDetail(fmt::format("CopyTexture dst={} type={} sub_or_format={} xyz={},{},{} src={} type={} sub_or_format={} box={}",
            static_cast<void*>(dst->pResource), static_cast<u32>(dst->Type),
            dst->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? dst->SubresourceIndex : static_cast<u32>(dst->PlacedFootprint.Footprint.Format),
            x, y, z, static_cast<void*>(src->pResource), static_cast<u32>(src->Type),
            src->Type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ? src->SubresourceIndex : static_cast<u32>(src->PlacedFootprint.Footprint.Format), box != nullptr)) : 0;
        Breadcrumb(token, false);
        const D3D12_TEXTURE_COPY_LOCATION dst_copy = *dst;
        const D3D12_TEXTURE_COPY_LOCATION src_copy = *src;
        const std::optional<D3D12_BOX> box_copy = box ? std::optional{*box} : std::nullopt;
        recorder.Record([=](List* list) {
            list->CopyTextureRegion(&dst_copy, x, y, z, &src_copy,
                                    box_copy ? &*box_copy : nullptr);
        });
        Breadcrumb(token, true);
    }
    void BeginQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
        recorder.Record([=](List* list) { list->BeginQuery(heap, type, index); });
    }
    void EndQuery(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT index) {
        recorder.Record([=](List* list) { list->EndQuery(heap, type, index); });
    }
    void ResolveQueryData(ID3D12QueryHeap* heap, D3D12_QUERY_TYPE type, UINT first, UINT count,
                          ID3D12Resource* dst, UINT64 dst_offset) {
        recorder.Record([=](List* list) {
            list->ResolveQueryData(heap, type, first, count, dst, dst_offset);
        });
    }

private:
    /// With gpu_breadcrumbs=1, the GPU writes a command's detail token as it starts it (finished
    /// false) and once it and all work before it are done (finished true). See DumpBreadcrumbs.
    void Breadcrumb(u64 token, bool finished) {
        if (token == 0) {
            return;
        }
        const D3D12_GPU_VIRTUAL_ADDRESS address = GpuBreadcrumbAddress();
        if (address == 0) {
            return;
        }
        recorder.Record([=](List* list) {
            WriteGpuBreadcrumb(list, address, static_cast<u32>(token), finished);
        });
    }

    template <typename T, typename Func>
    void Array(const T* data, UINT count, Func&& func) {
        recorder.Record(std::span<const T>{data, count}, std::forward<Func>(func));
    }

    ListRecorder& recorder;
};

} // namespace D3D12
