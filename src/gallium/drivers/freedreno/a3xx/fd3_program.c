/* -*- mode: C; c-file-style: "k&r"; tab-width 4; indent-tabs-mode: t; -*- */

/*
 * Copyright (C) 2013 Rob Clark <robclark@freedesktop.org>
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
 * Authors:
 *    Rob Clark <robclark@freedesktop.org>
 */

#include "pipe/p_state.h"
#include "util/u_string.h"
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_parse.h"

#include "freedreno_lowering.h"

#include "fd3_program.h"
#include "fd3_compiler.h"
#include "fd3_emit.h"
#include "fd3_texture.h"
#include "fd3_util.h"

static void
delete_shader(struct fd3_shader_stateobj *so)
{
	ir3_shader_destroy(so->ir);
	fd_bo_del(so->bo);
	free(so);
}

static void
assemble_shader(struct pipe_context *pctx, struct fd3_shader_stateobj *so)
{
	struct fd_context *ctx = fd_context(pctx);
	uint32_t sz, *bin;

	bin = ir3_shader_assemble(so->ir, &so->info);
	sz = so->info.sizedwords * 4;

	so->bo = fd_bo_new(ctx->dev, sz,
			DRM_FREEDRENO_GEM_CACHE_WCOMBINE |
			DRM_FREEDRENO_GEM_TYPE_KMEM);

	memcpy(fd_bo_map(so->bo), bin, sz);

	free(bin);

	so->instrlen = so->info.sizedwords / 8;
	so->constlen = so->info.max_const + 1;
}

/* for vertex shader, the inputs are loaded into registers before the shader
 * is executed, so max_regs from the shader instructions might not properly
 * reflect the # of registers actually used:
 */
static void
fixup_vp_regfootprint(struct fd3_shader_stateobj *so)
{
	unsigned i;
	for (i = 0; i < so->inputs_count; i++) {
		so->info.max_reg = MAX2(so->info.max_reg, so->inputs[i].regid >> 2);
	}
}

static struct fd3_shader_stateobj *
create_shader(struct pipe_context *pctx, const struct pipe_shader_state *cso,
		enum shader_t type)
{
	struct fd3_shader_stateobj *so = CALLOC_STRUCT(fd3_shader_stateobj);
	const struct tgsi_token *tokens = fd_transform_lowering(cso->tokens);
	int ret;

	if (!so)
		return NULL;

	so->type = type;

	if (fd_mesa_debug & FD_DBG_DISASM) {
		DBG("dump tgsi: type=%d", so->type);
		tgsi_dump(tokens, 0);
	}

	if ((type == SHADER_FRAGMENT) && (fd_mesa_debug & FD_DBG_FRAGHALF))
		so->half_precision = true;

	ret = fd3_compile_shader(so, tokens);
	if (ret) {
		debug_error("compile failed!");
		goto fail;
	}

	assemble_shader(pctx, so);
	if (!so->bo) {
		debug_error("assemble failed!");
		goto fail;
	}

	if (type == SHADER_VERTEX)
		fixup_vp_regfootprint(so);

	if (fd_mesa_debug & FD_DBG_DISASM) {
		DBG("disassemble: type=%d", so->type);
		disasm_a3xx(fd_bo_map(so->bo), so->info.sizedwords, 0, so->type);
	}

	return so;

fail:
	delete_shader(so);
	return NULL;
}

static void *
fd3_fp_state_create(struct pipe_context *pctx,
		const struct pipe_shader_state *cso)
{
	return create_shader(pctx, cso, SHADER_FRAGMENT);
}

static void
fd3_fp_state_delete(struct pipe_context *pctx, void *hwcso)
{
	struct fd3_shader_stateobj *so = hwcso;
	delete_shader(so);
}

static void
fd3_fp_state_bind(struct pipe_context *pctx, void *hwcso)
{
	struct fd_context *ctx = fd_context(pctx);
	ctx->prog.fp = hwcso;
	ctx->prog.dirty |= FD_SHADER_DIRTY_FP;
	ctx->dirty |= FD_DIRTY_PROG;
}

