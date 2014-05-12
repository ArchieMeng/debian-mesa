/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "main/blend.h"
#include "main/mtypes.h"
#include "main/samplerobj.h"
#include "main/texformat.h"
#include "program/prog_parameter.h"

#include "intel_mipmap_tree.h"
#include "intel_batchbuffer.h"
#include "intel_tex.h"
#include "intel_fbo.h"
#include "intel_buffer_objects.h"

#include "brw_context.h"
#include "brw_state.h"
#include "brw_defines.h"
#include "brw_wm.h"

static uint32_t
surface_tiling_mode(uint32_t tiling)
{
   switch (tiling) {
   case I915_TILING_X:
      return GEN8_SURFACE_TILING_X;
   case I915_TILING_Y:
      return GEN8_SURFACE_TILING_Y;
   default:
      return GEN8_SURFACE_TILING_NONE;
   }
}

static unsigned
vertical_alignment(struct intel_mipmap_tree *mt)
{
   switch (mt->align_h) {
   case 4:
      return GEN8_SURFACE_VALIGN_4;
   case 8:
      return GEN8_SURFACE_VALIGN_8;
   case 16:
      return GEN8_SURFACE_VALIGN_16;
   default:
      assert(!"Unsupported vertical surface alignment.");
      return GEN8_SURFACE_VALIGN_4;
   }
}

static unsigned
horizontal_alignment(struct intel_mipmap_tree *mt)
{
   switch (mt->align_w) {
   case 4:
      return GEN8_SURFACE_HALIGN_4;
   case 8:
      return GEN8_SURFACE_HALIGN_8;
   case 16:
      return GEN8_SURFACE_HALIGN_16;
   default:
      assert(!"Unsupported horizontal surface alignment.");
      return GEN8_SURFACE_HALIGN_4;
   }
}

static uint32_t
surface_num_multisamples(unsigned num_samples)
{
   assert(num_samples >= 0 && num_samples <= 16);

   if (num_samples == 0)
      return GEN7_SURFACE_MULTISAMPLECOUNT_1;

   /* The SURFACE_MULTISAMPLECOUNT_X enums are simply log2(num_samples) << 3. */
   return (ffs(num_samples) - 1) << 3;
}

static void
gen8_emit_buffer_surface_state(struct brw_context *brw,
                               uint32_t *out_offset,
                               drm_intel_bo *bo,
                               unsigned buffer_offset,
                               unsigned surface_format,
                               unsigned buffer_size,
                               unsigned pitch,
                               unsigned mocs,
                               bool rw)
{
   uint32_t *surf = brw_state_batch(brw, AUB_TRACE_SURFACE_STATE,
                                    13 * 4, 64, out_offset);
   memset(surf, 0, 13 * 4);

   surf[0] = BRW_SURFACE_BUFFER << BRW_SURFACE_TYPE_SHIFT |
             surface_format << BRW_SURFACE_FORMAT_SHIFT |
             BRW_SURFACE_RC_READ_WRITE;

   surf[2] = SET_FIELD((buffer_size - 1) & 0x7f, GEN7_SURFACE_WIDTH) |
             SET_FIELD(((buffer_size - 1) >> 7) & 0x3fff, GEN7_SURFACE_HEIGHT);
   surf[3] = SET_FIELD(((buffer_size - 1) >> 21) & 0x3f, BRW_SURFACE_DEPTH) |
             (pitch - 1);
   surf[7] = SET_FIELD(HSW_SCS_RED,   GEN7_SURFACE_SCS_R) |
             SET_FIELD(HSW_SCS_GREEN, GEN7_SURFACE_SCS_G) |
             SET_FIELD(HSW_SCS_BLUE,  GEN7_SURFACE_SCS_B) |
             SET_FIELD(HSW_SCS_ALPHA, GEN7_SURFACE_SCS_A);
   /* reloc */
   *((uint64_t *) &surf[8]) = (bo ? bo->offset64 : 0) + buffer_offset;

   /* Emit relocation to surface contents. */
   if (bo) {
      drm_intel_bo_emit_reloc(brw->batch.bo, *out_offset + 8 * 4,
                              bo, buffer_offset, I915_GEM_DOMAIN_SAMPLER,
                              rw ? I915_GEM_DOMAIN_SAMPLER : 0);
   }
}

