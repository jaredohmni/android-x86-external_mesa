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


#include "util/u_memory.h"
#include "util/u_debug.h" 
#include "util/u_debug_dump.h" 


#define DEBUG_DUMP_INVALID_NAME "<invalid>"


#if 0
static const char *
debug_dump_strip_prefix(const char *name,
                        const char *prefix) 
{
   const char *stripped;
   assert(name);
   assert(prefix);
   stripped = name;
   while(*prefix) {
      if(*stripped != *prefix)
	 return name;

      ++stripped;
      ++prefix;
   }
   return stripped;
}
#endif

static const char *
debug_dump_enum_continuous(unsigned value, 
                           unsigned num_names,
                           const char **names)
{
   if (value >= num_names)
      return DEBUG_DUMP_INVALID_NAME;
   return names[value];
}


#define DEFINE_DEBUG_DUMP_CONTINUOUS(_name) \
   const char * \
   debug_dump_##_name(unsigned value, boolean shortened) \
   { \
      if(shortened) \
         return debug_dump_enum_continuous(value, Elements(debug_dump_##_name##_short_names), debug_dump_##_name##_short_names); \
      else \
         return debug_dump_enum_continuous(value, Elements(debug_dump_##_name##_names), debug_dump_##_name##_names); \
   }


static const char *
debug_dump_blend_factor_names[] = {
   DEBUG_DUMP_INVALID_NAME, /* 0x0 */
   "PIPE_BLENDFACTOR_ONE",
   "PIPE_BLENDFACTOR_SRC_COLOR",
   "PIPE_BLENDFACTOR_SRC_ALPHA",
   "PIPE_BLENDFACTOR_DST_ALPHA",
   "PIPE_BLENDFACTOR_DST_COLOR",
   "PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE",
   "PIPE_BLENDFACTOR_CONST_COLOR",
   "PIPE_BLENDFACTOR_CONST_ALPHA",
   "PIPE_BLENDFACTOR_SRC1_COLOR",
   "PIPE_BLENDFACTOR_SRC1_ALPHA",
   DEBUG_DUMP_INVALID_NAME, /* 0x0b */
   DEBUG_DUMP_INVALID_NAME, /* 0x0c */
   DEBUG_DUMP_INVALID_NAME, /* 0x0d */
   DEBUG_DUMP_INVALID_NAME, /* 0x0e */
   DEBUG_DUMP_INVALID_NAME, /* 0x0f */
   DEBUG_DUMP_INVALID_NAME, /* 0x10 */
   "PIPE_BLENDFACTOR_ZERO",
   "PIPE_BLENDFACTOR_INV_SRC_COLOR",
   "PIPE_BLENDFACTOR_INV_SRC_ALPHA",
   "PIPE_BLENDFACTOR_INV_DST_ALPHA",
   "PIPE_BLENDFACTOR_INV_DST_COLOR",
   DEBUG_DUMP_INVALID_NAME, /* 0x16 */
   "PIPE_BLENDFACTOR_INV_CONST_COLOR",
   "PIPE_BLENDFACTOR_INV_CONST_ALPHA",
   "PIPE_BLENDFACTOR_INV_SRC1_COLOR",
   "PIPE_BLENDFACTOR_INV_SRC1_ALPHA"
};

static const char *
debug_dump_blend_factor_short_names[] = {
   DEBUG_DUMP_INVALID_NAME, /* 0x0 */
   "one",
   "src_color",
   "src_alpha",
   "dst_alpha",
   "dst_color",
   "src_alpha_saturate",
   "const_color",
   "const_alpha",
   "src1_color",
   "src1_alpha",
   DEBUG_DUMP_INVALID_NAME, /* 0x0b */
   DEBUG_DUMP_INVALID_NAME, /* 0x0c */
   DEBUG_DUMP_INVALID_NAME, /* 0x0d */
   DEBUG_DUMP_INVALID_NAME, /* 0x0e */
   DEBUG_DUMP_INVALID_NAME, /* 0x0f */
   DEBUG_DUMP_INVALID_NAME, /* 0x10 */
   "zero",
   "inv_src_color",
   "inv_src_alpha",
   "inv_dst_alpha",
   "inv_dst_color",
   DEBUG_DUMP_INVALID_NAME, /* 0x16 */
   "inv_const_color",
   "inv_const_alpha",
   "inv_src1_color",
   "inv_src1_alpha"
};

DEFINE_DEBUG_DUMP_CONTINUOUS(blend_factor)


static const char *
debug_dump_blend_func_names[] = {
   "PIPE_BLEND_ADD",
   "PIPE_BLEND_SUBTRACT",
   "PIPE_BLEND_REVERSE_SUBTRACT",
   "PIPE_BLEND_MIN",
   "PIPE_BLEND_MAX"
};

static const char *
debug_dump_blend_func_short_names[] = {
   "add",
   "sub",
   "rev_sub",
   "min",
   "max"
};

DEFINE_DEBUG_DUMP_CONTINUOUS(blend_func)


static const char *
debug_dump_func_names[] = {
   "PIPE_FUNC_NEVER",
   "PIPE_FUNC_LESS",
   "PIPE_FUNC_EQUAL",
   "PIPE_FUNC_LEQUAL",
   "PIPE_FUNC_GREATER",
   "PIPE_FUNC_NOTEQUAL",
   "PIPE_FUNC_GEQUAL",
   "PIPE_FUNC_ALWAYS"
};

static const char *
debug_dump_func_short_names[] = {
   "never",
   "less",
   "equal",
   "less_equal",
   "greater",
   "not_equal",
   "greater_equal",
   "always"
};

DEFINE_DEBUG_DUMP_CONTINUOUS(func)
