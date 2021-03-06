/**************************************************************************
 * 
 * Copyright 2007 Tungsten Graphics, Inc., Cedar Park, Texas.
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
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

#ifndef SP_TILE_CACHE_H
#define SP_TILE_CACHE_H

#define TILE_CLEAR_OPTIMIZATION 1


#include "pipe/p_compiler.h"


struct softpipe_context;
struct softpipe_tile_cache;


/**
 * Cache tile size (width and height). This needs to be a power of two.
 */
#define TILE_SIZE 64



struct softpipe_cached_tile
{
   int x, y;           /**< pos of tile in window coords */
   int z, face, level; /**< Extra texture indexes */
   union {
      float color[TILE_SIZE][TILE_SIZE][4];
      uint color32[TILE_SIZE][TILE_SIZE];
      uint depth32[TILE_SIZE][TILE_SIZE];
      ushort depth16[TILE_SIZE][TILE_SIZE];
      ubyte stencil8[TILE_SIZE][TILE_SIZE];
      ubyte any[1];
   } data;
};


extern struct softpipe_tile_cache *
sp_create_tile_cache( struct pipe_screen *screen );

extern void
sp_destroy_tile_cache(struct softpipe_tile_cache *tc);

extern void
sp_tile_cache_set_surface(struct softpipe_tile_cache *tc,
                          struct pipe_surface *sps);

extern struct pipe_surface *
sp_tile_cache_get_surface(struct softpipe_tile_cache *tc);

extern void
sp_tile_cache_map_transfers(struct softpipe_tile_cache *tc);

extern void
sp_tile_cache_unmap_transfers(struct softpipe_tile_cache *tc);

extern void
sp_tile_cache_set_texture(struct pipe_context *pipe,
                          struct softpipe_tile_cache *tc,
                          struct pipe_texture *texture);

extern void
sp_flush_tile_cache(struct softpipe_context *softpipe,
                    struct softpipe_tile_cache *tc);

extern void
sp_tile_cache_clear(struct softpipe_tile_cache *tc, const float *rgba,
                    uint clearValue);

extern struct softpipe_cached_tile *
sp_get_cached_tile(struct softpipe_context *softpipe,
                   struct softpipe_tile_cache *tc, int x, int y);

extern const struct softpipe_cached_tile *
sp_get_cached_tile_tex(struct softpipe_context *softpipe,
                       struct softpipe_tile_cache *tc, int x, int y, int z,
                       int face, int level);


#endif /* SP_TILE_CACHE_H */