static void
gen8_update_texture_surface(struct gl_context *ctx,
                            unsigned unit,
                            uint32_t *surf_offset,
                            bool for_gather)
{
   struct brw_context *brw = brw_context(ctx);
   struct gl_texture_object *tObj = ctx->Texture.Unit[unit]._Current;
   struct intel_texture_object *intelObj = intel_texture_object(tObj);
   struct intel_mipmap_tree *mt = intelObj->mt;
   struct gl_texture_image *firstImage = tObj->Image[0][tObj->BaseLevel];
   struct gl_sampler_object *sampler = _mesa_get_samplerobj(ctx, unit);

   if (tObj->Target == GL_TEXTURE_BUFFER) {
      brw_update_buffer_texture_surface(ctx, unit, surf_offset);
      return;
   }

   uint32_t tex_format = translate_tex_format(brw,
                                              mt->format,
                                              sampler->sRGBDecode);

   uint32_t *surf = brw_state_batch(brw, AUB_TRACE_SURFACE_STATE,
                                    13 * 4, 64, surf_offset);

   surf[0] = translate_tex_target(tObj->Target) << BRW_SURFACE_TYPE_SHIFT |
             tex_format << BRW_SURFACE_FORMAT_SHIFT |
             vertical_alignment(mt) |
             horizontal_alignment(mt) |
             surface_tiling_mode(mt->region->tiling);

   if (tObj->Target == GL_TEXTURE_CUBE_MAP ||
       tObj->Target == GL_TEXTURE_CUBE_MAP_ARRAY) {
      surf[0] |= BRW_SURFACE_CUBEFACE_ENABLES;
   }

   if (mt->logical_depth0 > 1 && tObj->Target != GL_TEXTURE_3D)
      surf[0] |= GEN8_SURFACE_IS_ARRAY;

   surf[1] = mt->qpitch >> 2;

   surf[2] = SET_FIELD(mt->logical_width0 - 1, GEN7_SURFACE_WIDTH) |
             SET_FIELD(mt->logical_height0 - 1, GEN7_SURFACE_HEIGHT);

   surf[3] = SET_FIELD(mt->logical_depth0 - 1, BRW_SURFACE_DEPTH) |
             (mt->region->pitch - 1);

   surf[4] = surface_num_multisamples(mt->num_samples);

   surf[5] = SET_FIELD(tObj->BaseLevel - mt->first_level, GEN7_SURFACE_MIN_LOD) |
             (intelObj->_MaxLevel - tObj->BaseLevel); /* mip count */

   surf[6] = 0;

   /* Handling GL_ALPHA as a surface format override breaks 1.30+ style
    * texturing functions that return a float, as our code generation always
    * selects the .x channel (which would always be 0).
    */
   const bool alpha_depth = tObj->DepthMode == GL_ALPHA &&
      (firstImage->_BaseFormat == GL_DEPTH_COMPONENT ||
       firstImage->_BaseFormat == GL_DEPTH_STENCIL);

   const int swizzle =
      unlikely(alpha_depth) ? SWIZZLE_XYZW : brw_get_texture_swizzle(ctx, tObj);
   surf[7] =
      SET_FIELD(brw_swizzle_to_scs(GET_SWZ(swizzle, 0), false), GEN7_SURFACE_SCS_R) |
      SET_FIELD(brw_swizzle_to_scs(GET_SWZ(swizzle, 1), false), GEN7_SURFACE_SCS_G) |
      SET_FIELD(brw_swizzle_to_scs(GET_SWZ(swizzle, 2), false), GEN7_SURFACE_SCS_B) |
      SET_FIELD(brw_swizzle_to_scs(GET_SWZ(swizzle, 3), false), GEN7_SURFACE_SCS_A);

   *((uint64_t *) &surf[8]) = mt->region->bo->offset64 + mt->offset; /* reloc */

   surf[10] = 0;
   surf[11] = 0;
   surf[12] = 0;

   /* Emit relocation to surface contents */
   drm_intel_bo_emit_reloc(brw->batch.bo,
                           *surf_offset + 8 * 4,
                           mt->region->bo,
                           mt->offset,
                           I915_GEM_DOMAIN_SAMPLER, 0);
}

/**
 * Create the constant buffer surface.  Vertex/fragment shader constants will be
 * read from this buffer with Data Port Read instructions/messages.
 */
static void
gen8_update_null_renderbuffer_surface(struct brw_context *brw, unsigned unit)
{
   struct gl_context *ctx = &brw->ctx;

   /* _NEW_BUFFERS */
   const struct gl_framebuffer *fb = ctx->DrawBuffer;
   uint32_t surf_index =
      brw->wm.prog_data->binding_table.render_target_start + unit;

   uint32_t *surf = brw_state_batch(brw, AUB_TRACE_SURFACE_STATE, 13 * 4, 64,
                                    &brw->wm.base.surf_offset[surf_index]);
   memset(surf, 0, 13 * 4);

   surf[0] = BRW_SURFACE_NULL << BRW_SURFACE_TYPE_SHIFT |
             BRW_SURFACEFORMAT_B8G8R8A8_UNORM << BRW_SURFACE_FORMAT_SHIFT |
             GEN8_SURFACE_TILING_Y;
   surf[2] = SET_FIELD(fb->Width - 1, GEN7_SURFACE_WIDTH) |
             SET_FIELD(fb->Height - 1, GEN7_SURFACE_HEIGHT);
}

/**
 * Sets up a surface state structure to point at the given region.
 * While it is only used for the front/back buffer currently, it should be
 * usable for further buffers when doing ARB_draw_buffer support.
 */
