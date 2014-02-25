/*
 * Copyright 2013 Advanced Micro Devices, Inc.
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Authors: Marek Olšák <maraeo@gmail.com>
 *
 */

/**
 * This file contains common screen and context structures and functions
 * for r600g and radeonsi.
 */

#ifndef R600_PIPE_COMMON_H
#define R600_PIPE_COMMON_H

#include <stdio.h>

#include "../../winsys/radeon/drm/radeon_winsys.h"

#include "util/u_double_list.h"
#include "util/u_range.h"
#include "util/u_slab.h"
#include "util/u_suballoc.h"
#include "util/u_transfer.h"

#define R600_RESOURCE_FLAG_TRANSFER		(PIPE_RESOURCE_FLAG_DRV_PRIV << 0)
#define R600_RESOURCE_FLAG_FLUSHED_DEPTH	(PIPE_RESOURCE_FLAG_DRV_PRIV << 1)
#define R600_RESOURCE_FLAG_FORCE_TILING		(PIPE_RESOURCE_FLAG_DRV_PRIV << 2)

#define R600_QUERY_DRAW_CALLS		(PIPE_QUERY_DRIVER_SPECIFIC + 0)
#define R600_QUERY_REQUESTED_VRAM	(PIPE_QUERY_DRIVER_SPECIFIC + 1)
#define R600_QUERY_REQUESTED_GTT	(PIPE_QUERY_DRIVER_SPECIFIC + 2)
#define R600_QUERY_BUFFER_WAIT_TIME	(PIPE_QUERY_DRIVER_SPECIFIC + 3)

/* read caches */
#define R600_CONTEXT_INV_VERTEX_CACHE		(1 << 0)
#define R600_CONTEXT_INV_TEX_CACHE		(1 << 1)
#define R600_CONTEXT_INV_CONST_CACHE		(1 << 2)
#define R600_CONTEXT_INV_SHADER_CACHE		(1 << 3)
/* read-write caches */
#define R600_CONTEXT_STREAMOUT_FLUSH		(1 << 8)
#define R600_CONTEXT_FLUSH_AND_INV		(1 << 9)
#define R600_CONTEXT_FLUSH_AND_INV_CB_META	(1 << 10)
#define R600_CONTEXT_FLUSH_AND_INV_DB_META	(1 << 11)
#define R600_CONTEXT_FLUSH_AND_INV_DB		(1 << 12)
#define R600_CONTEXT_FLUSH_AND_INV_CB		(1 << 13)
/* engine synchronization */
#define R600_CONTEXT_PS_PARTIAL_FLUSH		(1 << 16)
#define R600_CONTEXT_WAIT_3D_IDLE		(1 << 17)
#define R600_CONTEXT_WAIT_CP_DMA_IDLE		(1 << 18)
#define R600_CONTEXT_VGT_FLUSH			(1 << 19)

/* Debug flags. */
/* logging */
#define DBG_TEX			(1 << 0)
#define DBG_TEXMIP		(1 << 1)
#define DBG_COMPUTE		(1 << 2)
#define DBG_VM			(1 << 3)
#define DBG_TRACE_CS		(1 << 4)
/* shaders */
#define DBG_FS			(1 << 8)
#define DBG_VS			(1 << 9)
#define DBG_GS			(1 << 10)
#define DBG_PS			(1 << 11)
#define DBG_CS			(1 << 12)
/* features */
#define DBG_HYPERZ		(1 << 13)
#define DBG_NO_DISCARD_RANGE	(1 << 14)
/* The maximum allowed bit is 15. */

#define R600_MAP_BUFFER_ALIGNMENT 64

struct r600_common_context;

struct r600_resource {
	struct u_resource		b;

	/* Winsys objects. */
	struct pb_buffer		*buf;
	struct radeon_winsys_cs_handle	*cs_buf;

	/* Resource state. */
	enum radeon_bo_domain		domains;

	/* The buffer range which is initialized (with a write transfer,
	 * streamout, DMA, or as a random access target). The rest of
	 * the buffer is considered invalid and can be mapped unsynchronized.
	 *
	 * This allows unsychronized mapping of a buffer range which hasn't
	 * been used yet. It's for applications which forget to use
	 * the unsynchronized map flag and expect the driver to figure it out.
         */
	struct util_range		valid_buffer_range;
};

struct r600_transfer {
	struct pipe_transfer		transfer;
	struct r600_resource		*staging;
	unsigned			offset;
};

struct r600_fmask_info {
	unsigned offset;
	unsigned size;
	unsigned alignment;
	unsigned pitch;
	unsigned bank_height;
	unsigned slice_tile_max;
	unsigned tile_mode_index;
};

struct r600_cmask_info {
	unsigned offset;
	unsigned size;
	unsigned alignment;
	unsigned slice_tile_max;
};

