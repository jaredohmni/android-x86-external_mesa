/**************************************************************************
 * 
 * Copyright 2009 VMware, Inc.
 * Copyright 2007-2008 Tungsten Graphics, Inc., Cedar Park, Texas.
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

/**
 * @file
 * TGSI to LLVM IR translation -- SoA.
 *
 * @author Jose Fonseca <jfonseca@vmware.com>
 *
 * Based on tgsi_sse2.c code written by Michal Krol, Keith Whitwell,
 * Brian Paul, and others.
 */

#include "pipe/p_config.h"
#include "pipe/p_shader_tokens.h"
#include "util/u_debug.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "tgsi/tgsi_info.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_util.h"
#include "tgsi/tgsi_exec.h"
#include "lp_bld_type.h"
#include "lp_bld_const.h"
#include "lp_bld_intr.h"
#include "lp_bld_arit.h"
#include "lp_bld_logic.h"
#include "lp_bld_swizzle.h"
#include "lp_bld_flow.h"
#include "lp_bld_tgsi.h"
#include "lp_bld_debug.h"


#define LP_MAX_TEMPS 256
#define LP_MAX_IMMEDIATES 256


#define FOR_EACH_CHANNEL( CHAN )\
   for (CHAN = 0; CHAN < NUM_CHANNELS; CHAN++)

#define IS_DST0_CHANNEL_ENABLED( INST, CHAN )\
   ((INST)->FullDstRegisters[0].DstRegister.WriteMask & (1 << (CHAN)))

#define IF_IS_DST0_CHANNEL_ENABLED( INST, CHAN )\
   if (IS_DST0_CHANNEL_ENABLED( INST, CHAN ))

#define FOR_EACH_DST0_ENABLED_CHANNEL( INST, CHAN )\
   FOR_EACH_CHANNEL( CHAN )\
      IF_IS_DST0_CHANNEL_ENABLED( INST, CHAN )

#define CHAN_X 0
#define CHAN_Y 1
#define CHAN_Z 2
#define CHAN_W 3


struct lp_build_tgsi_soa_context
{
   struct lp_build_context base;

   LLVMValueRef consts_ptr;
   const LLVMValueRef *pos;
   const LLVMValueRef (*inputs)[NUM_CHANNELS];
   LLVMValueRef (*outputs)[NUM_CHANNELS];

   struct lp_build_sampler_soa *sampler;

   LLVMValueRef immediates[LP_MAX_IMMEDIATES][NUM_CHANNELS];
   LLVMValueRef temps[LP_MAX_TEMPS][NUM_CHANNELS];

   struct lp_build_mask_context *mask;
};


/**
 * Register fetch.
 */
static LLVMValueRef
emit_fetch(
   struct lp_build_tgsi_soa_context *bld,
   const struct tgsi_full_instruction *inst,
   unsigned index,
   const unsigned chan_index )
{
   const struct tgsi_full_src_register *reg = &inst->FullSrcRegisters[index];
   unsigned swizzle = tgsi_util_get_full_src_register_extswizzle( reg, chan_index );
   LLVMValueRef res;

   switch (swizzle) {
   case TGSI_EXTSWIZZLE_X:
   case TGSI_EXTSWIZZLE_Y:
   case TGSI_EXTSWIZZLE_Z:
   case TGSI_EXTSWIZZLE_W:

      switch (reg->SrcRegister.File) {
      case TGSI_FILE_CONSTANT: {
         LLVMValueRef index = LLVMConstInt(LLVMInt32Type(), reg->SrcRegister.Index*4 + swizzle, 0);
         LLVMValueRef scalar_ptr = LLVMBuildGEP(bld->base.builder, bld->consts_ptr, &index, 1, "");
         LLVMValueRef scalar = LLVMBuildLoad(bld->base.builder, scalar_ptr, "");
         res = lp_build_broadcast_scalar(&bld->base, scalar);
         break;
      }

      case TGSI_FILE_IMMEDIATE:
         res = bld->immediates[reg->SrcRegister.Index][swizzle];
         assert(res);
         break;

      case TGSI_FILE_INPUT:
         res = bld->inputs[reg->SrcRegister.Index][swizzle];
         assert(res);
         break;

      case TGSI_FILE_TEMPORARY:
         res = bld->temps[reg->SrcRegister.Index][swizzle];
         if(!res)
            return bld->base.undef;
         break;

      default:
         assert( 0 );
         return bld->base.undef;
      }
      break;

   case TGSI_EXTSWIZZLE_ZERO:
      res = bld->base.zero;
      break;

   case TGSI_EXTSWIZZLE_ONE:
      res = bld->base.one;
      break;

   default:
      assert( 0 );
      return bld->base.undef;
   }

   switch( tgsi_util_get_full_src_register_sign_mode( reg, chan_index ) ) {
   case TGSI_UTIL_SIGN_CLEAR:
      res = lp_build_abs( &bld->base, res );
      break;

   case TGSI_UTIL_SIGN_SET:
      /* TODO: Use bitwese OR for floating point */
      res = lp_build_abs( &bld->base, res );
      res = LLVMBuildNeg( bld->base.builder, res, "" );
      break;

   case TGSI_UTIL_SIGN_TOGGLE:
      res = LLVMBuildNeg( bld->base.builder, res, "" );
      break;

   case TGSI_UTIL_SIGN_KEEP:
      break;
   }

   return res;
}


