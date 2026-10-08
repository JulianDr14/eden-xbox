// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <vector>

#include "shader_recompiler/shader_info.h"

namespace D3D12 {

/// What a graphics pipeline reads of a stage's Shader::Info when it draws: its descriptors and
/// varyings. A pipeline kept a whole Shader::Info per stage, about 2 KiB with its inline
/// capacities, and a large game builds over 14000 pipelines from about 2400 programs: 175 MiB of
/// copies. Pipelines now share one immutable StageBindings per distinct content (InternStage).
struct StageBindings {
    std::vector<Shader::ConstantBufferDescriptor> constant_buffer_descriptors;
    std::vector<Shader::StorageBufferDescriptor> storage_buffers_descriptors;
    std::vector<Shader::TextureBufferDescriptor> texture_buffer_descriptors;
    std::vector<Shader::ImageBufferDescriptor> image_buffer_descriptors;
    std::vector<Shader::TextureDescriptor> texture_descriptors;
    std::vector<Shader::ImageDescriptor> image_descriptors;
    Shader::VaryingState loads;
    Shader::VaryingState stores; ///< Read by the draw trace only.
    bool uses_render_area{};

    bool operator==(const StageBindings& other) const;
};

/// The shared StageBindings equal to info's. Thread-safe; an entry lives while a pipeline uses it.
[[nodiscard]] std::shared_ptr<const StageBindings> InternStage(const Shader::Info& info);

} // namespace D3D12
