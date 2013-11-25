#include "util/u_memory.h"

#include "../radeon/r600_cs.h"
#include "radeonsi_pipe.h"
#include "radeonsi_shader.h"

#include "radeon_llvm_util.h"

#define MAX_GLOBAL_BUFFERS 20

struct si_pipe_compute {
	struct r600_context *ctx;

	unsigned local_size;
	unsigned private_size;
	unsigned input_size;
	unsigned num_kernels;
	struct si_pipe_shader *kernels;
	unsigned num_user_sgprs;

        struct pipe_resource *global_buffers[MAX_GLOBAL_BUFFERS];

};

static void *radeonsi_create_compute_state(
	struct pipe_context *ctx,
	const struct pipe_compute_state *cso)
{
	struct r600_context *rctx = (struct r600_context *)ctx;
	struct si_pipe_compute *program =
					CALLOC_STRUCT(si_pipe_compute);
	const struct pipe_llvm_program_header *header;
	const unsigned char *code;
	unsigned i;

	header = cso->prog;
	code = cso->prog + sizeof(struct pipe_llvm_program_header);

	program->ctx = rctx;
	program->local_size = cso->req_local_mem;
	program->private_size = cso->req_private_mem;
	program->input_size = cso->req_input_mem;

	program->num_kernels = radeon_llvm_get_num_kernels(code,
							header->num_bytes);
	program->kernels = CALLOC(sizeof(struct si_pipe_shader),
							program->num_kernels);
	for (i = 0; i < program->num_kernels; i++) {
		LLVMModuleRef mod = radeon_llvm_get_kernel_module(i, code,
							header->num_bytes);
		si_compile_llvm(rctx, &program->kernels[i], mod);
		LLVMDisposeModule(mod);
	}

	return program;
}

static void radeonsi_bind_compute_state(struct pipe_context *ctx, void *state)
{
	struct r600_context *rctx = (struct r600_context*)ctx;
	rctx->cs_shader_state.program = (struct si_pipe_compute*)state;
}

static void radeonsi_set_global_binding(
	struct pipe_context *ctx, unsigned first, unsigned n,
	struct pipe_resource **resources,
	uint32_t **handles)
{
	unsigned i;
	struct r600_context *rctx = (struct r600_context*)ctx;
	struct si_pipe_compute *program = rctx->cs_shader_state.program;

	if (!resources) {
		for (i = first; i < first + n; i++) {
			program->global_buffers[i] = NULL;
		}
		return;
	}

	for (i = first; i < first + n; i++) {
		uint64_t va;
		program->global_buffers[i] = resources[i];
		va = r600_resource_va(ctx->screen, resources[i]);
		memcpy(handles[i], &va, sizeof(va));
	}
}

