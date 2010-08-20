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
 *
 * Authors:
 *      Jerome Glisse
 *      Corbin Simpson
 */
#include <pipe/p_screen.h>
#include <util/u_format.h>
#include <util/u_math.h>
#include <util/u_inlines.h>
#include <util/u_memory.h>
#include "state_tracker/drm_driver.h"
#include "r600_screen.h"
#include "r600_context.h"
#include "r600_resource.h"
#include "r600d.h"

extern struct u_resource_vtbl r600_texture_vtbl;

static unsigned long r600_texture_get_offset(struct r600_resource_texture *rtex,
					unsigned level, unsigned zslice,
					unsigned face)
{
	unsigned long offset = rtex->offset[level];

	switch (rtex->resource.base.b.target) {
	case PIPE_TEXTURE_3D:
		assert(face == 0);
		return offset + zslice * rtex->layer_size[level];
	case PIPE_TEXTURE_CUBE:
		assert(zslice == 0);
		return offset + face * rtex->layer_size[level];
	default:
		assert(zslice == 0 && face == 0);
		return offset;
	}
}

static void r600_setup_miptree(struct r600_screen *rscreen, struct r600_resource_texture *rtex)
{
	struct pipe_resource *ptex = &rtex->resource.base.b;
	unsigned long w, h, pitch, size, layer_size, i, offset;

	rtex->bpt = util_format_get_blocksize(ptex->format);
	for (i = 0, offset = 0; i <= ptex->last_level; i++) {
		w = u_minify(ptex->width0, i);
		h = u_minify(ptex->height0, i);
		h = util_next_power_of_two(h);
		pitch = util_format_get_stride(ptex->format, align(w, 64));
		pitch = align(pitch, 256);
		layer_size = pitch * h;
		if (ptex->target == PIPE_TEXTURE_CUBE)
			size = layer_size * 6;
		else
			size = layer_size * u_minify(ptex->depth0, i);
		rtex->offset[i] = offset;
		rtex->layer_size[i] = layer_size;
		rtex->pitch[i] = pitch;
		offset += size;
	}
	rtex->size = offset;
}

struct pipe_resource *r600_texture_create(struct pipe_screen *screen,
						const struct pipe_resource *templ)
{
	struct r600_resource_texture *rtex;
	struct r600_resource *resource;
	struct r600_screen *rscreen = r600_screen(screen);

	rtex = CALLOC_STRUCT(r600_resource_texture);
	if (!rtex) {
		return NULL;
	}
	resource = &rtex->resource;
	resource->base.b = *templ;
	resource->base.vtbl = &r600_texture_vtbl;
	pipe_reference_init(&resource->base.b.reference, 1);
	resource->base.b.screen = screen;
	r600_setup_miptree(rscreen, rtex);

	/* FIXME alignment 4096 enought ? too much ? */
	resource->domain = r600_domain_from_usage(resource->base.b.bind);
	resource->bo = radeon_bo(rscreen->rw, 0, rtex->size, 4096, NULL);
	if (resource->bo == NULL) {
		FREE(rtex);
		return NULL;
	}

	return &resource->base.b;
}

static void r600_texture_destroy(struct pipe_screen *screen,
				 struct pipe_resource *ptex)
{
	struct r600_resource_texture *rtex = (struct r600_resource_texture*)ptex;
	struct r600_resource *resource = &rtex->resource;
	struct r600_screen *rscreen = r600_screen(screen);

	if (resource->bo) {
		radeon_bo_decref(rscreen->rw, resource->bo);
	}
	FREE(rtex);
}

static struct pipe_surface *r600_get_tex_surface(struct pipe_screen *screen,
						struct pipe_resource *texture,
						unsigned face, unsigned level,
						unsigned zslice, unsigned flags)
{
	struct r600_resource_texture *rtex = (struct r600_resource_texture*)texture;
	struct pipe_surface *surface = CALLOC_STRUCT(pipe_surface);
	unsigned long offset;

	if (surface == NULL)
		return NULL;
	offset = r600_texture_get_offset(rtex, level, zslice, face);
	pipe_reference_init(&surface->reference, 1);
	pipe_resource_reference(&surface->texture, texture);
	surface->format = texture->format;
	surface->width = u_minify(texture->width0, level);
	surface->height = u_minify(texture->height0, level);
	surface->offset = offset;
	surface->usage = flags;
	surface->zslice = zslice;
	surface->texture = texture;
	surface->face = face;
	surface->level = level;
	return surface;
}

