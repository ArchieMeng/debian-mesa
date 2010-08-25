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
#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_scan.h"
#include "tgsi/tgsi_dump.h"
#include "util/u_format.h"
#include "r600_screen.h"
#include "r600_context.h"
#include "r600_shader.h"
#include "r600_asm.h"
#include "r600_sq.h"
#include "r600d.h"
#include <stdio.h>
#include <errno.h>


struct r600_shader_tgsi_instruction;

struct r600_shader_ctx {
	struct tgsi_shader_info			info;
	struct tgsi_parse_context		parse;
	const struct tgsi_token			*tokens;
	unsigned				type;
	unsigned				file_offset[TGSI_FILE_COUNT];
	unsigned				temp_reg;
	struct r600_shader_tgsi_instruction	*inst_info;
	struct r600_bc				*bc;
	struct r600_shader			*shader;
	u32					value[4];
};

struct r600_shader_tgsi_instruction {
	unsigned	tgsi_opcode;
	unsigned	is_op3;
	unsigned	r600_opcode;
	int (*process)(struct r600_shader_ctx *ctx);
};

static struct r600_shader_tgsi_instruction r600_shader_tgsi_instruction[];
static int r600_shader_from_tgsi(const struct tgsi_token *tokens, struct r600_shader *shader);

static int r600_shader_update(struct pipe_context *ctx, struct r600_shader *shader)
{
	struct r600_context *rctx = r600_context(ctx);
	const struct util_format_description *desc;
	enum pipe_format resource_format[160];
	unsigned i, nresources = 0;
	struct r600_bc *bc = &shader->bc;
	struct r600_bc_cf *cf;
	struct r600_bc_vtx *vtx;

	if (shader->processor_type != TGSI_PROCESSOR_VERTEX)
		return 0;
	for (i = 0; i < rctx->vertex_elements->count; i++) {
		resource_format[nresources++] = rctx->vertex_elements->elements[i].src_format;
	}
	LIST_FOR_EACH_ENTRY(cf, &bc->cf, list) {
		switch (cf->inst) {
		case V_SQ_CF_WORD1_SQ_CF_INST_VTX:
		case V_SQ_CF_WORD1_SQ_CF_INST_VTX_TC:
			LIST_FOR_EACH_ENTRY(vtx, &cf->vtx, list) {
				desc = util_format_description(resource_format[vtx->buffer_id]);
				if (desc == NULL) {
					R600_ERR("unknown format %d\n", resource_format[vtx->buffer_id]);
					return -EINVAL;
				}
				vtx->dst_sel_x = desc->swizzle[0];
				vtx->dst_sel_y = desc->swizzle[1];
				vtx->dst_sel_z = desc->swizzle[2];
				vtx->dst_sel_w = desc->swizzle[3];
			}
			break;
		default:
			break;
		}
	}
	return r600_bc_build(&shader->bc);
}

int r600_pipe_shader_create(struct pipe_context *ctx,
			struct r600_context_state *rpshader,
			const struct tgsi_token *tokens)
{
	struct r600_screen *rscreen = r600_screen(ctx->screen);
	int r;

//fprintf(stderr, "--------------------------------------------------------------\n");
//tgsi_dump(tokens, 0);
	if (rpshader == NULL)
		return -ENOMEM;
	rpshader->shader.family = radeon_get_family(rscreen->rw);
	r = r600_shader_from_tgsi(tokens, &rpshader->shader);
	if (r) {
		R600_ERR("translation from TGSI failed !\n");
		return r;
	}
	r = r600_bc_build(&rpshader->shader.bc);
	if (r) {
		R600_ERR("building bytecode failed !\n");
		return r;
	}
//fprintf(stderr, "______________________________________________________________\n");
	return 0;
}

static int r600_pipe_shader_vs(struct pipe_context *ctx, struct r600_context_state *rpshader)
{
	struct r600_screen *rscreen = r600_screen(ctx->screen);
	struct r600_shader *rshader = &rpshader->shader;
	struct radeon_state *state;
	unsigned i, tmp;

	rpshader->rstate = radeon_state_decref(rpshader->rstate);
	state = radeon_state(rscreen->rw, R600_VS_SHADER_TYPE, R600_VS_SHADER);
	if (state == NULL)
		return -ENOMEM;
	for (i = 0; i < 10; i++) {
		state->states[R600_VS_SHADER__SPI_VS_OUT_ID_0 + i] = 0;
	}
	/* so far never got proper semantic id from tgsi */
	for (i = 0; i < 32; i++) {
		tmp = i << ((i & 3) * 8);
		state->states[R600_VS_SHADER__SPI_VS_OUT_ID_0 + i / 4] |= tmp;
	}
	state->states[R600_VS_SHADER__SPI_VS_OUT_CONFIG] = S_0286C4_VS_EXPORT_COUNT(rshader->noutput - 2);
	state->states[R600_VS_SHADER__SQ_PGM_RESOURCES_VS] = S_028868_NUM_GPRS(rshader->bc.ngpr);
	rpshader->rstate = state;
	rpshader->rstate->bo[0] = radeon_bo_incref(rscreen->rw, rpshader->bo);
	rpshader->rstate->bo[1] = radeon_bo_incref(rscreen->rw, rpshader->bo);
	rpshader->rstate->nbo = 2;
	rpshader->rstate->placement[0] = RADEON_GEM_DOMAIN_GTT;
	rpshader->rstate->placement[2] = RADEON_GEM_DOMAIN_GTT;
	return radeon_state_pm4(state);
}

static int r600_pipe_shader_ps(struct pipe_context *ctx, struct r600_context_state *rpshader)
{
	const struct pipe_rasterizer_state *rasterizer;
	struct r600_screen *rscreen = r600_screen(ctx->screen);
	struct r600_shader *rshader = &rpshader->shader;
	struct r600_context *rctx = r600_context(ctx);
	struct radeon_state *state;
	unsigned i, tmp, exports_ps, num_cout;

	rasterizer = &rctx->rasterizer->state.rasterizer;
	rpshader->rstate = radeon_state_decref(rpshader->rstate);
	state = radeon_state(rscreen->rw, R600_PS_SHADER_TYPE, R600_PS_SHADER);
	if (state == NULL)
		return -ENOMEM;
	for (i = 0; i < rshader->ninput; i++) {
		tmp = S_028644_SEMANTIC(i);
		tmp |= S_028644_SEL_CENTROID(1);
		if (rshader->input[i].name == TGSI_SEMANTIC_COLOR ||
			rshader->input[i].name == TGSI_SEMANTIC_BCOLOR) {
			tmp |= S_028644_FLAT_SHADE(rshader->flat_shade);
		}
		if (rasterizer->sprite_coord_enable & (1 << i)) {
			tmp |= S_028644_PT_SPRITE_TEX(1);
		}
		state->states[R600_PS_SHADER__SPI_PS_INPUT_CNTL_0 + i] = tmp;
	}

	exports_ps = 0;
	num_cout = 0;
	for (i = 0; i < rshader->noutput; i++) {
		if (rshader->output[i].name == TGSI_SEMANTIC_POSITION)
			exports_ps |= 1;
		else if (rshader->output[i].name == TGSI_SEMANTIC_COLOR) {
			exports_ps |= (1 << (num_cout+1));
			num_cout++;
		}
	}
	if (!exports_ps) {
		/* always at least export 1 component per pixel */
		exports_ps = 2;
	}
	state->states[R600_PS_SHADER__SPI_PS_IN_CONTROL_0] = S_0286CC_NUM_INTERP(rshader->ninput) |
							S_0286CC_PERSP_GRADIENT_ENA(1);
	state->states[R600_PS_SHADER__SPI_PS_IN_CONTROL_1] = 0x00000000;
	state->states[R600_PS_SHADER__SQ_PGM_RESOURCES_PS] = S_028868_NUM_GPRS(rshader->bc.ngpr);
	state->states[R600_PS_SHADER__SQ_PGM_EXPORTS_PS] = exports_ps;
	rpshader->rstate = state;
	rpshader->rstate->bo[0] = radeon_bo_incref(rscreen->rw, rpshader->bo);
	rpshader->rstate->nbo = 1;
	rpshader->rstate->placement[0] = RADEON_GEM_DOMAIN_GTT;
	return radeon_state_pm4(state);
}