struct r600_texture {
	struct r600_resource		resource;

	unsigned			size;
	unsigned			pitch_override;
	bool				is_depth;
	unsigned			dirty_level_mask; /* each bit says if that mipmap is compressed */
	struct r600_texture		*flushed_depth_texture;
	boolean				is_flushing_texture;
	struct radeon_surface		surface;

	/* Colorbuffer compression and fast clear. */
	struct r600_fmask_info		fmask;
	struct r600_cmask_info		cmask;
	struct r600_resource		*cmask_buffer;
	unsigned			color_clear_value[2];

	/* Depth buffer compression and fast clear. */
	struct r600_resource		*htile_buffer;
	float				depth_clear_value;

	bool				non_disp_tiling; /* R600-Cayman only */
	unsigned			mipmap_shift;
};

struct r600_tiling_info {
	unsigned num_channels;
	unsigned num_banks;
	unsigned group_bytes;
};

struct r600_common_screen {
	struct pipe_screen		b;
	struct radeon_winsys		*ws;
	enum radeon_family		family;
	enum chip_class			chip_class;
	struct radeon_info		info;
	struct r600_tiling_info		tiling_info;
	unsigned			debug_flags;
	bool				has_cp_dma;
	bool				has_streamout;

	/* Auxiliary context. Mainly used to initialize resources.
	 * It must be locked prior to using and flushed before unlocking. */
	struct pipe_context		*aux_context;
	pipe_mutex			aux_context_lock;

	struct r600_resource		*trace_bo;
	uint32_t			*trace_ptr;
	unsigned			cs_count;
};

/* This encapsulates a state or an operation which can emitted into the GPU
 * command stream. */
struct r600_atom {
	void (*emit)(struct r600_common_context *ctx, struct r600_atom *state);
	unsigned		num_dw;
	bool			dirty;
};

struct r600_so_target {
	struct pipe_stream_output_target b;

	/* The buffer where BUFFER_FILLED_SIZE is stored. */
	struct r600_resource	*buf_filled_size;
	unsigned		buf_filled_size_offset;

	unsigned		stride_in_dw;
};

struct r600_streamout {
	struct r600_atom		begin_atom;
	bool				begin_emitted;
	unsigned			num_dw_for_end;

	unsigned			enabled_mask;
	unsigned			num_targets;
	struct r600_so_target		*targets[PIPE_MAX_SO_BUFFERS];

	unsigned			append_bitmask;
	bool				suspended;

	/* External state which comes from the vertex shader,
	 * it must be set explicitly when binding a shader. */
	unsigned			*stride_in_dw;
};

struct r600_ring {
	struct radeon_winsys_cs		*cs;
	bool				flushing;
	void (*flush)(void *ctx, unsigned flags);
};

struct r600_rings {
	struct r600_ring		gfx;
	struct r600_ring		dma;
};

struct r600_common_context {
	struct pipe_context b; /* base class */

	struct r600_common_screen	*screen;
	struct radeon_winsys		*ws;
	enum radeon_family		family;
	enum chip_class			chip_class;
	struct r600_rings		rings;
	unsigned			initial_gfx_cs_size;

	struct u_upload_mgr		*uploader;
	struct u_suballocator		*allocator_so_filled_size;
	struct util_slab_mempool	pool_transfers;

	/* Current unaccounted memory usage. */
	uint64_t			vram;
	uint64_t			gtt;

	/* States. */
	struct r600_streamout		streamout;

	/* Additional context states. */
	unsigned flags; /* flush flags */

	/* Queries. */
	/* The list of active queries. Only one query of each type can be active. */
	int				num_occlusion_queries;
	int				num_pipelinestat_queries;
	/* Keep track of non-timer queries, because they should be suspended
	 * during context flushing.
	 * The timer queries (TIME_ELAPSED) shouldn't be suspended. */
	struct list_head		active_nontimer_queries;
	unsigned			num_cs_dw_nontimer_queries_suspend;
	/* If queries have been suspended. */
	bool				nontimer_queries_suspended;
	/* Additional hardware info. */
	unsigned			backend_mask;
	unsigned			max_db; /* for OQ */
	/* Misc stats. */
	unsigned			num_draw_calls;

	/* Render condition. */
	struct pipe_query		*current_render_cond;
	unsigned			current_render_cond_mode;
	boolean				current_render_cond_cond;
	boolean				predicate_drawing;

	/* Copy one resource to another using async DMA.
	 * False is returned if the copy couldn't be done. */
	boolean (*dma_copy)(struct pipe_context *ctx,
			    struct pipe_resource *dst,
			    unsigned dst_level,
			    unsigned dst_x, unsigned dst_y, unsigned dst_z,
			    struct pipe_resource *src,
			    unsigned src_level,
			    const struct pipe_box *src_box);