static void r600_tex_surface_destroy(struct pipe_surface *surface)
{
	pipe_resource_reference(&surface->texture, NULL);
	FREE(surface);
}

struct pipe_resource *r600_texture_from_handle(struct pipe_screen *screen,
					       const struct pipe_resource *templ,
					       struct winsys_handle *whandle)
{
	struct radeon *rw = (struct radeon*)screen->winsys;
	struct r600_resource_texture *rtex;
	struct r600_resource *resource;
	struct radeon_bo *bo = NULL;

	bo = radeon_bo(rw, whandle->handle, 0, 0, NULL);
	if (bo == NULL) {
		return NULL;
	}

	/* Support only 2D textures without mipmaps */
	if (templ->target != PIPE_TEXTURE_2D || templ->depth0 != 1 || templ->last_level != 0)
		return NULL;

	rtex = CALLOC_STRUCT(r600_resource_texture);
	if (rtex == NULL)
		return NULL;

	resource = &rtex->resource;
	resource->base.b = *templ;
	resource->base.vtbl = &r600_texture_vtbl;
	pipe_reference_init(&resource->base.b.reference, 1);
	resource->base.b.screen = screen;
	resource->bo = bo;
	rtex->pitch_override = whandle->stride;
	rtex->bpt = util_format_get_blocksize(templ->format);
	rtex->pitch[0] = whandle->stride;
	rtex->offset[0] = 0;
	rtex->size = align(rtex->pitch[0] * templ->height0, 64);

	return &resource->base.b;
}

static unsigned int r600_texture_is_referenced(struct pipe_context *context,
						struct pipe_resource *texture,
						unsigned face, unsigned level)
{
	/* FIXME */
	return PIPE_REFERENCED_FOR_READ | PIPE_REFERENCED_FOR_WRITE;
}

struct pipe_transfer* r600_texture_get_transfer(struct pipe_context *ctx,
						struct pipe_resource *texture,
						struct pipe_subresource sr,
						unsigned usage,
						const struct pipe_box *box)
{
	struct r600_resource_texture *rtex = (struct r600_resource_texture*)texture;
	struct r600_transfer *trans;

	trans = CALLOC_STRUCT(r600_transfer);
	if (trans == NULL)
		return NULL;
	pipe_resource_reference(&trans->transfer.resource, texture);
	trans->transfer.sr = sr;
	trans->transfer.usage = usage;
	trans->transfer.box = *box;
	trans->transfer.stride = rtex->pitch[sr.level];
	trans->offset = r600_texture_get_offset(rtex, sr.level, box->z, sr.face);
	return &trans->transfer;
}

void r600_texture_transfer_destroy(struct pipe_context *ctx,
				   struct pipe_transfer *trans)
{
	pipe_resource_reference(&trans->resource, NULL);
	FREE(trans);
}

void* r600_texture_transfer_map(struct pipe_context *ctx,
				struct pipe_transfer* transfer)
{
	struct r600_transfer *rtransfer = (struct r600_transfer*)transfer;
	struct r600_resource *resource;
	enum pipe_format format = transfer->resource->format;
	struct r600_screen *rscreen = r600_screen(ctx->screen);
	char *map;

	r600_flush(ctx, 0, NULL);

	resource = (struct r600_resource *)transfer->resource;
	if (radeon_bo_map(rscreen->rw, resource->bo)) {
		return NULL;
	}
	radeon_bo_wait(rscreen->rw, resource->bo);

	map = resource->bo->data;

	return map + rtransfer->offset +
		transfer->box.y / util_format_get_blockheight(format) * transfer->stride +
		transfer->box.x / util_format_get_blockwidth(format) * util_format_get_blocksize(format);
}

void r600_texture_transfer_unmap(struct pipe_context *ctx,
				 struct pipe_transfer* transfer)
{
	struct r600_screen *rscreen = r600_screen(ctx->screen);
	struct r600_resource *resource;

	resource = (struct r600_resource *)transfer->resource;
	radeon_bo_unmap(rscreen->rw, resource->bo);
}

struct u_resource_vtbl r600_texture_vtbl =
{
	u_default_resource_get_handle,	/* get_handle */
	r600_texture_destroy,		/* resource_destroy */
	r600_texture_is_referenced,	/* is_resource_referenced */
	r600_texture_get_transfer,	/* get_transfer */
	r600_texture_transfer_destroy,	/* transfer_destroy */
	r600_texture_transfer_map,	/* transfer_map */
	u_default_transfer_flush_region,/* transfer_flush_region */
	r600_texture_transfer_unmap,	/* transfer_unmap */
	u_default_transfer_inline_write	/* transfer_inline_write */
};

