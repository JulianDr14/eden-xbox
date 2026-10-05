/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Adapted from Mesa dxil_nir_lower_int_samplers.c: sampler state is a draw-time
 * UBO rather than compile-time Gallium state. Integer texel bits are never converted to float.
 */
#include "dxil_nir_lower_int_samplers.h"
#include "dxil_nir_lower_int_cubemaps.h"
#include "eden_spirv_to_dxil.h"
#include "nir_builder.h"
#include "nir_builtin_builder.h"

static nir_def *
dx_get_texture_lod(nir_builder *b, nir_tex_instr *tex)
{
   nir_tex_instr *tql;

   unsigned num_srcs = 0;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      if (tex->src[i].src_type == nir_tex_src_coord ||
          tex->src[i].src_type == nir_tex_src_texture_deref ||
          tex->src[i].src_type == nir_tex_src_sampler_deref ||
          tex->src[i].src_type == nir_tex_src_texture_offset ||
          tex->src[i].src_type == nir_tex_src_sampler_offset ||
          tex->src[i].src_type == nir_tex_src_texture_handle ||
          tex->src[i].src_type == nir_tex_src_sampler_handle)
         num_srcs++;
   }

   tql = nir_tex_instr_create(b->shader, num_srcs);
   tql->op = nir_texop_lod;
   unsigned coord_components = tex->coord_components;
   if (tex->is_array)
      --coord_components;

   tql->coord_components = coord_components;
   tql->sampler_dim = tex->sampler_dim;
   tql->is_shadow = tex->is_shadow;
   tql->is_new_style_shadow = tex->is_new_style_shadow;
   tql->texture_index = tex->texture_index;
   tql->sampler_index = tex->sampler_index;
   tql->can_speculate = tex->can_speculate;
   tql->dest_type = nir_type_float32;

   /* The coordinate needs special handling because we might have
    * to strip the array index. Don't clutter the code  with an additional
    * check for is_array though, in the worst case we create an additional
    * move the the optimization will remove later again. */
   int coord_index = nir_tex_instr_src_index(tex, nir_tex_src_coord);
   nir_def *ssa_src = nir_trim_vector(b, tex->src[coord_index].src.ssa,
                                          coord_components);
   tql->src[0].src = nir_src_for_ssa(ssa_src);
   tql->src[0].src_type = nir_tex_src_coord;

   unsigned idx = 1;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      if (tex->src[i].src_type == nir_tex_src_texture_deref ||
          tex->src[i].src_type == nir_tex_src_sampler_deref ||
          tex->src[i].src_type == nir_tex_src_texture_offset ||
          tex->src[i].src_type == nir_tex_src_sampler_offset ||
          tex->src[i].src_type == nir_tex_src_texture_handle ||
          tex->src[i].src_type == nir_tex_src_sampler_handle) {
         tql->src[idx].src = nir_src_for_ssa(tex->src[i].src.ssa);
         tql->src[idx].src_type = tex->src[i].src_type;
         idx++;
      }
   }

   nir_def_init(&tql->instr, &tql->def, 2, 32);
   nir_builder_instr_insert(b, &tql->instr);

   /* DirectX LOD only has a value in x channel */
   return nir_channel(b, &tql->def, 0);
}

typedef struct {
   nir_def *coords;
   nir_def *use_border_color;
} wrap_result_t;

typedef struct {
   nir_def *lod;
   nir_def *size;
   int ncoord_comp;
   wrap_result_t wrap[3];
} wrap_lower_param_t;

static void
wrap_clamp_to_edge(nir_builder *b, wrap_result_t *wrap_params, nir_def *size)
{
   /* clamp(coord, 0, size - 1) */
   wrap_params->coords = nir_fmin(b, nir_fadd_imm(b, size, -1.0f),
                                  nir_fmax(b, wrap_params->coords, nir_imm_float(b, 0.0f)));
}