static void radeonsi_launch_grid(
		struct pipe_context *ctx,
		const uint *block_layout, const uint *grid_layout,
		uint32_t pc, const void *input)
{
	struct r600_context *rctx = (struct r600_context*)ctx;
	struct si_pipe_compute *program = rctx->cs_shader_state.program;
	struct si_pm4_state *pm4 = CALLOC_STRUCT(si_pm4_state);
	struct r600_resource *kernel_args_buffer = NULL;
	unsigned kernel_args_size;
	unsigned num_work_size_bytes = 36;
	uint32_t kernel_args_offset = 0;
	uint32_t *kernel_args;
	uint64_t kernel_args_va;
	uint64_t shader_va;
	unsigned arg_user_sgpr_count = 2;
	unsigned i;
	struct si_pipe_shader *shader = &program->kernels[pc];
	unsigned lds_blocks;

	pm4->compute_pkt = true;
	si_cmd_context_control(pm4);

	si_pm4_cmd_begin(pm4, PKT3_EVENT_WRITE);
	si_pm4_cmd_add(pm4, EVENT_TYPE(EVENT_TYPE_CACHE_FLUSH) |
	                    EVENT_INDEX(0x7) |
			    EVENT_WRITE_INV_L2);
	si_pm4_cmd_end(pm4, false);

	si_pm4_inval_texture_cache(pm4);
	si_pm4_inval_shader_cache(pm4);
	si_cmd_surface_sync(pm4, pm4->cp_coher_cntl);

	/* Upload the kernel arguments */

	/* The extra num_work_size_bytes are for work group / work item size information */
	kernel_args_size = program->input_size + num_work_size_bytes;
	kernel_args = MALLOC(kernel_args_size);
	for (i = 0; i < 3; i++) {
		kernel_args[i] = grid_layout[i];
		kernel_args[i + 3] = grid_layout[i] * block_layout[i];
		kernel_args[i + 6] = block_layout[i];
	}

	memcpy(kernel_args + (num_work_size_bytes / 4), input, program->input_size);

	r600_upload_const_buffer(rctx, &kernel_args_buffer, (uint8_t*)kernel_args,
					kernel_args_size, &kernel_args_offset);
	kernel_args_va = r600_resource_va(ctx->screen,
				(struct pipe_resource*)kernel_args_buffer);
	kernel_args_va += kernel_args_offset;

	si_pm4_add_bo(pm4, kernel_args_buffer, RADEON_USAGE_READ);

	si_pm4_set_reg(pm4, R_00B900_COMPUTE_USER_DATA_0, kernel_args_va);
	si_pm4_set_reg(pm4, R_00B900_COMPUTE_USER_DATA_0 + 4, S_008F04_BASE_ADDRESS_HI (kernel_args_va >> 32) | S_008F04_STRIDE(0));

	si_pm4_set_reg(pm4, R_00B810_COMPUTE_START_X, 0);
	si_pm4_set_reg(pm4, R_00B814_COMPUTE_START_Y, 0);
	si_pm4_set_reg(pm4, R_00B818_COMPUTE_START_Z, 0);

	si_pm4_set_reg(pm4, R_00B81C_COMPUTE_NUM_THREAD_X,
				S_00B81C_NUM_THREAD_FULL(block_layout[0]));
	si_pm4_set_reg(pm4, R_00B820_COMPUTE_NUM_THREAD_Y,
				S_00B820_NUM_THREAD_FULL(block_layout[1]));
	si_pm4_set_reg(pm4, R_00B824_COMPUTE_NUM_THREAD_Z,
				S_00B824_NUM_THREAD_FULL(block_layout[2]));

	/* Global buffers */
	for (i = 0; i < MAX_GLOBAL_BUFFERS; i++) {
		struct r600_resource *buffer =
				(struct r600_resource*)program->global_buffers[i];
		if (!buffer) {
			continue;
		}
		si_pm4_add_bo(pm4, buffer, RADEON_USAGE_READWRITE);
	}

	/* This register has been moved to R_00CD20_COMPUTE_MAX_WAVE_ID
	 * and is now per pipe, so it should be handled in the
	 * kernel if we want to use something other than the default value,
	 * which is now 0x22f.
	 */
	if (rctx->b.chip_class <= SI) {
		/* XXX: This should be:
		 * (number of compute units) * 4 * (waves per simd) - 1 */

		si_pm4_set_reg(pm4, R_00B82C_COMPUTE_MAX_WAVE_ID,
						0x190 /* Default value */);
	}

	shader_va = r600_resource_va(ctx->screen, (void *)shader->bo);
	si_pm4_add_bo(pm4, shader->bo, RADEON_USAGE_READ);
	si_pm4_set_reg(pm4, R_00B830_COMPUTE_PGM_LO, (shader_va >> 8) & 0xffffffff);
	si_pm4_set_reg(pm4, R_00B834_COMPUTE_PGM_HI, shader_va >> 40);

	si_pm4_set_reg(pm4, R_00B848_COMPUTE_PGM_RSRC1,
		/* We always use at least 3 VGPRS, these come from
		 * TIDIG_COMP_CNT.
		 * XXX: The compiler should account for this.
		 */
		S_00B848_VGPRS((MAX2(3, shader->num_vgprs) - 1) / 4)
		/* We always use at least 4 + arg_user_sgpr_count.  The 4 extra
		 * sgprs are from TGID_X_EN, TGID_Y_EN, TGID_Z_EN, TG_SIZE_EN
		 * XXX: The compiler should account for this.
		 */
		|  S_00B848_SGPRS(((MAX2(4 + arg_user_sgpr_count,
		                        shader->num_sgprs)) - 1) / 8))
		;

	lds_blocks = shader->lds_size;
	/* XXX: We are over allocating LDS.  For SI, the shader reports LDS in
	 * blocks of 256 bytes, so if there are 4 bytes lds allocated in
	 * the shader and 4 bytes allocated by the state tracker, then
	 * we will set LDS_SIZE to 512 bytes rather than 256.
	 */
	if (rctx->b.chip_class <= SI) {
		lds_blocks += align(program->local_size, 256) >> 8;
	} else {
		lds_blocks += align(program->local_size, 512) >> 9;
	}

	assert(lds_blocks <= 0xFF);

	si_pm4_set_reg(pm4, R_00B84C_COMPUTE_PGM_RSRC2,
		S_00B84C_SCRATCH_EN(0)
		| S_00B84C_USER_SGPR(arg_user_sgpr_count)
		| S_00B84C_TGID_X_EN(1)
		| S_00B84C_TGID_Y_EN(1)
		| S_00B84C_TGID_Z_EN(1)
		| S_00B84C_TG_SIZE_EN(1)
		| S_00B84C_TIDIG_COMP_CNT(2)
		| S_00B84C_LDS_SIZE(lds_blocks)
		| S_00B84C_EXCP_EN(0))
		;
	si_pm4_set_reg(pm4, R_00B854_COMPUTE_RESOURCE_LIMITS, 0);

	si_pm4_set_reg(pm4, R_00B858_COMPUTE_STATIC_THREAD_MGMT_SE0,
		S_00B858_SH0_CU_EN(0xffff /* Default value */)
		| S_00B858_SH1_CU_EN(0xffff /* Default value */))
		;

	si_pm4_set_reg(pm4, R_00B85C_COMPUTE_STATIC_THREAD_MGMT_SE1,
		S_00B85C_SH0_CU_EN(0xffff /* Default value */)
		| S_00B85C_SH1_CU_EN(0xffff /* Default value */))
		;

	si_pm4_cmd_begin(pm4, PKT3_DISPATCH_DIRECT);
	si_pm4_cmd_add(pm4, grid_layout[0]); /* Thread groups DIM_X */
	si_pm4_cmd_add(pm4, grid_layout[1]); /* Thread groups DIM_Y */
	si_pm4_cmd_add(pm4, grid_layout[2]); /* Thread gropus DIM_Z */
	si_pm4_cmd_add(pm4, 1); /* DISPATCH_INITIATOR */
        si_pm4_cmd_end(pm4, false);

	si_pm4_cmd_begin(pm4, PKT3_EVENT_WRITE);
	si_pm4_cmd_add(pm4, EVENT_TYPE(V_028A90_CS_PARTIAL_FLUSH | EVENT_INDEX(0x4)));
	si_pm4_cmd_end(pm4, false);

	si_pm4_inval_texture_cache(pm4);
	si_pm4_inval_shader_cache(pm4);
	si_cmd_surface_sync(pm4, pm4->cp_coher_cntl);

	si_pm4_emit(rctx, pm4);

#if 0
	fprintf(stderr, "cdw: %i\n", rctx->cs->cdw);
	for (i = 0; i < rctx->cs->cdw; i++) {
		fprintf(stderr, "%4i : 0x%08X\n", i, rctx->cs->buf[i]);
	}
#endif

	FREE(pm4);
	FREE(kernel_args);
}


static void si_delete_compute_state(struct pipe_context *ctx, void* state){
	struct si_pipe_compute *program = (struct si_pipe_compute *)state;

	if (!state) {
		return;
	}

	if (program->kernels) {
		FREE(program->kernels);
	}

	//And then free the program itself.
	FREE(program);
}

static void si_set_compute_resources(struct pipe_context * ctx_,
		unsigned start, unsigned count,
		struct pipe_surface ** surfaces) { }

void si_init_compute_functions(struct r600_context *rctx)
{
	rctx->b.b.create_compute_state = radeonsi_create_compute_state;
	rctx->b.b.delete_compute_state = si_delete_compute_state;
	rctx->b.b.bind_compute_state = radeonsi_bind_compute_state;
/*	 ctx->context.create_sampler_view = evergreen_compute_create_sampler_view; */
	rctx->b.b.set_compute_resources = si_set_compute_resources;
	rctx->b.b.set_global_binding = radeonsi_set_global_binding;
	rctx->b.b.launch_grid = radeonsi_launch_grid;
}