void r600_init_screen_texture_functions(struct pipe_screen *screen)
{
	screen->get_tex_surface = r600_get_tex_surface;
	screen->tex_surface_destroy = r600_tex_surface_destroy;
}

static unsigned r600_get_swizzle_combined(const unsigned char *swizzle_format,
					  const unsigned char *swizzle_view)
{
    unsigned i;
    unsigned char swizzle[4];
    unsigned result = 0;
    const uint32_t swizzle_shift[4] = {
	    16, 19, 22, 25,
    };
    const uint32_t swizzle_bit[4] = {
	    0, 1, 2, 3,
    };

    if (swizzle_view) {
        /* Combine two sets of swizzles. */
        for (i = 0; i < 4; i++) {
            swizzle[i] = swizzle_view[i] <= UTIL_FORMAT_SWIZZLE_W ?
                         swizzle_format[swizzle_view[i]] : swizzle_view[i];
        }
    } else {
        memcpy(swizzle, swizzle_format, 4);
    }

    /* Get swizzle. */
    for (i = 0; i < 4; i++) {
        switch (swizzle[i]) {
            case UTIL_FORMAT_SWIZZLE_Y:
                result |= swizzle_bit[1] << swizzle_shift[i];
                break;
            case UTIL_FORMAT_SWIZZLE_Z:
                result |= swizzle_bit[2] << swizzle_shift[i];
                break;
            case UTIL_FORMAT_SWIZZLE_W:
                result |= swizzle_bit[3] << swizzle_shift[i];
                break;
            case UTIL_FORMAT_SWIZZLE_0:
                result |= V_038010_SQ_SEL_0 << swizzle_shift[i];
                break;
            case UTIL_FORMAT_SWIZZLE_1:
                result |= V_038010_SQ_SEL_1 << swizzle_shift[i];
                break;
            default: /* UTIL_FORMAT_SWIZZLE_X */
                result |= swizzle_bit[0] << swizzle_shift[i];
        }
    }
    return result;
}