static int r600_pipe_shader(struct pipe_context *ctx, struct r600_context_state *rpshader)
{
	struct r600_screen *rscreen = r600_screen(ctx->screen);
	struct r600_context *rctx = r600_context(ctx);
	struct r600_shader *rshader = &rpshader->shader;
	int r;

	/* copy new shader */
	radeon_bo_decref(rscreen->rw, rpshader->bo);
	rpshader->bo = NULL;
	rpshader->bo = radeon_bo(rscreen->rw, 0, rshader->bc.ndw * 4,
				4096, NULL);
	if (rpshader->bo == NULL) {
		return -ENOMEM;
	}
	radeon_bo_map(rscreen->rw, rpshader->bo);
	memcpy(rpshader->bo->data, rshader->bc.bytecode, rshader->bc.ndw * 4);
	radeon_bo_unmap(rscreen->rw, rpshader->bo);
	/* build state */
	rshader->flat_shade = rctx->flat_shade;
	switch (rshader->processor_type) {
	case TGSI_PROCESSOR_VERTEX:
		r = r600_pipe_shader_vs(ctx, rpshader);
		break;
	case TGSI_PROCESSOR_FRAGMENT:
		r = r600_pipe_shader_ps(ctx, rpshader);
		break;
	default:
		r = -EINVAL;
		break;
	}
	return r;
}

int r600_pipe_shader_update(struct pipe_context *ctx, struct r600_context_state *rpshader)
{
	struct r600_context *rctx = r600_context(ctx);
	int r;

	if (rpshader == NULL)
		return -EINVAL;
	/* there should be enough input */
	if (rctx->vertex_elements->count < rpshader->shader.bc.nresource) {
		R600_ERR("%d resources provided, expecting %d\n",
			rctx->vertex_elements->count, rpshader->shader.bc.nresource);
		return -EINVAL;
	}
	r = r600_shader_update(ctx, &rpshader->shader);
	if (r)
		return r;
	return r600_pipe_shader(ctx, rpshader);
}

static int tgsi_is_supported(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *i = &ctx->parse.FullToken.FullInstruction;
	int j;

	if (i->Instruction.NumDstRegs > 1) {
		R600_ERR("too many dst (%d)\n", i->Instruction.NumDstRegs);
		return -EINVAL;
	}
	if (i->Instruction.Predicate) {
		R600_ERR("predicate unsupported\n");
		return -EINVAL;
	}
	if (i->Instruction.Label) {
		R600_ERR("label unsupported\n");
		return -EINVAL;
	}
	for (j = 0; j < i->Instruction.NumSrcRegs; j++) {
		if (i->Src[j].Register.Indirect ||
			i->Src[j].Register.Dimension ||
			i->Src[j].Register.Absolute) {
			R600_ERR("unsupported src (indirect|dimension|absolute)\n");
			return -EINVAL;
		}
	}
	for (j = 0; j < i->Instruction.NumDstRegs; j++) {
		if (i->Dst[j].Register.Indirect || i->Dst[j].Register.Dimension) {
			R600_ERR("unsupported dst (indirect|dimension)\n");
			return -EINVAL;
		}
	}
	return 0;
}

static int tgsi_declaration(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_declaration *d = &ctx->parse.FullToken.FullDeclaration;
	struct r600_bc_vtx vtx;
	unsigned i;
	int r;

	switch (d->Declaration.File) {
	case TGSI_FILE_INPUT:
		i = ctx->shader->ninput++;
		ctx->shader->input[i].name = d->Semantic.Name;
		ctx->shader->input[i].sid = d->Semantic.Index;
		ctx->shader->input[i].interpolate = d->Declaration.Interpolate;
		ctx->shader->input[i].gpr = ctx->file_offset[TGSI_FILE_INPUT] + i;
		if (ctx->type == TGSI_PROCESSOR_VERTEX) {
			/* turn input into fetch */
			memset(&vtx, 0, sizeof(struct r600_bc_vtx));
			vtx.inst = 0;
			vtx.fetch_type = 0;
			vtx.buffer_id = i;
			/* register containing the index into the buffer */
			vtx.src_gpr = 0;
			vtx.src_sel_x = 0;
			vtx.mega_fetch_count = 0x1F;
			vtx.dst_gpr = ctx->shader->input[i].gpr;
			vtx.dst_sel_x = 0;
			vtx.dst_sel_y = 1;
			vtx.dst_sel_z = 2;
			vtx.dst_sel_w = 3;
			r = r600_bc_add_vtx(ctx->bc, &vtx);
			if (r)
				return r;
		}
		break;
	case TGSI_FILE_OUTPUT:
		i = ctx->shader->noutput++;
		ctx->shader->output[i].name = d->Semantic.Name;
		ctx->shader->output[i].sid = d->Semantic.Index;
		ctx->shader->output[i].gpr = ctx->file_offset[TGSI_FILE_OUTPUT] + i;
		ctx->shader->output[i].interpolate = d->Declaration.Interpolate;
		break;
	case TGSI_FILE_CONSTANT:
	case TGSI_FILE_TEMPORARY:
	case TGSI_FILE_SAMPLER:
		break;
	default:
		R600_ERR("unsupported file %d declaration\n", d->Declaration.File);
		return -EINVAL;
	}
	return 0;
}