/**
 * Register store.
 */
static void
emit_store(
   struct lp_build_tgsi_soa_context *bld,
   const struct tgsi_full_instruction *inst,
   unsigned index,
   unsigned chan_index,
   LLVMValueRef value)
{
   const struct tgsi_full_dst_register *reg = &inst->FullDstRegisters[index];

   switch( inst->Instruction.Saturate ) {
   case TGSI_SAT_NONE:
      break;

   case TGSI_SAT_ZERO_ONE:
      value = lp_build_max(&bld->base, value, bld->base.zero);
      value = lp_build_min(&bld->base, value, bld->base.one);
      break;

   case TGSI_SAT_MINUS_PLUS_ONE:
      value = lp_build_max(&bld->base, value, lp_build_const_scalar(bld->base.type, -1.0));
      value = lp_build_min(&bld->base, value, bld->base.one);
      break;

   default:
      assert(0);
   }

   switch( reg->DstRegister.File ) {
   case TGSI_FILE_OUTPUT:
      bld->outputs[reg->DstRegister.Index][chan_index] = value;
      break;

   case TGSI_FILE_TEMPORARY:
      bld->temps[reg->DstRegister.Index][chan_index] = value;
      break;

   case TGSI_FILE_ADDRESS:
      /* FIXME */
      assert(0);
      break;

   default:
      assert( 0 );
   }
}


/**
 * High-level instruction translators.
 */

static void
emit_tex( struct lp_build_tgsi_soa_context *bld,
          const struct tgsi_full_instruction *inst,
          boolean apply_lodbias,
          boolean projected)
{
   const uint unit = inst->FullSrcRegisters[1].SrcRegister.Index;
   LLVMValueRef lodbias;
   LLVMValueRef oow;
   LLVMValueRef coords[3];
   LLVMValueRef texel[4];
   unsigned num_coords;
   unsigned i;

   switch (inst->InstructionExtTexture.Texture) {
   case TGSI_TEXTURE_1D:
      num_coords = 1;
      break;
   case TGSI_TEXTURE_2D:
   case TGSI_TEXTURE_RECT:
      num_coords = 2;
      break;
   case TGSI_TEXTURE_SHADOW1D:
   case TGSI_TEXTURE_SHADOW2D:
   case TGSI_TEXTURE_SHADOWRECT:
   case TGSI_TEXTURE_3D:
   case TGSI_TEXTURE_CUBE:
      num_coords = 3;
      break;
   default:
      assert(0);
      return;
   }

   if(apply_lodbias)
      lodbias = emit_fetch( bld, inst, 0, 3 );
   else
      lodbias = bld->base.zero;

   if (projected) {
      oow = emit_fetch( bld, inst, 0, 3 );
      oow = lp_build_rcp(&bld->base, oow);
   }

   for (i = 0; i < num_coords; i++) {
      coords[i] = emit_fetch( bld, inst, 0, i );
      if (projected)
         coords[i] = lp_build_mul(&bld->base, coords[i], oow);
   }

   bld->sampler->emit_fetch_texel(bld->sampler,
                                  bld->base.builder,
                                  bld->base.type,
                                  unit, num_coords, coords, lodbias,
                                  texel);

   FOR_EACH_DST0_ENABLED_CHANNEL( inst, i ) {
      emit_store( bld, inst, 0, i, texel[i] );
   }
}


static void
emit_kil(
   struct lp_build_tgsi_soa_context *bld,
   const struct tgsi_full_instruction *inst )
{
   const struct tgsi_full_src_register *reg = &inst->FullSrcRegisters[0];
   LLVMValueRef terms[NUM_CHANNELS];
   LLVMValueRef mask;
   unsigned chan_index;

   memset(&terms, 0, sizeof terms);

   FOR_EACH_CHANNEL( chan_index ) {
      unsigned swizzle;

      /* Unswizzle channel */
      swizzle = tgsi_util_get_full_src_register_extswizzle( reg, chan_index );

      /* Note that we test if the value is less than zero, so 1.0 and 0.0 need
       * not to be tested. */
      if(swizzle == TGSI_EXTSWIZZLE_ZERO || swizzle == TGSI_EXTSWIZZLE_ONE)
         continue;

      /* Check if the component has not been already tested. */
      assert(swizzle < NUM_CHANNELS);
      if( !terms[swizzle] )
         /* TODO: change the comparison operator instead of setting the sign */
         terms[swizzle] =  emit_fetch(bld, inst, 0, chan_index );
   }

   mask = NULL;
   FOR_EACH_CHANNEL( chan_index ) {
      if(terms[chan_index]) {
         LLVMValueRef chan_mask;

         chan_mask = lp_build_cmp(&bld->base, PIPE_FUNC_GEQUAL, terms[chan_index], bld->base.zero);

         if(mask)
            mask = LLVMBuildAnd(bld->base.builder, mask, chan_mask, "");
         else
            mask = chan_mask;
      }
   }

   if(mask)
      lp_build_mask_update(bld->mask, mask);
}


