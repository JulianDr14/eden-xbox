// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Emit and compile synthetic fixtures through the production SPIR-V -> DXIL path.
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <spirv-tools/libspirv.h>
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

using namespace Shader;

static IR::Block* Begin(IR::Program& program, ObjectPool<IR::Inst>& insts,
                        ObjectPool<IR::Block>& blocks) {
    auto* block = blocks.Create(insts);
    program.blocks.push_back(block);
    program.post_order_blocks.push_back(block);
    IR::AbstractSyntaxNode node;
    node.type = IR::AbstractSyntaxNode::Type::Block;
    node.data.block = block;
    program.syntax_list.push_back(node);
    program.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
    return block;
}

int main(int argc, char** argv) try {
    if (argc != 2) throw std::runtime_error("usage: noperspective-lowering OUTPUT_DIR");
    Common::Log::Initialize();
    const std::filesystem::path out{argv[1]};
    std::filesystem::create_directories(out);
    const auto validator = spvContextCreate(SPV_ENV_VULKAN_1_1);
    D3D12::ShaderCompiler compiler;
    if (!compiler.IsAvailable()) throw std::runtime_error("production shader compiler unavailable");
    for (u32 late_w = 0; late_w < 2; ++late_w) {
        for (u32 lower = 0; lower < 2; ++lower) {
            ObjectPool<IR::Inst> insts;
            ObjectPool<IR::Block> blocks;
            IR::Program vs, ps;
            vs.stage = Stage::VertexB;
            ps.stage = Stage::Fragment;
            IR::IREmitter v{*Begin(vs, insts, blocks)};
            v.Prologue();
            const auto position = [&](u32 component) {
                v.SetAttribute(IR::Attribute::PositionX + component,
                    v.GetAttribute(IR::Attribute::Generic0X + component), v.Imm32(0U));
            };
            for (u32 c = 0; c < (late_w ? 3U : 4U); ++c) position(c);
            for (u32 index : {0U, 1U, 2U, 31U}) {
                for (u32 c = 0; c < 4; ++c) {
                    v.SetAttribute(IR::Attribute::Generic0X + 4*index+c,
                        v.GetAttribute(IR::Attribute::Generic1X + c), v.Imm32(0U));
                    vs.info.stores.Set(IR::Attribute::Generic0X + 4*index+c);
                }
            }
            if (late_w) position(3);
            v.Epilogue();
            for (u32 c = 0; c < 4; ++c) {
                vs.info.loads.Set(IR::Attribute::Generic0X+c);
                vs.info.loads.Set(IR::Attribute::Generic1X+c);
                vs.info.stores.Set(IR::Attribute::PositionX+c);
            }
            IR::IREmitter f{*Begin(ps, insts, blocks)};
            f.Prologue();
            const std::array attributes{IR::Attribute::Generic0X, IR::Attribute::Generic1Y,
                                        IR::Attribute::Generic2Z, IR::Attribute::Generic31W};
            for (u32 c = 0; c < 4; ++c) {
                f.SetFragColor(0, c, f.GetAttribute(attributes[c]));
                ps.info.loads.Set(attributes[c]);
            }
            ps.info.interpolation[0] = ps.info.interpolation[31] = Interpolation::NoPerspective;
            ps.info.interpolation[1] = Interpolation::Smooth;
            ps.info.interpolation[2] = Interpolation::Flat;
            ps.info.stores_frag_color[0] = true;
            f.Epilogue();
            Profile profile;
            RuntimeInfo vr, pr;
            vr.convert_depth_mode = true;
            vr.previous_stage_stores.mask.set();
            pr.previous_stage_stores = vs.info.stores;
            vr.lower_noperspective_mask = pr.lower_noperspective_mask = lower ? 0x80000001U : 0;
            Backend::Bindings binding;
            const auto vertex = Backend::SPIRV::EmitSPIRV(profile, vr, vs, binding);
            const auto fragment = Backend::SPIRV::EmitSPIRV(profile, pr, ps, binding);
            for (const auto& code : {vertex, fragment}) {
                spv_diagnostic diagnostic{};
                if (spvValidateBinary(validator, code.data(), code.size(), &diagnostic) != SPV_SUCCESS) {
                    throw std::runtime_error(diagnostic ? diagnostic->error : "invalid SPIR-V");
                }
                spvDiagnosticDestroy(diagnostic);
            }
            const D3D12::ShaderCompiler::PipelineStage stages[]{
                {vertex, DXIL_SPIRV_SHADER_VERTEX}, {fragment, DXIL_SPIRV_SHADER_FRAGMENT}};
            const auto linked = compiler.CompilePipeline(stages, {});
            if (linked.size() != 2) throw std::runtime_error("invalid linked stage count");
            const std::string prefix = "order"+std::to_string(late_w)+"_lower"+std::to_string(lower);
            for (size_t i = 0; i < linked.size(); ++i) {
                std::ofstream file{out/(prefix+(i ? "_ps.dxil" : "_vs.dxil")), std::ios::binary};
                file.write(reinterpret_cast<const char*>(linked[i].dxil.data()), linked[i].dxil.size());
                if (!file) throw std::runtime_error("fixture write failed");
            }
        }
    }
    spvContextDestroy(validator);
    std::cout << "PASS four production emitter/linker fixtures: native/lowered, late W, mixed smooth/flat, location31\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