int r600_shader_from_tgsi(const struct tgsi_token *tokens, struct r600_shader *shader)
{
	struct tgsi_full_immediate *immediate;
	struct r600_shader_ctx ctx;
	struct r600_bc_output output[32];
	unsigned output_done, noutput;
	unsigned opcode;
	int i, r = 0, pos0;

	ctx.bc = &shader->bc;
	ctx.shader = shader;
	r = r600_bc_init(ctx.bc, shader->family);
	if (r)
		return r;
	ctx.tokens = tokens;
	tgsi_scan_shader(tokens, &ctx.info);
	tgsi_parse_init(&ctx.parse, tokens);
	ctx.type = ctx.parse.FullHeader.Processor.Processor;
	shader->processor_type = ctx.type;

	/* register allocations */
	/* Values [0,127] correspond to GPR[0..127].
	 * Values [128,159] correspond to constant buffer bank 0
	 * Values [160,191] correspond to constant buffer bank 1
	 * Values [256,511] correspond to cfile constants c[0..255].
	 * Other special values are shown in the list below.
	 * 244  ALU_SRC_1_DBL_L: special constant 1.0 double-float, LSW. (RV670+)
	 * 245  ALU_SRC_1_DBL_M: special constant 1.0 double-float, MSW. (RV670+)
	 * 246  ALU_SRC_0_5_DBL_L: special constant 0.5 double-float, LSW. (RV670+)
	 * 247  ALU_SRC_0_5_DBL_M: special constant 0.5 double-float, MSW. (RV670+)
	 * 248	SQ_ALU_SRC_0: special constant 0.0.
	 * 249	SQ_ALU_SRC_1: special constant 1.0 float.
	 * 250	SQ_ALU_SRC_1_INT: special constant 1 integer.
	 * 251	SQ_ALU_SRC_M_1_INT: special constant -1 integer.
	 * 252	SQ_ALU_SRC_0_5: special constant 0.5 float.
	 * 253	SQ_ALU_SRC_LITERAL: literal constant.
	 * 254	SQ_ALU_SRC_PV: previous vector result.
	 * 255	SQ_ALU_SRC_PS: previous scalar result.
	 */
	for (i = 0; i < TGSI_FILE_COUNT; i++) {
		ctx.file_offset[i] = 0;
	}
	if (ctx.type == TGSI_PROCESSOR_VERTEX) {
		ctx.file_offset[TGSI_FILE_INPUT] = 1;
	}
	ctx.file_offset[TGSI_FILE_OUTPUT] = ctx.file_offset[TGSI_FILE_INPUT] +
						ctx.info.file_count[TGSI_FILE_INPUT];
	ctx.file_offset[TGSI_FILE_TEMPORARY] = ctx.file_offset[TGSI_FILE_OUTPUT] +
						ctx.info.file_count[TGSI_FILE_OUTPUT];
	ctx.file_offset[TGSI_FILE_CONSTANT] = 256;
	ctx.file_offset[TGSI_FILE_IMMEDIATE] = 253;
	ctx.temp_reg = ctx.file_offset[TGSI_FILE_TEMPORARY] +
			ctx.info.file_count[TGSI_FILE_TEMPORARY];

	while (!tgsi_parse_end_of_tokens(&ctx.parse)) {
		tgsi_parse_token(&ctx.parse);
		switch (ctx.parse.FullToken.Token.Type) {
		case TGSI_TOKEN_TYPE_IMMEDIATE:
			immediate = &ctx.parse.FullToken.FullImmediate;
			ctx.value[0] = immediate->u[0].Uint;
			ctx.value[1] = immediate->u[1].Uint;
			ctx.value[2] = immediate->u[2].Uint;
			ctx.value[3] = immediate->u[3].Uint;
			break;
		case TGSI_TOKEN_TYPE_DECLARATION:
			r = tgsi_declaration(&ctx);
			if (r)
				goto out_err;
			break;
		case TGSI_TOKEN_TYPE_INSTRUCTION:
			r = tgsi_is_supported(&ctx);
			if (r)
				goto out_err;
			opcode = ctx.parse.FullToken.FullInstruction.Instruction.Opcode;
			ctx.inst_info = &r600_shader_tgsi_instruction[opcode];
			r = ctx.inst_info->process(&ctx);
			if (r)
				goto out_err;
			r = r600_bc_add_literal(ctx.bc, ctx.value);
			if (r)
				goto out_err;
			break;
		default:
			R600_ERR("unsupported token type %d\n", ctx.parse.FullToken.Token.Type);
			r = -EINVAL;
			goto out_err;
		}
	}
	/* export output */
	noutput = shader->noutput;
	for (i = 0, pos0 = 0; i < noutput; i++) {
		memset(&output[i], 0, sizeof(struct r600_bc_output));
		output[i].gpr = shader->output[i].gpr;
		output[i].elem_size = 3;
		output[i].swizzle_x = 0;
		output[i].swizzle_y = 1;
		output[i].swizzle_z = 2;
		output[i].swizzle_w = 3;
		output[i].barrier = 1;
		output[i].type = V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM;
		output[i].array_base = i - pos0;
		output[i].inst = V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT;
		switch (ctx.type) {
		case TGSI_PROCESSOR_VERTEX:
			if (shader->output[i].name == TGSI_SEMANTIC_POSITION) {
				output[i].array_base = 60;
				output[i].type = V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS;
				/* position doesn't count in array_base */
				pos0++;
			}
			if (shader->output[i].name == TGSI_SEMANTIC_PSIZE) {
				output[i].array_base = 61;
				output[i].type = V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_POS;
				/* position doesn't count in array_base */
				pos0++;
			}
			break;
		case TGSI_PROCESSOR_FRAGMENT:
			if (shader->output[i].name == TGSI_SEMANTIC_COLOR) {
				output[i].array_base = shader->output[i].sid;
				output[i].type = V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL;
			} else if (shader->output[i].name == TGSI_SEMANTIC_POSITION) {
				output[i].array_base = 61;
				output[i].type = V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL;
			} else {
				R600_ERR("unsupported fragment output name %d\n", shader->output[i].name);
				r = -EINVAL;
				goto out_err;
			}
			break;
		default:
			R600_ERR("unsupported processor type %d\n", ctx.type);
			r = -EINVAL;
			goto out_err;
		}
	}
	/* add fake param output for vertex shader if no param is exported */
	if (ctx.type == TGSI_PROCESSOR_VERTEX) {
		for (i = 0, pos0 = 0; i < noutput; i++) {
			if (output[i].type == V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM) {
				pos0 = 1;
				break;
			}
		}
		if (!pos0) {
			memset(&output[i], 0, sizeof(struct r600_bc_output));
			output[i].gpr = 0;
			output[i].elem_size = 3;
			output[i].swizzle_x = 0;
			output[i].swizzle_y = 1;
			output[i].swizzle_z = 2;
			output[i].swizzle_w = 3;
			output[i].barrier = 1;
			output[i].type = V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PARAM;
			output[i].array_base = 0;
			output[i].inst = V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT;
			noutput++;
		}
	}
	/* add fake pixel export */
	if (ctx.type == TGSI_PROCESSOR_FRAGMENT && !noutput) {
		memset(&output[0], 0, sizeof(struct r600_bc_output));
		output[0].gpr = 0;
		output[0].elem_size = 3;
		output[0].swizzle_x = 7;
		output[0].swizzle_y = 7;
		output[0].swizzle_z = 7;
		output[0].swizzle_w = 7;
		output[0].barrier = 1;
		output[0].type = V_SQ_CF_ALLOC_EXPORT_WORD0_SQ_EXPORT_PIXEL;
		output[0].array_base = 0;
		output[0].inst = V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT;
		noutput++;
	}
	/* set export done on last export of each type */
	for (i = noutput - 1, output_done = 0; i >= 0; i--) {
		if (i == (noutput - 1)) {
			output[i].end_of_program = 1;
		}
		if (!(output_done & (1 << output[i].type))) {
			output_done |= (1 << output[i].type);
			output[i].inst = V_SQ_CF_ALLOC_EXPORT_WORD1_SQ_CF_INST_EXPORT_DONE;
		}
	}
	/* add output to bytecode */
	for (i = 0; i < noutput; i++) {
		r = r600_bc_add_output(ctx.bc, &output[i]);
		if (r)
			goto out_err;
	}
	tgsi_parse_free(&ctx.parse);
	return 0;
out_err:
	tgsi_parse_free(&ctx.parse);
	return r;
}