/**
 * Check if inst src/dest regs use indirect addressing into temporary
 * register file.
 */
static boolean
indirect_temp_reference(const struct tgsi_full_instruction *inst)
{
   uint i;
   for (i = 0; i < inst->Instruction.NumSrcRegs; i++) {
      const struct tgsi_full_src_register *reg = &inst->FullSrcRegisters[i];
      if (reg->SrcRegister.File == TGSI_FILE_TEMPORARY &&
          reg->SrcRegister.Indirect)
         return TRUE;
   }
   for (i = 0; i < inst->Instruction.NumDstRegs; i++) {
      const struct tgsi_full_dst_register *reg = &inst->FullDstRegisters[i];
      if (reg->DstRegister.File == TGSI_FILE_TEMPORARY &&
          reg->DstRegister.Indirect)
         return TRUE;
   }
   return FALSE;
}


static int
emit_instruction(
   struct lp_build_tgsi_soa_context *bld,
   struct tgsi_full_instruction *inst )
{
   unsigned chan_index;
   LLVMValueRef src0, src1, src2;
   LLVMValueRef tmp0, tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7;
   LLVMValueRef dst0;

   /* we can't handle indirect addressing into temp register file yet */
   if (indirect_temp_reference(inst))
      return FALSE;

   switch (inst->Instruction.Opcode) {
#if 0
   case TGSI_OPCODE_ARL:
      /* FIXME */
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         emit_flr(bld, 0, 0);
         emit_f2it( bld, 0 );
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;
#endif

   case TGSI_OPCODE_MOV:
   case TGSI_OPCODE_SWZ:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_LIT:
      if( IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ) ) {
         emit_store( bld, inst, 0, CHAN_X, bld->base.one);
      }
      if( IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ) ) {
         src0 = emit_fetch( bld, inst, 0, CHAN_X );
         dst0 = lp_build_max( &bld->base, src0, bld->base.zero);
         emit_store( bld, inst, 0, CHAN_Y, dst0);
      }
      if( IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z ) ) {
         /* XMM[1] = SrcReg[0].yyyy */
         tmp1 = emit_fetch( bld, inst, 0, CHAN_Y );
         /* XMM[1] = max(XMM[1], 0) */
         tmp1 = lp_build_max( &bld->base, tmp1, bld->base.zero);
         /* XMM[2] = SrcReg[0].wwww */
         tmp2 = emit_fetch( bld, inst, 0, CHAN_W );
         tmp1 = lp_build_pow( &bld->base, tmp1, tmp2);
         tmp0 = emit_fetch( bld, inst, 0, CHAN_X );
         tmp2 = lp_build_cmp(&bld->base, PIPE_FUNC_GREATER, tmp0, bld->base.zero);
         dst0 = lp_build_select(&bld->base, tmp2, tmp1, bld->base.zero);
         emit_store( bld, inst, 0, CHAN_Z, dst0);
      }
      if( IS_DST0_CHANNEL_ENABLED( inst, CHAN_W ) ) {
         emit_store( bld, inst, 0, CHAN_W, bld->base.one);
      }
      break;

   case TGSI_OPCODE_RCP:
   /* TGSI_OPCODE_RECIP */
      src0 = emit_fetch( bld, inst, 0, CHAN_X );
      dst0 = lp_build_rcp(&bld->base, src0);
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, dst0 );
      }
      break;

   case TGSI_OPCODE_RSQ:
   /* TGSI_OPCODE_RECIPSQRT */
      src0 = emit_fetch( bld, inst, 0, CHAN_X );
      src0 = lp_build_abs(&bld->base, src0);
      dst0 = lp_build_rsqrt(&bld->base, src0);
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, dst0 );
      }
      break;

   case TGSI_OPCODE_EXP:
      if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ) ||
          IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ) ||
          IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z )) {
         LLVMValueRef *p_exp2_int_part = NULL;
         LLVMValueRef *p_frac_part = NULL;
         LLVMValueRef *p_exp2 = NULL;

         src0 = emit_fetch( bld, inst, 0, CHAN_X );

         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ))
            p_exp2_int_part = &tmp0;
         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ))
            p_frac_part = &tmp1;
         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z ))
            p_exp2 = &tmp2;

         lp_build_exp2_approx(&bld->base, src0, p_exp2_int_part, p_frac_part, p_exp2);

         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ))
            emit_store( bld, inst, 0, CHAN_X, tmp0);
         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ))
            emit_store( bld, inst, 0, CHAN_Y, tmp1);
         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z ))
            emit_store( bld, inst, 0, CHAN_Z, tmp2);
      }
      /* dst.w = 1.0 */
      if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_W )) {
         tmp0 = bld->base.one;
         emit_store( bld, inst, 0, CHAN_W, tmp0);
      }
      break;

   case TGSI_OPCODE_LOG:
      if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ) ||
          IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ) ||
          IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z )) {
         LLVMValueRef *p_floor_log2;
         LLVMValueRef *p_exp;
         LLVMValueRef *p_log2;

         src0 = emit_fetch( bld, inst, 0, CHAN_X );
         src0 = lp_build_abs( &bld->base, src0 );

         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ))
            p_floor_log2 = &tmp0;
         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ))
            p_exp = &tmp1;
         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z ))
            p_log2 = &tmp2;

         lp_build_log2_approx(&bld->base, src0, p_exp, p_floor_log2, p_log2);

         /* dst.x = floor(lg2(abs(src.x))) */
         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ))
            emit_store( bld, inst, 0, CHAN_X, tmp0);
         /* dst.y = abs(src)/ex2(floor(lg2(abs(src.x)))) */
         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y )) {
            tmp1 = lp_build_div( &bld->base, src0, tmp1);
            emit_store( bld, inst, 0, CHAN_Y, tmp1);
         }
         /* dst.z = lg2(abs(src.x)) */
         if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z ))
            emit_store( bld, inst, 0, CHAN_Z, tmp2);
      }
      /* dst.w = 1.0 */
      if (IS_DST0_CHANNEL_ENABLED( inst, CHAN_W )) {
         tmp0 = bld->base.one;
         emit_store( bld, inst, 0, CHAN_W, tmp0);
      }
      break;

   case TGSI_OPCODE_MUL:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         dst0 = lp_build_mul(&bld->base, src0, src1);
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_ADD:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         dst0 = lp_build_add(&bld->base, src0, src1);
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_DP3:
   /* TGSI_OPCODE_DOT3 */
      tmp0 = emit_fetch( bld, inst, 0, CHAN_X );
      tmp1 = emit_fetch( bld, inst, 1, CHAN_X );
      tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);
      tmp1 = emit_fetch( bld, inst, 0, CHAN_Y );
      tmp2 = emit_fetch( bld, inst, 1, CHAN_Y );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      tmp1 = emit_fetch( bld, inst, 0, CHAN_Z );
      tmp2 = emit_fetch( bld, inst, 1, CHAN_Z );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_DP4:
   /* TGSI_OPCODE_DOT4 */
      tmp0 = emit_fetch( bld, inst, 0, CHAN_X );
      tmp1 = emit_fetch( bld, inst, 1, CHAN_X );
      tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);
      tmp1 = emit_fetch( bld, inst, 0, CHAN_Y );
      tmp2 = emit_fetch( bld, inst, 1, CHAN_Y );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      tmp1 = emit_fetch( bld, inst, 0, CHAN_Z );
      tmp2 = emit_fetch( bld, inst, 1, CHAN_Z );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      tmp1 = emit_fetch( bld, inst, 0, CHAN_W );
      tmp2 = emit_fetch( bld, inst, 1, CHAN_W );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_DST:
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ) {
         tmp0 = bld->base.one;
         emit_store( bld, inst, 0, CHAN_X, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ) {
         tmp0 = emit_fetch( bld, inst, 0, CHAN_Y );
         tmp1 = emit_fetch( bld, inst, 1, CHAN_Y );
         tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);
         emit_store( bld, inst, 0, CHAN_Y, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z ) {
         tmp0 = emit_fetch( bld, inst, 0, CHAN_Z );
         emit_store( bld, inst, 0, CHAN_Z, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_W ) {
         tmp0 = emit_fetch( bld, inst, 1, CHAN_W );
         emit_store( bld, inst, 0, CHAN_W, tmp0);
      }
      break;

   case TGSI_OPCODE_MIN:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         dst0 = lp_build_min( &bld->base, src0, src1 );
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_MAX:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         dst0 = lp_build_max( &bld->base, src0, src1 );
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_SLT:
   /* TGSI_OPCODE_SETLT */
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         tmp0 = lp_build_cmp( &bld->base, PIPE_FUNC_LESS, src0, src1 );
         dst0 = lp_build_select( &bld->base, tmp0, bld->base.one, bld->base.zero );
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_SGE:
   /* TGSI_OPCODE_SETGE */
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         tmp0 = lp_build_cmp( &bld->base, PIPE_FUNC_GEQUAL, src0, src1 );
         dst0 = lp_build_select( &bld->base, tmp0, bld->base.one, bld->base.zero );
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_MAD:
   /* TGSI_OPCODE_MADD */
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         tmp1 = emit_fetch( bld, inst, 1, chan_index );
         tmp2 = emit_fetch( bld, inst, 2, chan_index );
         tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);
         tmp0 = lp_build_add( &bld->base, tmp0, tmp2);
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_SUB:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         tmp1 = emit_fetch( bld, inst, 1, chan_index );
         tmp0 = lp_build_sub( &bld->base, tmp0, tmp1);
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_LRP:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         src2 = emit_fetch( bld, inst, 2, chan_index );
         tmp0 = lp_build_sub( &bld->base, src1, src2 );
         tmp0 = lp_build_mul( &bld->base, src0, tmp0 );
         dst0 = lp_build_add( &bld->base, tmp0, src2 );
         emit_store( bld, inst, 0, chan_index, dst0 );
      }
      break;

   case TGSI_OPCODE_CND:
      /* FIXME */
      return 0;
      break;

   case TGSI_OPCODE_DP2A:
      tmp0 = emit_fetch( bld, inst, 0, CHAN_X );  /* xmm0 = src[0].x */
      tmp1 = emit_fetch( bld, inst, 1, CHAN_X );  /* xmm1 = src[1].x */
      tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);              /* xmm0 = xmm0 * xmm1 */
      tmp1 = emit_fetch( bld, inst, 0, CHAN_Y );  /* xmm1 = src[0].y */
      tmp2 = emit_fetch( bld, inst, 1, CHAN_Y );  /* xmm2 = src[1].y */
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);              /* xmm1 = xmm1 * xmm2 */
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);              /* xmm0 = xmm0 + xmm1 */
      tmp1 = emit_fetch( bld, inst, 2, CHAN_X );  /* xmm1 = src[2].x */
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);              /* xmm0 = xmm0 + xmm1 */
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, tmp0);  /* dest[ch] = xmm0 */
      }
      break;

