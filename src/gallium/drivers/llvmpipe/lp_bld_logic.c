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
 * @file
 * Helper functions for logical operations.
 *
 * @author Jose Fonseca <jfonseca@vmware.com>
 */


#include "lp_bld_type.h"
#include "lp_bld_const.h"
#include "lp_bld_intr.h"
#include "lp_bld_logic.h"


LLVMValueRef
lp_build_cmp(struct lp_build_context *bld,
             unsigned func,
             LLVMValueRef a,
             LLVMValueRef b)
{
   const union lp_type type = bld->type;
   LLVMTypeRef vec_type = lp_build_vec_type(type);
   LLVMTypeRef int_vec_type = lp_build_int_vec_type(type);
   LLVMValueRef zeros = LLVMConstNull(int_vec_type);
   LLVMValueRef ones = LLVMConstAllOnes(int_vec_type);
   LLVMValueRef cond;
   LLVMValueRef res;
   unsigned i;

   if(func == PIPE_FUNC_NEVER)
      return zeros;
   if(func == PIPE_FUNC_ALWAYS)
      return ones;

   /* TODO: optimize the constant case */

   /* XXX: It is not clear if we should use the ordered or unordered operators */

#if defined(PIPE_ARCH_X86) || defined(PIPE_ARCH_X86_64)
   if(type.width * type.length == 128) {
      if(type.floating) {
         LLVMValueRef args[3];
         unsigned cc;
         boolean swap;

         swap = FALSE;
         switch(func) {
         case PIPE_FUNC_EQUAL:
            cc = 0;
            break;
         case PIPE_FUNC_NOTEQUAL:
            cc = 4;
            break;
         case PIPE_FUNC_LESS:
            cc = 1;
            break;
         case PIPE_FUNC_LEQUAL:
            cc = 2;
            break;
         case PIPE_FUNC_GREATER:
            cc = 1;
            swap = TRUE;
            break;
         case PIPE_FUNC_GEQUAL:
            cc = 2;
            swap = TRUE;
            break;
         default:
            assert(0);
            return bld->undef;
         }

         if(swap) {
            args[0] = b;
            args[1] = a;
         }
         else {
            args[0] = a;
            args[1] = b;
         }

         args[2] = LLVMConstInt(LLVMInt8Type(), cc, 0);
         res = lp_build_intrinsic(bld->builder,
                                  "llvm.x86.sse.cmp.ps",
                                  vec_type,
                                  args, 3);
         res = LLVMBuildBitCast(bld->builder, res, int_vec_type, "");
         return res;
      }
      else {
         static const struct {
            unsigned swap:1;
            unsigned eq:1;
            unsigned gt:1;
            unsigned not:1;
         } table[] = {
            {0, 0, 0, 1}, /* PIPE_FUNC_NEVER */
            {1, 0, 1, 0}, /* PIPE_FUNC_LESS */
            {0, 1, 0, 0}, /* PIPE_FUNC_EQUAL */
            {0, 0, 1, 1}, /* PIPE_FUNC_LEQUAL */
            {0, 0, 1, 0}, /* PIPE_FUNC_GREATER */
            {0, 1, 0, 1}, /* PIPE_FUNC_NOTEQUAL */
            {1, 0, 1, 1}, /* PIPE_FUNC_GEQUAL */
            {0, 0, 0, 0}  /* PIPE_FUNC_ALWAYS */
         };
         const char *pcmpeq;
         const char *pcmpgt;
         LLVMValueRef args[2];
         LLVMValueRef res;

         switch (type.width) {
         case 8:
            pcmpeq = "llvm.x86.sse2.pcmpeq.b";
            pcmpgt = "llvm.x86.sse2.pcmpgt.b";
            break;
         case 16:
            pcmpeq = "llvm.x86.sse2.pcmpeq.w";
            pcmpgt = "llvm.x86.sse2.pcmpgt.w";
            break;
         case 32:
            pcmpeq = "llvm.x86.sse2.pcmpeq.d";
            pcmpgt = "llvm.x86.sse2.pcmpgt.d";
            break;
         default:
            assert(0);
            return bld->undef;
         }

         /* There are no signed byte and unsigned word/dword comparison
          * instructions. So flip the sign bit so that the results match.
          */
         if(table[func].gt &&
            ((type.width == 8 && type.sign) ||
             (type.width != 8 && !type.sign))) {
            LLVMValueRef msb = lp_build_int_const_scalar(type, (unsigned long long)1 << (type.width - 1));
            a = LLVMBuildXor(bld->builder, a, msb, "");
            b = LLVMBuildXor(bld->builder, b, msb, "");
         }

         if(table[func].swap) {
            args[0] = b;
            args[1] = a;
         }
         else {
            args[0] = a;
            args[1] = b;
         }

         if(table[func].eq)
            res = lp_build_intrinsic(bld->builder, pcmpeq, vec_type, args, 2);
         else if (table[func].gt)
            res = lp_build_intrinsic(bld->builder, pcmpgt, vec_type, args, 2);
         else
            res = LLVMConstNull(vec_type);

         if(table[func].not)
            res = LLVMBuildNot(bld->builder, res, "");

         return res;
      }
   }
#endif

   if(type.floating) {
      LLVMRealPredicate op;
      switch(func) {
      case PIPE_FUNC_NEVER:
         op = LLVMRealPredicateFalse;
         break;
      case PIPE_FUNC_ALWAYS:
         op = LLVMRealPredicateTrue;
         break;
      case PIPE_FUNC_EQUAL:
         op = LLVMRealUEQ;
         break;
      case PIPE_FUNC_NOTEQUAL:
         op = LLVMRealUNE;
         break;
      case PIPE_FUNC_LESS:
         op = LLVMRealULT;
         break;
      case PIPE_FUNC_LEQUAL:
         op = LLVMRealULE;
         break;
      case PIPE_FUNC_GREATER:
         op = LLVMRealUGT;
         break;
      case PIPE_FUNC_GEQUAL:
         op = LLVMRealUGE;
         break;
      default:
         assert(0);
         return bld->undef;
      }

#if 0
      /* XXX: Although valid IR, no LLVM target currently support this */
      cond = LLVMBuildFCmp(bld->builder, op, a, b, "");
      res = LLVMBuildSelect(bld->builder, cond, ones, zeros, "");
#else
      debug_printf("%s: warning: using slow element-wise vector comparison\n",
                   __FUNCTION__);
      res = LLVMGetUndef(int_vec_type);
      for(i = 0; i < type.length; ++i) {
         LLVMValueRef index = LLVMConstInt(LLVMInt32Type(), i, 0);
         cond = LLVMBuildFCmp(bld->builder, op,
                              LLVMBuildExtractElement(bld->builder, a, index, ""),
                              LLVMBuildExtractElement(bld->builder, b, index, ""),
                              "");
         cond = LLVMBuildSelect(bld->builder, cond,
                                LLVMConstExtractElement(ones, index),
                                LLVMConstExtractElement(zeros, index),
                                "");
         res = LLVMBuildInsertElement(bld->builder, res, cond, index, "");
      }
#endif
   }
   else {
      LLVMIntPredicate op;
      switch(func) {
      case PIPE_FUNC_EQUAL:
         op = LLVMIntEQ;
         break;
      case PIPE_FUNC_NOTEQUAL:
         op = LLVMIntNE;
         break;
      case PIPE_FUNC_LESS:
         op = type.sign ? LLVMIntSLT : LLVMIntULT;
         break;
      case PIPE_FUNC_LEQUAL:
         op = type.sign ? LLVMIntSLE : LLVMIntULE;
         break;
      case PIPE_FUNC_GREATER:
         op = type.sign ? LLVMIntSGT : LLVMIntUGT;
         break;
      case PIPE_FUNC_GEQUAL:
         op = type.sign ? LLVMIntSGE : LLVMIntUGE;
         break;
      default:
         assert(0);
         return bld->undef;
      }

#if 0
      /* XXX: Although valid IR, no LLVM target currently support this */
      cond = LLVMBuildICmp(bld->builder, op, a, b, "");
      res = LLVMBuildSelect(bld->builder, cond, ones, zeros, "");
#else
      debug_printf("%s: warning: using slow element-wise vector comparison\n",
                   __FUNCTION__);
      res = LLVMGetUndef(int_vec_type);
      for(i = 0; i < type.length; ++i) {
         LLVMValueRef index = LLVMConstInt(LLVMInt32Type(), i, 0);
         cond = LLVMBuildICmp(bld->builder, op,
                              LLVMBuildExtractElement(bld->builder, a, index, ""),
                              LLVMBuildExtractElement(bld->builder, b, index, ""),
                              "");
         cond = LLVMBuildSelect(bld->builder, cond,
                                LLVMConstExtractElement(ones, index),
                                LLVMConstExtractElement(zeros, index),
                                "");
         res = LLVMBuildInsertElement(bld->builder, res, cond, index, "");
      }
#endif
   }

   return res;
}


