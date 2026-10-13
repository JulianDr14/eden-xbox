// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cassert>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include "common/settings.h"
#include <d3d12sdklayers.h>
#include <spirv-tools/libspirv.h>
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/ir_opt/passes.h"
using namespace Shader;

static size_t Count(const IR::Program& p, IR::Opcode op) {
    size_t count{};
    for (auto* b : p.blocks) for (const auto& i : b->Instructions()) count += i.GetOpcode() == op;
    return count;
}
static std::vector<u32> ReadSpirv(const std::filesystem::path& path) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    assert(file);
    std::vector<u32> code(static_cast<size_t>(file.tellg())/4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), code.size()*4);
    return code;
}
int main(int argc, char** argv) try {
    assert(argc == 2);
    Common::Log::Initialize();
    std::filesystem::create_directories(argv[1]);
    const auto validator = spvContextCreate(SPV_ENV_VULKAN_1_1);
    Settings::values.renderer_debug = true;
    D3D12::Device device;
    D3D12::ComPtr<ID3D12InfoQueue> debug;
    device.Get()->QueryInterface(IID_PPV_ARGS(&debug));
    if (debug) debug->ClearStoredMessages();
    D3D12_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    D3D12::ComPtr<ID3DBlob> root_blob, root_error;
    D3D12::ThrowIfFailed(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1,
        &root_blob, &root_error), "root signature serialization");
    D3D12::ComPtr<ID3D12RootSignature> root;
    D3D12::ThrowIfFailed(device.Get()->CreateRootSignature(0, root_blob->GetBufferPointer(),
        root_blob->GetBufferSize(), IID_PPV_ARGS(&root)), "root signature");
    const auto fixtures = std::filesystem::path(argv[1]).parent_path();
    const auto vs = ReadSpirv(fixtures / "vertex.spv");
    const auto fs = ReadSpirv(fixtures / "fragment.spv");
    D3D12::ShaderCompiler compiler;
    assert(compiler.IsAvailable());
    for (u32 topology = 0; topology < 3; ++topology) {
        for (u32 mask = 0; mask < 4; ++mask) {
            for (u32 mode = 0; mode < 4; ++mode) {
                if (mask == 3 && mode == 0) continue; // Sparse masks exercise explicit lowering.
                ObjectPool<IR::Inst> inst_pool;
                ObjectPool<IR::Block> block_pool;
                IR::Program p;
                p.stage = Stage::Geometry;
                p.invocations = 1;
                p.output_vertices = topology + 1;
                p.output_topology = topology == 0 ? OutputTopology::PointList :
                    topology == 1 ? OutputTopology::LineStrip : OutputTopology::TriangleStrip;
                p.is_geometry_passthrough = mode != 3;
                for (u32 c = 0; c < 4; ++c) {
                    p.info.passthrough.Set(IR::Attribute::PositionX + c);
                    if (mask != 0) p.info.passthrough.Set(IR::Attribute::Generic0X + c);
                    if (mask == 2) p.info.passthrough.Set(IR::Attribute::Generic31X + c);
                }
                if (mask == 3) {
                    p.info.passthrough.Set(IR::Attribute::Generic0Y, false);
                    p.info.passthrough.Set(IR::Attribute::Generic0W, false);
                    p.info.passthrough.Set(IR::Attribute::PositionY, false);
                    p.info.passthrough.Set(IR::Attribute::PositionW, false);
                }
                if (mode == 3) p.info.passthrough.mask.reset();
                auto* block = block_pool.Create(inst_pool);
                p.blocks.push_back(block);
                p.post_order_blocks.push_back(block);
                IR::AbstractSyntaxNode node;
                node.type = IR::AbstractSyntaxNode::Type::Block;
                node.data.block = block;
                p.syntax_list.push_back(node);
                p.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
                IR::IREmitter ir{*block};
                ir.Prologue();
                // Per-primitive values must survive every EmitVertex (not just the first).
                ir.SetAttribute(IR::Attribute::Layer, ir.Imm32(std::bit_cast<float>(7u)), ir.Imm32(0u));
                ir.SetAttribute(IR::Attribute::ViewportIndex, ir.Imm32(std::bit_cast<float>(2u)), ir.Imm32(0u));
                ir.SetAttribute(IR::Attribute::Generic1X, ir.Imm32(42.0f), ir.Imm32(0u));
                if (mode == 3) {
                    for (u32 c=0;c<4;++c) ir.SetAttribute(IR::Attribute::PositionX+c,ir.Imm32(float(c)),ir.Imm32(0u));
                    ir.EmitVertex(ir.Imm32(0u));
                    ir.EndPrimitive(ir.Imm32(0u));
                }
                if (mask == 3 && mode != 3) {
                    ir.SetAttribute(IR::Attribute::Generic0Y, ir.Imm32(17.0f), ir.Imm32(0u));
                    ir.SetAttribute(IR::Attribute::Generic0W, ir.Imm32(19.0f), ir.Imm32(0u));
                    ir.SetAttribute(IR::Attribute::PositionY, ir.Imm32(0.5f), ir.Imm32(0u));
                    ir.SetAttribute(IR::Attribute::PositionW, ir.Imm32(1.0f), ir.Imm32(0u));
                }
                ir.Epilogue();
                if (mode == 1 || mode == 2) {
                    Maxwell::LowerGeometryPassthrough(p);
                    assert(p.is_geometry_passthrough && p.geometry_passthrough_lowered);
                    assert(!p.UsesNativeGeometryPassthrough());
                    assert(Count(p, IR::Opcode::EmitVertex) == topology + 1);
                    const size_t count = block->Instructions().size();
                    Maxwell::LowerGeometryPassthrough(p);
                    assert(count == block->Instructions().size());
                }
                for (const auto& i : block->Instructions()) {
                    if (i.GetOpcode() == IR::Opcode::GetAttribute) p.info.loads.Set(i.Arg(0).Attribute());
                    if (i.GetOpcode() == IR::Opcode::SetAttribute) p.info.stores.Set(i.Arg(0).Attribute());
                }
                Profile profile;
                profile.supported_spirv = 0x00010000;
                profile.support_multi_viewport = true;
                profile.support_geometry_shader_passthrough = mode == 0 || mode == 2;
                RuntimeInfo runtime;
                runtime.input_topology = topology == 0 ? InputTopology::Points :
                    topology == 1 ? InputTopology::Lines : InputTopology::Triangles;
                runtime.convert_depth_mode = mask == 3 && mode != 3;
                runtime.previous_stage_stores.mask.set();
                Backend::Bindings bindings;
                auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, p, bindings);
                spv_diagnostic diagnostic{};
                const auto valid = spvValidateBinary(validator, code.data(), code.size(), &diagnostic);
                if (valid != SPV_SUCCESS) {
                    std::cerr << "Validation " << topology << "/" << mask << "/" << mode << ": "
                              << (diagnostic ? diagnostic->error : "unknown") << '\n';
                    spvDiagnosticDestroy(diagnostic);
                    return 1;
                }
                spv_text text{};
                assert(spvBinaryToText(validator, code.data(), code.size(),
                    SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES, &text, &diagnostic) == SPV_SUCCESS);
                if (mode != 0) {
                    auto dxil = compiler.Compile(code, DXIL_SPIRV_SHADER_GEOMETRY);
                    assert(!dxil.empty());
                    const D3D12::ShaderCompiler::PipelineStage stages[]{
                        {vs, DXIL_SPIRV_SHADER_VERTEX}, {code, DXIL_SPIRV_SHADER_GEOMETRY},
                        {fs, DXIL_SPIRV_SHADER_FRAGMENT}};
                    const auto linked = compiler.CompilePipeline(stages, {});
                    assert(linked.size() == 3);
                    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
                    desc.pRootSignature = root.Get();
                    desc.VS = {linked[0].dxil.data(), linked[0].dxil.size()};
                    desc.GS = {linked[1].dxil.data(), linked[1].dxil.size()};
                    desc.PS = {linked[2].dxil.data(), linked[2].dxil.size()};
                    desc.SampleMask = UINT_MAX;
                    desc.SampleDesc.Count = 1;
                    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
                    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    desc.RasterizerState.DepthClipEnable = TRUE;
                    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                    desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
                    desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
                    desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
                    desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
                    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;
                    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
                    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
                    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
                    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
                    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
                    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
                    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
                    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
                    desc.NumRenderTargets = 1;
                    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                    desc.PrimitiveTopologyType = topology == 0 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT :
                        topology == 1 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
                    D3D12::ComPtr<ID3D12PipelineState> pipeline;
                    const auto hr = device.Get()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
                    if (FAILED(hr) && debug) {
                        for (UINT64 i = 0; i < debug->GetNumStoredMessages(); ++i) {
                            SIZE_T size{};
                            debug->GetMessage(i, nullptr, &size);
                            std::vector<u8> bytes(size);
                            auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
                            debug->GetMessage(i, message, &size);
                            std::cerr << message->pDescription << '\n';
                        }
                        std::cerr << "Case " << topology << '/' << mask << '/' << mode << '\n';
                    }
                    D3D12::ThrowIfFailed(hr, "passthrough pipeline");
                }
                const auto name = std::to_string(topology) + "-" + std::to_string(mask) + "-" + std::to_string(mode) + ".spv";
                std::ofstream out{std::filesystem::path(argv[1]) / name, std::ios::binary};
                out.write(reinterpret_cast<const char*>(code.data()), code.size()*sizeof(u32));
                std::ofstream assembly{std::filesystem::path(argv[1]) / (name + "asm")};
                assembly.write(text->str, text->length);
                spvTextDestroy(text);
                if (mode != 3) {
                    const auto original_topology = runtime.input_topology;
                    runtime.input_topology = InputTopology::TrianglesAdjacency;
                    bool rejected{};
                    try { [[maybe_unused]] auto invalid = Backend::SPIRV::EmitSPIRV(profile, runtime, p, bindings); }
                    catch (const NotImplementedException&) { rejected = true; }
                    assert(rejected);
                    runtime.input_topology = original_topology;
                }
                if (mode == 0) {
                    assert(p.UsesNativeGeometryPassthrough());
                    p.invocations = 2;
                    bool invocations_rejected{};
                    try { [[maybe_unused]] auto invalid = Backend::SPIRV::EmitSPIRV(profile, runtime, p, bindings); }
                    catch (const NotImplementedException&) { invocations_rejected = true; }
                    assert(invocations_rejected);
                    p.invocations = 1;
                    profile.support_geometry_shader_passthrough = false;
                    bool rejected{};
                    try { [[maybe_unused]] auto rejected_code = Backend::SPIRV::EmitSPIRV(profile, runtime, p, bindings); }
                    catch (const LogicError&) { rejected = true; }
                    assert(rejected);
                }
            }
        }
    }
    if (debug) {
        for (UINT64 index = 0; index < debug->GetNumStoredMessages(); ++index) {
            SIZE_T size{};
            debug->GetMessage(index, nullptr, &size);
            std::vector<u8> bytes(size);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
            debug->GetMessage(index, msg, &size);
            if (msg->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << msg->pDescription << '\n';
                return 1;
            }
        }
    }
    spvContextDestroy(validator);
    std::cout << "PASS 45 SPIRV-Tools Vulkan1.1 modules; 36 signed DXIL GS + linked VS/GS/PS + D3D12 PSOs; native/lowered/conventional/mismatch/idempotence/sparse/depth\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