#if 0
   case TGSI_OPCODE_FRC:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         emit_frc( bld, 0, 0 );
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_CLAMP:
      return 0;
      break;

   case TGSI_OPCODE_FLR:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         emit_flr( bld, 0, 0 );
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_ROUND:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         emit_rnd( bld, 0, 0 );
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;
#endif

   case TGSI_OPCODE_EX2: {
      tmp0 = emit_fetch( bld, inst, 0, CHAN_X );
      tmp0 = lp_build_exp2( &bld->base, tmp0);
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;
   }

   case TGSI_OPCODE_LG2:
      tmp0 = emit_fetch( bld, inst, 0, CHAN_X );
      tmp0 = lp_build_log2( &bld->base, tmp0);
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_POW:
      src0 = emit_fetch( bld, inst, 0, CHAN_X );
      src1 = emit_fetch( bld, inst, 1, CHAN_X );
      dst0 = lp_build_pow( &bld->base, src0, src1 );
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, dst0 );
      }
      break;

   case TGSI_OPCODE_XPD:
      if( IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ) ||
          IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ) ) {
         tmp1 = emit_fetch( bld, inst, 1, CHAN_Z );
         tmp3 = emit_fetch( bld, inst, 0, CHAN_Z );
      }
      if( IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ) ||
          IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z ) ) {
         tmp0 = emit_fetch( bld, inst, 0, CHAN_Y );
         tmp4 = emit_fetch( bld, inst, 1, CHAN_Y );
      }
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ) {
         tmp2 = tmp0;
         tmp2 = lp_build_mul( &bld->base, tmp2, tmp1);
         tmp5 = tmp3;
         tmp5 = lp_build_mul( &bld->base, tmp5, tmp4);
         tmp2 = lp_build_sub( &bld->base, tmp2, tmp5);
         emit_store( bld, inst, 0, CHAN_X, tmp2);
      }
      if( IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ) ||
          IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z ) ) {
         tmp2 = emit_fetch( bld, inst, 1, CHAN_X );
         tmp5 = emit_fetch( bld, inst, 0, CHAN_X );
      }
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ) {
         tmp3 = lp_build_mul( &bld->base, tmp3, tmp2);
         tmp1 = lp_build_mul( &bld->base, tmp1, tmp5);
         tmp3 = lp_build_sub( &bld->base, tmp3, tmp1);
         emit_store( bld, inst, 0, CHAN_Y, tmp3);
      }
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z ) {
         tmp5 = lp_build_mul( &bld->base, tmp5, tmp4);
         tmp0 = lp_build_mul( &bld->base, tmp0, tmp2);
         tmp5 = lp_build_sub( &bld->base, tmp5, tmp0);
         emit_store( bld, inst, 0, CHAN_Z, tmp5);
      }
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_W ) {
         tmp0 = bld->base.one;
         emit_store( bld, inst, 0, CHAN_W, tmp0);
      }
      break;

   case TGSI_OPCODE_ABS:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         tmp0 = lp_build_abs( &bld->base, tmp0 ) ;
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_RCC:
      return 0;
      break;

   case TGSI_OPCODE_DPH:
      tmp0 = emit_fetch( bld, inst, 0, CHAN_X );
      tmp1 = emit_fetch( bld, inst, 1, CHAN_X );
      tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);
      tmp1 = emit_fetch( bld, inst, 0, CHAN_Y );
      tmp2 = emit_fetch( bld, inst, 1, CHAN_Y );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      tmp1 = emit_fetch( bld, inst, 0, CHAN_Z );
      tmp2 = emit_fetch( bld, inst, 1, CHAN_Z );
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      tmp1 = emit_fetch( bld, inst, 1, CHAN_W );
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_COS:
      tmp0 = emit_fetch( bld, inst, 0, CHAN_X );
      tmp0 = lp_build_cos( &bld->base, tmp0 );
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_DDX:
      return 0;
      break;

   case TGSI_OPCODE_DDY:
      return 0;
      break;

   case TGSI_OPCODE_KILP:
      /* predicated kill */
      /* FIXME */
      return 0;
      break;

   case TGSI_OPCODE_KIL:
      /* conditional kill */
      emit_kil( bld, inst );
      break;

   case TGSI_OPCODE_PK2H:
      return 0;
      break;

   case TGSI_OPCODE_PK2US:
      return 0;
      break;

   case TGSI_OPCODE_PK4B:
      return 0;
      break;

   case TGSI_OPCODE_PK4UB:
      return 0;
      break;

   case TGSI_OPCODE_RFL:
      return 0;
      break;

   case TGSI_OPCODE_SEQ:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         tmp0 = lp_build_cmp( &bld->base, PIPE_FUNC_EQUAL, src0, src1 );
         dst0 = lp_build_select( &bld->base, tmp0, bld->base.one, bld->base.zero );
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_SFL:
      return 0;
      break;

   case TGSI_OPCODE_SGT:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         tmp0 = lp_build_cmp( &bld->base, PIPE_FUNC_GREATER, src0, src1 );
         dst0 = lp_build_select( &bld->base, tmp0, bld->base.one, bld->base.zero );
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_SIN:
      tmp0 = emit_fetch( bld, inst, 0, CHAN_X );
      tmp0 = lp_build_sin( &bld->base, tmp0 );
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;

   case TGSI_OPCODE_SLE:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         tmp0 = lp_build_cmp( &bld->base, PIPE_FUNC_LEQUAL, src0, src1 );
         dst0 = lp_build_select( &bld->base, tmp0, bld->base.one, bld->base.zero );
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_SNE:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         tmp0 = lp_build_cmp( &bld->base, PIPE_FUNC_NOTEQUAL, src0, src1 );
         dst0 = lp_build_select( &bld->base, tmp0, bld->base.one, bld->base.zero );
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_STR:
      return 0;
      break;

   case TGSI_OPCODE_TEX:
      emit_tex( bld, inst, FALSE, FALSE );
      break;

   case TGSI_OPCODE_TXD:
      return 0;
      break;

   case TGSI_OPCODE_UP2H:
      return 0;
      break;

   case TGSI_OPCODE_UP2US:
      return 0;
      break;

   case TGSI_OPCODE_UP4B:
      return 0;
      break;

   case TGSI_OPCODE_UP4UB:
      return 0;
      break;

   case TGSI_OPCODE_X2D:
      return 0;
      break;

   case TGSI_OPCODE_ARA:
      return 0;
      break;

