/*
 * Copyright © 2018 Intel Corporation
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

#include "nir.h"
#include "nir_builder.h"
#include "nir_deref.h"

void
nir_deref_path_init(nir_deref_path *path,
                    nir_deref_instr *deref, void *mem_ctx)
{
   assert(deref != NULL);

   /* The length of the short path is at most ARRAY_SIZE - 1 because we need
    * room for the NULL terminator.
    */
   static const int max_short_path_len = ARRAY_SIZE(path->_short_path) - 1;

   int count = 0;

   nir_deref_instr **tail = &path->_short_path[max_short_path_len];
   nir_deref_instr **head = tail;

   *tail = NULL;
   for (nir_deref_instr *d = deref; d; d = nir_deref_instr_parent(d)) {
      count++;
      if (count <= max_short_path_len)
         *(--head) = d;
   }

   if (count <= max_short_path_len) {
      /* If we're under max_short_path_len, just use the short path. */
      path->path = head;
      goto done;
   }

#ifndef NDEBUG
   /* Just in case someone uses short_path by accident */
   for (unsigned i = 0; i < ARRAY_SIZE(path->_short_path); i++)
      path->_short_path[i] = (void *)0xdeadbeef;
#endif

   path->path = ralloc_array(mem_ctx, nir_deref_instr *, count + 1);
   head = tail = path->path + count;
   *tail = NULL;
   for (nir_deref_instr *d = deref; d; d = nir_deref_instr_parent(d))
      *(--head) = d;

done:
   assert(head == path->path);
   assert(tail == head + count);
   assert((*head)->deref_type == nir_deref_type_var);
   assert(*tail == NULL);
}

void
nir_deref_path_finish(nir_deref_path *path)
{
   if (path->path < &path->_short_path[0] ||
       path->path > &path->_short_path[ARRAY_SIZE(path->_short_path) - 1])
      ralloc_free(path->path);
}

/**
 * Recursively removes unused deref instructions
 */
bool
nir_deref_instr_remove_if_unused(nir_deref_instr *instr)
{
   bool progress = false;

   for (nir_deref_instr *d = instr; d; d = nir_deref_instr_parent(d)) {
      /* If anyone is using this deref, leave it alone */
      assert(d->dest.is_ssa);
      if (!list_empty(&d->dest.ssa.uses))
         break;

      nir_instr_remove(&d->instr);
      progress = true;
   }

   return progress;
}

bool
nir_deref_instr_has_indirect(nir_deref_instr *instr)
{
   while (instr->deref_type != nir_deref_type_var) {
      /* Consider casts to be indirects */
      if (instr->deref_type == nir_deref_type_cast)
         return true;

      if (instr->deref_type == nir_deref_type_array &&
          !nir_src_as_const_value(instr->arr.index))
         return true;

      instr = nir_deref_instr_parent(instr);
   }

   return false;
}

static unsigned
type_get_array_stride(const struct glsl_type *elem_type,
                      glsl_type_size_align_func size_align)
{
   unsigned elem_size, elem_align;
   glsl_get_natural_size_align_bytes(elem_type, &elem_size, &elem_align);
   return ALIGN_POT(elem_size, elem_align);
}

static unsigned
struct_type_get_field_offset(const struct glsl_type *struct_type,
                             glsl_type_size_align_func size_align,
                             unsigned field_idx)
{
   assert(glsl_type_is_struct(struct_type));
   unsigned offset = 0;
   for (unsigned i = 0; i <= field_idx; i++) {
      unsigned elem_size, elem_align;
      glsl_get_natural_size_align_bytes(glsl_get_struct_field(struct_type, i),
                                        &elem_size, &elem_align);
      offset = ALIGN_POT(offset, elem_align);
      if (i < field_idx)
         offset += elem_size;
   }
   return offset;
}

unsigned
nir_deref_instr_get_const_offset(nir_deref_instr *deref,
                                 glsl_type_size_align_func size_align)
{
   nir_deref_path path;
   nir_deref_path_init(&path, deref, NULL);

   assert(path.path[0]->deref_type == nir_deref_type_var);

   unsigned offset = 0;
   for (nir_deref_instr **p = &path.path[1]; *p; p++) {
      if ((*p)->deref_type == nir_deref_type_array) {
         offset += nir_src_as_const_value((*p)->arr.index)->u32[0] *
                   type_get_array_stride((*p)->type, size_align);
      } else if ((*p)->deref_type == nir_deref_type_struct) {
         /* p starts at path[1], so this is safe */
         nir_deref_instr *parent = *(p - 1);
         offset += struct_type_get_field_offset(parent->type, size_align,
                                                (*p)->strct.index);
      } else {
         unreachable("Unsupported deref type");
      }
   }

   nir_deref_path_finish(&path);

   return offset;
}

nir_ssa_def *
nir_build_deref_offset(nir_builder *b, nir_deref_instr *deref,
                       glsl_type_size_align_func size_align)
{
   nir_deref_path path;
   nir_deref_path_init(&path, deref, NULL);

   assert(path.path[0]->deref_type == nir_deref_type_var);

   nir_ssa_def *offset = nir_imm_int(b, 0);
   for (nir_deref_instr **p = &path.path[1]; *p; p++) {
      if ((*p)->deref_type == nir_deref_type_array) {
         nir_ssa_def *index = nir_ssa_for_src(b, (*p)->arr.index, 1);
         nir_ssa_def *stride =
            nir_imm_int(b, type_get_array_stride((*p)->type, size_align));
         offset = nir_iadd(b, offset, nir_imul(b, index, stride));
      } else if ((*p)->deref_type == nir_deref_type_struct) {
         /* p starts at path[1], so this is safe */
         nir_deref_instr *parent = *(p - 1);
         unsigned field_offset =
            struct_type_get_field_offset(parent->type, size_align,
                                         (*p)->strct.index);
         nir_iadd(b, offset, nir_imm_int(b, field_offset));
      } else {
         unreachable("Unsupported deref type");
      }
   }

   nir_deref_path_finish(&path);

   return offset;
}

bool
nir_remove_dead_derefs_impl(nir_function_impl *impl)
{
   bool progress = false;

   nir_foreach_block(block, impl) {
      nir_foreach_instr_safe(instr, block) {
         if (instr->type == nir_instr_type_deref &&
             nir_deref_instr_remove_if_unused(nir_instr_as_deref(instr)))
            progress = true;
      }
   }

   if (progress)
      nir_metadata_preserve(impl, nir_metadata_block_index |
                                  nir_metadata_dominance);

   return progress;
}

bool
nir_remove_dead_derefs(nir_shader *shader)
{
   bool progress = false;
   nir_foreach_function(function, shader) {
      if (function->impl && nir_remove_dead_derefs_impl(function->impl))
         progress = true;
   }

   return progress;
}

void
nir_fixup_deref_modes(nir_shader *shader)
{
   nir_foreach_function(function, shader) {
      if (!function->impl)
         continue;

      nir_foreach_block(block, function->impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_deref)
               continue;

            nir_deref_instr *deref = nir_instr_as_deref(instr);

            nir_variable_mode parent_mode;
            if (deref->deref_type == nir_deref_type_var) {
               parent_mode = deref->var->data.mode;
            } else {
               assert(deref->parent.is_ssa);
               nir_deref_instr *parent =
                  nir_instr_as_deref(deref->parent.ssa->parent_instr);
               parent_mode = parent->mode;
            }

            deref->mode = parent_mode;
         }
      }
   }
}
