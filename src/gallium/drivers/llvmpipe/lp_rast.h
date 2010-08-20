/**************************************************************************
 *
 * Copyright 2009 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * The rast code is concerned with rasterization of command bins.
 * Each screen tile has a bin associated with it.  To render the
 * scene we iterate over the tile bins and execute the commands
 * in each bin.
 * We'll do that with multiple threads...
 */


#ifndef LP_RAST_H
#define LP_RAST_H

#include "pipe/p_compiler.h"
#include "lp_jit.h"


struct lp_rasterizer;
struct lp_scene;
struct lp_fence;
struct cmd_bin;

/** For sub-pixel positioning */
#define FIXED_ORDER 4
#define FIXED_ONE (1<<FIXED_ORDER)


struct lp_rasterizer_task;


/**
 * Rasterization state.
 * Objects of this type are put into the shared data bin and pointed
 * to by commands in the per-tile bins.
 */
struct lp_rast_state {
   /* State for the shader.  This also contains state which feeds into
    * the fragment shader, such as blend color and alpha ref value.
    */
   struct lp_jit_context jit_context;
   
   /* The shader itself.  Probably we also need to pass a pointer to
    * the tile color/z/stencil data somehow
     */
   struct lp_fragment_shader_variant *variant;
};


/**
 * Coefficients necessary to run the shader at a given location.
 * First coefficient is position.
 * These pointers point into the bin data buffer.
 */
struct lp_rast_shader_inputs {
   float facing;     /** Positive for front-facing, negative for back-facing */

   float (*a0)[4];
   float (*dadx)[4];
   float (*dady)[4];

   const struct lp_rast_state *state;
};

struct lp_rast_clearzs {
   unsigned clearzs_value;
   unsigned clearzs_mask;
};

struct lp_rast_plane {
   /* one-pixel sized trivial accept offsets for each plane */
   int ei;

   /* one-pixel sized trivial reject offsets for each plane */
   int eo;

   /* edge function values at minx,miny ?? */
   int c;

   int dcdx;
   int dcdy;
};

/**
 * Rasterization information for a triangle known to be in this bin,
 * plus inputs to run the shader:
 * These fields are tile- and bin-independent.
 * Objects of this type are put into the lp_setup_context::data buffer.
 */
struct lp_rast_triangle {
   /* inputs for the shader */
   struct lp_rast_shader_inputs inputs;

#ifdef DEBUG
   float v[3][2];
#endif

   struct lp_rast_plane plane[7]; /* NOTE: may allocate fewer planes */
};



struct lp_rasterizer *
lp_rast_create( unsigned num_threads );

void
lp_rast_destroy( struct lp_rasterizer * );

unsigned
lp_rast_get_num_threads( struct lp_rasterizer * );

void 
lp_rast_queue_scene( struct lp_rasterizer *rast,
                     struct lp_scene *scene );

void
lp_rast_finish( struct lp_rasterizer *rast );


union lp_rast_cmd_arg {
   const struct lp_rast_shader_inputs *shade_tile;
   struct {
      const struct lp_rast_triangle *tri;
      unsigned plane_mask;
   } triangle;
   const struct lp_rast_state *set_state;
   uint8_t clear_color[4];
   const struct lp_rast_clearzs *clear_zstencil;
   struct lp_fence *fence;
   struct llvmpipe_query *query_obj;
};


/* Cast wrappers.  Hopefully these compile to noops!
 */
static INLINE union lp_rast_cmd_arg
lp_rast_arg_inputs( const struct lp_rast_shader_inputs *shade_tile )
{
   union lp_rast_cmd_arg arg;
   arg.shade_tile = shade_tile;
   return arg;
}

static INLINE union lp_rast_cmd_arg
lp_rast_arg_triangle( const struct lp_rast_triangle *triangle,
                      unsigned plane_mask)
{
   union lp_rast_cmd_arg arg;
   arg.triangle.tri = triangle;
   arg.triangle.plane_mask = plane_mask;
   return arg;
}

static INLINE union lp_rast_cmd_arg
lp_rast_arg_state( const struct lp_rast_state *state )
{
   union lp_rast_cmd_arg arg;
   arg.set_state = state;
   return arg;
}

static INLINE union lp_rast_cmd_arg
lp_rast_arg_fence( struct lp_fence *fence )
{
   union lp_rast_cmd_arg arg;
   arg.fence = fence;
   return arg;
}


static INLINE union lp_rast_cmd_arg
lp_rast_arg_clearzs( const struct lp_rast_clearzs *clearzs )
{
   union lp_rast_cmd_arg arg;
   arg.clear_zstencil = clearzs;
   return arg;
}

static INLINE union lp_rast_cmd_arg
lp_rast_arg_null( void )
{
   union lp_rast_cmd_arg arg;
   arg.set_state = NULL;
   return arg;
}


/**
 * Binnable Commands.
 * These get put into bins by the setup code and are called when
 * the bins are executed.
 */

void lp_rast_clear_color( struct lp_rasterizer_task *, 
                          const union lp_rast_cmd_arg );

void lp_rast_clear_zstencil( struct lp_rasterizer_task *, 
                             const union lp_rast_cmd_arg );

void lp_rast_triangle_1( struct lp_rasterizer_task *, 
                         const union lp_rast_cmd_arg );
void lp_rast_triangle_2( struct lp_rasterizer_task *, 
                         const union lp_rast_cmd_arg );
void lp_rast_triangle_3( struct lp_rasterizer_task *, 
                         const union lp_rast_cmd_arg );
void lp_rast_triangle_4( struct lp_rasterizer_task *, 
                         const union lp_rast_cmd_arg );
void lp_rast_triangle_5( struct lp_rasterizer_task *, 
                         const union lp_rast_cmd_arg );
void lp_rast_triangle_6( struct lp_rasterizer_task *, 
                         const union lp_rast_cmd_arg );
void lp_rast_triangle_7( struct lp_rasterizer_task *, 
                         const union lp_rast_cmd_arg );

void lp_rast_shade_tile( struct lp_rasterizer_task *,
                         const union lp_rast_cmd_arg );

void lp_rast_shade_tile_opaque( struct lp_rasterizer_task *,
                                const union lp_rast_cmd_arg );

void lp_rast_fence( struct lp_rasterizer_task *,
                    const union lp_rast_cmd_arg );

void lp_rast_store_linear_color( struct lp_rasterizer_task *,
                          const union lp_rast_cmd_arg );


void lp_rast_begin_query(struct lp_rasterizer_task *,
                         const union lp_rast_cmd_arg );

void lp_rast_end_query(struct lp_rasterizer_task *,
                       const union lp_rast_cmd_arg );

void
lp_rast_triangle_3_16(struct lp_rasterizer_task *task,
                      const union lp_rast_cmd_arg arg);


#endif