static void *
fd3_vp_state_create(struct pipe_context *pctx,
		const struct pipe_shader_state *cso)
{
	return create_shader(pctx, cso, SHADER_VERTEX);
}

static void
fd3_vp_state_delete(struct pipe_context *pctx, void *hwcso)
{
	struct fd3_shader_stateobj *so = hwcso;
	delete_shader(so);
}

static void
fd3_vp_state_bind(struct pipe_context *pctx, void *hwcso)
{
	struct fd_context *ctx = fd_context(pctx);
	ctx->prog.vp = hwcso;
	ctx->prog.dirty |= FD_SHADER_DIRTY_VP;
	ctx->dirty |= FD_DIRTY_PROG;
}

static void
emit_shader(struct fd_ringbuffer *ring, const struct fd3_shader_stateobj *so)
{
	const struct ir3_shader_info *si = &so->info;
	enum adreno_state_block sb;
	enum adreno_state_src src;
	uint32_t i, sz, *bin;

	if (so->type == SHADER_VERTEX) {
		sb = SB_VERT_SHADER;
	} else {
		sb = SB_FRAG_SHADER;
	}

	if (fd_mesa_debug & FD_DBG_DIRECT) {
		sz = si->sizedwords;
		src = SS_DIRECT;
		bin = fd_bo_map(so->bo);
	} else {
		sz = 0;
		src = SS_INDIRECT;
		bin = NULL;
	}

	OUT_PKT3(ring, CP_LOAD_STATE, 2 + sz);
	OUT_RING(ring, CP_LOAD_STATE_0_DST_OFF(0) |
			CP_LOAD_STATE_0_STATE_SRC(src) |
			CP_LOAD_STATE_0_STATE_BLOCK(sb) |
			CP_LOAD_STATE_0_NUM_UNIT(so->instrlen));
	if (bin) {
		OUT_RING(ring, CP_LOAD_STATE_1_EXT_SRC_ADDR(0) |
				CP_LOAD_STATE_1_STATE_TYPE(ST_SHADER));
	} else {
		OUT_RELOC(ring, so->bo, 0,
				CP_LOAD_STATE_1_STATE_TYPE(ST_SHADER), 0);
	}
	for (i = 0; i < sz; i++) {
		OUT_RING(ring, bin[i]);
	}
}

static int
find_output(const struct fd3_shader_stateobj *so, fd3_semantic semantic)
{
	int j;
	for (j = 0; j < so->outputs_count; j++)
		if (so->outputs[j].semantic == semantic)
			return j;
	return 0;
}

static uint32_t
find_regid(const struct fd3_shader_stateobj *so, fd3_semantic semantic)
{
	int j;
	for (j = 0; j < so->outputs_count; j++)
		if (so->outputs[j].semantic == semantic)
			return so->outputs[j].regid;
	return regid(63, 0);
}

void
fd3_program_emit(struct fd_ringbuffer *ring,
		struct fd_program_stateobj *prog, bool binning)
{
	const struct fd3_shader_stateobj *vp = prog->vp;
	const struct fd3_shader_stateobj *fp = prog->fp;
	const struct ir3_shader_info *vsi = &vp->info;
	const struct ir3_shader_info *fsi = &fp->info;
	uint32_t pos_regid, posz_regid, psize_regid, color_regid;
	int i;

	if (binning) {
		/* use dummy stateobj to simplify binning vs non-binning: */
		static const struct fd3_shader_stateobj binning_fp = {};
		fp = &binning_fp;
		fsi = &fp->info;
	}

	pos_regid = find_regid(vp,
		fd3_semantic_name(TGSI_SEMANTIC_POSITION, 0));
	posz_regid = find_regid(fp,
		fd3_semantic_name(TGSI_SEMANTIC_POSITION, 0));
	psize_regid = find_regid(vp,
		fd3_semantic_name(TGSI_SEMANTIC_PSIZE, 0));
	color_regid = find_regid(fp,
		fd3_semantic_name(TGSI_SEMANTIC_COLOR, 0));

