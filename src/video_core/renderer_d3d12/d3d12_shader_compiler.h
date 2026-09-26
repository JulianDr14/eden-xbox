// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <vector>

#include <spirv_to_dxil.h>

#include "common/dynamic_library.h"
#include "video_core/renderer_d3d12/d3d12_device.h"

struct IDxcValidator;

namespace D3D12 {

/// Register spaces the translated shaders use for data Vulkan passes outside descriptor sets.
/// Guest descriptors keep the Vulkan model: space = set, register = binding.
constexpr u32 PUSH_CONSTANT_SPACE = 30;
constexpr u32 RUNTIME_DATA_SPACE = 31;

/// SPIR-V -> signed DXIL, via Mesa's spirv_to_dxil and the DXIL validator in dxil.dll.
///
/// Both DLLs ship in the package and are loaded at runtime, so a console that rejects either one
/// loses the shader path (IsAvailable() == false) instead of failing to activate the app.
class ShaderCompiler {
public:
    ShaderCompiler();
    ~ShaderCompiler();

    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;

    bool IsAvailable() const {
        return available;
    }

    /// Translates and signs one stage. Throws std::runtime_error with the translator's or the
    /// validator's message on failure. Vertex-pipeline stages get Vulkan's Y-down clip space flipped
    /// to D3D's when flip_y is set.
    std::vector<u8> Compile(std::span<const u32> spirv, dxil_spirv_shader_stage stage,
                            bool flip_y = false) const;

private:
    void Sign(std::vector<u8>& dxil) const;

    using PFN_spirv_to_dxil = decltype(&::spirv_to_dxil);
    using PFN_spirv_to_dxil_free = decltype(&::spirv_to_dxil_free);

    Common::DynamicLibrary spirv_to_dxil_library;
    Common::DynamicLibrary dxil_library;
    PFN_spirv_to_dxil translate{};
    PFN_spirv_to_dxil_free free_dxil{};
    ComPtr<IDxcValidator> validator;
    bool available{};
};

} // namespace D3D12
