// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Validate helper-preserving discard through the production emitter and Mesa linker.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <spirv-tools/libspirv.h>
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

using namespace Shader;

static IR::Block* Block(IR::Program& p, ObjectPool<IR::Inst>& insts, ObjectPool<IR::Block>& blocks) {
    auto* b = blocks.Create(insts);
    p.blocks.push_back(b);
    p.post_order_blocks.push_back(b);
    return b;
}
static void Append(IR::Program& p, IR::Block* b) {
    IR::AbstractSyntaxNode n;
    n.type = IR::AbstractSyntaxNode::Type::Block;
    n.data.block = b;
    p.syntax_list.push_back(n);
}
int main(int argc, char** argv) try {
    if (argc != 2 && argc != 4) throw std::runtime_error("usage: demote-helper OUTPUT_DIR [VS.spv PS.spv]");
    Common::Log::Initialize();
    std::filesystem::create_directories(argv[1]);
    D3D12::ShaderCompiler compiler;
    if (!compiler.IsAvailable()) throw std::runtime_error("compiler unavailable");
    if (argc == 4) {
        const auto read = [](const char* path) {
            std::ifstream file{path, std::ios::binary | std::ios::ate};
            const auto bytes = file.tellg();
            if (bytes <= 0 || bytes % 4) throw std::runtime_error("invalid SPIR-V input");
            std::vector<u32> words(static_cast<std::size_t>(bytes) / 4);
            file.seekg(0);
            file.read(reinterpret_cast<char*>(words.data()), bytes);
            if (!file) throw std::runtime_error("input read failed");
            return words;
        };
        const auto vertex = read(argv[2]), pixel = read(argv[3]);
        const D3D12::ShaderCompiler::PipelineStage stages[]{
            {vertex, DXIL_SPIRV_SHADER_VERTEX}, {pixel, DXIL_SPIRV_SHADER_FRAGMENT}};
        const auto linked = compiler.CompilePipeline(stages, {});
        for (std::size_t i = 0; i < linked.size(); ++i) {
            std::ofstream file{std::filesystem::path(argv[1])/(i ? "ps.dxil" : "vs.dxil"), std::ios::binary};
            file.write(reinterpret_cast<const char*>(linked[i].dxil.data()), linked[i].dxil.size());
            if (!file) throw std::runtime_error("output write failed");
        }
        return 0;
    }
    auto validator = spvContextCreate(SPV_ENV_VULKAN_1_1);
    for (bool native : {false, true}) {
        ObjectPool<IR::Inst> insts;
        ObjectPool<IR::Block> blocks;
        IR::Program vs, ps;
        vs.stage = Stage::VertexB;
        ps.stage = Stage::Fragment;
        auto* vb = Block(vs, insts, blocks);
        Append(vs, vb);
        vs.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
        IR::IREmitter v{*vb};
        v.Prologue();
        for (u32 c = 0; c < 4; ++c) {
            auto a = v.GetAttribute(IR::Attribute::Generic0X + c);
            v.SetAttribute(IR::Attribute::PositionX + c, a, v.Imm32(0U));
            v.SetAttribute(IR::Attribute::Generic0X + c, a, v.Imm32(0U));
            vs.info.loads.Set(IR::Attribute::Generic0X+c);
            vs.info.stores.Set(IR::Attribute::PositionX+c);
            vs.info.stores.Set(IR::Attribute::Generic0X+c);
        }
        v.Epilogue();
        auto* before = Block(ps, insts, blocks);
        auto* killed = Block(ps, insts, blocks);
        auto* after = Block(ps, insts, blocks);
        Append(ps, before);
        IR::IREmitter f{*before};
        f.Prologue();
        const auto x = f.GetAttribute(IR::Attribute::Generic0X);
        const auto condition = f.FPLessThan(x, f.Imm32(0.0f));
        IR::AbstractSyntaxNode branch;
        branch.type = IR::AbstractSyntaxNode::Type::If;
        branch.data.if_node = {condition, killed, after};
        ps.syntax_list.push_back(branch);
        Append(ps, killed);
        IR::IREmitter{*killed}.DemoteToHelperInvocation();
        IR::AbstractSyntaxNode end;
        end.type = IR::AbstractSyntaxNode::Type::EndIf;
        end.data.end_if.merge = after;
        ps.syntax_list.push_back(end);
        Append(ps, after);
        IR::IREmitter a{*after};
        a.SetFragColor(0, 0, a.DPdxFine(a.FPMul(x, x)));
        a.SetFragColor(0, 1, a.DPdyFine(x));
        a.SetFragColor(0, 2, x);
        a.SetFragColor(0, 3, a.Imm32(1.0f));
        a.Epilogue();
        ps.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
        ps.info.loads.Set(IR::Attribute::Generic0X);
        ps.info.stores_frag_color[0] = true;
        ps.info.uses_demote_to_helper_invocation = true;
        ps.info.uses_derivatives = true;
        Profile profile;
        profile.support_demote_to_helper_invocation = native;
        profile.support_derivative_control = true;
        RuntimeInfo vr, pr;
        vr.previous_stage_stores.mask.set();
        pr.previous_stage_stores = vs.info.stores;
        Backend::Bindings bindings;
        const auto vertex = Backend::SPIRV::EmitSPIRV(profile, vr, vs, bindings);
        const auto pixel = Backend::SPIRV::EmitSPIRV(profile, pr, ps, bindings);
        unsigned kills{}, demotes{};
        for (std::size_t i = 5; i < pixel.size(); i += pixel[i] >> 16) {
            kills += (pixel[i] & 0xffff) == 252;
            demotes += (pixel[i] & 0xffff) == 5380;
        }
        if ((native && (kills || demotes != 1)) || (!native && (kills != 1 || demotes)))
            throw std::runtime_error("incorrect discard instruction");
        spv_diagnostic diagnostic{};
        if (spvValidateBinary(validator, pixel.data(), pixel.size(), &diagnostic) != SPV_SUCCESS)
            throw std::runtime_error(diagnostic ? diagnostic->error : "invalid SPIR-V");
        spvDiagnosticDestroy(diagnostic);
        const D3D12::ShaderCompiler::PipelineStage stages[]{
            {vertex, DXIL_SPIRV_SHADER_VERTEX}, {pixel, DXIL_SPIRV_SHADER_FRAGMENT}};
        const auto linked = compiler.CompilePipeline(stages, {});
        if (linked.size() != 2) throw std::runtime_error("link failed");
        const auto prefix = native ? "demote" : "kill";
        std::ofstream file{std::filesystem::path(argv[1])/(std::string(prefix)+".dxil"), std::ios::binary};
        file.write(reinterpret_cast<const char*>(linked[1].dxil.data()), linked[1].dxil.size());
        if (!file) throw std::runtime_error("write failed");
    }
    spvContextDestroy(validator);
    std::cout << "PASS conditional demote followed by derivatives: SPIR-V validation, linked signed DXIL\n";
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