	/* we could probably divide this up into things that need to be
	 * emitted if frag-prog is dirty vs if vert-prog is dirty..
	 */

	OUT_PKT0(ring, REG_A3XX_HLSQ_CONTROL_0_REG, 6);
	OUT_RING(ring, A3XX_HLSQ_CONTROL_0_REG_FSTHREADSIZE(FOUR_QUADS) |
			/* NOTE:  I guess SHADERRESTART and CONSTFULLUPDATE maybe
			 * flush some caches? I think we only need to set those
			 * bits if we have updated const or shader..
			 */
			A3XX_HLSQ_CONTROL_0_REG_SPSHADERRESTART |
			A3XX_HLSQ_CONTROL_0_REG_SPCONSTFULLUPDATE);
	OUT_RING(ring, A3XX_HLSQ_CONTROL_1_REG_VSTHREADSIZE(TWO_QUADS) |
			A3XX_HLSQ_CONTROL_1_REG_VSSUPERTHREADENABLE);
	OUT_RING(ring, A3XX_HLSQ_CONTROL_2_REG_PRIMALLOCTHRESHOLD(31));
	OUT_RING(ring, 0x00000000);        /* HLSQ_CONTROL_3_REG */
	OUT_RING(ring, A3XX_HLSQ_VS_CONTROL_REG_CONSTLENGTH(vp->constlen) |
			A3XX_HLSQ_VS_CONTROL_REG_CONSTSTARTOFFSET(0) |
			A3XX_HLSQ_VS_CONTROL_REG_INSTRLENGTH(vp->instrlen));
	OUT_RING(ring, A3XX_HLSQ_FS_CONTROL_REG_CONSTLENGTH(fp->constlen) |
			A3XX_HLSQ_FS_CONTROL_REG_CONSTSTARTOFFSET(128) |
			A3XX_HLSQ_FS_CONTROL_REG_INSTRLENGTH(fp->instrlen));

	OUT_PKT0(ring, REG_A3XX_SP_SP_CTRL_REG, 1);
	OUT_RING(ring, A3XX_SP_SP_CTRL_REG_CONSTMODE(0) |
			COND(binning, A3XX_SP_SP_CTRL_REG_BINNING) |
			A3XX_SP_SP_CTRL_REG_SLEEPMODE(1) |
			A3XX_SP_SP_CTRL_REG_L0MODE(0));

	OUT_PKT0(ring, REG_A3XX_SP_VS_LENGTH_REG, 1);
	OUT_RING(ring, A3XX_SP_VS_LENGTH_REG_SHADERLENGTH(vp->instrlen));

	OUT_PKT0(ring, REG_A3XX_SP_VS_CTRL_REG0, 3);
	OUT_RING(ring, A3XX_SP_VS_CTRL_REG0_THREADMODE(MULTI) |
			A3XX_SP_VS_CTRL_REG0_INSTRBUFFERMODE(BUFFER) |
			A3XX_SP_VS_CTRL_REG0_CACHEINVALID |
			A3XX_SP_VS_CTRL_REG0_HALFREGFOOTPRINT(vsi->max_half_reg + 1) |
			A3XX_SP_VS_CTRL_REG0_FULLREGFOOTPRINT(vsi->max_reg + 1) |
			A3XX_SP_VS_CTRL_REG0_INOUTREGOVERLAP(0) |
			A3XX_SP_VS_CTRL_REG0_THREADSIZE(TWO_QUADS) |
			A3XX_SP_VS_CTRL_REG0_SUPERTHREADMODE |
			COND(vp->samplers_count > 0, A3XX_SP_VS_CTRL_REG0_PIXLODENABLE) |
			A3XX_SP_VS_CTRL_REG0_LENGTH(vp->instrlen));
	OUT_RING(ring, A3XX_SP_VS_CTRL_REG1_CONSTLENGTH(vp->constlen) |
			A3XX_SP_VS_CTRL_REG1_INITIALOUTSTANDING(vp->total_in) |
			A3XX_SP_VS_CTRL_REG1_CONSTFOOTPRINT(MAX2(vsi->max_const, 0)));
	OUT_RING(ring, A3XX_SP_VS_PARAM_REG_POSREGID(pos_regid) |
			A3XX_SP_VS_PARAM_REG_PSIZEREGID(psize_regid) |
			A3XX_SP_VS_PARAM_REG_TOTALVSOUTVAR(fp->inputs_count));

