// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <spirv-tools/libspirv.h>
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/demote_return_pass.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

using namespace Shader;
static void Require(bool v) { if (!v) throw std::runtime_error("regression failed"); }
struct Fixture {
    ObjectPool<IR::Inst> insts;
    ObjectPool<IR::Block> blocks;
    IR::Program p;
    Fixture() {
        p.stage = Stage::Fragment;
        p.info.uses_demote_to_helper_invocation = true;
        p.info.uses_derivatives = true;
        p.info.loads.Set(IR::Attribute::Generic0X);
        p.info.stores_frag_color[0] = true;
    }
    IR::Block* Make() {
        auto* b = blocks.Create(insts);
        p.blocks.push_back(b);
        p.post_order_blocks.push_back(b);
        return b;
    }
    void Append(IR::Block* b) {
        IR::AbstractSyntaxNode n;
        n.type = IR::AbstractSyntaxNode::Type::Block;
        n.data.block = b;
        p.syntax_list.push_back(n);
    }
    void Arm(unsigned depth, unsigned count, bool killed = true) {
        auto* entry = Make(); auto* body = Make(); auto* merge = Make();
        Append(entry);
        IR::IREmitter e{*entry};
        const auto cond = e.FPLessThan(e.GetAttribute(IR::Attribute::Generic0X), e.Imm32(0.0f));
        IR::AbstractSyntaxNode branch;
        branch.type = IR::AbstractSyntaxNode::Type::If;
        branch.data.if_node = {cond, body, merge};
        p.syntax_list.push_back(branch);
        Append(body);
        if (depth) {
            for (unsigned i = 0; i < count; ++i) Arm(depth - 1, 1, killed);
        } else {
            IR::IREmitter d{*body};
            if (killed) d.DemoteToHelperInvocation();
            d.SetFragColor(0, 0, d.Imm32(0.0f));
            d.Epilogue();
            p.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
        }
        IR::AbstractSyntaxNode end;
        end.type = IR::AbstractSyntaxNode::Type::EndIf;
        end.data.end_if.merge = merge;
        p.syntax_list.push_back(end);
        Append(merge);
    }
    void Finish() {
        auto* b = Make(); Append(b);
        IR::IREmitter e{*b};
        auto x = e.GetAttribute(IR::Attribute::Generic0X);
        e.SetFragColor(0, 0, e.DPdxFine(e.FPMul(x, x)));
        e.SetFragColor(0, 1, e.DPdyFine(x));
        e.SetFragColor(0, 2, x);
        e.SetFragColor(0, 3, e.Imm32(1.0f));
        e.Epilogue();
        p.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
    }
};
int main(int argc, char** argv) try {
    if (argc != 2) throw std::runtime_error("OUTPUT_DIR required");
    Common::Log::Initialize();
    D3D12::ShaderCompiler compiler;
    Require(compiler.IsAvailable());
    auto validator = spvContextCreate(SPV_ENV_VULKAN_1_1);
    unsigned cases{};
    for (unsigned depth = 0; depth <= 12; ++depth) {
        for (unsigned count = 1; count <= 4; ++count) {
            Fixture f;
            if (!depth) for (unsigned i = 0; i < count; ++i) f.Arm(0, 1);
            else f.Arm(depth, count);
            f.Finish();
            const auto changed = Optimization::DemoteReturnPass(f.p);
            Require(changed == count);
            Require(Optimization::DemoteReturnPass(f.p) == 0);
            Profile profile;
            profile.support_demote_to_helper_invocation = true;
            profile.support_derivative_control = true;
            RuntimeInfo runtime;
            runtime.previous_stage_stores.mask.set();
            Backend::Bindings bindings;
            const auto spirv = Backend::SPIRV::EmitSPIRV(profile, runtime, f.p, bindings);
            spv_diagnostic diagnostic{};
            if (spvValidateBinary(validator, spirv.data(), spirv.size(), &diagnostic) != SPV_SUCCESS)
                throw std::runtime_error(diagnostic ? diagnostic->error : "invalid SPIR-V");
            spvDiagnosticDestroy(diagnostic);
            const auto dxil = compiler.Compile(spirv, DXIL_SPIRV_SHADER_FRAGMENT);
            Require(!dxil.empty());
            if (depth == 0 && count == 2) {
                std::ofstream file{std::filesystem::path(argv[1])/"two_returns.dxil", std::ios::binary};
                file.write(reinterpret_cast<const char*>(dxil.data()), dxil.size());
                Fixture vertex;
                vertex.p.stage = Stage::VertexB;
                vertex.p.info = {};
                auto* b = vertex.Make(); vertex.Append(b);
                IR::IREmitter v{*b}; v.Prologue();
                for (u32 component = 0; component < 4; ++component) {
                    v.SetAttribute(IR::Attribute::PositionX + component,
                        v.GetAttribute(IR::Attribute::Generic0X + component), v.Imm32(0U));
                    vertex.p.info.loads.Set(IR::Attribute::Generic0X + component);
                    vertex.p.info.stores.Set(IR::Attribute::PositionX + component);
                }
                v.SetAttribute(IR::Attribute::Generic0X,
                    v.GetAttribute(IR::Attribute::Generic1X), v.Imm32(0U));
                vertex.p.info.loads.Set(IR::Attribute::Generic1X);
                vertex.p.info.stores.Set(IR::Attribute::Generic0X);
                v.Epilogue();
                vertex.p.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Return;
                Backend::Bindings linked_bindings;
                RuntimeInfo vr; vr.previous_stage_stores.mask.set();
                const auto vs = Backend::SPIRV::EmitSPIRV(profile, vr, vertex.p, linked_bindings);
                const D3D12::ShaderCompiler::PipelineStage stages[]{
                    {vs, DXIL_SPIRV_SHADER_VERTEX}, {spirv, DXIL_SPIRV_SHADER_FRAGMENT}};
                const auto linked = compiler.CompilePipeline(stages, {});
                for (std::size_t stage = 0; stage < linked.size(); ++stage) {
                    std::ofstream output{std::filesystem::path(argv[1])/
                        (stage ? "two_returns_ps.dxil" : "two_returns_vs.dxil"), std::ios::binary};
                    output.write(reinterpret_cast<const char*>(linked[stage].dxil.data()), linked[stage].dxil.size());
                    Require(static_cast<bool>(output));
                }
            }
            ++cases;
        }
    }
    for (unsigned kind = 0; kind < 12; ++kind) {
        Fixture f; f.Arm(0, 1, kind != 0); f.Finish();
        if (kind == 1) f.p.info.uses_subgroup_shuffles = true;
        if (kind == 2) f.p.info.uses_global_memory = true;
        if (kind == 3) f.p.local_memory_size = 4;
        if (kind == 4) f.p.info.uses_derivatives = false;
        if (kind == 5) f.p.stage = Stage::Compute;
        if (kind == 6) f.p.syntax_list.emplace_back().type = IR::AbstractSyntaxNode::Type::Loop;
        if (kind == 7) f.p.blocks.back()->AppendNewInst(IR::Opcode::Phi, {});
        if (kind == 8) f.p.info.uses_quad_shuffles = true;
        if (kind == 9) f.p.blocks[1]->AppendNewInst(IR::Opcode::Barrier, {});
        if (kind == 10) f.p.syntax_list.erase(f.p.syntax_list.begin() + 1); // Missing If.
        if (kind == 11) f.p.shared_memory_size = 4;
        const auto before = f.p.syntax_list.size();
        Require(Optimization::DemoteReturnPass(f.p) == 0);
        Require(before == f.p.syntax_list.size());
        ++cases;
    }
    spvContextDestroy(validator);
    std::cout << "PASS " << cases << " recursive/nested/multiple-return cases, SPIR-V and signed DXIL, idempotence and exclusions\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