static int tgsi_unsupported(struct r600_shader_ctx *ctx)
{
	R600_ERR("%d tgsi opcode unsupported\n", ctx->inst_info->tgsi_opcode);
	return -EINVAL;
}

static int tgsi_end(struct r600_shader_ctx *ctx)
{
	return 0;
}

static int tgsi_src(struct r600_shader_ctx *ctx,
			const struct tgsi_full_src_register *tgsi_src,
			struct r600_bc_alu_src *r600_src)
{
	memset(r600_src, 0, sizeof(struct r600_bc_alu_src));
	r600_src->sel = tgsi_src->Register.Index;
	if (tgsi_src->Register.File == TGSI_FILE_IMMEDIATE) {
		r600_src->sel = 0;
	}
	r600_src->neg = tgsi_src->Register.Negate;
	r600_src->sel += ctx->file_offset[tgsi_src->Register.File];
	return 0;
}

static int tgsi_dst(struct r600_shader_ctx *ctx,
			const struct tgsi_full_dst_register *tgsi_dst,
			unsigned swizzle,
			struct r600_bc_alu_dst *r600_dst)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;

	r600_dst->sel = tgsi_dst->Register.Index;
	r600_dst->sel += ctx->file_offset[tgsi_dst->Register.File];
	r600_dst->chan = swizzle;
	r600_dst->write = 1;
	if (inst->Instruction.Saturate) {
		r600_dst->clamp = 1;
	}
	return 0;
}

static unsigned tgsi_chan(const struct tgsi_full_src_register *tgsi_src, unsigned swizzle)
{
	switch (swizzle) {
	case 0:
		return tgsi_src->Register.SwizzleX;
	case 1:
		return tgsi_src->Register.SwizzleY;
	case 2:
		return tgsi_src->Register.SwizzleZ;
	case 3:
		return tgsi_src->Register.SwizzleW;
	default:
		return 0;
	}
}

static int tgsi_split_constant(struct r600_shader_ctx *ctx, struct r600_bc_alu_src r600_src[3])
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu alu;
	int i, j, k, nconst, r;

	for (i = 0, nconst = 0; i < inst->Instruction.NumSrcRegs; i++) {
		if (inst->Src[i].Register.File == TGSI_FILE_CONSTANT) {
			nconst++;
		}
		r = tgsi_src(ctx, &inst->Src[i], &r600_src[i]);
		if (r) {
			return r;
		}
	}
	for (i = 0, j = nconst - 1; i < inst->Instruction.NumSrcRegs; i++) {
		if (inst->Src[j].Register.File == TGSI_FILE_CONSTANT && j > 0) {
			for (k = 0; k < 4; k++) {
				memset(&alu, 0, sizeof(struct r600_bc_alu));
				alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV;
				alu.src[0].sel = r600_src[0].sel;
				alu.src[0].chan = k;
				alu.dst.sel = ctx->temp_reg + j;
				alu.dst.chan = k;
				alu.dst.write = 1;
				if (k == 3)
					alu.last = 1;
				r = r600_bc_add_alu(ctx->bc, &alu);
				if (r)
					return r;
			}
			r600_src[0].sel = ctx->temp_reg + j;
			j--;
		}
	}
	return 0;
}

static int tgsi_op2(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu_src r600_src[3];
	struct r600_bc_alu alu;
	int i, j, r;

	r = tgsi_split_constant(ctx, r600_src);
	if (r)
		return r;
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		if (!(inst->Dst[0].Register.WriteMask & (1 << i))) {
			alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP;
			alu.dst.chan = i;
		} else {
			alu.inst = ctx->inst_info->r600_opcode;
			for (j = 0; j < inst->Instruction.NumSrcRegs; j++) {
				alu.src[j] = r600_src[j];
				alu.src[j].chan = tgsi_chan(&inst->Src[j], i);
			}
			r = tgsi_dst(ctx, &inst->Dst[0], i, &alu.dst);
			if (r)
				return r;
		}
		/* handle some special cases */
		switch (ctx->inst_info->tgsi_opcode) {
		case TGSI_OPCODE_SUB:
			alu.src[1].neg = 1;
			break;
		case TGSI_OPCODE_ABS:
			alu.src[0].abs = 1;
			break;
		default:
			break;
		}
		if (i == 3) {
			alu.last = 1;
		}
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return 0;
}

/* 
 * r600 - trunc to -PI..PI range
 * r700 - normalize by dividing by 2PI
 * see fdo bug 27901
 */
static int tgsi_trig(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu_src r600_src[3];
	struct r600_bc_alu alu;
	int i, r;
	uint32_t lit_vals[4];

	memset(lit_vals, 0, 4*4);
	r = tgsi_split_constant(ctx, r600_src);
	if (r)
		return r;
	lit_vals[0] = fui(1.0 /(3.1415926535 * 2));
	lit_vals[1] = fui(0.5f);

	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_MULADD;
	alu.is_op3 = 1;

	alu.dst.chan = 0;
	alu.dst.sel = ctx->temp_reg;
	alu.dst.write = 1;

	alu.src[0] = r600_src[0];
	alu.src[0].chan = tgsi_chan(&inst->Src[0], 0);
		
	alu.src[1].sel = V_SQ_ALU_SRC_LITERAL;
	alu.src[1].chan = 0;
	alu.src[2].sel = V_SQ_ALU_SRC_LITERAL;
	alu.src[2].chan = 1;
	alu.last = 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;
	r = r600_bc_add_literal(ctx->bc, lit_vals);
	if (r)
		return r;

	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FRACT;
		
	alu.dst.chan = 0;
	alu.dst.sel = ctx->temp_reg;
	alu.dst.write = 1;

	alu.src[0].sel = ctx->temp_reg;
	alu.src[0].chan = 0;
	alu.last = 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;

	if (ctx->bc->chiprev == 0) {
		lit_vals[0] = fui(3.1415926535897f * 2.0f);
		lit_vals[1] = fui(-3.1415926535897f);
	} else {
		lit_vals[0] = fui(1.0f);
		lit_vals[1] = fui(-0.5f);
	}

	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_MULADD;
	alu.is_op3 = 1;

	alu.dst.chan = 0;
	alu.dst.sel = ctx->temp_reg;
	alu.dst.write = 1;

	alu.src[0].sel = ctx->temp_reg;
	alu.src[0].chan = 0;
		
	alu.src[1].sel = V_SQ_ALU_SRC_LITERAL;
	alu.src[1].chan = 0;
	alu.src[2].sel = V_SQ_ALU_SRC_LITERAL;
	alu.src[2].chan = 1;
	alu.last = 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;
	r = r600_bc_add_literal(ctx->bc, lit_vals);
	if (r)
		return r;

	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = ctx->inst_info->r600_opcode;
	alu.dst.chan = 0;
	alu.dst.sel = ctx->temp_reg;
	alu.dst.write = 1;

	alu.src[0].sel = ctx->temp_reg;
	alu.src[0].chan = 0;
	alu.last = 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;

	/* replicate result */
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.src[0].sel = ctx->temp_reg;
		alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV;
		alu.dst.chan = i;
		r = tgsi_dst(ctx, &inst->Dst[0], i, &alu.dst);
		if (r)
			return r;
		alu.dst.write = (inst->Dst[0].Register.WriteMask >> i) & 1;
		if (i == 3)
			alu.last = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return 0;
}

