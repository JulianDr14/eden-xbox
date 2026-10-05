/*
 * SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
 * SPDX-License-Identifier: MIT
 *
 * Linked-pipeline entry point the Eden Xbox port adds to Mesa's spirv_to_dxil.dll
 * (tools/xbox/mesa/eden_pipeline.c, built by tools/xbox/build-spirv-to-dxil.ps1).
 *
 * spirv_to_dxil() translates one stage in isolation, so each stage packs its varyings on its own
 * and the signatures of two stages only match when both use the exact same locations. This entry
 * point runs Mesa's inter-stage link (dxil_spirv_nir_link, as spirv2dxil and Dozen do) before
 * emitting DXIL, which kills unused outputs, packs both sides of every interface the same way and
 * propagates interpolation modes.
 */

#ifndef EDEN_SPIRV_TO_DXIL_H
#define EDEN_SPIRV_TO_DXIL_H

#include "spirv_to_dxil.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDEN_SPIRV_TO_DXIL_MAX_STAGES 5

/* v2 integer texture sampling: b0 space 29, indexed by the original texture binding.
 * Keep fields in 16-byte rows for DXIL constant-buffer loads. No C bitfields in this ABI. */
#define EDEN_INTEGER_SAMPLER_SPACE 29
#define EDEN_INTEGER_SAMPLER_MAX_BINDINGS 1024
struct eden_integer_sampler_state {
   uint32_t border_color[4];
   float lod_bias, min_lod, max_lod;
   uint32_t last_level;
   uint32_t wrap[3]; /* D3D12_TEXTURE_ADDRESS_MODE values */
   uint32_t padding;
   uint32_t reserved[4];
};

struct eden_spirv_to_dxil_stage {
   const uint32_t *words;
   size_t word_count;
   dxil_spirv_shader_stage stage;
   const char *entry_point;
   /* Per stage: yz_flip may only be set on the last pre-rasterization stage. */
   const struct dxil_spirv_runtime_conf *conf;
};

/* Translates the stages of one pipeline, given in pipeline order (vertex first), and links each
 * stage with the previous one. On success, out[i] holds stage i and must be released with
 * spirv_to_dxil_free(); on failure, nothing needs releasing. A single compute stage is also
 * accepted and behaves like spirv_to_dxil(). */
bool
eden_spirv_to_dxil_pipeline(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                            enum dxil_validator_version validator_version_max,
                            const struct dxil_spirv_debug_options *debug_options,
                            const struct dxil_spirv_logger *logger,
                            struct dxil_spirv_object *out);

/* Separate symbol: an older DLL must never silently compile shaders that expect the v2 ABI. */
bool
eden_spirv_to_dxil_pipeline_v2(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                             enum dxil_validator_version validator_version_max,
                             const struct dxil_spirv_debug_options *debug_options,
                             const struct dxil_spirv_logger *logger,
                             struct dxil_spirv_object *out);

#ifdef __cplusplus
}
#endif

#endif
