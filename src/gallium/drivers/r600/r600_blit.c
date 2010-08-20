/*
 * Copyright 2009 Marek Olšák <maraeo@gmail.com>
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
 *
 * Authors:
 *      Jerome Glisse
 *      Marek Olšák
 */
#include <pipe/p_screen.h>
#include <util/u_blitter.h>
#include <util/u_inlines.h>
#include <util/u_memory.h>
#include "util/u_surface.h"
#include "r600_screen.h"
#include "r600_context.h"

static void r600_blitter_save_states(struct r600_context *rctx)
{
	util_blitter_save_blend(rctx->blitter, rctx->blend);
	util_blitter_save_depth_stencil_alpha(rctx->blitter, rctx->dsa);
	if (rctx->stencil_ref) {
		util_blitter_save_stencil_ref(rctx->blitter,
					&rctx->stencil_ref->state.stencil_ref);
	}
	util_blitter_save_rasterizer(rctx->blitter, rctx->rasterizer);
	util_blitter_save_fragment_shader(rctx->blitter, rctx->ps_shader);
	util_blitter_save_vertex_shader(rctx->blitter, rctx->vs_shader);
	util_blitter_save_vertex_elements(rctx->blitter, rctx->vertex_elements);
	if (rctx->viewport) {
		util_blitter_save_viewport(rctx->blitter, &rctx->viewport->state.viewport);
	}
	if (rctx->clip)
 	    util_blitter_save_clip(rctx->blitter, &rctx->clip->state.clip);
	util_blitter_save_vertex_buffers(rctx->blitter, rctx->nvertex_buffer,
					rctx->vertex_buffer);

	/* remove ptr so they don't get deleted */
	rctx->blend = NULL;
	rctx->clip = NULL;
	rctx->vs_shader = NULL;
	rctx->ps_shader = NULL;
	rctx->rasterizer = NULL;
	rctx->dsa = NULL;
	rctx->vertex_elements = NULL;
}

static void r600_clear(struct pipe_context *ctx, unsigned buffers,
		       const float *rgba, double depth, unsigned stencil)
{
	struct r600_context *rctx = r600_context(ctx);
	struct pipe_framebuffer_state *fb = &rctx->framebuffer->state.framebuffer;

	r600_blitter_save_states(rctx);
	util_blitter_clear(rctx->blitter, fb->width, fb->height,
				fb->nr_cbufs, buffers, rgba, depth,
				stencil);
}

static void r600_clear_render_target(struct pipe_context *pipe,
				     struct pipe_surface *dst,
				     const float *rgba,
				     unsigned dstx, unsigned dsty,
				     unsigned width, unsigned height)
{
	struct r600_context *rctx = r600_context(pipe);
	struct pipe_framebuffer_state *fb = &rctx->framebuffer->state.framebuffer;

	r600_blitter_save_states(rctx);
	util_blitter_save_framebuffer(rctx->blitter, fb);

	util_blitter_clear_render_target(rctx->blitter, dst, rgba,
					 dstx, dsty, width, height);
}

static void r600_clear_depth_stencil(struct pipe_context *pipe,
				     struct pipe_surface *dst,
				     unsigned clear_flags,
				     double depth,
				     unsigned stencil,
				     unsigned dstx, unsigned dsty,
				     unsigned width, unsigned height)
{
	struct r600_context *rctx = r600_context(pipe);
	struct pipe_framebuffer_state *fb = &rctx->framebuffer->state.framebuffer;

	r600_blitter_save_states(rctx);
	util_blitter_save_framebuffer(rctx->blitter, fb);

	util_blitter_clear_depth_stencil(rctx->blitter, dst, clear_flags, depth, stencil,
					 dstx, dsty, width, height);
}

static void r600_resource_copy_region(struct pipe_context *pipe,
				      struct pipe_resource *dst,
				      struct pipe_subresource subdst,
				      unsigned dstx, unsigned dsty, unsigned dstz,
				      struct pipe_resource *src,
				      struct pipe_subresource subsrc,
				      unsigned srcx, unsigned srcy, unsigned srcz,
				      unsigned width, unsigned height)
{
	util_resource_copy_region(pipe, dst, subdst, dstx, dsty, dstz,
				  src, subsrc, srcx, srcy, srcz, width, height);
}

void r600_init_blit_functions(struct r600_context *rctx)
{
	rctx->context.clear = r600_clear;
	rctx->context.clear_render_target = r600_clear_render_target;
	rctx->context.clear_depth_stencil = r600_clear_depth_stencil;
	rctx->context.resource_copy_region = r600_resource_copy_region;
}