static void
gen8_update_renderbuffer_surface(struct brw_context *brw,
                                 struct gl_renderbuffer *rb,
                                 bool layered,
                                 unsigned unit)
{
   struct gl_context *ctx = &brw->ctx;
   struct intel_renderbuffer *irb = intel_renderbuffer(rb);
   struct intel_mipmap_tree *mt = irb->mt;
   struct intel_region *region = mt->region;
   uint32_t format = 0;
   uint32_t surf_type;
   bool is_array = false;
   int depth = MAX2(rb->Depth, 1);
   int min_array_element;

   GLenum gl_target =
      rb->TexImage ? rb->TexImage->TexObject->Target : GL_TEXTURE_2D;

   if (gl_target == GL_TEXTURE_1D_ARRAY)
      depth = MAX2(rb->Height, 1);

   uint32_t surf_index =
      brw->wm.prog_data->binding_table.render_target_start + unit;

   intel_miptree_used_for_rendering(mt);

   /* Render targets can't use IMS layout. */
   assert(mt->msaa_layout != INTEL_MSAA_LAYOUT_IMS);

   switch (gl_target) {
   case GL_TEXTURE_CUBE_MAP_ARRAY:
   case GL_TEXTURE_CUBE_MAP:
      surf_type = BRW_SURFACE_2D;
      is_array = true;
      depth *= 6;
      break;
   default:
      surf_type = translate_tex_target(gl_target);
      is_array = _mesa_tex_target_is_array(gl_target);
      break;
   }

   if (layered) {
      min_array_element = 0;
   } else if (mt->num_samples > 1) {
      min_array_element = irb->mt_layer / mt->num_samples;
   } else {
      min_array_element = irb->mt_layer;
   }

   /* _NEW_BUFFERS */
   mesa_format rb_format = _mesa_get_render_format(ctx, intel_rb_format(irb));
   assert(brw_render_target_supported(brw, rb));
   format = brw->render_target_format[rb_format];
   if (unlikely(!brw->format_supported_as_render_target[rb_format])) {
      _mesa_problem(ctx, "%s: renderbuffer format %s unsupported\n",
                    __FUNCTION__, _mesa_get_format_name(rb_format));
   }

   uint32_t *surf = brw_state_batch(brw, AUB_TRACE_SURFACE_STATE, 13 * 4, 64,
                                    &brw->wm.base.surf_offset[surf_index]);

   surf[0] = (surf_type << BRW_SURFACE_TYPE_SHIFT) |
             (is_array ? GEN7_SURFACE_IS_ARRAY : 0) |
             (format << BRW_SURFACE_FORMAT_SHIFT) |
             vertical_alignment(mt) |
             horizontal_alignment(mt) |
             surface_tiling_mode(region->tiling);

   surf[1] = mt->qpitch >> 2;

   surf[2] = SET_FIELD(mt->logical_width0 - 1, GEN7_SURFACE_WIDTH) |
             SET_FIELD(mt->logical_height0 - 1, GEN7_SURFACE_HEIGHT);

   surf[3] = (depth - 1) << BRW_SURFACE_DEPTH_SHIFT |
             (region->pitch - 1); /* Surface Pitch */

   surf[4] = surface_num_multisamples(mt->num_samples) |
             min_array_element << GEN7_SURFACE_MIN_ARRAY_ELEMENT_SHIFT |
             (depth - 1) << GEN7_SURFACE_RENDER_TARGET_VIEW_EXTENT_SHIFT;

   surf[5] = irb->mt_level - irb->mt->first_level;

   surf[6] = 0; /* Nothing of relevance. */

   surf[7] = mt->fast_clear_color_value |
             SET_FIELD(HSW_SCS_RED,   GEN7_SURFACE_SCS_R) |
             SET_FIELD(HSW_SCS_GREEN, GEN7_SURFACE_SCS_G) |
             SET_FIELD(HSW_SCS_BLUE,  GEN7_SURFACE_SCS_B) |
             SET_FIELD(HSW_SCS_ALPHA, GEN7_SURFACE_SCS_A);

   *((uint64_t *) &surf[8]) = region->bo->offset64; /* reloc */

   /* Nothing of relevance. */
   surf[10] = 0;
   surf[11] = 0;
   surf[12] = 0;

   drm_intel_bo_emit_reloc(brw->batch.bo,
                           brw->wm.base.surf_offset[surf_index] + 8 * 4,
                           region->bo,
                           0,
                           I915_GEM_DOMAIN_RENDER,
                           I915_GEM_DOMAIN_RENDER);
}

void
gen8_init_vtable_surface_functions(struct brw_context *brw)
{
   brw->vtbl.update_texture_surface = gen8_update_texture_surface;
   brw->vtbl.update_renderbuffer_surface = gen8_update_renderbuffer_surface;
   brw->vtbl.update_null_renderbuffer_surface =
      gen8_update_null_renderbuffer_surface;
   brw->vtbl.emit_buffer_surface_state = gen8_emit_buffer_surface_state;
}