	for (i = 0; i < fp->inputs_count; ) {
		uint32_t reg = 0;
		int j;

		OUT_PKT0(ring, REG_A3XX_SP_VS_OUT_REG(i/2), 1);

		j = find_output(vp, fp->inputs[i].semantic);
		reg |= A3XX_SP_VS_OUT_REG_A_REGID(vp->outputs[j].regid);
		reg |= A3XX_SP_VS_OUT_REG_A_COMPMASK(fp->inputs[i].compmask);
		i++;

		j = find_output(vp, fp->inputs[i].semantic);
		reg |= A3XX_SP_VS_OUT_REG_B_REGID(vp->outputs[j].regid);
		reg |= A3XX_SP_VS_OUT_REG_B_COMPMASK(fp->inputs[i].compmask);
		i++;

		OUT_RING(ring, reg);
	}

	for (i = 0; i < fp->inputs_count; ) {
		uint32_t reg = 0;

		OUT_PKT0(ring, REG_A3XX_SP_VS_VPC_DST_REG(i/4), 1);

		reg |= A3XX_SP_VS_VPC_DST_REG_OUTLOC0(fp->inputs[i++].inloc);
		reg |= A3XX_SP_VS_VPC_DST_REG_OUTLOC1(fp->inputs[i++].inloc);
		reg |= A3XX_SP_VS_VPC_DST_REG_OUTLOC2(fp->inputs[i++].inloc);
		reg |= A3XX_SP_VS_VPC_DST_REG_OUTLOC3(fp->inputs[i++].inloc);

		OUT_RING(ring, reg);
	}

	OUT_PKT0(ring, REG_A3XX_SP_VS_OBJ_OFFSET_REG, 2);
	OUT_RING(ring, A3XX_SP_VS_OBJ_OFFSET_REG_CONSTOBJECTOFFSET(0) |
			A3XX_SP_VS_OBJ_OFFSET_REG_SHADEROBJOFFSET(0));
	OUT_RELOC(ring, vp->bo, 0, 0, 0);  /* SP_VS_OBJ_START_REG */

