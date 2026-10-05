/*
 * SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
 * SPDX-License-Identifier: MIT
 *
 * Linked-pipeline variant of spirv_to_dxil() for the Eden Xbox port. build-spirv-to-dxil.ps1
 * copies this file into Mesa's src/microsoft/spirv_to_dxil/ and exports its entry point.
 *
 * It follows spirv_to_dxil() (spirv_to_dxil.c) for each stage, and the link loop of spirv2dxil.c
 * and Dozen (dzn_pipeline.c): translate + prep + passes per stage, then link from the last stage
 * back to the first, then emit DXIL.
 */

#include "dxil_spirv_nir.h"
#include "eden_spirv_to_dxil.h"
#include "eden_integer_sampling.h"
#include "nir_builder.h"
#include "nir_to_dxil.h"
#include "spirv/nir_spirv.h"
#include "util/blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
free_objects(struct dxil_spirv_object *out, unsigned count)
{
   for (unsigned i = 0; i < count; ++i) {
      free(out[i].binary.buffer);
      memset(&out[i], 0, sizeof(out[i]));
   }
}

/* Guest (Maxwell) shaders rely on IEEE results: rsq(0) = inf, min/max picking the non-NaN operand,
 * x * 0 staying NaN for x = inf... nir_to_dxil tags every non-exact float op with fast math
 * (DXIL_UNSAFE_ALGEBRA, "no NaN/Inf"), and the Xbox Series' shader compiler takes it: lit pixels
 * of Mario Wonder came out NaN there (the black silhouettes) while the PC's driver kept IEEE. */
static bool
mark_alu_exact(nir_builder *b, nir_instr *instr, void *data)
{
   (void)b;
   (void)data;
   if (instr->type != nir_instr_type_alu)
      return false;
   nir_instr_as_alu(instr)->fp_math_ctrl |= nir_fp_exact;
   return true;
}

static bool
translate_pipeline(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                   enum dxil_validator_version validator_version_max,
                   const struct dxil_spirv_debug_options *debug_options,
                   const struct dxil_spirv_logger *logger, struct dxil_spirv_object *out,
                   bool lower_integer_sampling)
{
   if (count == 0 || count > EDEN_SPIRV_TO_DXIL_MAX_STAGES)
      return false;
   for (unsigned i = 0; i < count; ++i) {
      const dxil_spirv_shader_stage stage = stages[i].stage;
      if (stage == DXIL_SPIRV_SHADER_NONE || stage == DXIL_SPIRV_SHADER_KERNEL)
         return false;
      if (stage == DXIL_SPIRV_SHADER_COMPUTE && count != 1)
         return false;
      if (i > 0 && stage <= stages[i - 1].stage)
         return false;
   }
   memset(out, 0, sizeof(*out) * count);

   glsl_type_singleton_init_or_ref();

   /* Each shader keeps a pointer to its compiler options until it is freed. */
   nir_shader_compiler_options nir_options[EDEN_SPIRV_TO_DXIL_MAX_STAGES];
   nir_shader *nir[EDEN_SPIRV_TO_DXIL_MAX_STAGES] = {0};
   const struct spirv_to_nir_options *spirv_opts = dxil_spirv_nir_get_spirv_options();
   const unsigned supported_bit_sizes = 16 | 32 | 64;
   bool success = true;

   for (unsigned i = 0; i < count && success; ++i) {
      const struct dxil_spirv_runtime_conf *conf = stages[i].conf;
      dxil_get_nir_compiler_options(&nir_options[i], conf->shader_model_max,
                                    supported_bit_sizes, supported_bit_sizes);
      nir_options[i].lower_base_vertex =
         conf->first_vertex_and_base_instance_mode != DXIL_SPIRV_SYSVAL_TYPE_ZERO;

      nir[i] = spirv_to_nir(stages[i].words, stages[i].word_count, NULL,
                            (mesa_shader_stage)stages[i].stage, stages[i].entry_point,
                            spirv_opts, &nir_options[i]);
      if (!nir[i]) {
         success = false;
         break;
      }
      nir_validate_shader(nir[i], "Validate before feeding NIR to the DXIL compiler");
      dxil_spirv_nir_prep(nir[i]);
      dxil_spirv_nir_passes(nir[i], conf, &out[i].metadata);
      /* After the passes: tex instructions are final (cube and implicit-LOD lowering done) and
       * nir_to_dxil's own optimization loop scalarizes and lowers what this emits. */
      if (lower_integer_sampling)
         eden_lower_integer_sampling(nir[i]);
   }

   if (success) {
      /* Reverse order, so outputs the next stage never reads are gone before the previous stage
       * is linked against its own predecessor. */
      for (unsigned i = count; i-- > 0;) {
         struct dxil_spirv_metadata link_metadata = {0};
         dxil_spirv_nir_link(nir[i], i > 0 ? nir[i - 1] : NULL, stages[i].conf, &link_metadata);
         out[i].metadata.requires_runtime_data |= link_metadata.requires_runtime_data;
         out[i].metadata.needs_draw_sysvals |= link_metadata.needs_draw_sysvals;
      }
   }

   struct dxil_logger logger_inner = {.priv = logger->priv, .log = logger->log};
   for (unsigned i = 0; i < count && success; ++i) {
      if (debug_options->dump_nir)
         nir_print_shader(nir[i], stderr);

      /* lower_int16: widen every 16-bit ALU op to 32 bits. The Xbox Series has no native 16-bit
       * shader ops, and NIR still makes 16-bit ops out of mediump (RelaxedPrecision) code even
       * with the recompiler's fp16/int16 off; the console driver then rejects the pixel shader
       * (E_INVALIDARG). Dozen lowers only when the app enables 16-bit types, which misses those. */
      nir_shader_instructions_pass(nir[i], mark_alu_exact, nir_metadata_all, NULL);
      const struct nir_to_dxil_options opts = {
         .environment = DXIL_ENVIRONMENT_VULKAN,
         .lower_int16 = true,
         .disable_math_refactoring = true,
         .shader_model_max = stages[i].conf->shader_model_max,
         .validator_version_max = validator_version_max,
      };
      struct blob dxil_blob;
      if (!nir_to_dxil(nir[i], &opts, &logger_inner, &dxil_blob)) {
         if (dxil_blob.allocated)
            blob_finish(&dxil_blob);
         success = false;
         break;
      }
      blob_finish_get_buffer(&dxil_blob, &out[i].binary.buffer, &out[i].binary.size);
   }

   for (unsigned i = 0; i < count; ++i)
      ralloc_free(nir[i]);
   if (!success)
      free_objects(out, count);

   glsl_type_singleton_decref();
   return success;
}

bool
eden_spirv_to_dxil_pipeline(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                            enum dxil_validator_version validator_version_max,
                            const struct dxil_spirv_debug_options *debug_options,
                            const struct dxil_spirv_logger *logger,
                            struct dxil_spirv_object *out)
{
   return translate_pipeline(stages, count, validator_version_max, debug_options, logger, out,
                             false);
}

/* Integer textures sampled through the b0 space 29 table (eden_integer_sampler_state). */
bool
eden_spirv_to_dxil_pipeline_v2(const struct eden_spirv_to_dxil_stage *stages, unsigned count,
                               enum dxil_validator_version validator_version_max,
                               const struct dxil_spirv_debug_options *debug_options,
                               const struct dxil_spirv_logger *logger,
                               struct dxil_spirv_object *out)
{
   return translate_pipeline(stages, count, validator_version_max, debug_options, logger, out,
                             true);
}