#if 0
   case TGSI_OPCODE_ARR:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         emit_rnd( bld, 0, 0 );
         emit_f2it( bld, 0 );
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;
#endif

   case TGSI_OPCODE_BRA:
      return 0;
      break;

   case TGSI_OPCODE_CAL:
      return 0;
      break;

#if 0
   case TGSI_OPCODE_RET:
      emit_ret( bld );
      break;
#endif

   case TGSI_OPCODE_END:
      break;

#if 0
   case TGSI_OPCODE_SSG:
   /* TGSI_OPCODE_SGN */
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         emit_sgn( bld, 0, 0 );
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;
#endif

   case TGSI_OPCODE_CMP:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         src0 = emit_fetch( bld, inst, 0, chan_index );
         src1 = emit_fetch( bld, inst, 1, chan_index );
         src2 = emit_fetch( bld, inst, 2, chan_index );
         tmp0 = lp_build_cmp( &bld->base, PIPE_FUNC_LESS, src0, bld->base.zero );
         dst0 = lp_build_select( &bld->base, tmp0, src1, src2);
         emit_store( bld, inst, 0, chan_index, dst0);
      }
      break;

   case TGSI_OPCODE_SCS:
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_X ) {
         tmp0 = emit_fetch( bld, inst, 0, CHAN_X );
         tmp0 = lp_build_cos( &bld->base, tmp0 );
         emit_store( bld, inst, 0, CHAN_X, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_Y ) {
         tmp0 = emit_fetch( bld, inst, 0, CHAN_X );
         tmp0 = lp_build_sin( &bld->base, tmp0 );
         emit_store( bld, inst, 0, CHAN_Y, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_Z ) {
         tmp0 = bld->base.zero;
         emit_store( bld, inst, 0, CHAN_Z, tmp0);
      }
      IF_IS_DST0_CHANNEL_ENABLED( inst, CHAN_W ) {
         tmp0 = bld->base.one;
         emit_store( bld, inst, 0, CHAN_W, tmp0);
      }
      break;

   case TGSI_OPCODE_TXB:
      emit_tex( bld, inst, TRUE, FALSE );
      break;

   case TGSI_OPCODE_NRM:
      /* fall-through */
   case TGSI_OPCODE_NRM4:
      /* 3 or 4-component normalization */
      {
         uint dims = (inst->Instruction.Opcode == TGSI_OPCODE_NRM) ? 3 : 4;

         if (IS_DST0_CHANNEL_ENABLED(inst, CHAN_X) ||
             IS_DST0_CHANNEL_ENABLED(inst, CHAN_Y) ||
             IS_DST0_CHANNEL_ENABLED(inst, CHAN_Z) ||
             (IS_DST0_CHANNEL_ENABLED(inst, CHAN_W) && dims == 4)) {

            /* NOTE: Cannot use xmm regs 2/3 here (see emit_rsqrt() above). */

            /* xmm4 = src.x */
            /* xmm0 = src.x * src.x */
            tmp0 = emit_fetch(bld, inst, 0, CHAN_X);
            if (IS_DST0_CHANNEL_ENABLED(inst, CHAN_X)) {
               tmp4 = tmp0;
            }
            tmp0 = lp_build_mul( &bld->base, tmp0, tmp0);

            /* xmm5 = src.y */
            /* xmm0 = xmm0 + src.y * src.y */
            tmp1 = emit_fetch(bld, inst, 0, CHAN_Y);
            if (IS_DST0_CHANNEL_ENABLED(inst, CHAN_Y)) {
               tmp5 = tmp1;
            }
            tmp1 = lp_build_mul( &bld->base, tmp1, tmp1);
            tmp0 = lp_build_add( &bld->base, tmp0, tmp1);

            /* xmm6 = src.z */
            /* xmm0 = xmm0 + src.z * src.z */
            tmp1 = emit_fetch(bld, inst, 0, CHAN_Z);
            if (IS_DST0_CHANNEL_ENABLED(inst, CHAN_Z)) {
               tmp6 = tmp1;
            }
            tmp1 = lp_build_mul( &bld->base, tmp1, tmp1);
            tmp0 = lp_build_add( &bld->base, tmp0, tmp1);

            if (dims == 4) {
               /* xmm7 = src.w */
               /* xmm0 = xmm0 + src.w * src.w */
               tmp1 = emit_fetch(bld, inst, 0, CHAN_W);
               if (IS_DST0_CHANNEL_ENABLED(inst, CHAN_W)) {
                  tmp7 = tmp1;
               }
               tmp1 = lp_build_mul( &bld->base, tmp1, tmp1);
               tmp0 = lp_build_add( &bld->base, tmp0, tmp1);
            }

            /* xmm1 = 1 / sqrt(xmm0) */
            tmp1 = lp_build_rsqrt( &bld->base, tmp0);

            /* dst.x = xmm1 * src.x */
            if (IS_DST0_CHANNEL_ENABLED(inst, CHAN_X)) {
               tmp4 = lp_build_mul( &bld->base, tmp4, tmp1);
               emit_store(bld, inst, 0, CHAN_X, tmp4);
            }

            /* dst.y = xmm1 * src.y */
            if (IS_DST0_CHANNEL_ENABLED(inst, CHAN_Y)) {
               tmp5 = lp_build_mul( &bld->base, tmp5, tmp1);
               emit_store(bld, inst, 0, CHAN_Y, tmp5);
            }

            /* dst.z = xmm1 * src.z */
            if (IS_DST0_CHANNEL_ENABLED(inst, CHAN_Z)) {
               tmp6 = lp_build_mul( &bld->base, tmp6, tmp1);
               emit_store(bld, inst, 0, CHAN_Z, tmp6);
            }

            /* dst.w = xmm1 * src.w */
            if (IS_DST0_CHANNEL_ENABLED(inst, CHAN_X) && dims == 4) {
               tmp7 = lp_build_mul( &bld->base, tmp7, tmp1);
               emit_store(bld, inst, 0, CHAN_W, tmp7);
            }
         }

         /* dst0.w = 1.0 */
         if (IS_DST0_CHANNEL_ENABLED(inst, CHAN_W) && dims == 3) {
            tmp0 = bld->base.one;
            emit_store(bld, inst, 0, CHAN_W, tmp0);
         }
      }
      break;

   case TGSI_OPCODE_DIV:
      return 0;
      break;

   case TGSI_OPCODE_DP2:
      tmp0 = emit_fetch( bld, inst, 0, CHAN_X );  /* xmm0 = src[0].x */
      tmp1 = emit_fetch( bld, inst, 1, CHAN_X );  /* xmm1 = src[1].x */
      tmp0 = lp_build_mul( &bld->base, tmp0, tmp1);              /* xmm0 = xmm0 * xmm1 */
      tmp1 = emit_fetch( bld, inst, 0, CHAN_Y );  /* xmm1 = src[0].y */
      tmp2 = emit_fetch( bld, inst, 1, CHAN_Y );  /* xmm2 = src[1].y */
      tmp1 = lp_build_mul( &bld->base, tmp1, tmp2);              /* xmm1 = xmm1 * xmm2 */
      tmp0 = lp_build_add( &bld->base, tmp0, tmp1);              /* xmm0 = xmm0 + xmm1 */
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         emit_store( bld, inst, 0, chan_index, tmp0);  /* dest[ch] = xmm0 */
      }
      break;

   case TGSI_OPCODE_TXL:
      emit_tex( bld, inst, TRUE, FALSE );
      break;

   case TGSI_OPCODE_TXP:
      emit_tex( bld, inst, FALSE, TRUE );
      break;
      
   case TGSI_OPCODE_BRK:
      return 0;
      break;

   case TGSI_OPCODE_IF:
      return 0;
      break;

   case TGSI_OPCODE_BGNFOR:
      return 0;
      break;

   case TGSI_OPCODE_REP:
      return 0;
      break;

   case TGSI_OPCODE_ELSE:
      return 0;
      break;

   case TGSI_OPCODE_ENDIF:
      return 0;
      break;

   case TGSI_OPCODE_ENDFOR:
      return 0;
      break;

   case TGSI_OPCODE_ENDREP:
      return 0;
      break;

   case TGSI_OPCODE_PUSHA:
      return 0;
      break;

   case TGSI_OPCODE_POPA:
      return 0;
      break;

   case TGSI_OPCODE_CEIL:
      return 0;
      break;

   case TGSI_OPCODE_I2F:
      return 0;
      break;

   case TGSI_OPCODE_NOT:
      return 0;
      break;

#if 0
   case TGSI_OPCODE_TRUNC:
      FOR_EACH_DST0_ENABLED_CHANNEL( inst, chan_index ) {
         tmp0 = emit_fetch( bld, inst, 0, chan_index );
         emit_f2it( bld, 0 );
         emit_i2f( bld, 0 );
         emit_store( bld, inst, 0, chan_index, tmp0);
      }
      break;
#endif

   case TGSI_OPCODE_SHL:
      return 0;
      break;

   case TGSI_OPCODE_SHR:
      return 0;
      break;

   case TGSI_OPCODE_AND:
      return 0;
      break;

   case TGSI_OPCODE_OR:
      return 0;
      break;

   case TGSI_OPCODE_MOD:
      return 0;
      break;

   case TGSI_OPCODE_XOR:
      return 0;
      break;

   case TGSI_OPCODE_SAD:
      return 0;
      break;

   case TGSI_OPCODE_TXF:
      return 0;
      break;

   case TGSI_OPCODE_TXQ:
      return 0;
      break;

   case TGSI_OPCODE_CONT:
      return 0;
      break;

   case TGSI_OPCODE_EMIT:
      return 0;
      break;

   case TGSI_OPCODE_ENDPRIM:
      return 0;
      break;

   default:
      return 0;
   }
   
   return 1;
}