	if (binning) {
		OUT_PKT0(ring, REG_A3XX_SP_FS_LENGTH_REG, 1);
		OUT_RING(ring, 0x00000000);

		OUT_PKT0(ring, REG_A3XX_SP_FS_CTRL_REG0, 2);
		OUT_RING(ring, A3XX_SP_FS_CTRL_REG0_THREADMODE(MULTI) |
				A3XX_SP_FS_CTRL_REG0_INSTRBUFFERMODE(BUFFER));
		OUT_RING(ring, 0x00000000);
	} else {
		OUT_PKT0(ring, REG_A3XX_SP_FS_LENGTH_REG, 1);
		OUT_RING(ring, A3XX_SP_FS_LENGTH_REG_SHADERLENGTH(fp->instrlen));

		OUT_PKT0(ring, REG_A3XX_SP_FS_CTRL_REG0, 2);
		OUT_RING(ring, A3XX_SP_FS_CTRL_REG0_THREADMODE(MULTI) |
				A3XX_SP_FS_CTRL_REG0_INSTRBUFFERMODE(BUFFER) |
				A3XX_SP_FS_CTRL_REG0_CACHEINVALID |
				A3XX_SP_FS_CTRL_REG0_HALFREGFOOTPRINT(fsi->max_half_reg + 1) |
				A3XX_SP_FS_CTRL_REG0_FULLREGFOOTPRINT(fsi->max_reg + 1) |
				A3XX_SP_FS_CTRL_REG0_INOUTREGOVERLAP(1) |
				A3XX_SP_FS_CTRL_REG0_THREADSIZE(FOUR_QUADS) |
				A3XX_SP_FS_CTRL_REG0_SUPERTHREADMODE |
				COND(fp->samplers_count > 0, A3XX_SP_FS_CTRL_REG0_PIXLODENABLE) |
				A3XX_SP_FS_CTRL_REG0_LENGTH(fp->instrlen));
		OUT_RING(ring, A3XX_SP_FS_CTRL_REG1_CONSTLENGTH(fp->constlen) |
				A3XX_SP_FS_CTRL_REG1_INITIALOUTSTANDING(fp->total_in) |
				A3XX_SP_FS_CTRL_REG1_CONSTFOOTPRINT(MAX2(fsi->max_const, 0)) |
				A3XX_SP_FS_CTRL_REG1_HALFPRECVAROFFSET(63));
		OUT_PKT0(ring, REG_A3XX_SP_FS_OBJ_OFFSET_REG, 2);
		OUT_RING(ring, A3XX_SP_FS_OBJ_OFFSET_REG_CONSTOBJECTOFFSET(128) |
				A3XX_SP_FS_OBJ_OFFSET_REG_SHADEROBJOFFSET(0));
		OUT_RELOC(ring, fp->bo, 0, 0, 0);  /* SP_FS_OBJ_START_REG */
	}

	OUT_PKT0(ring, REG_A3XX_SP_FS_FLAT_SHAD_MODE_REG_0, 2);
	OUT_RING(ring, 0x00000000);        /* SP_FS_FLAT_SHAD_MODE_REG_0 */
	OUT_RING(ring, 0x00000000);        /* SP_FS_FLAT_SHAD_MODE_REG_1 */

	OUT_PKT0(ring, REG_A3XX_SP_FS_OUTPUT_REG, 1);
	if (fp->writes_pos) {
		OUT_RING(ring, A3XX_SP_FS_OUTPUT_REG_DEPTH_ENABLE |
				A3XX_SP_FS_OUTPUT_REG_DEPTH_REGID(posz_regid));
	} else {
		OUT_RING(ring, 0x00000000);
	}

	OUT_PKT0(ring, REG_A3XX_SP_FS_MRT_REG(0), 4);
	OUT_RING(ring, A3XX_SP_FS_MRT_REG_REGID(color_regid) |
			COND(fp->half_precision, A3XX_SP_FS_MRT_REG_HALF_PRECISION));
	OUT_RING(ring, A3XX_SP_FS_MRT_REG_REGID(0));
	OUT_RING(ring, A3XX_SP_FS_MRT_REG_REGID(0));
	OUT_RING(ring, A3XX_SP_FS_MRT_REG_REGID(0));

	if (binning) {
		OUT_PKT0(ring, REG_A3XX_VPC_ATTR, 2);
		OUT_RING(ring, A3XX_VPC_ATTR_THRDASSIGN(1) |
				A3XX_VPC_ATTR_LMSIZE(1));
		OUT_RING(ring, 0x00000000);
	} else {
		OUT_PKT0(ring, REG_A3XX_VPC_ATTR, 2);
		OUT_RING(ring, A3XX_VPC_ATTR_TOTALATTR(fp->total_in) |
				A3XX_VPC_ATTR_THRDASSIGN(1) |
				A3XX_VPC_ATTR_LMSIZE(1));
		OUT_RING(ring, A3XX_VPC_PACK_NUMFPNONPOSVAR(fp->total_in) |
				A3XX_VPC_PACK_NUMNONPOSVSVAR(fp->total_in));

		OUT_PKT0(ring, REG_A3XX_VPC_VARYING_INTERP_MODE(0), 4);
		OUT_RING(ring, fp->vinterp[0]);    /* VPC_VARYING_INTERP[0].MODE */
		OUT_RING(ring, fp->vinterp[1]);    /* VPC_VARYING_INTERP[1].MODE */
		OUT_RING(ring, fp->vinterp[2]);    /* VPC_VARYING_INTERP[2].MODE */
		OUT_RING(ring, fp->vinterp[3]);    /* VPC_VARYING_INTERP[3].MODE */

		OUT_PKT0(ring, REG_A3XX_VPC_VARYING_PS_REPL_MODE(0), 4);
		OUT_RING(ring, fp->vpsrepl[0]);    /* VPC_VARYING_PS_REPL[0].MODE */
		OUT_RING(ring, fp->vpsrepl[1]);    /* VPC_VARYING_PS_REPL[1].MODE */
		OUT_RING(ring, fp->vpsrepl[2]);    /* VPC_VARYING_PS_REPL[2].MODE */
		OUT_RING(ring, fp->vpsrepl[3]);    /* VPC_VARYING_PS_REPL[3].MODE */
	}