/* texture format translate */
uint32_t r600_translate_texformat(enum pipe_format format,
				  const unsigned char *swizzle_view, 
				  uint32_t *word4_p, uint32_t *yuv_format_p)
{
	uint32_t result = 0, word4 = 0, yuv_format = 0;
	const struct util_format_description *desc;
	boolean uniform = TRUE;
	int i;
	const uint32_t sign_bit[4] = {
		S_038010_FORMAT_COMP_X(V_038010_SQ_FORMAT_COMP_SIGNED),
		S_038010_FORMAT_COMP_Y(V_038010_SQ_FORMAT_COMP_SIGNED),
		S_038010_FORMAT_COMP_Z(V_038010_SQ_FORMAT_COMP_SIGNED),
		S_038010_FORMAT_COMP_W(V_038010_SQ_FORMAT_COMP_SIGNED)
	};
	desc = util_format_description(format);

	/* Colorspace (return non-RGB formats directly). */
	switch (desc->colorspace) {
		/* Depth stencil formats */
	case UTIL_FORMAT_COLORSPACE_ZS:
		switch (format) {
		case PIPE_FORMAT_Z16_UNORM:
			result = V_028010_DEPTH_16;
			goto out_word4;
		case PIPE_FORMAT_Z24X8_UNORM:
			result = V_028010_DEPTH_X8_24;
			goto out_word4;
		case PIPE_FORMAT_Z24_UNORM_S8_USCALED:
			result = V_028010_DEPTH_8_24;
			goto out_word4;
		default:
			goto out_unknown;
		}

	case UTIL_FORMAT_COLORSPACE_YUV:
		yuv_format |= (1 << 30);
		switch (format) {
                case PIPE_FORMAT_UYVY:
                case PIPE_FORMAT_YUYV:
		default:
			break;
		}
		goto out_unknown; /* TODO */
		
	case UTIL_FORMAT_COLORSPACE_SRGB:
		word4 |= S_038010_FORCE_DEGAMMA(1);
		if (format == PIPE_FORMAT_L8A8_SRGB || format == PIPE_FORMAT_L8_SRGB)
			goto out_unknown; /* fails for some reason - TODO */
		break;

	default:
		break;
	}
	
	word4 |= r600_get_swizzle_combined(desc->swizzle, swizzle_view);

	/* S3TC formats. TODO */
	if (desc->layout == UTIL_FORMAT_LAYOUT_S3TC) {
		goto out_unknown;
	}


	for (i = 0; i < desc->nr_channels; i++) {	
		if (desc->channel[i].type == UTIL_FORMAT_TYPE_SIGNED) {
			word4 |= sign_bit[i];
		}
	}

	/* R8G8Bx_SNORM - TODO CxV8U8 */

	/* RGTC - TODO */

	/* See whether the components are of the same size. */
	for (i = 1; i < desc->nr_channels; i++) {
		uniform = uniform && desc->channel[0].size == desc->channel[i].size;
	}
	
	/* Non-uniform formats. */
	if (!uniform) {
		switch(desc->nr_channels) {
		case 3:
			if (desc->channel[0].size == 5 &&
			    desc->channel[1].size == 6 &&
			    desc->channel[2].size == 5) {
				result |= V_0280A0_COLOR_5_6_5;
				goto out_word4;
			}
			goto out_unknown;
		case 4:
			if (desc->channel[0].size == 5 &&
			    desc->channel[1].size == 5 &&
			    desc->channel[2].size == 5 &&
			    desc->channel[3].size == 1) {
				result |= V_0280A0_COLOR_1_5_5_5;
				goto out_word4;
			}
			if (desc->channel[0].size == 10 &&
			    desc->channel[1].size == 10 &&
			    desc->channel[2].size == 10 &&
			    desc->channel[3].size == 2) {
				result |= V_0280A0_COLOR_10_10_10_2;
				goto out_word4;
			}
			goto out_unknown;
		}
		goto out_unknown;
	}

	/* uniform formats */
	switch (desc->channel[0].type) {
	case UTIL_FORMAT_TYPE_UNSIGNED:
	case UTIL_FORMAT_TYPE_SIGNED:
		if (!desc->channel[0].normalized &&
		    desc->colorspace != UTIL_FORMAT_COLORSPACE_SRGB) {
			goto out_unknown;
		}

		switch (desc->channel[0].size) {
		case 4:
			switch (desc->nr_channels) {
			case 2:
				result |= V_0280A0_COLOR_4_4;
				goto out_word4;
			case 4:
				result |= V_0280A0_COLOR_4_4_4_4;
				goto out_word4;
			}
			goto out_unknown;
		case 8:
			switch (desc->nr_channels) {
			case 1:
				result |= V_0280A0_COLOR_8;
				goto out_word4;
			case 2:
				result |= V_0280A0_COLOR_8_8;
				goto out_word4;
			case 4:
				result |= V_0280A0_COLOR_8_8_8_8;
				goto out_word4;
			}
			goto out_unknown;
		case 16:
			switch (desc->nr_channels) {
			case 1:
				result |= V_0280A0_COLOR_16;
				goto out_word4;
			case 2:
				result |= V_0280A0_COLOR_16_16;
				goto out_word4;
			case 4:
				result |= V_0280A0_COLOR_16_16_16_16;
				goto out_word4;
			}
		}
		goto out_unknown;

	case UTIL_FORMAT_TYPE_FLOAT:
		switch (desc->channel[0].size) {
		case 16:
			switch (desc->nr_channels) {
			case 1:
				result |= V_0280A0_COLOR_16_FLOAT;
				goto out_word4;
			case 2:
				result |= V_0280A0_COLOR_16_16_FLOAT;
				goto out_word4;
			case 4:
				result |= V_0280A0_COLOR_16_16_16_16_FLOAT;
				goto out_word4;
			}
			goto out_unknown;
		case 32:
			switch (desc->nr_channels) {
			case 1:
				result |= V_0280A0_COLOR_32_FLOAT;
				goto out_word4;
			case 2:
				result |= V_0280A0_COLOR_32_32_FLOAT;
				goto out_word4;
			case 4:
				result |= V_0280A0_COLOR_32_32_32_32_FLOAT;
				goto out_word4;
			}
		}
		
	}
out_word4:
	if (word4_p)
		*word4_p = word4;
	if (yuv_format_p)
		*yuv_format_p = yuv_format;
//	fprintf(stderr,"returning %08x %08x %08x\n", result, word4, yuv_format);
	return result;
out_unknown:
//	R600_ERR("Unable to handle texformat %d %s\n", format, util_format_name(format));
	return ~0;
}