static void
wrap_repeat(nir_builder *b, wrap_result_t *wrap_params, nir_def *size)
{
   /* mod(coord, size)
    * This instruction must be exact, otherwise certain sizes result in
    * incorrect sampling */
   wrap_params->coords = nir_fmod(b, wrap_params->coords, size);
   nir_def_as_alu(wrap_params->coords)->fp_math_ctrl |= nir_fp_exact;
}

static nir_def *
mirror(nir_builder *b, nir_def *coord)
{
   /* coord if >= 0, otherwise -(1 + coord) */
   return nir_bcsel(b, nir_fge_imm(b, coord, 0.0f), coord,
                    nir_fneg(b, nir_fadd_imm(b, coord, 1.0f)));
}

static void
wrap_mirror_repeat(nir_builder *b, wrap_result_t *wrap_params, nir_def *size)
{
   /* (size − 1) − mirror(mod(coord, 2 * size) − size) */
   nir_def *coord_mod2size = nir_fmod(b, wrap_params->coords, nir_fmul_imm(b, size, 2.0f));
   nir_def_as_alu(coord_mod2size)->fp_math_ctrl |= nir_fp_exact;
   nir_def *a = nir_fsub(b, coord_mod2size, size);
   wrap_params->coords = nir_fsub(b, nir_fadd_imm(b, size, -1.0f), mirror(b, a));
}

static void
wrap_mirror_clamp_to_edge(nir_builder *b, wrap_result_t *wrap_params, nir_def *size)
{
   /* clamp(mirror(coord), 0, size - 1) */
   wrap_params->coords = nir_fmin(b, nir_fadd_imm(b, size, -1.0f),
                                  nir_fmax(b, mirror(b, wrap_params->coords), nir_imm_float(b, 0.0f)));
}

static void
wrap_clamp(nir_builder *b, wrap_result_t *wrap_params, nir_def *size)
{
   nir_def *is_low = nir_flt_imm(b, wrap_params->coords, 0.0);
   nir_def *is_high = nir_fge(b, wrap_params->coords, size);
   wrap_params->use_border_color = nir_ior(b, is_low, is_high);
}

static void
wrap_mirror_clamp(nir_builder *b, wrap_result_t *wrap_params, nir_def *size)
{
   /* We have to take care of the boundaries */
   nir_def *is_low = nir_flt(b, wrap_params->coords, nir_fmul_imm(b, size, -1.0));
   nir_def *is_high = nir_flt(b, nir_fmul_imm(b, size, 2.0), wrap_params->coords);
   wrap_params->use_border_color = nir_ior(b, is_low, is_high);

   /* Within the boundaries this acts like mirror_repeat */
   wrap_mirror_repeat(b, wrap_params, size);

}

static wrap_result_t
wrap_coords(nir_builder *b, nir_def *coords, enum dxil_tex_wrap wrap,
            nir_def *size)
{
   wrap_result_t result = {coords, nir_imm_false(b)};

   switch (wrap) {
   case DXIL_TEX_WRAP_CLAMP_TO_EDGE:
      wrap_clamp_to_edge(b, &result, size);
      break;
   case DXIL_TEX_WRAP_REPEAT:
      wrap_repeat(b, &result, size);
      break;
   case DXIL_TEX_WRAP_MIRROR_REPEAT:
      wrap_mirror_repeat(b, &result, size);
      break;
   case DXIL_TEX_WRAP_MIRROR_CLAMP:
   case DXIL_TEX_WRAP_MIRROR_CLAMP_TO_EDGE:
      wrap_mirror_clamp_to_edge(b, &result, size);
      break;
   case DXIL_TEX_WRAP_CLAMP:
   case DXIL_TEX_WRAP_CLAMP_TO_BORDER:
      wrap_clamp(b, &result, size);
      break;
   case DXIL_TEX_WRAP_MIRROR_CLAMP_TO_BORDER:
      wrap_mirror_clamp(b, &result, size);
      break;
   }
   return result;
}