	OUT_PKT0(ring, REG_A3XX_VFD_VS_THREADING_THRESHOLD, 1);
	OUT_RING(ring, A3XX_VFD_VS_THREADING_THRESHOLD_REGID_THRESHOLD(15) |
			A3XX_VFD_VS_THREADING_THRESHOLD_REGID_VTXCNT(252));

	emit_shader(ring, vp);

	OUT_PKT0(ring, REG_A3XX_VFD_PERFCOUNTER0_SELECT, 1);
	OUT_RING(ring, 0x00000000);        /* VFD_PERFCOUNTER0_SELECT */

	if (!binning) {
		emit_shader(ring, fp);

		OUT_PKT0(ring, REG_A3XX_VFD_PERFCOUNTER0_SELECT, 1);
		OUT_RING(ring, 0x00000000);        /* VFD_PERFCOUNTER0_SELECT */
	}

	OUT_PKT0(ring, REG_A3XX_VFD_CONTROL_0, 2);
	OUT_RING(ring, A3XX_VFD_CONTROL_0_TOTALATTRTOVS(vp->total_in) |
			A3XX_VFD_CONTROL_0_PACKETSIZE(2) |
			A3XX_VFD_CONTROL_0_STRMDECINSTRCNT(vp->inputs_count) |
			A3XX_VFD_CONTROL_0_STRMFETCHINSTRCNT(vp->inputs_count));
	OUT_RING(ring, A3XX_VFD_CONTROL_1_MAXSTORAGE(1) | // XXX
			A3XX_VFD_CONTROL_1_REGID4VTX(regid(63,0)) |
			A3XX_VFD_CONTROL_1_REGID4INST(regid(63,0)));
}

/* once the compiler is good enough, we should construct TGSI in the
 * core freedreno driver, and then let the a2xx/a3xx parts compile
 * the internal shaders from TGSI the same as regular shaders.  This
 * would be the first step towards handling most of clear (and the
 * gmem<->mem blits) from the core via normal state changes and shader
 * state objects.
 *
 * (Well, there would still be some special bits, because there are
 * some registers that don't get set for normal draw, but this should
 * be relatively small and could be handled via callbacks from core
 * into a2xx/a3xx..)
 */
static struct fd3_shader_stateobj *
create_internal_shader(struct pipe_context *pctx, enum shader_t type,
		struct ir3_shader *ir)
{
	struct fd3_shader_stateobj *so = CALLOC_STRUCT(fd3_shader_stateobj);

	if (!so) {
		ir3_shader_destroy(ir);
		return NULL;
	}

	so->type = type;
	so->ir = ir;

	assemble_shader(pctx, so);
	assert(so->bo);

	return so;
}

/* Creates shader:
 *    (sy)(ss)(rpt1)bary.f (ei)r0.z, (r)0, r0.x
 *    (rpt5)nop
 *    sam (f32)(xyzw)r0.x, r0.z, s#0, t#0
 *    (sy)(rpt3)cov.f32f16 hr0.x, (r)r0.x
 *    end
 */
