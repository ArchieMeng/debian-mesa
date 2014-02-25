/*
 * Copyright 2013 Advanced Micro Devices, Inc.
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
 *      Marek Olšák
 */

#include "r600_cs.h"
#include "util/u_memory.h"
#include "util/u_upload_mgr.h"
#include <inttypes.h>
#include <stdio.h>

boolean r600_rings_is_buffer_referenced(struct r600_common_context *ctx,
					struct radeon_winsys_cs_handle *buf,
					enum radeon_bo_usage usage)
{
	if (ctx->ws->cs_is_buffer_referenced(ctx->rings.gfx.cs, buf, usage)) {
		return TRUE;
	}
	if (ctx->rings.dma.cs &&
	    ctx->ws->cs_is_buffer_referenced(ctx->rings.dma.cs, buf, usage)) {
		return TRUE;
	}
	return FALSE;
}

void *r600_buffer_map_sync_with_rings(struct r600_common_context *ctx,
                                      struct r600_resource *resource,
                                      unsigned usage)
{
	enum radeon_bo_usage rusage = RADEON_USAGE_READWRITE;

	if (usage & PIPE_TRANSFER_UNSYNCHRONIZED) {
		return ctx->ws->buffer_map(resource->cs_buf, NULL, usage);
	}

	if (!(usage & PIPE_TRANSFER_WRITE)) {
		/* have to wait for the last write */
		rusage = RADEON_USAGE_WRITE;
	}

	if (ctx->rings.gfx.cs->cdw != ctx->initial_gfx_cs_size &&
	    ctx->ws->cs_is_buffer_referenced(ctx->rings.gfx.cs,
					     resource->cs_buf, rusage)) {
		if (usage & PIPE_TRANSFER_DONTBLOCK) {
			ctx->rings.gfx.flush(ctx, RADEON_FLUSH_ASYNC);
			return NULL;
		} else {
			ctx->rings.gfx.flush(ctx, 0);
		}
	}
	if (ctx->rings.dma.cs &&
	    ctx->rings.dma.cs->cdw &&
	    ctx->ws->cs_is_buffer_referenced(ctx->rings.dma.cs,
					     resource->cs_buf, rusage)) {
		if (usage & PIPE_TRANSFER_DONTBLOCK) {
			ctx->rings.dma.flush(ctx, RADEON_FLUSH_ASYNC);
			return NULL;
		} else {
			ctx->rings.dma.flush(ctx, 0);
		}
	}

	if (ctx->ws->buffer_is_busy(resource->buf, rusage)) {
		if (usage & PIPE_TRANSFER_DONTBLOCK) {
			return NULL;
		} else {
			/* We will be wait for the GPU. Wait for any offloaded
			 * CS flush to complete to avoid busy-waiting in the winsys. */
			ctx->ws->cs_sync_flush(ctx->rings.gfx.cs);
			if (ctx->rings.dma.cs)
				ctx->ws->cs_sync_flush(ctx->rings.dma.cs);
		}
	}

	return ctx->ws->buffer_map(resource->cs_buf, NULL, usage);
}

bool r600_init_resource(struct r600_common_screen *rscreen,
			struct r600_resource *res,
			unsigned size, unsigned alignment,
			bool use_reusable_pool, unsigned usage)
{
	uint32_t initial_domain, domains;

	switch(usage) {
	case PIPE_USAGE_STAGING:
		/* Staging resources participate in transfers, i.e. are used
		 * for uploads and downloads from regular resources.
		 * We generate them internally for some transfers.
		 */
		initial_domain = RADEON_DOMAIN_GTT;
		domains = RADEON_DOMAIN_GTT;
		break;
	case PIPE_USAGE_DYNAMIC:
	case PIPE_USAGE_STREAM:
		/* Default to GTT, but allow the memory manager to move it to VRAM. */
		initial_domain = RADEON_DOMAIN_GTT;
		domains = RADEON_DOMAIN_GTT | RADEON_DOMAIN_VRAM;
		break;
	case PIPE_USAGE_DEFAULT:
	case PIPE_USAGE_STATIC:
	case PIPE_USAGE_IMMUTABLE:
	default:
		/* Don't list GTT here, because the memory manager would put some
		 * resources to GTT no matter what the initial domain is.
		 * Not listing GTT in the domains improves performance a lot. */
		initial_domain = RADEON_DOMAIN_VRAM;
		domains = RADEON_DOMAIN_VRAM;
		break;
	}

	res->buf = rscreen->ws->buffer_create(rscreen->ws, size, alignment,
                                              use_reusable_pool,
                                              initial_domain);
	if (!res->buf) {
		return false;
	}

	res->cs_buf = rscreen->ws->buffer_get_cs_handle(res->buf);
	res->domains = domains;
	util_range_set_empty(&res->valid_buffer_range);

	if (rscreen->debug_flags & DBG_VM && res->b.b.target == PIPE_BUFFER) {
		fprintf(stderr, "VM start=0x%"PRIu64"  end=0x%"PRIu64" | Buffer %u bytes\n",
			r600_resource_va(&rscreen->b, &res->b.b),
			r600_resource_va(&rscreen->b, &res->b.b) + res->buf->size,
			res->buf->size);
	}
	return true;
}

