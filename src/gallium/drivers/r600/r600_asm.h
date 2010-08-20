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
#ifndef R600_ASM_H
#define R600_ASM_H

#include "radeon.h"
#include "util/u_double_list.h"

struct r600_bc_alu_src {
	unsigned			sel;
	unsigned			chan;
	unsigned			neg;
	unsigned			abs;
};

struct r600_bc_alu_dst {
	unsigned			sel;
	unsigned			chan;
	unsigned			clamp;
	unsigned			write;
};

struct r600_bc_alu {
	struct list_head		list;
	struct r600_bc_alu_src		src[3];
	struct r600_bc_alu_dst		dst;
	unsigned			inst;
	unsigned			last;
	unsigned			is_op3;
	unsigned			nliteral;
	unsigned			literal_added;
	u32				value[4];
};

struct r600_bc_tex {
	struct list_head		list;
	unsigned			inst;
	unsigned			resource_id;
	unsigned			src_gpr;
	unsigned			src_rel;
	unsigned			dst_gpr;
	unsigned			dst_rel;
	unsigned			dst_sel_x;
	unsigned			dst_sel_y;
	unsigned			dst_sel_z;
	unsigned			dst_sel_w;
	unsigned			lod_bias;
	unsigned			coord_type_x;
	unsigned			coord_type_y;
	unsigned			coord_type_z;
	unsigned			coord_type_w;
	unsigned			offset_x;
	unsigned			offset_y;
	unsigned			offset_z;
	unsigned			sampler_id;
	unsigned			src_sel_x;
	unsigned			src_sel_y;
	unsigned			src_sel_z;
	unsigned			src_sel_w;
};

struct r600_bc_vtx {
	struct list_head		list;
	unsigned			inst;
	unsigned			fetch_type;
	unsigned			buffer_id;
	unsigned			src_gpr;
	unsigned			src_sel_x;
	unsigned			mega_fetch_count;
	unsigned			dst_gpr;
	unsigned			dst_sel_x;
	unsigned			dst_sel_y;
	unsigned			dst_sel_z;
	unsigned			dst_sel_w;
};

struct r600_bc_output {
	unsigned			array_base;
	unsigned			type;
	unsigned			end_of_program;
	unsigned			inst;
	unsigned			elem_size;
	unsigned			gpr;
	unsigned			swizzle_x;
	unsigned			swizzle_y;
	unsigned			swizzle_z;
	unsigned			swizzle_w;
	unsigned			barrier;
};

struct r600_bc_cf {
	struct list_head		list;
	unsigned			inst;
	unsigned			addr;
	unsigned			ndw;
	unsigned			id;
	struct list_head		alu;
	struct list_head		tex;
	struct list_head		vtx;
	struct r600_bc_output		output;
};

struct r600_bc {
	enum radeon_family		family;
	int chiprev; /* 0 - r600, 1 - r700, 2 - evergreen */
	struct list_head		cf;
	struct r600_bc_cf		*cf_last;
	unsigned			ndw;
	unsigned			ncf;
	unsigned			ngpr;
	unsigned			nresource;
	unsigned			force_add_cf;
	u32				*bytecode;
};

int r600_bc_init(struct r600_bc *bc, enum radeon_family family);
int r600_bc_add_alu(struct r600_bc *bc, const struct r600_bc_alu *alu);
int r600_bc_add_literal(struct r600_bc *bc, const u32 *value);
int r600_bc_add_vtx(struct r600_bc *bc, const struct r600_bc_vtx *vtx);
int r600_bc_add_tex(struct r600_bc *bc, const struct r600_bc_tex *tex);
int r600_bc_add_output(struct r600_bc *bc, const struct r600_bc_output *output);
int r600_bc_build(struct r600_bc *bc);

#endif