static nir_tex_instr *
create_txf_from_tex(nir_builder *b, nir_tex_instr *tex)
{
   nir_tex_instr *txf;

   unsigned num_srcs = 0;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      if (tex->src[i].src_type == nir_tex_src_texture_deref ||
          tex->src[i].src_type == nir_tex_src_texture_offset ||
          tex->src[i].src_type == nir_tex_src_texture_handle)
         num_srcs++;
   }

   txf = nir_tex_instr_create(b->shader, num_srcs);
   txf->op = nir_texop_txf;
   txf->coord_components = tex->coord_components;
   txf->sampler_dim = tex->sampler_dim;
   txf->is_array = tex->is_array;
   txf->is_shadow = tex->is_shadow;
   txf->is_new_style_shadow = tex->is_new_style_shadow;
   txf->texture_index = tex->texture_index;
   txf->sampler_index = tex->sampler_index;
   txf->can_speculate = tex->can_speculate;
   txf->dest_type = tex->dest_type;

   unsigned idx = 0;
   for (unsigned i = 0; i < tex->num_srcs; i++) {
      if (tex->src[i].src_type == nir_tex_src_texture_deref ||
          tex->src[i].src_type == nir_tex_src_texture_offset ||
          tex->src[i].src_type == nir_tex_src_texture_handle) {
         txf->src[idx].src = nir_src_for_ssa(tex->src[i].src.ssa);
         txf->src[idx].src_type = tex->src[i].src_type;
         idx++;
      }
   }

   nir_def_init(&txf->instr, &txf->def, nir_tex_instr_dest_size(txf), 32);
   nir_builder_instr_insert(b, &txf->instr);

   return txf;
}

static nir_def *
load_texel(nir_builder *b, nir_tex_instr *tex, wrap_lower_param_t *params)
{
   nir_def *texcoord = NULL;

   /* Put coordinates back together */
   switch (tex->coord_components) {
   case 1:
      texcoord = params->wrap[0].coords;
      break;
   case 2:
      texcoord = nir_vec2(b, params->wrap[0].coords, params->wrap[1].coords);
      break;
   case 3:
      texcoord = nir_vec3(b, params->wrap[0].coords, params->wrap[1].coords, params->wrap[2].coords);
      break;
   default:
      ;
   }

   texcoord = nir_f2i32(b, texcoord);

   nir_tex_instr *load = create_txf_from_tex(b, tex);
   nir_tex_instr_add_src(load, nir_tex_src_lod, params->lod);
   nir_tex_instr_add_src(load, nir_tex_src_coord, texcoord);
   b->cursor = nir_after_instr(&load->instr);
   return &load->def;
}


static nir_def *
load_state(nir_builder *b, nir_def *binding, unsigned row)
{
   nir_def *index = nir_vulkan_resource_index(b, 2, 32, nir_imm_int(b, 0),
      .desc_set = EDEN_INTEGER_SAMPLER_SPACE, .binding = 0,
      .desc_type = nir_descriptor_type_uniform_buffer);
   nir_def *desc = nir_load_vulkan_descriptor(b, 2, 32, index,
      .desc_type = nir_descriptor_type_uniform_buffer);
   nir_def *offset = nir_iadd_imm(b, nir_imul_imm(b, binding,
      sizeof(struct eden_integer_sampler_state)), row * 16);
   return nir_load_ubo(b, 4, 32, nir_channel(b, desc, 0), offset,
      .align_mul = 16, .align_offset = 0, .range_base = 0,
      .range = EDEN_INTEGER_SAMPLER_MAX_BINDINGS * sizeof(struct eden_integer_sampler_state));
}

static nir_def *
texture_binding(nir_builder *b, nir_tex_instr *tex)
{
   int src = nir_tex_instr_src_index(tex, nir_tex_src_texture_deref);
   assert(src >= 0);
   nir_deref_instr *deref = nir_src_as_deref(tex->src[src].src);
   nir_variable *var = nir_deref_instr_get_variable(deref);
   nir_def *binding = nir_imm_int(b, var->data.binding);
   /* SPIR-V descriptor arrays use consecutive bindings in this backend. */
   if (deref->deref_type == nir_deref_type_array)
      binding = nir_iadd(b, binding, deref->arr.index.ssa);
   return binding;
}