static int tgsi_kill(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu alu;
	int i, r;

	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = ctx->inst_info->r600_opcode;
		alu.dst.chan = i;
		alu.src[0].sel = V_SQ_ALU_SRC_0;
		r = tgsi_src(ctx, &inst->Src[0], &alu.src[1]);
		if (r)
			return r;
		alu.src[1].chan = tgsi_chan(&inst->Src[0], i);
		if (i == 3) {
			alu.last = 1;
		}
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return 0;
}

static int tgsi_slt(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu_src r600_src[3];
	struct r600_bc_alu alu;
	int i, r;

	r = tgsi_split_constant(ctx, r600_src);
	if (r)
		return r;
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		if (!(inst->Dst[0].Register.WriteMask & (1 << i))) {
			alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP;
			alu.dst.chan = i;
		} else {
			alu.inst = ctx->inst_info->r600_opcode;
			alu.src[1] = r600_src[0];
			alu.src[1].chan = tgsi_chan(&inst->Src[0], i);
			alu.src[0] = r600_src[1];
			alu.src[0].chan = tgsi_chan(&inst->Src[1], i);
			r = tgsi_dst(ctx, &inst->Dst[0], i, &alu.dst);
			if (r)
				return r;
		}
		if (i == 3) {
			alu.last = 1;
		}
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return 0;
}

static int tgsi_lit(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu alu;
	int r;

	/* dst.x, <- 1.0  */
	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV;
	alu.src[0].sel  = V_SQ_ALU_SRC_1; /*1.0*/
	alu.src[0].chan = 0;
	r = tgsi_dst(ctx, &inst->Dst[0], 0, &alu.dst);
	if (r)
		return r;
	alu.dst.write = (inst->Dst[0].Register.WriteMask >> 0) & 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;

	/* dst.y = max(src.x, 0.0) */
	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MAX;
	r = tgsi_src(ctx, &inst->Src[0], &alu.src[0]);
	if (r)
		return r;
	alu.src[1].sel  = V_SQ_ALU_SRC_0; /*0.0*/
	alu.src[1].chan = tgsi_chan(&inst->Src[0], 0);
	r = tgsi_dst(ctx, &inst->Dst[0], 1, &alu.dst);
	if (r)
		return r;
	alu.dst.write = (inst->Dst[0].Register.WriteMask >> 1) & 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;

	/* dst.z = NOP - fill Z slot */
	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP;
	alu.dst.chan = 2;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;

	/* dst.w, <- 1.0  */
	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV;
	alu.src[0].sel  = V_SQ_ALU_SRC_1;
	alu.src[0].chan = 0;
	r = tgsi_dst(ctx, &inst->Dst[0], 3, &alu.dst);
	if (r)
		return r;
	alu.dst.write = (inst->Dst[0].Register.WriteMask >> 3) & 1;
	alu.last = 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;

	if (inst->Dst[0].Register.WriteMask & (1 << 2))
	{
		int chan;
		int sel;

		/* dst.z = log(src.y) */
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_LOG_CLAMPED;
		r = tgsi_src(ctx, &inst->Src[0], &alu.src[0]);
		if (r)
			return r;
		alu.src[0].chan = tgsi_chan(&inst->Src[0], 1);
		r = tgsi_dst(ctx, &inst->Dst[0], 2, &alu.dst);
		if (r)
			return r;
		alu.last = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;

		chan = alu.dst.chan;
		sel = alu.dst.sel;

		/* tmp.x = amd MUL_LIT(src.w, dst.z, src.x ) */
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_MUL_LIT;
		r = tgsi_src(ctx, &inst->Src[0], &alu.src[0]);
		if (r)
			return r;
		alu.src[0].chan = tgsi_chan(&inst->Src[0], 3);
		alu.src[1].sel  = sel;
		alu.src[1].chan = chan;
		r = tgsi_src(ctx, &inst->Src[0], &alu.src[2]);
		if (r)
			return r;
		alu.src[2].chan = tgsi_chan(&inst->Src[0], 0);
		alu.dst.sel = ctx->temp_reg;
		alu.dst.chan = 0;
		alu.dst.write = 1;
		alu.is_op3 = 1;
		alu.last = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;

		/* dst.z = exp(tmp.x) */
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_EXP_IEEE;
		alu.src[0].sel = ctx->temp_reg;
		alu.src[0].chan = 0;
		r = tgsi_dst(ctx, &inst->Dst[0], 2, &alu.dst);
		if (r)
			return r;
		alu.last = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return 0;
}

static int tgsi_trans(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu alu;
	int i, j, r;

	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		if (inst->Dst[0].Register.WriteMask & (1 << i)) {
			alu.inst = ctx->inst_info->r600_opcode;
			for (j = 0; j < inst->Instruction.NumSrcRegs; j++) {
				r = tgsi_src(ctx, &inst->Src[j], &alu.src[j]);
				if (r)
					return r;
				alu.src[j].chan = tgsi_chan(&inst->Src[j], i);
			}
			r = tgsi_dst(ctx, &inst->Dst[0], i, &alu.dst);
			if (r)
				return r;
			alu.last = 1;
			r = r600_bc_add_alu(ctx->bc, &alu);
			if (r)
				return r;
		}
	}
	return 0;
}

static int tgsi_helper_tempx_replicate(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu alu;
	int i, r;

	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.src[0].sel = ctx->temp_reg;
		alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV;
		alu.dst.chan = i;
		r = tgsi_dst(ctx, &inst->Dst[0], i, &alu.dst);
		if (r)
			return r;
		alu.dst.write = (inst->Dst[0].Register.WriteMask >> i) & 1;
		if (i == 3)
			alu.last = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return 0;
}

