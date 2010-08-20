/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
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
#ifndef R600_CONTEXT_H
#define R600_CONTEXT_H

#include <stdio.h>
#include <pipe/p_state.h>
#include <pipe/p_context.h>
#include <tgsi/tgsi_scan.h>
#include <tgsi/tgsi_parse.h>
#include <tgsi/tgsi_util.h>
#include <util/u_blitter.h>
#include "radeon.h"
#include "r600_shader.h"

/* XXX move this to a more appropriate place */
union pipe_states {
	struct pipe_rasterizer_state		rasterizer;
	struct pipe_poly_stipple		poly_stipple;
	struct pipe_scissor_state		scissor;
	struct pipe_clip_state			clip;
	struct pipe_shader_state		shader;
	struct pipe_depth_state			depth;
	struct pipe_stencil_state		stencil;
	struct pipe_alpha_state			alpha;
	struct pipe_depth_stencil_alpha_state	dsa;
	struct pipe_blend_state			blend;
	struct pipe_blend_color			blend_color;
	struct pipe_stencil_ref			stencil_ref;
	struct pipe_framebuffer_state		framebuffer;
	struct pipe_sampler_state		sampler;
	struct pipe_sampler_view		sampler_view;
	struct pipe_viewport_state		viewport;
};

enum pipe_state_type {
	pipe_rasterizer_type = 1,
	pipe_poly_stipple_type,
	pipe_scissor_type,
	pipe_clip_type,
	pipe_shader_type,
	pipe_depth_type,
	pipe_stencil_type,
	pipe_alpha_type,
	pipe_dsa_type,
	pipe_blend_type,
	pipe_stencil_ref_type,
	pipe_framebuffer_type,
	pipe_sampler_type,
	pipe_sampler_view_type,
	pipe_viewport_type,
	pipe_type_count
};

struct r600_context_state {
	union pipe_states		state;
	unsigned			refcount;
	unsigned			type;
	struct radeon_state		*rstate;
	struct r600_shader		shader;
	struct radeon_bo		*bo;
};

struct r600_vertex_element
{
	unsigned			refcount;
	unsigned			count;
	struct pipe_vertex_element	elements[32];
};

struct r600_context_hw_states {
	struct radeon_state	*rasterizer;
	struct radeon_state	*scissor;
	struct radeon_state	*dsa;
	struct radeon_state	*blend;
	struct radeon_state	*viewport;
	struct radeon_state	*cb[8];
	struct radeon_state	*config;
	struct radeon_state	*cb_cntl;
	struct radeon_state	*db;
	struct radeon_state	*ucp[6];
	unsigned		ps_nresource;
	unsigned		ps_nsampler;
	struct radeon_state	*ps_resource[160];
	struct radeon_state	*ps_sampler[16];
};

struct r600_context {
	struct pipe_context		context;
	struct r600_screen		*screen;
	struct radeon			*rw;
	struct radeon_ctx		*ctx;
	struct blitter_context		*blitter;
	struct radeon_draw		*draw;
	/* hw states */
	struct r600_context_hw_states	hw_states;
	/* pipe states */
	unsigned			flat_shade;
	unsigned			ps_nsampler;
	unsigned			vs_nsampler;
	unsigned			ps_nsampler_view;
	unsigned			vs_nsampler_view;
	unsigned			nvertex_buffer;
	struct r600_context_state	*rasterizer;
	struct r600_context_state	*poly_stipple;
	struct r600_context_state	*scissor;
	struct r600_context_state	*clip;
	struct r600_context_state	*ps_shader;
	struct r600_context_state	*vs_shader;
	struct r600_context_state	*depth;
	struct r600_context_state	*stencil;
	struct r600_context_state	*alpha;
	struct r600_context_state	*dsa;
	struct r600_context_state	*blend;
	struct r600_context_state	*stencil_ref;
	struct r600_context_state	*viewport;
	struct r600_context_state	*framebuffer;
	struct r600_context_state	*ps_sampler[PIPE_MAX_ATTRIBS];
	struct r600_context_state	*vs_sampler[PIPE_MAX_ATTRIBS];
	struct r600_context_state	*ps_sampler_view[PIPE_MAX_ATTRIBS];
	struct r600_context_state	*vs_sampler_view[PIPE_MAX_ATTRIBS];
	struct r600_vertex_element	*vertex_elements;
	struct pipe_vertex_buffer	vertex_buffer[PIPE_MAX_ATTRIBS];
	struct pipe_index_buffer	index_buffer;
	struct pipe_blend_color         blend_color;
};

/* Convenience cast wrapper. */
static INLINE struct r600_context *r600_context(struct pipe_context *pipe)
{
    return (struct r600_context*)pipe;
}

struct r600_context_state *r600_context_state(struct r600_context *rctx, unsigned type, const void *state);
struct r600_context_state *r600_context_state_incref(struct r600_context_state *rstate);
struct r600_context_state *r600_context_state_decref(struct r600_context_state *rstate);
void r600_flush(struct pipe_context *ctx, unsigned flags,
			struct pipe_fence_handle **fence);

int r600_context_hw_states(struct r600_context *rctx);

void r600_draw_vbo(struct pipe_context *ctx,
                   const struct pipe_draw_info *info);

void r600_init_blit_functions(struct r600_context *rctx);
void r600_init_state_functions(struct r600_context *rctx);
void r600_init_query_functions(struct r600_context* rctx);
struct pipe_context *r600_create_context(struct pipe_screen *screen, void *priv);

extern int r600_pipe_shader_create(struct pipe_context *ctx,
			struct r600_context_state *rstate,
			const struct tgsi_token *tokens);
extern int r600_pipe_shader_update(struct pipe_context *ctx,
				struct r600_context_state *rstate);

#define R600_ERR(fmt, args...) \
	fprintf(stderr, "EE %s/%s:%d - "fmt, __FILE__, __func__, __LINE__, ##args)

uint32_t r600_translate_texformat(enum pipe_format format,
				  const unsigned char *swizzle_view, 
				  uint32_t *word4_p, uint32_t *yuv_format_p);
#endif
