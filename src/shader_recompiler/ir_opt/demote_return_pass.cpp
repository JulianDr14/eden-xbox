// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/ir_opt/demote_return_pass.h"

namespace Shader::Optimization {
namespace {
using Type = IR::AbstractSyntaxNode::Type;
using Op = IR::Opcode;

bool OutputOnly(Op op) {
    return op == Op::SetFragColor || op == Op::SetFragDepth ||
           op == Op::SetSampleMask || op == Op::Epilogue;
}

bool Eligible(const IR::Program& p) {
    const auto& info = p.info;
    if (p.stage != Stage::Fragment || !info.uses_demote_to_helper_invocation ||
        info.uses_subgroup_shuffles || info.uses_quad_shuffles || info.uses_subgroup_vote ||
        info.uses_subgroup_mask || info.uses_subgroup_invocation_id || info.uses_global_memory ||
        p.local_memory_size || p.shared_memory_size ||
        !info.storage_buffers_descriptors.empty() || !info.image_descriptors.empty() ||
        !info.image_buffer_descriptors.empty()) return false;
    for (const auto& n : p.syntax_list) {
        if (n.type == Type::Loop || n.type == Type::Repeat || n.type == Type::Break ||
            n.type == Type::Unreachable) return false;
    }
    bool needs_helpers = info.uses_derivatives;
    for (const auto* b : p.blocks) {
        for (const auto& i : b->Instructions()) {
            const auto op = i.GetOpcode();
            needs_helpers |= op == Op::ImageSampleImplicitLod ||
                             op == Op::ImageSampleDrefImplicitLod;
            // Adding a predecessor requires updating phis. Do not approximate it.
            if (op == Op::Phi || op == Op::PhiMove) return false;
            if (i.MayHaveSideEffects() && !OutputOnly(op) && op != Op::Prologue &&
                op != Op::DemoteToHelperInvocation && op != Op::ConditionRef &&
                op != Op::Reference) return false;
        }
    }
    return needs_helpers;
}
}

std::size_t DemoteReturnPass(IR::Program& program) {
    if (!Eligible(program)) return 0;
    auto& syntax = program.syntax_list;
    struct Candidate {
        std::size_t first, last, returned;
        IR::Block* tail;
        IR::Block* merge;
    };
    std::vector<std::size_t> stack;
    std::vector<Candidate> candidates;
    for (std::size_t end = 0; end < syntax.size(); ++end) {
        if (syntax[end].type == Type::If) {
            stack.push_back(end);
            continue;
        }
        if (syntax[end].type != Type::EndIf) continue;
        if (stack.empty()) return 0;
        const auto begin = stack.back();
        stack.pop_back();
        const auto& branch = syntax[begin].data.if_node;
        if (end <= begin + 2 || syntax[end - 1].type != Type::Return ||
            syntax[begin + 1].type != Type::Block ||
            syntax[begin + 1].data.block != branch.body ||
            syntax[end].data.end_if.merge != branch.merge || end + 1 >= syntax.size() ||
            syntax[end + 1].type != Type::Block || syntax[end + 1].data.block != branch.merge)
            continue;
        bool demoted = false, safe = true;
        IR::Block* tail{};
        for (auto n = begin + 1; safe && n < end - 1; ++n) {
            if (syntax[n].type != Type::Block) { safe = false; break; }
            tail = syntax[n].data.block;
            for (const auto& inst : tail->Instructions()) {
                const auto op = inst.GetOpcode();
                if (op == Op::DemoteToHelperInvocation) demoted = true;
                else if (op == Op::Identity || op == Op::Void) continue;
                else if (!demoted || !OutputOnly(op)) { safe = false; break; }
            }
        }
        if (safe && demoted && tail && tail->ImmSuccessors().empty())
            candidates.push_back({begin + 1, end - 1, end - 1, tail, branch.merge});
    }
    if (!stack.empty() || candidates.empty()) return 0;
    std::vector<bool> removed(syntax.size());
    for (const auto& c : candidates) {
        for (auto n = c.first; n < c.last; ++n) {
            for (auto& inst : syntax[n].data.block->Instructions()) {
                if (OutputOnly(inst.GetOpcode())) inst.Invalidate();
            }
        }
        c.tail->AddBranch(c.merge);
        removed[c.returned] = true;
    }
    // Compact in source order; do not depend on an algorithm's predicate call order.
    auto output = syntax.begin();
    for (std::size_t index = 0; index < syntax.size(); ++index) {
        if (!removed[index]) *output++ = syntax[index];
    }
    syntax.erase(output, syntax.end());
    return candidates.size();
}
}