static wrap_result_t
wrap_dynamic(nir_builder *b, nir_def *coord, nir_def *mode, nir_def *size)
{
   /* Direct3D address enum: repeat=1, mirror=2, edge=3, border=4, mirror-once=5.
    * Select rather than branch: these parameters are uniform for every draw. */
   wrap_result_t result = wrap_coords(b, coord, DXIL_TEX_WRAP_CLAMP_TO_EDGE, size);
   const enum dxil_tex_wrap wraps[] = {DXIL_TEX_WRAP_REPEAT, DXIL_TEX_WRAP_MIRROR_REPEAT,
      DXIL_TEX_WRAP_CLAMP_TO_EDGE, DXIL_TEX_WRAP_CLAMP_TO_BORDER,
      DXIL_TEX_WRAP_MIRROR_CLAMP_TO_EDGE};
   for (unsigned i = 0; i < ARRAY_SIZE(wraps); ++i) {
      if (i == 2) continue;
      wrap_result_t candidate = wrap_coords(b, coord, wraps[i], size);
      nir_def *active = nir_ieq_imm(b, mode, i + 1);
      result.coords = nir_bcsel(b, active, candidate.coords, result.coords);
      result.use_border_color = nir_bcsel(b, active, candidate.use_border_color,
                                         result.use_border_color);
   }
   return result;
}

static bool
integer_sample_filter(const nir_instr *instr, const void *unused)
{
   (void)unused;
   if (instr->type != nir_instr_type_tex) return false;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   return (tex->dest_type & (nir_type_int | nir_type_uint)) &&
      (tex->op == nir_texop_tex || tex->op == nir_texop_txb ||
       tex->op == nir_texop_txl || tex->op == nir_texop_txd);
}