static void r600_buffer_destroy(struct pipe_screen *screen,
				struct pipe_resource *buf)
{
	struct r600_resource *rbuffer = r600_resource(buf);

	util_range_destroy(&rbuffer->valid_buffer_range);
	pb_reference(&rbuffer->buf, NULL);
	FREE(rbuffer);
}

static void *r600_buffer_get_transfer(struct pipe_context *ctx,
				      struct pipe_resource *resource,
                                      unsigned level,
                                      unsigned usage,
                                      const struct pipe_box *box,
				      struct pipe_transfer **ptransfer,
				      void *data, struct r600_resource *staging,
				      unsigned offset)
{
	struct r600_common_context *rctx = (struct r600_common_context*)ctx;
	struct r600_transfer *transfer = util_slab_alloc(&rctx->pool_transfers);

	transfer->transfer.resource = resource;
	transfer->transfer.level = level;
	transfer->transfer.usage = usage;
	transfer->transfer.box = *box;
	transfer->transfer.stride = 0;
	transfer->transfer.layer_stride = 0;
	transfer->offset = offset;
	transfer->staging = staging;
	*ptransfer = &transfer->transfer;
	return data;
}

static void *r600_buffer_transfer_map(struct pipe_context *ctx,
                                      struct pipe_resource *resource,
                                      unsigned level,
                                      unsigned usage,
                                      const struct pipe_box *box,
                                      struct pipe_transfer **ptransfer)
{
	struct r600_common_context *rctx = (struct r600_common_context*)ctx;
	struct r600_common_screen *rscreen = (struct r600_common_screen*)ctx->screen;
        struct r600_resource *rbuffer = r600_resource(resource);
        uint8_t *data;

	assert(box->x + box->width <= resource->width0);

	/* See if the buffer range being mapped has never been initialized,
	 * in which case it can be mapped unsynchronized. */
	if (!(usage & PIPE_TRANSFER_UNSYNCHRONIZED) &&
	    usage & PIPE_TRANSFER_WRITE &&
	    !util_ranges_intersect(&rbuffer->valid_buffer_range, box->x, box->x + box->width)) {
		usage |= PIPE_TRANSFER_UNSYNCHRONIZED;
	}

	/* If discarding the entire range, discard the whole resource instead. */
	if (usage & PIPE_TRANSFER_DISCARD_RANGE &&
	    box->x == 0 && box->width == resource->width0) {
		usage |= PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE;
	}