static int tgsi_trans_srcx_replicate(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu alu;
	int i, r;

	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = ctx->inst_info->r600_opcode;
	for (i = 0; i < inst->Instruction.NumSrcRegs; i++) {
		r = tgsi_src(ctx, &inst->Src[i], &alu.src[i]);
		if (r)
			return r;
		alu.src[i].chan = tgsi_chan(&inst->Src[i], 0);
	}
	alu.dst.sel = ctx->temp_reg;
	alu.dst.write = 1;
	alu.last = 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;
	/* replicate result */
	return tgsi_helper_tempx_replicate(ctx);
}

static int tgsi_pow(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu alu;
	int r;

	/* LOG2(a) */
	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_LOG_IEEE;
	r = tgsi_src(ctx, &inst->Src[0], &alu.src[0]);
	if (r)
		return r;
	alu.src[0].chan = tgsi_chan(&inst->Src[0], 0);
	alu.dst.sel = ctx->temp_reg;
	alu.dst.write = 1;
	alu.last = 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;
	/* b * LOG2(a) */
	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL_IEEE;
	r = tgsi_src(ctx, &inst->Src[1], &alu.src[0]);
	if (r)
		return r;
	alu.src[0].chan = tgsi_chan(&inst->Src[1], 0);
	alu.src[1].sel = ctx->temp_reg;
	alu.dst.sel = ctx->temp_reg;
	alu.dst.write = 1;
	alu.last = 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;
	/* POW(a,b) = EXP2(b * LOG2(a))*/
	memset(&alu, 0, sizeof(struct r600_bc_alu));
	alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_EXP_IEEE;
	alu.src[0].sel = ctx->temp_reg;
	alu.dst.sel = ctx->temp_reg;
	alu.dst.write = 1;
	alu.last = 1;
	r = r600_bc_add_alu(ctx->bc, &alu);
	if (r)
		return r;
	return tgsi_helper_tempx_replicate(ctx);
}

static int tgsi_ssg(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu alu;
	struct r600_bc_alu_src r600_src[3];
	int i, r;

	r = tgsi_split_constant(ctx, r600_src);
	if (r)
		return r;

	/* tmp = (src > 0 ? 1 : src) */
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_CNDGT;
		alu.is_op3 = 1;
		alu.dst.sel = ctx->temp_reg;
		alu.dst.write = 1;

		alu.src[0] = r600_src[0];
		alu.src[0].chan = tgsi_chan(&inst->Src[0], i);

		alu.src[1].sel = V_SQ_ALU_SRC_1;

		alu.src[2] = r600_src[0];
		alu.src[2].chan = tgsi_chan(&inst->Src[0], i);
		if (i == 3)
			alu.last = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}

	/* dst = (-tmp > 0 ? -1 : tmp) */
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_CNDGT;
		alu.is_op3 = 1;
		r = tgsi_dst(ctx, &inst->Dst[0], i, &alu.dst);
		if (r)
			return r;

		alu.src[0].sel = ctx->temp_reg;
		alu.src[0].neg = 1;

		alu.src[1].sel = V_SQ_ALU_SRC_1;
		alu.src[1].neg = 1;

		alu.src[2].sel = ctx->temp_reg;

		alu.dst.write = 1;
		if (i == 3)
			alu.last = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return 0;
}

static int tgsi_helper_copy(struct r600_shader_ctx *ctx, struct tgsi_full_instruction *inst)
{
	struct r600_bc_alu alu;
	int i, r;

	r = r600_bc_add_literal(ctx->bc, ctx->value);
	if (r)
		return r;
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		if (!(inst->Dst[0].Register.WriteMask & (1 << i))) {
			alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP;
			alu.dst.chan = i;
		} else {
			alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV;
			r = tgsi_dst(ctx, &inst->Dst[0], i, &alu.dst);
			if (r)
				return r;
			alu.src[0].sel = ctx->temp_reg;
			alu.src[0].chan = i;
		}
		if (i == 3) {
			alu.last = 1;
		}
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return 0;
}

static int tgsi_op3(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu_src r600_src[3];
	struct r600_bc_alu alu;
	int i, j, r;

	r = tgsi_split_constant(ctx, r600_src);
	if (r)
		return r;
	/* do it in 2 step as op3 doesn't support writemask */
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = ctx->inst_info->r600_opcode;
		for (j = 0; j < inst->Instruction.NumSrcRegs; j++) {
			alu.src[j] = r600_src[j];
			alu.src[j].chan = tgsi_chan(&inst->Src[j], i);
		}
		alu.dst.sel = ctx->temp_reg;
		alu.dst.chan = i;
		alu.dst.write = 1;
		alu.is_op3 = 1;
		if (i == 3) {
			alu.last = 1;
		}
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return tgsi_helper_copy(ctx, inst);
}

static int tgsi_dp(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu_src r600_src[3];
	struct r600_bc_alu alu;
	int i, j, r;

	r = tgsi_split_constant(ctx, r600_src);
	if (r)
		return r;
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = ctx->inst_info->r600_opcode;
		for (j = 0; j < inst->Instruction.NumSrcRegs; j++) {
			alu.src[j] = r600_src[j];
			alu.src[j].chan = tgsi_chan(&inst->Src[j], i);
		}
		alu.dst.sel = ctx->temp_reg;
		alu.dst.chan = i;
		alu.dst.write = 1;
		/* handle some special cases */
		switch (ctx->inst_info->tgsi_opcode) {
		case TGSI_OPCODE_DP2:
			if (i > 1) {
				alu.src[0].sel = alu.src[1].sel = V_SQ_ALU_SRC_0;
				alu.src[0].chan = alu.src[1].chan = 0;
			}
			break;
		case TGSI_OPCODE_DP3:
			if (i > 2) {
				alu.src[0].sel = alu.src[1].sel = V_SQ_ALU_SRC_0;
				alu.src[0].chan = alu.src[1].chan = 0;
			}
			break;
		default:
			break;
		}
		if (i == 3) {
			alu.last = 1;
		}
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return tgsi_helper_copy(ctx, inst);
}