	void (*clear_buffer)(struct pipe_context *ctx, struct pipe_resource *dst,
			     unsigned offset, unsigned size, unsigned value);

	void (*blit_decompress_depth)(struct pipe_context *ctx,
				      struct r600_texture *texture,
				      struct r600_texture *staging,
				      unsigned first_level, unsigned last_level,
				      unsigned first_layer, unsigned last_layer,
				      unsigned first_sample, unsigned last_sample);

	/* Reallocate the buffer and update all resource bindings where
	 * the buffer is bound, including all resource descriptors. */
	void (*invalidate_buffer)(struct pipe_context *ctx, struct pipe_resource *buf);

	/* Enable or disable occlusion queries. */
	void (*set_occlusion_query_state)(struct pipe_context *ctx, bool enable);

	/* This ensures there is enough space in the command stream. */
	void (*need_gfx_cs_space)(struct pipe_context *ctx, unsigned num_dw,
				  bool include_draw_vbo);
};

/* r600_buffer.c */
boolean r600_rings_is_buffer_referenced(struct r600_common_context *ctx,
					struct radeon_winsys_cs_handle *buf,
					enum radeon_bo_usage usage);
void *r600_buffer_map_sync_with_rings(struct r600_common_context *ctx,
                                      struct r600_resource *resource,
                                      unsigned usage);
bool r600_init_resource(struct r600_common_screen *rscreen,
			struct r600_resource *res,
			unsigned size, unsigned alignment,
			bool use_reusable_pool, unsigned usage);
struct pipe_resource *r600_buffer_create(struct pipe_screen *screen,
					 const struct pipe_resource *templ,
					 unsigned alignment);

/* r600_common_pipe.c */
bool r600_common_screen_init(struct r600_common_screen *rscreen,
			     struct radeon_winsys *ws);
void r600_destroy_common_screen(struct r600_common_screen *rscreen);
bool r600_common_context_init(struct r600_common_context *rctx,
			      struct r600_common_screen *rscreen);
void r600_common_context_cleanup(struct r600_common_context *rctx);
void r600_context_add_resource_size(struct pipe_context *ctx, struct pipe_resource *r);
bool r600_can_dump_shader(struct r600_common_screen *rscreen,
			  const struct tgsi_token *tokens);
void r600_screen_clear_buffer(struct r600_common_screen *rscreen, struct pipe_resource *dst,
			      unsigned offset, unsigned size, unsigned value);
struct pipe_resource *r600_resource_create_common(struct pipe_screen *screen,
						  const struct pipe_resource *templ);
const char *r600_get_llvm_processor_name(enum radeon_family family);

/* r600_query.c */
void r600_query_init(struct r600_common_context *rctx);
void r600_suspend_nontimer_queries(struct r600_common_context *ctx);
void r600_resume_nontimer_queries(struct r600_common_context *ctx);
void r600_query_init_backend_mask(struct r600_common_context *ctx);

/* r600_streamout.c */
void r600_streamout_buffers_dirty(struct r600_common_context *rctx);
void r600_set_streamout_targets(struct pipe_context *ctx,
				unsigned num_targets,
				struct pipe_stream_output_target **targets,
				unsigned append_bitmask);
void r600_emit_streamout_end(struct r600_common_context *rctx);
void r600_streamout_init(struct r600_common_context *rctx);

/* r600_texture.c */
void r600_texture_get_fmask_info(struct r600_common_screen *rscreen,
				 struct r600_texture *rtex,
				 unsigned nr_samples,
				 struct r600_fmask_info *out);
void r600_texture_get_cmask_info(struct r600_common_screen *rscreen,
				 struct r600_texture *rtex,
				 struct r600_cmask_info *out);
void r600_texture_init_cmask(struct r600_common_screen *rscreen,
			     struct r600_texture *rtex);
bool r600_init_flushed_depth_texture(struct pipe_context *ctx,
				     struct pipe_resource *texture,
				     struct r600_texture **staging);
struct pipe_resource *r600_texture_create(struct pipe_screen *screen,
					const struct pipe_resource *templ);
void r600_init_texture_functions(struct r600_common_screen *rscreen);


/* Inline helpers. */

static INLINE struct r600_resource *r600_resource(struct pipe_resource *r)
{
	return (struct r600_resource*)r;
}

static INLINE void
r600_resource_reference(struct r600_resource **ptr, struct r600_resource *res)
{
	pipe_resource_reference((struct pipe_resource **)ptr,
				(struct pipe_resource *)res);
}

#define R600_ERR(fmt, args...) \
	fprintf(stderr, "EE %s:%d %s - "fmt, __FILE__, __LINE__, __func__, ##args)

#endif