static struct fd3_shader_stateobj *
create_blit_fp(struct pipe_context *pctx)
{
	struct fd3_shader_stateobj *so;
	struct ir3_shader *ir = ir3_shader_create();
	struct ir3_instruction *instr;

	/* (sy)(ss)(rpt1)bary.f (ei)r0.z, (r)0, r0.x */
	instr = ir3_instr_create(ir, 2, OPC_BARY_F);
	instr->flags = IR3_INSTR_SY | IR3_INSTR_SS;
	instr->repeat = 1;

	ir3_reg_create(instr, regid(0,2), IR3_REG_EI);    /* (ei)r0.z */
	ir3_reg_create(instr, 0, IR3_REG_R |              /* (r)0 */
			IR3_REG_IMMED)->iim_val = 0;
	ir3_reg_create(instr, regid(0,0), 0);             /* r0.x */

	/* (rpt5)nop */
	instr = ir3_instr_create(ir, 0, OPC_NOP);
	instr->repeat = 5;

	/* sam (f32)(xyzw)r0.x, r0.z, s#0, t#0 */
	instr = ir3_instr_create(ir, 5, OPC_SAM);
	instr->cat5.samp = 0;
	instr->cat5.tex  = 0;
	instr->cat5.type = TYPE_F32;

	ir3_reg_create(instr, regid(0,0),                 /* (xyzw)r0.x */
			0)->wrmask = 0xf;
	ir3_reg_create(instr, regid(0,2), 0);             /* r0.z */

	/* (sy)(rpt3)cov.f32f16 hr0.x, (r)r0.x */
	instr = ir3_instr_create(ir, 1, 0);  /* mov/cov instructions have no opc */
	instr->flags = IR3_INSTR_SY;
	instr->repeat = 3;
	instr->cat1.src_type = TYPE_F32;
	instr->cat1.dst_type = TYPE_F16;

	ir3_reg_create(instr, regid(0,0), IR3_REG_HALF);  /* hr0.x */
	ir3_reg_create(instr, regid(0,0), IR3_REG_R);     /* (r)r0.x */

	/* end */
	instr = ir3_instr_create(ir, 0, OPC_END);

	so = create_internal_shader(pctx, SHADER_FRAGMENT, ir);
	if (!so)
		return NULL;

	so->half_precision = true;
	so->inputs_count = 1;
	so->inputs[0].semantic =
		fd3_semantic_name(TGSI_SEMANTIC_TEXCOORD, 0);
	so->inputs[0].inloc = 8;
	so->inputs[0].compmask = 0x3;
	so->total_in = 2;
	so->outputs_count = 1;
	so->outputs[0].semantic =
		fd3_semantic_name(TGSI_SEMANTIC_COLOR, 0);
	so->outputs[0].regid = regid(0,0);
	so->samplers_count = 1;

	so->vpsrepl[0] = 0x99999999;
	so->vpsrepl[1] = 0x99999999;
	so->vpsrepl[2] = 0x99999999;
	so->vpsrepl[3] = 0x99999999;

	return so;
}

/* Creates shader:
 *    (sy)(ss)end
 */
static struct fd3_shader_stateobj *
create_blit_vp(struct pipe_context *pctx)
{
	struct fd3_shader_stateobj *so;
	struct ir3_shader *ir = ir3_shader_create();
	struct ir3_instruction *instr;

	/* (sy)(ss)end */
	instr = ir3_instr_create(ir, 0, OPC_END);
	instr->flags = IR3_INSTR_SY | IR3_INSTR_SS;

	so = create_internal_shader(pctx, SHADER_VERTEX, ir);
	if (!so)
		return NULL;

	so->inputs_count = 2;
	so->inputs[0].regid = regid(0,0);
	so->inputs[0].compmask = 0xf;
	so->inputs[1].regid = regid(1,0);
	so->inputs[1].compmask = 0xf;
	so->total_in = 8;
	so->outputs_count = 2;
	so->outputs[0].semantic =
		fd3_semantic_name(TGSI_SEMANTIC_TEXCOORD, 0);
	so->outputs[0].regid = regid(0,0);
	so->outputs[1].semantic =
		fd3_semantic_name(TGSI_SEMANTIC_POSITION, 0);
	so->outputs[1].regid = regid(1,0);

	fixup_vp_regfootprint(so);

	return so;
}