static int tgsi_tex(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_tex tex;
	struct r600_bc_alu alu;
	unsigned src_gpr;
	int r, i;

	src_gpr = ctx->file_offset[inst->Src[0].Register.File] + inst->Src[0].Register.Index;

	if (inst->Instruction.Opcode == TGSI_OPCODE_TXP) {
		/* Add perspective divide */
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_RECIP_IEEE;
		alu.src[0].sel = src_gpr;
		alu.src[0].chan = tgsi_chan(&inst->Src[0], 3);
		alu.dst.sel = ctx->temp_reg;
		alu.dst.chan = 3;
		alu.last = 1;
		alu.dst.write = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
		
		for (i = 0; i < 3; i++) {
			memset(&alu, 0, sizeof(struct r600_bc_alu));
			alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL;
			alu.src[0].sel = ctx->temp_reg;
			alu.src[0].chan = 3;
			alu.src[1].sel = src_gpr;
			alu.src[1].chan = tgsi_chan(&inst->Src[0], i);
			alu.dst.sel = ctx->temp_reg;
			alu.dst.chan = i;
			alu.dst.write = 1;
			r = r600_bc_add_alu(ctx->bc, &alu);
			if (r)
				return r;
		}
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV;
		alu.src[0].sel = V_SQ_ALU_SRC_1;
		alu.src[0].chan = 0;
		alu.dst.sel = ctx->temp_reg;
		alu.dst.chan = 3;
		alu.last = 1;
		alu.dst.write = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
		src_gpr = ctx->temp_reg;
	} else if (inst->Src[0].Register.File != TGSI_FILE_TEMPORARY) {
		for (i = 0; i < 4; i++) {
			memset(&alu, 0, sizeof(struct r600_bc_alu));
			alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV;
			alu.src[0].sel = src_gpr;
			alu.src[0].chan = i;
			alu.dst.sel = ctx->temp_reg;
			alu.dst.chan = i;
			if (i == 3)
				alu.last = 1;
			alu.dst.write = 1;
			r = r600_bc_add_alu(ctx->bc, &alu);
			if (r)
				return r;
		}
		src_gpr = ctx->temp_reg;
	}

	memset(&tex, 0, sizeof(struct r600_bc_tex));
	tex.inst = ctx->inst_info->r600_opcode;
	tex.resource_id = ctx->file_offset[inst->Src[1].Register.File] + inst->Src[1].Register.Index;
	tex.sampler_id = tex.resource_id;
	tex.src_gpr = src_gpr;
	tex.dst_gpr = ctx->file_offset[inst->Dst[0].Register.File] + inst->Dst[0].Register.Index;
	tex.dst_sel_x = 0;
	tex.dst_sel_y = 1;
	tex.dst_sel_z = 2;
	tex.dst_sel_w = 3;
	tex.src_sel_x = 0;
	tex.src_sel_y = 1;
	tex.src_sel_z = 2;
	tex.src_sel_w = 3;

	if (inst->Texture.Texture != TGSI_TEXTURE_RECT) {
		tex.coord_type_x = 1;
		tex.coord_type_y = 1;
		tex.coord_type_z = 1;
		tex.coord_type_w = 1;
	}
	return r600_bc_add_tex(ctx->bc, &tex);
}

static int tgsi_lrp(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu_src r600_src[3];
	struct r600_bc_alu alu;
	unsigned i;
	int r;

	r = tgsi_split_constant(ctx, r600_src);
	if (r)
		return r;
	/* 1 - src0 */
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD;
		alu.src[0].sel = V_SQ_ALU_SRC_1;
		alu.src[0].chan = 0;
		alu.src[1] = r600_src[0];
		alu.src[1].chan = tgsi_chan(&inst->Src[0], i);
		alu.src[1].neg = 1;
		alu.dst.sel = ctx->temp_reg;
		alu.dst.chan = i;
		if (i == 3) {
			alu.last = 1;
		}
		alu.dst.write = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	r = r600_bc_add_literal(ctx->bc, ctx->value);
	if (r)
		return r;

	/* (1 - src0) * src2 */
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL;
		alu.src[0].sel = ctx->temp_reg;
		alu.src[0].chan = i;
		alu.src[1] = r600_src[2];
		alu.src[1].chan = tgsi_chan(&inst->Src[2], i);
		alu.dst.sel = ctx->temp_reg;
		alu.dst.chan = i;
		if (i == 3) {
			alu.last = 1;
		}
		alu.dst.write = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	r = r600_bc_add_literal(ctx->bc, ctx->value);
	if (r)
		return r;

	/* src0 * src1 + (1 - src0) * src2 */
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_MULADD;
		alu.is_op3 = 1;
		alu.src[0] = r600_src[0];
		alu.src[0].chan = tgsi_chan(&inst->Src[0], i);
		alu.src[1] = r600_src[1];
		alu.src[1].chan = tgsi_chan(&inst->Src[1], i);
		alu.src[2].sel = ctx->temp_reg;
		alu.src[2].chan = i;
		alu.dst.sel = ctx->temp_reg;
		alu.dst.chan = i;
		if (i == 3) {
			alu.last = 1;
		}
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	return tgsi_helper_copy(ctx, inst);
}

static int tgsi_cmp(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu_src r600_src[3];
	struct r600_bc_alu alu;
	int use_temp = 0;
	int i, r;

	r = tgsi_split_constant(ctx, r600_src);
	if (r)
		return r;

	if (inst->Dst[0].Register.WriteMask != 0xf)
		use_temp = 1;

	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_CNDGE;
		alu.src[0] = r600_src[0];
		alu.src[0].chan = tgsi_chan(&inst->Src[0], i);

		alu.src[1] = r600_src[2];
		alu.src[1].chan = tgsi_chan(&inst->Src[2], i);

		alu.src[2] = r600_src[1];
		alu.src[2].chan = tgsi_chan(&inst->Src[1], i);

		if (use_temp)
			alu.dst.sel = ctx->temp_reg;
		else {
			r = tgsi_dst(ctx, &inst->Dst[0], i, &alu.dst);
			if (r)
				return r;
		}
		alu.dst.chan = i;
		alu.dst.write = 1;
		alu.is_op3 = 1;
		if (i == 3)
			alu.last = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}       
	if (use_temp)
		return tgsi_helper_copy(ctx, inst);
	return 0;
}

static int tgsi_xpd(struct r600_shader_ctx *ctx)
{
	struct tgsi_full_instruction *inst = &ctx->parse.FullToken.FullInstruction;
	struct r600_bc_alu_src r600_src[3];
	struct r600_bc_alu alu;
	uint32_t use_temp = 0;
	int i, r;

	if (inst->Dst[0].Register.WriteMask != 0xf)
		use_temp = 1;

	r = tgsi_split_constant(ctx, r600_src);
	if (r)
		return r;
	
	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL;

		alu.src[0] = r600_src[0];
		switch (i) {
		case 0:
			alu.src[0].chan = tgsi_chan(&inst->Src[0], 2);
			break;
		case 1:
			alu.src[0].chan = tgsi_chan(&inst->Src[0], 0);
			break;
		case 2:
			alu.src[0].chan = tgsi_chan(&inst->Src[0], 1);
			break;
		case 3:
			alu.src[0].sel = V_SQ_ALU_SRC_0;
			alu.src[0].chan = i;
		}

		alu.src[1] = r600_src[1];
		switch (i) {
		case 0:
			alu.src[1].chan = tgsi_chan(&inst->Src[1], 1);
			break;
		case 1:
			alu.src[1].chan = tgsi_chan(&inst->Src[1], 2);
			break;
		case 2:
			alu.src[1].chan = tgsi_chan(&inst->Src[1], 0);
			break;
		case 3:
			alu.src[1].sel = V_SQ_ALU_SRC_0;
			alu.src[1].chan = i;
		}

		alu.dst.sel = ctx->temp_reg;
		alu.dst.chan = i;
		alu.dst.write = 1;

		if (i == 3)
			alu.last = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}

	for (i = 0; i < 4; i++) {
		memset(&alu, 0, sizeof(struct r600_bc_alu));
		alu.inst = V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_MULADD;

		alu.src[0] = r600_src[0];
		switch (i) {
		case 0:
			alu.src[0].chan = tgsi_chan(&inst->Src[0], 1);
			break;
		case 1:
			alu.src[0].chan = tgsi_chan(&inst->Src[0], 2);
			break;
		case 2:
			alu.src[0].chan = tgsi_chan(&inst->Src[0], 0);
			break;
		case 3:
			alu.src[0].sel = V_SQ_ALU_SRC_0;
			alu.src[0].chan = i;
		}

		alu.src[1] = r600_src[1];
		switch (i) {
		case 0:
			alu.src[1].chan = tgsi_chan(&inst->Src[1], 2);
			break;
		case 1:
			alu.src[1].chan = tgsi_chan(&inst->Src[1], 0);
			break;
		case 2:
			alu.src[1].chan = tgsi_chan(&inst->Src[1], 1);
			break;
		case 3:
			alu.src[1].sel = V_SQ_ALU_SRC_0;
			alu.src[1].chan = i;
		}

		alu.src[2].sel = ctx->temp_reg;
		alu.src[2].neg = 1;
		alu.src[2].chan = i;

		if (use_temp)
			alu.dst.sel = ctx->temp_reg;
		else {
			r = tgsi_dst(ctx, &inst->Dst[0], i, &alu.dst);
			if (r)
				return r;
		}
		alu.dst.chan = i;
		alu.dst.write = 1;
		alu.is_op3 = 1;
		if (i == 3)
			alu.last = 1;
		r = r600_bc_add_alu(ctx->bc, &alu);
		if (r)
			return r;
	}
	if (use_temp)
		return tgsi_helper_copy(ctx, inst);
	return 0;
}