LLVMValueRef
lp_build_select(struct lp_build_context *bld,
                LLVMValueRef mask,
                LLVMValueRef a,
                LLVMValueRef b)
{
   union lp_type type = bld->type;
   LLVMValueRef res;

   if(a == b)
      return a;

   if(type.floating) {
      LLVMTypeRef int_vec_type = lp_build_int_vec_type(type);
      a = LLVMBuildBitCast(bld->builder, a, int_vec_type, "");
      b = LLVMBuildBitCast(bld->builder, b, int_vec_type, "");
   }

   a = LLVMBuildAnd(bld->builder, a, mask, "");

   /* This often gets translated to PANDN, but sometimes the NOT is
    * pre-computed and stored in another constant. The best strategy depends
    * on available registers, so it is not a big deal -- hopefully LLVM does
    * the right decision attending the rest of the program.
    */
   b = LLVMBuildAnd(bld->builder, b, LLVMBuildNot(bld->builder, mask, ""), "");

   res = LLVMBuildOr(bld->builder, a, b, "");

   if(type.floating) {
      LLVMTypeRef vec_type = lp_build_vec_type(type);
      res = LLVMBuildBitCast(bld->builder, res, vec_type, "");
   }

   return res;
}


LLVMValueRef
lp_build_select_aos(struct lp_build_context *bld,
                    LLVMValueRef a,
                    LLVMValueRef b,
                    boolean cond[4])
{
   const union lp_type type = bld->type;
   const unsigned n = type.length;
   unsigned i, j;

   if(a == b)
      return a;
   if(cond[0] && cond[1] && cond[2] && cond[3])
      return a;
   if(!cond[0] && !cond[1] && !cond[2] && !cond[3])
      return b;
   if(a == bld->undef || b == bld->undef)
      return bld->undef;

   /*
    * There are three major ways of accomplishing this:
    * - with a shuffle,
    * - with a select,
    * - or with a bit mask.
    *
    * Select isn't supported for vector types yet.
    * The flip between these is empirical and might need to be.
    */
   if (n <= 4) {
      /*
       * Shuffle.
       */
      LLVMTypeRef elem_type = LLVMInt32Type();
      LLVMValueRef shuffles[LP_MAX_VECTOR_LENGTH];

      for(j = 0; j < n; j += 4)
         for(i = 0; i < 4; ++i)
            shuffles[j + i] = LLVMConstInt(elem_type, (cond[i] ? 0 : n) + j + i, 0);

      return LLVMBuildShuffleVector(bld->builder, a, b, LLVMConstVector(shuffles, n), "");
   }
   else {
#if 0
      /* XXX: Unfortunately select of vectors do not work */
      /* Use a select */
      LLVMTypeRef elem_type = LLVMInt1Type();
      LLVMValueRef cond[LP_MAX_VECTOR_LENGTH];

      for(j = 0; j < n; j += 4)
         for(i = 0; i < 4; ++i)
            cond[j + i] = LLVMConstInt(elem_type, cond[i] ? 1 : 0, 0);

      return LLVMBuildSelect(bld->builder, LLVMConstVector(cond, n), a, b, "");
#else
      LLVMValueRef mask = lp_build_const_mask_aos(type, cond);
      return lp_build_select(bld, mask, a, b);
#endif
   }
}