/* Creates shader:
 *    (sy)(ss)(rpt3)mov.f16f16 hr0.x, (r)hc0.x
 *    end
 */
static struct fd3_shader_stateobj *
create_solid_fp(struct pipe_context *pctx)
{
	struct fd3_shader_stateobj *so;
	struct ir3_shader *ir = ir3_shader_create();
	struct ir3_instruction *instr;

	/* (sy)(ss)(rpt3)mov.f16f16 hr0.x, (r)hc0.x */
	instr = ir3_instr_create(ir, 1, 0);  /* mov/cov instructions have no opc */
	instr->flags = IR3_INSTR_SY | IR3_INSTR_SS;
	instr->repeat = 3;
	instr->cat1.src_type = TYPE_F16;
	instr->cat1.dst_type = TYPE_F16;

	ir3_reg_create(instr, regid(0,0), IR3_REG_HALF);  /* hr0.x */
	ir3_reg_create(instr, regid(0,0), IR3_REG_HALF |  /* (r)hc0.x */
			IR3_REG_CONST | IR3_REG_R);

	/* end */
	instr = ir3_instr_create(ir, 0, OPC_END);

	so = create_internal_shader(pctx, SHADER_FRAGMENT, ir);
	if (!so)
		return NULL;

	so->half_precision = true;
	so->inputs_count = 0;
	so->outputs_count = 1;
	so->outputs[0].semantic =
		fd3_semantic_name(TGSI_SEMANTIC_COLOR, 0);
	so->outputs[0].regid = regid(0, 0);
	so->total_in = 0;

	return so;
}

/* Creates shader:
 *    (sy)(ss)end
 */
static struct fd3_shader_stateobj *
create_solid_vp(struct pipe_context *pctx)
{
	struct fd3_shader_stateobj *so;
	struct ir3_shader *ir = ir3_shader_create();
	struct ir3_instruction *instr;

	/* (sy)(ss)end */
	instr = ir3_instr_create(ir, 0, OPC_END);
	instr->flags = IR3_INSTR_SY | IR3_INSTR_SS;


	so = create_internal_shader(pctx, SHADER_VERTEX, ir);
	if (!so)
		return NULL;

	so->inputs_count = 1;
	so->inputs[0].regid = regid(0,0);
	so->inputs[0].compmask = 0xf;
	so->total_in = 4;

	so->outputs_count = 1;
	so->outputs[0].semantic =
		fd3_semantic_name(TGSI_SEMANTIC_POSITION, 0);
	so->outputs[0].regid = regid(0,0);

	fixup_vp_regfootprint(so);

	return so;
}

void
fd3_prog_init(struct pipe_context *pctx)
{
	struct fd_context *ctx = fd_context(pctx);

	pctx->create_fs_state = fd3_fp_state_create;
	pctx->bind_fs_state = fd3_fp_state_bind;
	pctx->delete_fs_state = fd3_fp_state_delete;

	pctx->create_vs_state = fd3_vp_state_create;
	pctx->bind_vs_state = fd3_vp_state_bind;
	pctx->delete_vs_state = fd3_vp_state_delete;

	ctx->solid_prog.fp = create_solid_fp(pctx);
	ctx->solid_prog.vp = create_solid_vp(pctx);
	ctx->blit_prog.fp = create_blit_fp(pctx);
	ctx->blit_prog.vp = create_blit_vp(pctx);
}

void
fd3_prog_fini(struct pipe_context *pctx)
{
	struct fd_context *ctx = fd_context(pctx);

	delete_shader(ctx->solid_prog.vp);
	delete_shader(ctx->solid_prog.fp);
	delete_shader(ctx->blit_prog.vp);
	delete_shader(ctx->blit_prog.fp);
}