static nir_def *
integer_sample(nir_builder *b, nir_instr *instr, void *unused)
{
   (void)unused;
   nir_tex_instr *tex = nir_instr_as_tex(instr);
   b->cursor = nir_before_instr(instr);
   nir_def *binding = texture_binding(b, tex);
   nir_def *border = load_state(b, binding, 0);
   nir_def *state = load_state(b, binding, 1);
   nir_def *wrap = load_state(b, binding, 2);
   nir_def *size0 = nir_get_texture_size(b, tex);
   const unsigned ncoord = tex->coord_components - tex->is_array;
   nir_def *lod;
   if (tex->op == nir_texop_txl) {
      lod = tex->src[nir_tex_instr_src_index(tex, nir_tex_src_lod)].src.ssa;
   } else if (tex->op == nir_texop_txd) {
      nir_def *dx = tex->src[nir_tex_instr_src_index(tex, nir_tex_src_ddx)].src.ssa;
      nir_def *dy = tex->src[nir_tex_instr_src_index(tex, nir_tex_src_ddy)].src.ssa;
      nir_def *rho2 = nir_imm_float(b, 0);
      nir_def *sigma2 = nir_imm_float(b, 0);
      for (unsigned i = 0; i < ncoord; ++i) {
         nir_def *size = nir_i2f32(b, nir_channel(b, size0, i));
         nir_def *x = nir_fmul(b, nir_channel(b, dx, i), size);
         nir_def *y = nir_fmul(b, nir_channel(b, dy, i), size);
         rho2 = nir_fadd(b, rho2, nir_fmul(b, x, x));
         sigma2 = nir_fadd(b, sigma2, nir_fmul(b, y, y));
      }
      lod = nir_fmul_imm(b, nir_flog2(b, nir_fmax(b, rho2, sigma2)), 0.5f);
   } else {
      /* DXIL CalculateLOD returns the derivative LOD; apply the sampler bias below. */
      lod = b->shader->info.stage == MESA_SHADER_FRAGMENT ?
         dx_get_texture_lod(b, tex) : nir_imm_float(b, 0);
   }
   if (tex->op != nir_texop_txl) {
      nir_def *bias = nir_channel(b, state, 0);
      if (tex->op == nir_texop_txb)
         bias = nir_fadd(b, bias,
            tex->src[nir_tex_instr_src_index(tex, nir_tex_src_bias)].src.ssa);
      lod = nir_fadd(b, lod, nir_fclamp(b, bias, nir_imm_float(b, -16), nir_imm_float(b, 15.99f)));
   }
   lod = nir_fclamp(b, lod, nir_fmax(b, nir_channel(b, state, 1), nir_imm_float(b, 0)),
                   nir_channel(b, state, 2));
   int clamp_index = nir_tex_instr_src_index(tex, nir_tex_src_min_lod);
   if (clamp_index >= 0) lod = nir_fmax(b, lod, tex->src[clamp_index].src.ssa);
   /* Nearest mip selection: ceil(lod + 0.5) - 1, clamped to the view's levels. */
   nir_def *ilevel = nir_f2i32(b, nir_fadd_imm(b, nir_fceil(b, nir_fadd_imm(b, lod, 0.5f)), -1));
   ilevel = nir_iclamp(b, ilevel, nir_imm_int(b, 0), nir_channel(b, state, 3));
   wrap_lower_param_t params = {.lod = ilevel, .ncoord_comp = ncoord};
   params.size = nir_i2f32(b, nir_imax(b, nir_ishr(b, size0, ilevel), nir_imm_int(b, 1)));
   nir_def *coord = tex->src[nir_tex_instr_src_index(tex, nir_tex_src_coord)].src.ssa;
   nir_def *use_border = nir_imm_false(b);
   int offset_index = nir_tex_instr_src_index(tex, nir_tex_src_offset);
   for (unsigned i = 0; i < ncoord; ++i) {
      nir_def *c = nir_ffloor(b, nir_fmul(b, nir_channel(b, coord, i),
                                       nir_channel(b, params.size, i)));
      if (offset_index >= 0)
         c = nir_fadd(b, c, nir_i2f32(b, nir_channel(b, tex->src[offset_index].src.ssa, i)));
      params.wrap[i] = wrap_dynamic(b, c, nir_channel(b, wrap, i), nir_channel(b, params.size, i));
      use_border = nir_ior(b, use_border, params.wrap[i].use_border_color);
   }
   if (tex->is_array) {
      nir_def *layer = nir_ffloor(b, nir_fadd_imm(b, nir_channel(b, coord, ncoord), 0.5f));
      params.wrap[ncoord] = wrap_coords(b, layer, DXIL_TEX_WRAP_CLAMP_TO_EDGE,
                                       nir_i2f32(b, nir_channel(b, size0, ncoord)));
   }
   nir_if *border_if = nir_push_if(b, use_border);
   nir_def *border_value = nir_trim_vector(b, border, tex->def.num_components);
   nir_push_else(b, border_if);
   nir_def *value = load_texel(b, tex, &params);
   nir_pop_if(b, border_if);
   return nir_if_phi(b, border_value, value);
}

static bool
eden_lower_integer_sampling(nir_shader *nir)
{
   bool has_integer_samples = false;
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            has_integer_samples |= integer_sample_filter(instr, NULL);
         }
      }
   }
   if (!has_integer_samples) return false;
   const struct glsl_type *array = glsl_array_type(glsl_uint_type(),
      EDEN_INTEGER_SAMPLER_MAX_BINDINGS * sizeof(struct eden_integer_sampler_state) / 4, 4);
   const struct glsl_struct_field field = {array, "words"};
   nir_variable *var = nir_variable_create(nir, nir_var_mem_ubo,
      glsl_struct_type(&field, 1, "integer_sampler_data", false), "integer_sampler_data");
   var->data.descriptor_set = EDEN_INTEGER_SAMPLER_SPACE;
   var->data.binding = 0;
   var->data.how_declared = nir_var_hidden;
   return nir_shader_lower_instructions(nir, integer_sample_filter, integer_sample, NULL);
}
