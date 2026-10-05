// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <stdexcept>
#include <string>

// First: dxcapi.h relies on the Windows/COM declarations d3d12_device.h pulls in.
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"

#include <dxcapi.h>
#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/frame_trace.h"

namespace D3D12 {

namespace {

/// Shader model the translator may use: the Xbox Series UWP runtime reports 6.4.
constexpr dxil_shader_model SHADER_MODEL = SHADER_MODEL_6_4;
/// Validator rules the translator targets; dxil.dll only has to be at least this new.
constexpr dxil_validator_version VALIDATOR_VERSION = DXIL_VALIDATOR_1_4;

/// Minimal IDxcBlob over caller-owned memory, so the validator can sign a buffer in place.
class BorrowedBlob final : public IDxcBlob {
public:
    BorrowedBlob(void* data_, size_t size_) : data{data_}, size{size_} {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDxcBlob)) {
            *out = static_cast<IDxcBlob*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    // Lives on the caller's stack for the duration of one Validate call.
    ULONG STDMETHODCALLTYPE AddRef() override {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return 1;
    }
    LPVOID STDMETHODCALLTYPE GetBufferPointer() override {
        return data;
    }
    SIZE_T STDMETHODCALLTYPE GetBufferSize() override {
        return size;
    }

private:
    void* data;
    size_t size;
};

void LogTranslatorMessage(void*, const char* msg) {
    LOG_ERROR(Render, "spirv_to_dxil: {}", msg);
}

} // Anonymous namespace

ShaderCompiler::ShaderCompiler() {
    if (!spirv_to_dxil_library.Open("spirv_to_dxil.dll")) {
        LOG_ERROR(Render, "D3D12: spirv_to_dxil.dll could not be loaded (error {})",
                  GetLastError());
        return;
    }
    if (!spirv_to_dxil_library.GetSymbol("spirv_to_dxil", &translate) ||
        !spirv_to_dxil_library.GetSymbol("spirv_to_dxil_free", &free_dxil)) {
        LOG_ERROR(Render, "D3D12: spirv_to_dxil.dll lacks its entry points");
        return;
    }
    if (spirv_to_dxil_library.GetSymbol("eden_spirv_to_dxil_pipeline_v2", &translate_pipeline)) {
        lowers_integer_sampling = true;
    } else if (!spirv_to_dxil_library.GetSymbol("eden_spirv_to_dxil_pipeline",
                                                &translate_pipeline)) {
        LOG_WARNING(Render, "D3D12: spirv_to_dxil.dll cannot link stages (rebuild it with "
                            "tools/xbox/build-spirv-to-dxil.ps1); stages are translated alone");
    }
    if (!dxil_library.Open("dxil.dll")) {
        LOG_ERROR(Render, "D3D12: dxil.dll could not be loaded (error {})", GetLastError());
        return;
    }
    DxcCreateInstanceProc create_instance{};
    if (!dxil_library.GetSymbol("DxcCreateInstance", &create_instance)) {
        LOG_ERROR(Render, "D3D12: dxil.dll lacks DxcCreateInstance");
        return;
    }
    const HRESULT hr = create_instance(CLSID_DxcValidator, IID_PPV_ARGS(&validator));
    if (FAILED(hr)) {
        LOG_ERROR(Render, "D3D12: creating the DXIL validator failed (HRESULT 0x{:08X})",
                  static_cast<u32>(hr));
        return;
    }

    UINT32 major = 0;
    UINT32 minor = 0;
    ComPtr<IDxcVersionInfo> version;
    if (SUCCEEDED(validator.As(&version))) {
        version->GetVersion(&major, &minor);
    }
    LOG_INFO(Render, "D3D12: shader path ready (spirv_to_dxil{}{}, DXIL validator {}.{})",
             translate_pipeline ? " with stage linking" : "",
             lowers_integer_sampling ? " and integer sampling" : "", major, minor);
    if (translate_pipeline && !lowers_integer_sampling) {
        LOG_WARNING(Render, "D3D12: spirv_to_dxil.dll predates integer texture sampling; shaders "
                            "sampling integer textures will fail validation (rebuild it)");
    }
    available = true;
}

ShaderCompiler::~ShaderCompiler() {
    // The validator's code lives in dxil.dll: release it before the library unloads.
    validator.Reset();
}

std::vector<u8> ShaderCompiler::Compile(std::span<const u32> spirv, dxil_spirv_shader_stage stage,
                                        bool flip_y) const {
    if (!available) {
        throw std::runtime_error("D3D12: the shader path is unavailable");
    }
    dxil_spirv_runtime_conf conf = MakeConf();
    // An unconditional flip only applies to the viewports set in its mask.
    if (flip_y) {
        conf.yz_flip = {.mode = DXIL_SPIRV_Y_FLIP_UNCONDITIONAL, .y_mask = 1, .z_mask = 0};
    }

    // spirv_to_dxil dereferences both of these unconditionally.
    const dxil_spirv_debug_options debug_options{};
    const dxil_spirv_logger logger{.priv = nullptr, .log = LogTranslatorMessage};
    dxil_spirv_object object{};
    if (!translate(spirv.data(), spirv.size(), nullptr, 0, stage, "main", VALIDATOR_VERSION,
                   &debug_options, &conf, &logger, &object)) {
        throw std::runtime_error(
            fmt::format("D3D12: spirv_to_dxil failed for stage {}", static_cast<int>(stage)));
    }
    std::vector<u8> dxil(static_cast<const u8*>(object.binary.buffer),
                         static_cast<const u8*>(object.binary.buffer) + object.binary.size);
    free_dxil(&object);

    Sign(dxil);
    return dxil;
}

std::vector<ShaderCompiler::CompiledStage> ShaderCompiler::CompilePipeline(
    std::span<const PipelineStage> stages, const PipelineOptions& options, u64 trace_pipeline) const {
    if (!available) {
        throw std::runtime_error("D3D12: the shader path is unavailable");
    }
    if (stages.empty() || stages.size() > EDEN_SPIRV_TO_DXIL_MAX_STAGES) {
        throw std::runtime_error(fmt::format("D3D12: invalid pipeline of {} stages", stages.size()));
    }
    // The last stage before the fragment one owns the clip-space position the flip applies to.
    size_t flip_stage = stages.size();
    for (size_t i = 0; i < stages.size(); ++i) {
        if (stages[i].stage != DXIL_SPIRV_SHADER_FRAGMENT &&
            stages[i].stage != DXIL_SPIRV_SHADER_COMPUTE) {
            flip_stage = i;
        }
    }
    std::array<dxil_spirv_runtime_conf, EDEN_SPIRV_TO_DXIL_MAX_STAGES> confs{};
    std::array<eden_spirv_to_dxil_stage, EDEN_SPIRV_TO_DXIL_MAX_STAGES> inputs{};
    for (size_t i = 0; i < stages.size(); ++i) {
        confs[i] = MakeConf();
        confs[i].first_vertex_and_base_instance_mode = options.first_vertex_and_base_instance;
        if (i == flip_stage) {
            confs[i].yz_flip = {.mode = options.yz_flip,
                                .y_mask = options.y_flip_mask,
                                .z_mask = options.z_flip_mask};
        }
        inputs[i] = {.words = stages[i].spirv.data(),
                     .word_count = stages[i].spirv.size(),
                     .stage = stages[i].stage,
                     .entry_point = "main",
                     .conf = &confs[i]};
    }

    const dxil_spirv_debug_options debug_options{};
    const dxil_spirv_logger logger{.priv = nullptr, .log = LogTranslatorMessage};
    std::array<dxil_spirv_object, EDEN_SPIRV_TO_DXIL_MAX_STAGES> objects{};
    VideoCore::FrameTrace::ScopedSpan translate_span{
        VideoCore::FrameTrace::Event::PipelineTranslateLong, trace_pipeline, trace_pipeline != 0};
    if (translate_pipeline) {
        if (!translate_pipeline(inputs.data(), static_cast<unsigned>(stages.size()),
                                VALIDATOR_VERSION, &debug_options, &logger, objects.data())) {
            throw std::runtime_error("D3D12: eden_spirv_to_dxil_pipeline failed");
        }
    } else {
        for (size_t i = 0; i < stages.size(); ++i) {
            if (translate(inputs[i].words, inputs[i].word_count, nullptr, 0, inputs[i].stage,
                          "main", VALIDATOR_VERSION, &debug_options, &confs[i], &logger,
                          &objects[i])) {
                continue;
            }
            for (size_t j = 0; j < i; ++j) {
                free_dxil(&objects[j]);
            }
            throw std::runtime_error(fmt::format("D3D12: spirv_to_dxil failed for stage {}",
                                                 static_cast<int>(inputs[i].stage)));
        }
    }

    translate_span.Finish();
    std::vector<CompiledStage> result(stages.size());
    for (size_t i = 0; i < stages.size(); ++i) {
        const auto* data = static_cast<const u8*>(objects[i].binary.buffer);
        result[i].dxil.assign(data, data + objects[i].binary.size);
        result[i].metadata = objects[i].metadata;
        free_dxil(&objects[i]);
    }
    for (CompiledStage& stage : result) {
        Sign(stage.dxil, trace_pipeline);
    }
    return result;
}

dxil_spirv_runtime_conf ShaderCompiler::MakeConf() const {
    dxil_spirv_runtime_conf conf{};
    conf.runtime_data_cbv = {.register_space = RUNTIME_DATA_SPACE, .base_shader_register = 0};
    conf.push_constant_cbv = {.register_space = PUSH_CONSTANT_SPACE, .base_shader_register = 0};
    conf.first_vertex_and_base_instance_mode = DXIL_SPIRV_SYSVAL_TYPE_ZERO;
    conf.workgroup_id_mode = DXIL_SPIRV_SYSVAL_TYPE_NATIVE;
    conf.declared_read_only_images_as_srvs = true;
    conf.shader_model_max = SHADER_MODEL;
    return conf;
}

void ShaderCompiler::Sign(std::vector<u8>& dxil, u64 trace_pipeline) const {
    // The runtime refuses unsigned DXIL outside developer mode; the validator writes the hash in place.
    BorrowedBlob blob{dxil.data(), dxil.size()};
    ComPtr<IDxcOperationResult> result;
    VideoCore::FrameTrace::ScopedSpan lock_span{
        VideoCore::FrameTrace::Event::PipelineValidatorLockLong, trace_pipeline, trace_pipeline != 0};
    std::scoped_lock lock{validator_mutex};
    lock_span.Finish();
    VideoCore::FrameTrace::ScopedSpan sign_span{
        VideoCore::FrameTrace::Event::PipelineSignLong, trace_pipeline, trace_pipeline != 0};
    ThrowIfFailed(validator->Validate(&blob, DxcValidatorFlags_InPlaceEdit, &result),
                  "IDxcValidator::Validate");
    HRESULT status = E_FAIL;
    result->GetStatus(&status);
    if (SUCCEEDED(status)) {
        return;
    }
    std::string message = "(no details)";
    ComPtr<IDxcBlobEncoding> errors;
    if (SUCCEEDED(result->GetErrorBuffer(&errors)) && errors && errors->GetBufferSize() > 0) {
        message.assign(static_cast<const char*>(errors->GetBufferPointer()),
                       errors->GetBufferSize());
    }
    throw std::runtime_error(fmt::format("D3D12: DXIL validation failed (0x{:08X}): {}",
                                         static_cast<u32>(status), message));
}

} // namespace D3D12