	if (usage & PIPE_TRANSFER_DISCARD_WHOLE_RESOURCE &&
	    !(usage & PIPE_TRANSFER_UNSYNCHRONIZED)) {
		assert(usage & PIPE_TRANSFER_WRITE);

		/* Check if mapping this buffer would cause waiting for the GPU. */
		if (r600_rings_is_buffer_referenced(rctx, rbuffer->cs_buf, RADEON_USAGE_READWRITE) ||
		    rctx->ws->buffer_is_busy(rbuffer->buf, RADEON_USAGE_READWRITE)) {
			rctx->invalidate_buffer(&rctx->b, &rbuffer->b.b);
		}
		/* At this point, the buffer is always idle. */
		usage |= PIPE_TRANSFER_UNSYNCHRONIZED;
	}
	else if ((usage & PIPE_TRANSFER_DISCARD_RANGE) &&
		 !(usage & PIPE_TRANSFER_UNSYNCHRONIZED) &&
		 !(rscreen->debug_flags & DBG_NO_DISCARD_RANGE) &&
		 (rscreen->has_cp_dma ||
		  (rscreen->has_streamout &&
		   /* The buffer range must be aligned to 4 with streamout. */
		   box->x % 4 == 0 && box->width % 4 == 0))) {
		assert(usage & PIPE_TRANSFER_WRITE);

		/* Check if mapping this buffer would cause waiting for the GPU. */
		if (r600_rings_is_buffer_referenced(rctx, rbuffer->cs_buf, RADEON_USAGE_READWRITE) ||
		    rctx->ws->buffer_is_busy(rbuffer->buf, RADEON_USAGE_READWRITE)) {
			/* Do a wait-free write-only transfer using a temporary buffer. */
			unsigned offset;
			struct r600_resource *staging = NULL;

			u_upload_alloc(rctx->uploader, 0, box->width + (box->x % R600_MAP_BUFFER_ALIGNMENT),
				       &offset, (struct pipe_resource**)&staging, (void**)&data);

			if (staging) {
				data += box->x % R600_MAP_BUFFER_ALIGNMENT;
				return r600_buffer_get_transfer(ctx, resource, level, usage, box,
								ptransfer, data, staging, offset);
			}
		}
	}

	data = r600_buffer_map_sync_with_rings(rctx, rbuffer, usage);
	if (!data) {
		return NULL;
	}
	data += box->x;

	return r600_buffer_get_transfer(ctx, resource, level, usage, box,
					ptransfer, data, NULL, 0);
}

static void r600_buffer_transfer_unmap(struct pipe_context *ctx,
				       struct pipe_transfer *transfer)
{
	struct r600_common_context *rctx = (struct r600_common_context*)ctx;
	struct r600_transfer *rtransfer = (struct r600_transfer*)transfer;
	struct r600_resource *rbuffer = r600_resource(transfer->resource);

	if (rtransfer->staging) {
		struct pipe_resource *dst, *src;
		unsigned soffset, doffset, size;
		struct pipe_box box;

		dst = transfer->resource;
		src = &rtransfer->staging->b.b;
		size = transfer->box.width;
		doffset = transfer->box.x;
		soffset = rtransfer->offset + transfer->box.x % R600_MAP_BUFFER_ALIGNMENT;

		u_box_1d(soffset, size, &box);

		/* Copy the staging buffer into the original one. */
		if (!(size % 4) && !(doffset % 4) && !(soffset % 4) &&
		    rctx->dma_copy(ctx, dst, 0, doffset, 0, 0, src, 0, &box)) {
			/* DONE. */
		} else {
			ctx->resource_copy_region(ctx, dst, 0, doffset, 0, 0, src, 0, &box);
		}
		pipe_resource_reference((struct pipe_resource**)&rtransfer->staging, NULL);
	}

	if (transfer->usage & PIPE_TRANSFER_WRITE) {
		util_range_add(&rbuffer->valid_buffer_range, transfer->box.x,
			       transfer->box.x + transfer->box.width);
	}
	util_slab_free(&rctx->pool_transfers, transfer);
}

static const struct u_resource_vtbl r600_buffer_vtbl =
{
	NULL,				/* get_handle */
	r600_buffer_destroy,		/* resource_destroy */
	r600_buffer_transfer_map,	/* transfer_map */
	NULL,				/* transfer_flush_region */
	r600_buffer_transfer_unmap,	/* transfer_unmap */
	NULL				/* transfer_inline_write */
};

struct pipe_resource *r600_buffer_create(struct pipe_screen *screen,
					 const struct pipe_resource *templ,
					 unsigned alignment)
{
	struct r600_common_screen *rscreen = (struct r600_common_screen*)screen;
	struct r600_resource *rbuffer;

	rbuffer = MALLOC_STRUCT(r600_resource);

	rbuffer->b.b = *templ;
	pipe_reference_init(&rbuffer->b.b.reference, 1);
	rbuffer->b.b.screen = screen;
	rbuffer->b.vtbl = &r600_buffer_vtbl;
	util_range_init(&rbuffer->valid_buffer_range);

	if (!r600_init_resource(rscreen, rbuffer, templ->width0, alignment, TRUE, templ->usage)) {
		FREE(rbuffer);
		return NULL;
	}
	return &rbuffer->b.b;
}
