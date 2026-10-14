// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
// Local forensic harness. Operates on a COPY of the user's shader cache.
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>
#include <bit>
#include <sstream>
#include <map>
#include <nlohmann/json.hpp>
#include "common/logging.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "video_core/shader_environment.h"
#include "video_core/renderer_d3d12/d3d12_pipeline_cache.h"
int main(int argc, char** argv) try {
    if (argc != 4) { std::cerr << "usage: inspect-captured-shader CACHE OUTPUT HASH[,HASH...]\n"; return 2; }
    Common::Log::Initialize();
    std::set<u64> wanted;
    std::istringstream requested{argv[3]};
    for (std::string hash; std::getline(requested, hash, ',');) wanted.insert(std::stoull(hash, nullptr, 16));
    const auto output = std::filesystem::absolute(argv[2]);
    std::filesystem::create_directories(output);
    const auto cache_copy = output / "cache-input-copy.bin";
    if (std::filesystem::absolute(argv[1]).lexically_normal() == cache_copy.lexically_normal()) return 2;
    // LoadPipelines can remove a bad cache. It must never receive the original evidence.
    std::filesystem::copy_file(argv[1], cache_copy, std::filesystem::copy_options::overwrite_existing);
    Shader::HostTranslateInfo host{
        .min_ssbo_alignment=16,.max_per_stage_descriptor_sampled_images=128,
        .max_per_stage_resources=128,.max_descriptor_set_samplers=2048,
        .max_descriptor_set_uniform_buffers=14,.max_descriptor_set_storage_buffers=64,
        .max_descriptor_set_sampled_images=128,.max_descriptor_set_storage_images=64,
        .support_float64=true,.support_float16=false,.support_int64=true,
        .needs_demote_reorder=true,.support_snorm_render_buffer=true,
        .support_viewport_index_layer=true,.support_geometry_shader_passthrough=false,
        .support_conditional_barrier=false};
    host.ApplyDescriptorLimitPolicy();
    std::set<u64> done;
    VideoCommon::LoadPipelines({}, cache_copy, 1,
        [](std::ifstream& file, VideoCommon::FileEnvironment) {
            D3D12::ComputePipelineCacheKey key{};
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
        },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
            D3D12::GraphicsPipelineCacheKey key{};
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
            size_t env_index=0;
            for(size_t stage=0;stage<key.unique_hashes.size();++stage) {
                const auto hash=key.unique_hashes[stage];
                if(!hash) continue;
                auto& env=envs.at(env_index++);
                if(!wanted.contains(hash) || done.contains(hash)) continue;
                D3D12::ShaderPools pools;
                Shader::Maxwell::Flow::CFG cfg(env,pools.flow_block,
                    static_cast<u32>(env.StartAddress()+sizeof(Shader::ProgramHeader)),stage==0);
                const auto p=Shader::Maxwell::TranslateProgram(pools.inst,pools.block,env,cfg,host);
                const auto text=Shader::IR::DumpProgram(p);
                std::ofstream{std::filesystem::path(argv[2])/fmt::format("{:016x}.ir.txt",hash)}<<text;
                nlohmann::json graph;
                graph["hash"] = fmt::format("{:016x}", hash);
                graph["stage"] = stage;
                size_t linear_block = 0;
                bool returned = false;
                graph["straight_line"] = std::all_of(p.syntax_list.begin(), p.syntax_list.end(),
                    [&](const auto& node) {
                        using T = Shader::IR::AbstractSyntaxNode::Type;
                        if (returned) return false;
                        if (node.type == T::Return) { returned = true; return true; }
                        return node.type == T::Block && linear_block < p.blocks.size() &&
                            node.data.block == p.blocks[linear_block++];
                    }) && linear_block == p.blocks.size();
                graph["cbuf_descriptors"] = nlohmann::json::array();
                for (const auto& cb : p.info.constant_buffer_descriptors)
                    graph["cbuf_descriptors"].push_back({{"index",cb.index},{"count",cb.count}});
                graph["textures"] = nlohmann::json::array();
                for (const auto& tx : p.info.texture_descriptors)
                    graph["textures"].push_back({{"cbuf",tx.cbuf_index},{"offset",tx.cbuf_offset},
                        {"type",static_cast<u32>(tx.type)},{"depth",tx.is_depth},{"integer",tx.is_integer},
                        {"count",tx.count},{"secondary",tx.has_secondary}});
                std::map<const Shader::IR::Inst*, size_t> ids;
                for (auto* block : p.blocks) for (const auto& inst : *block) ids.emplace(&inst, ids.size());
                graph["blocks"] = nlohmann::json::array();
                for (auto* block : p.blocks) {
                    nlohmann::json instructions = nlohmann::json::array();
                    for (const auto& inst : *block) {
                        nlohmann::json record{{"id",ids.at(&inst)},
                            {"op",fmt::format("{}",inst.GetOpcode())},
                            {"type",fmt::format("{}",inst.Type())},{"flags",inst.Flags<u32>()}};
                        record["args"] = nlohmann::json::array();
                        for (size_t i=0;i<inst.NumArgs();++i) {
                            const auto arg=inst.Arg(i);
                            nlohmann::json value{{"type",fmt::format("{}",arg.Type())}};
                            if (arg.IsEmpty()) value["empty"]=true;
                            else if (!arg.IsImmediate()) value["ref"]=ids.at(arg.Inst());
                            else {
                                using T=Shader::IR::Type;
                                switch (arg.Type()) {
                                case T::F32: value["bits"]=std::bit_cast<u32>(arg.F32()); break;
                                case T::F64: value["bits"]=std::bit_cast<u64>(arg.F64()); break;
                                case T::U32: value["value"]=arg.U32(); break;
                                case T::U64: value["value"]=arg.U64(); break;
                                case T::U1: value["value"]=arg.U1(); break;
                                case T::U8: value["value"]=arg.U8(); break;
                                case T::U16: value["value"]=arg.U16(); break;
                                case T::Attribute: value["value"]=static_cast<u64>(arg.Attribute()); break;
                                default: throw std::runtime_error("unsupported immediate in forensic export");
                                }
                            }
                            record["args"].push_back(value);
                        }
                        instructions.push_back(record);
                    }
                    graph["blocks"].push_back(instructions);
                }
                std::ofstream{output / fmt::format("{:016x}.ir.json",hash)} << graph.dump(2);
                std::cout<<fmt::format("{:016x} stage={} bytes={}\n",hash,stage,text.size());
                done.insert(hash);
            }
        });
    std::cout<<"found "<<done.size()<<" / "<<wanted.size()<<'\n';
    return done == wanted ? 0 : 1;
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