void
lp_build_tgsi_soa(LLVMBuilderRef builder,
                  const struct tgsi_token *tokens,
                  union lp_type type,
                  struct lp_build_mask_context *mask,
                  LLVMValueRef consts_ptr,
                  const LLVMValueRef *pos,
                  const LLVMValueRef (*inputs)[NUM_CHANNELS],
                  LLVMValueRef (*outputs)[NUM_CHANNELS],
                  struct lp_build_sampler_soa *sampler)
{
   struct lp_build_tgsi_soa_context bld;
   struct tgsi_parse_context parse;
   uint num_immediates = 0;
   unsigned i;

   /* Setup build context */
   memset(&bld, 0, sizeof bld);
   lp_build_context_init(&bld.base, builder, type);
   bld.mask = mask;
   bld.pos = pos;
   bld.inputs = inputs;
   bld.outputs = outputs;
   bld.consts_ptr = consts_ptr;
   bld.sampler = sampler;

   tgsi_parse_init( &parse, tokens );

   while( !tgsi_parse_end_of_tokens( &parse ) ) {
      tgsi_parse_token( &parse );

      switch( parse.FullToken.Token.Type ) {
      case TGSI_TOKEN_TYPE_DECLARATION:
         /* Inputs already interpolated */
         break;

      case TGSI_TOKEN_TYPE_INSTRUCTION:
         if (!emit_instruction( &bld, &parse.FullToken.FullInstruction )) {
            unsigned opcode = parse.FullToken.FullInstruction.Instruction.Opcode;
            const struct tgsi_opcode_info *info = tgsi_get_opcode_info(opcode);
	    _debug_printf("warning: failed to translate tgsi opcode %s to LLVM\n",
	                  info ? info->mnemonic : "<invalid>");
	 }
         break;

      case TGSI_TOKEN_TYPE_IMMEDIATE:
         /* simply copy the immediate values into the next immediates[] slot */
         {
            const uint size = parse.FullToken.FullImmediate.Immediate.NrTokens - 1;
            assert(size <= 4);
            assert(num_immediates < LP_MAX_IMMEDIATES);
            for( i = 0; i < size; ++i )
               bld.immediates[num_immediates][i] =
                  lp_build_const_scalar(type, parse.FullToken.FullImmediate.u[i].Float);
            for( i = size; i < 4; ++i )
               bld.immediates[num_immediates][i] = bld.base.undef;
            num_immediates++;
         }
         break;

      default:
         assert( 0 );
      }
   }

   tgsi_parse_free( &parse );
}