static struct r600_shader_tgsi_instruction r600_shader_tgsi_instruction[] = {
	{TGSI_OPCODE_ARL,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_MOV,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV, tgsi_op2},
	{TGSI_OPCODE_LIT,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_lit},
	{TGSI_OPCODE_RCP,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_RECIP_IEEE, tgsi_trans_srcx_replicate},
	{TGSI_OPCODE_RSQ,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_RECIPSQRT_IEEE, tgsi_trans_srcx_replicate},
	{TGSI_OPCODE_EXP,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_LOG,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_MUL,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MUL, tgsi_op2},
	{TGSI_OPCODE_ADD,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD, tgsi_op2},
	{TGSI_OPCODE_DP3,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_DOT4, tgsi_dp},
	{TGSI_OPCODE_DP4,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_DOT4, tgsi_dp},
	{TGSI_OPCODE_DST,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_MIN,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MIN, tgsi_op2},
	{TGSI_OPCODE_MAX,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MAX, tgsi_op2},
	{TGSI_OPCODE_SLT,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETGT, tgsi_slt},
	{TGSI_OPCODE_SGE,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETGE, tgsi_op2},
	{TGSI_OPCODE_MAD,	1, V_SQ_ALU_WORD1_OP3_SQ_OP3_INST_MULADD, tgsi_op3},
	{TGSI_OPCODE_SUB,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_ADD, tgsi_op2},
	{TGSI_OPCODE_LRP,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_lrp},
	{TGSI_OPCODE_CND,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	/* gap */
	{20,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_DP2A,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	/* gap */
	{22,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{23,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_FRC,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FRACT, tgsi_op2},
	{TGSI_OPCODE_CLAMP,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_FLR,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_FLOOR, tgsi_op2},
	{TGSI_OPCODE_ROUND,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_EX2,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_EXP_IEEE, tgsi_trans_srcx_replicate},
	{TGSI_OPCODE_LG2,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_LOG_IEEE, tgsi_trans_srcx_replicate},
	{TGSI_OPCODE_POW,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_pow},
	{TGSI_OPCODE_XPD,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_xpd},
	/* gap */
	{32,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ABS,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_MOV, tgsi_op2},
	{TGSI_OPCODE_RCC,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_DPH,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_COS,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_COS, tgsi_trig},
	{TGSI_OPCODE_DDX,	0, SQ_TEX_INST_GET_GRADIENTS_H, tgsi_tex},
	{TGSI_OPCODE_DDY,	0, SQ_TEX_INST_GET_GRADIENTS_V, tgsi_tex},
	{TGSI_OPCODE_KILP,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},  /* predicated kill */
	{TGSI_OPCODE_PK2H,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_PK2US,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_PK4B,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_PK4UB,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_RFL,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_SEQ,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETE, tgsi_op2},
	{TGSI_OPCODE_SFL,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_SGT,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETGT, tgsi_op2},
	{TGSI_OPCODE_SIN,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SIN, tgsi_trig},
	{TGSI_OPCODE_SLE,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETGE, tgsi_slt},
	{TGSI_OPCODE_SNE,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_SETNE, tgsi_op2},
	{TGSI_OPCODE_STR,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_TEX,	0, SQ_TEX_INST_SAMPLE, tgsi_tex},
	{TGSI_OPCODE_TXD,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_TXP,	0, SQ_TEX_INST_SAMPLE, tgsi_tex},
	{TGSI_OPCODE_UP2H,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_UP2US,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_UP4B,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_UP4UB,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_X2D,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ARA,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ARR,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_BRA,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_CAL,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_RET,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_SSG,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_ssg},
	{TGSI_OPCODE_CMP,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_cmp},
	{TGSI_OPCODE_SCS,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_TXB,	0, SQ_TEX_INST_SAMPLE_L, tgsi_tex},
	{TGSI_OPCODE_NRM,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_DIV,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_DP2,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_DOT4, tgsi_dp},
	{TGSI_OPCODE_TXL,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_BRK,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_IF,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	/* gap */
	{75,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{76,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ELSE,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ENDIF,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	/* gap */
	{79,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{80,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_PUSHA,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_POPA,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_CEIL,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_I2F,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_NOT,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_TRUNC,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_TRUNC, tgsi_trans_srcx_replicate},
	{TGSI_OPCODE_SHL,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	/* gap */
	{88,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_AND,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_OR,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_MOD,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_XOR,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_SAD,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_TXF,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_TXQ,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_CONT,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_EMIT,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ENDPRIM,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_BGNLOOP,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_BGNSUB,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ENDLOOP,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ENDSUB,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	/* gap */
	{103,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{104,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{105,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{106,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_NOP,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	/* gap */
	{108,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{109,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{110,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{111,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_NRM4,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_CALLNZ,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_IFC,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_BREAKC,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_KIL,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_KILLGT, tgsi_kill},  /* conditional kill */
	{TGSI_OPCODE_END,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_end},  /* aka HALT */
	/* gap */
	{118,			0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_F2I,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_IDIV,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_IMAX,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_IMIN,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_INEG,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ISGE,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ISHR,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ISLT,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_F2U,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_U2F,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_UADD,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_UDIV,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_UMAD,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_UMAX,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_UMIN,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_UMOD,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_UMUL,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_USEQ,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_USGE,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_USHR,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_USLT,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_USNE,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_SWITCH,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_CASE,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_DEFAULT,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_ENDSWITCH,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
	{TGSI_OPCODE_LAST,	0, V_SQ_ALU_WORD1_OP2_SQ_OP2_INST_NOP, tgsi_unsupported},
};
