#include "radeon_priv.h"

#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_double_list.h"
#include "pipebuffer/pb_buffer.h"
#include "pipebuffer/pb_bufmgr.h"

struct radeon_bo_pb {
	struct pb_buffer b;
	struct radeon_bo *bo;

	struct radeon_bo_pbmgr *mgr;
	struct list_head maplist;
};

extern const struct pb_vtbl radeon_bo_pb_vtbl;

static INLINE struct radeon_bo_pb *radeon_bo_pb(struct pb_buffer *buf)
{
	assert(buf);
	assert(buf->vtbl == &radeon_bo_pb_vtbl);
	return (struct radeon_bo_pb *)buf;
}

struct radeon_bo_pbmgr {
	struct pb_manager b;
	struct radeon *radeon;
	struct list_head buffer_map_list;
};

static INLINE struct radeon_bo_pbmgr *radeon_bo_pbmgr(struct pb_manager *mgr)
{
	assert(mgr);
	return (struct radeon_bo_pbmgr *)mgr;
}

static void radeon_bo_pb_destroy(struct pb_buffer *_buf)
{
	struct radeon_bo_pb *buf = radeon_bo_pb(_buf);

	if (buf->bo->data != NULL) {
		LIST_DEL(&buf->maplist);
		radeon_bo_unmap(buf->mgr->radeon, buf->bo);
	}
	radeon_bo_decref(buf->mgr->radeon, buf->bo);
	FREE(buf);
}

static void *
radeon_bo_pb_map_internal(struct pb_buffer *_buf,
			  unsigned flags)
{
	struct radeon_bo_pb *buf = radeon_bo_pb(_buf);
	
	if (buf->bo->data != NULL)
		return buf->bo->data;

	if (flags & PB_USAGE_DONTBLOCK) {
		uint32_t domain;
		if (radeon_bo_busy(buf->mgr->radeon, buf->bo, &domain))
			return NULL;
	}

	if (radeon_bo_map(buf->mgr->radeon, buf->bo)) {
		return NULL;
	}
	LIST_ADDTAIL(&buf->maplist, &buf->mgr->buffer_map_list);
	return buf->bo->data;
}

static void radeon_bo_pb_unmap_internal(struct pb_buffer *_buf)
{
	(void)_buf;
}

static void
radeon_bo_pb_get_base_buffer(struct pb_buffer *buf,
			     struct pb_buffer **base_buf,
			     unsigned *offset)
{
	*base_buf = buf;
	*offset = 0;
}

static enum pipe_error
radeon_bo_pb_validate(struct pb_buffer *_buf, 
		      struct pb_validate *vl,
		      unsigned flags)
{
	/* Always pinned */
	return PIPE_OK;
}

static void
radeon_bo_pb_fence(struct pb_buffer *buf,
		   struct pipe_fence_handle *fence)
{
}

const struct pb_vtbl radeon_bo_pb_vtbl = {
    radeon_bo_pb_destroy,
    radeon_bo_pb_map_internal,
    radeon_bo_pb_unmap_internal,
    radeon_bo_pb_validate,
    radeon_bo_pb_fence,
    radeon_bo_pb_get_base_buffer,
};

static struct pb_buffer *
radeon_bo_pb_create_buffer(struct pb_manager *_mgr,
			   pb_size size,
			   const struct pb_desc *desc)
{
	struct radeon_bo_pbmgr *mgr = radeon_bo_pbmgr(_mgr);
	struct radeon *radeon = mgr->radeon;
	struct radeon_bo_pb *bo;
	uint32_t domain;

	bo = CALLOC_STRUCT(radeon_bo_pb);
	if (!bo)
		goto error1;

	pipe_reference_init(&bo->b.base.reference, 1);
	bo->b.base.alignment = desc->alignment;
	bo->b.base.usage = desc->usage;
	bo->b.base.size = size;
	bo->b.vtbl = &radeon_bo_pb_vtbl;
	bo->mgr = mgr;

	LIST_INITHEAD(&bo->maplist);

	bo->bo = radeon_bo(radeon, 0, size,
			   desc->alignment, NULL);
	if (bo->bo == NULL)
		goto error2;
	return &bo->b;

error2:
	FREE(bo);
error1:
	return NULL;
}

static void
radeon_bo_pbmgr_flush(struct pb_manager *mgr)
{
    /* NOP */
}

static void
radeon_bo_pbmgr_destroy(struct pb_manager *_mgr)
{
	struct radeon_bo_pbmgr *mgr = radeon_bo_pbmgr(_mgr);
	FREE(mgr);
}

struct pb_manager *radeon_bo_pbmgr_create(struct radeon *radeon)
{
	struct radeon_bo_pbmgr *mgr;

	mgr = CALLOC_STRUCT(radeon_bo_pbmgr);
	if (!mgr)
		return NULL;

	mgr->b.destroy = radeon_bo_pbmgr_destroy;
	mgr->b.create_buffer = radeon_bo_pb_create_buffer;
	mgr->b.flush = radeon_bo_pbmgr_flush;

	mgr->radeon = radeon;
	LIST_INITHEAD(&mgr->buffer_map_list);
	return &mgr->b;
}

void radeon_bo_pbmgr_flush_maps(struct pb_manager *_mgr)
{
	struct radeon_bo_pbmgr *mgr = radeon_bo_pbmgr(_mgr);
	struct radeon_bo_pb *rpb, *t_rpb;

	LIST_FOR_EACH_ENTRY_SAFE(rpb, t_rpb, &mgr->buffer_map_list, maplist) {
		radeon_bo_unmap(mgr->radeon, rpb->bo);
		LIST_DEL(&rpb->maplist);
	}

	LIST_INITHEAD(&mgr->buffer_map_list);
}
