/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
 *
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "r500_fragprog.h"

#include "../r300_reg.h"

static struct prog_src_register shadow_ambient(struct radeon_compiler * c, int tmu)
{
	struct prog_src_register reg = { 0, };

	reg.File = PROGRAM_STATE_VAR;
	reg.Index = rc_constants_add_state(&c->Program.Constants, RC_STATE_SHADOW_AMBIENT, tmu);
	reg.Swizzle = SWIZZLE_WWWW;
	return reg;
}

/**
 * Transform TEX, TXP, TXB, and KIL instructions in the following way:
 *  - implement texture compare (shadow extensions)
 *  - extract non-native source / destination operands
 */
GLboolean r500_transform_TEX(
	struct radeon_compiler * c,
	struct rc_instruction * inst,
	void* data)
{
	struct r300_fragment_program_compiler *compiler =
		(struct r300_fragment_program_compiler*)data;

	if (inst->I.Opcode != OPCODE_TEX &&
	    inst->I.Opcode != OPCODE_TXB &&
	    inst->I.Opcode != OPCODE_TXP &&
	    inst->I.Opcode != OPCODE_KIL)
		return GL_FALSE;

	/* ARB_shadow & EXT_shadow_funcs */
	if (inst->I.Opcode != OPCODE_KIL &&
	    c->Program.ShadowSamplers & (1 << inst->I.TexSrcUnit)) {
		GLuint comparefunc = GL_NEVER + compiler->state.unit[inst->I.TexSrcUnit].texture_compare_func;

		if (comparefunc == GL_NEVER || comparefunc == GL_ALWAYS) {
			inst->I.Opcode = OPCODE_MOV;

			if (comparefunc == GL_ALWAYS) {
				inst->I.SrcReg[0].File = PROGRAM_BUILTIN;
				inst->I.SrcReg[0].Swizzle = SWIZZLE_1111;
			} else {
				inst->I.SrcReg[0] = shadow_ambient(c, inst->I.TexSrcUnit);
			}

			return GL_TRUE;
		} else {
			GLuint comparefunc = GL_NEVER + compiler->state.unit[inst->I.TexSrcUnit].texture_compare_func;
			GLuint depthmode = compiler->state.unit[inst->I.TexSrcUnit].depth_texture_mode;
			struct rc_instruction * inst_rcp = rc_insert_new_instruction(c, inst);
			struct rc_instruction * inst_mad = rc_insert_new_instruction(c, inst_rcp);
			struct rc_instruction * inst_cmp = rc_insert_new_instruction(c, inst_mad);
			int pass, fail;

			inst_rcp->I.Opcode = OPCODE_RCP;
			inst_rcp->I.DstReg.File = PROGRAM_TEMPORARY;
			inst_rcp->I.DstReg.Index = rc_find_free_temporary(c);
			inst_rcp->I.DstReg.WriteMask = WRITEMASK_W;
			inst_rcp->I.SrcReg[0] = inst->I.SrcReg[0];
			inst_rcp->I.SrcReg[0].Swizzle = SWIZZLE_WWWW;

			inst_cmp->I.DstReg = inst->I.DstReg;
			inst->I.DstReg.File = PROGRAM_TEMPORARY;
			inst->I.DstReg.Index = rc_find_free_temporary(c);
			inst->I.DstReg.WriteMask = WRITEMASK_XYZW;

			inst_mad->I.Opcode = OPCODE_MAD;
			inst_mad->I.DstReg.File = PROGRAM_TEMPORARY;
			inst_mad->I.DstReg.Index = rc_find_free_temporary(c);
			inst_mad->I.SrcReg[0] = inst->I.SrcReg[0];
			inst_mad->I.SrcReg[0].Swizzle = SWIZZLE_ZZZZ;
			inst_mad->I.SrcReg[1].File = PROGRAM_TEMPORARY;
			inst_mad->I.SrcReg[1].Index = inst_rcp->I.DstReg.Index;
			inst_mad->I.SrcReg[1].Swizzle = SWIZZLE_WWWW;
			inst_mad->I.SrcReg[2].File = PROGRAM_TEMPORARY;
			inst_mad->I.SrcReg[2].Index = inst->I.DstReg.Index;
			if (depthmode == 0) /* GL_LUMINANCE */
				inst_mad->I.SrcReg[2].Swizzle = MAKE_SWIZZLE4(SWIZZLE_X, SWIZZLE_Y, SWIZZLE_Z, SWIZZLE_Z);
			else if (depthmode == 2) /* GL_ALPHA */
				inst_mad->I.SrcReg[2].Swizzle = SWIZZLE_WWWW;

			/* Recall that SrcReg[0] is tex, SrcReg[2] is r and:
			 *   r  < tex  <=>      -tex+r < 0
			 *   r >= tex  <=> not (-tex+r < 0 */
			if (comparefunc == GL_LESS || comparefunc == GL_GEQUAL)
				inst_mad->I.SrcReg[2].Negate = inst_mad->I.SrcReg[2].Negate ^ NEGATE_XYZW;
			else
				inst_mad->I.SrcReg[0].Negate = inst_mad->I.SrcReg[0].Negate ^ NEGATE_XYZW;

			inst_cmp->I.Opcode = OPCODE_CMP;
			/* DstReg has been filled out above */
			inst_cmp->I.SrcReg[0].File = PROGRAM_TEMPORARY;
			inst_cmp->I.SrcReg[0].Index = inst_mad->I.DstReg.Index;

			if (comparefunc == GL_LESS || comparefunc == GL_GREATER) {
				pass = 1;
				fail = 2;
			} else {
				pass = 2;
				fail = 1;
			}

			inst_cmp->I.SrcReg[pass].File = PROGRAM_BUILTIN;
			inst_cmp->I.SrcReg[pass].Swizzle = SWIZZLE_1111;
			inst_cmp->I.SrcReg[fail] = shadow_ambient(c, inst->I.TexSrcUnit);
		}
	}

	/* Cannot write texture to output registers */
	if (inst->I.Opcode != OPCODE_KIL && inst->I.DstReg.File != PROGRAM_TEMPORARY) {
		struct rc_instruction * inst_mov = rc_insert_new_instruction(c, inst);

		inst_mov->I.Opcode = OPCODE_MOV;
		inst_mov->I.DstReg = inst->I.DstReg;
		inst_mov->I.SrcReg[0].File = PROGRAM_TEMPORARY;
		inst_mov->I.SrcReg[0].Index = rc_find_free_temporary(c);

		inst->I.DstReg.File = PROGRAM_TEMPORARY;
		inst->I.DstReg.Index = inst_mov->I.SrcReg[0].Index;
		inst->I.DstReg.WriteMask = WRITEMASK_XYZW;
	}

	/* Cannot read texture coordinate from constants file */
	if (inst->I.SrcReg[0].File != PROGRAM_TEMPORARY && inst->I.SrcReg[0].File != PROGRAM_INPUT) {
		struct rc_instruction * inst_mov = rc_insert_new_instruction(c, inst->Prev);

		inst_mov->I.Opcode = OPCODE_MOV;
		inst_mov->I.DstReg.File = PROGRAM_TEMPORARY;
		inst_mov->I.DstReg.Index = rc_find_free_temporary(c);
		inst_mov->I.SrcReg[0] = inst->I.SrcReg[0];

		reset_srcreg(&inst->I.SrcReg[0]);
		inst->I.SrcReg[0].File = PROGRAM_TEMPORARY;
		inst->I.SrcReg[0].Index = inst_mov->I.DstReg.Index;
	}

	return GL_TRUE;
}

GLboolean r500FPIsNativeSwizzle(GLuint opcode, struct prog_src_register reg)
{
	GLuint relevant;
	int i;

	if (opcode == OPCODE_TEX ||
	    opcode == OPCODE_TXB ||
	    opcode == OPCODE_TXP ||
	    opcode == OPCODE_KIL) {
		if (reg.Abs)
			return GL_FALSE;

		if (opcode == OPCODE_KIL && (reg.Swizzle != SWIZZLE_NOOP || reg.Negate != NEGATE_NONE))
			return GL_FALSE;

		if (reg.Negate)
			reg.Negate ^= NEGATE_XYZW;

		for(i = 0; i < 4; ++i) {
			GLuint swz = GET_SWZ(reg.Swizzle, i);
			if (swz == SWIZZLE_NIL) {
				reg.Negate &= ~(1 << i);
				continue;
			}
			if (swz >= 4)
				return GL_FALSE;
		}

		if (reg.Negate)
			return GL_FALSE;

		return GL_TRUE;
	} else if (opcode == OPCODE_DDX || opcode == OPCODE_DDY) {
		/* DDX/MDH and DDY/MDV explicitly ignore incoming swizzles;
		 * if it doesn't fit perfectly into a .xyzw case... */
		if (reg.Swizzle == SWIZZLE_NOOP && !reg.Abs && !reg.Negate)
			return GL_TRUE;

		return GL_FALSE;
	} else {
		/* ALU instructions support almost everything */
		if (reg.Abs)
			return GL_TRUE;

		relevant = 0;
		for(i = 0; i < 3; ++i) {
			GLuint swz = GET_SWZ(reg.Swizzle, i);
			if (swz != SWIZZLE_NIL && swz != SWIZZLE_ZERO)
				relevant |= 1 << i;
		}
		if ((reg.Negate & relevant) && ((reg.Negate & relevant) != relevant))
			return GL_FALSE;

		return GL_TRUE;
	}
}

/**
 * Implement a MOV with a potentially non-native swizzle.
 *
 * The only thing we *cannot* do in an ALU instruction is per-component
 * negation. Therefore, we split the MOV into two instructions when necessary.
 */
void r500FPBuildSwizzle(struct nqssadce_state *s, struct prog_dst_register dst, struct prog_src_register src)
{
	GLuint negatebase[2] = { 0, 0 };
	int i;

	for(i = 0; i < 4; ++i) {
		GLuint swz = GET_SWZ(src.Swizzle, i);
		if (swz == SWIZZLE_NIL)
			continue;
		negatebase[GET_BIT(src.Negate, i)] |= 1 << i;
	}

	for(i = 0; i <= 1; ++i) {
		if (!negatebase[i])
			continue;

		struct rc_instruction *inst = rc_insert_new_instruction(s->Compiler, s->IP->Prev);
		inst->I.Opcode = OPCODE_MOV;
		inst->I.DstReg = dst;
		inst->I.DstReg.WriteMask = negatebase[i];
		inst->I.SrcReg[0] = src;
		inst->I.SrcReg[0].Negate = (i == 0) ? NEGATE_NONE : NEGATE_XYZW;
	}
}


static char *toswiz(int swiz_val) {
  switch(swiz_val) {
  case 0: return "R";
  case 1: return "G";
  case 2: return "B";
  case 3: return "A";
  case 4: return "0";
  case 5: return "1/2";
  case 6: return "1";
  case 7: return "U";
  }
  return NULL;
}

static char *toop(int op_val)
{
  char *str = NULL;
  switch (op_val) {
  case 0: str = "MAD"; break;
  case 1: str = "DP3"; break;
  case 2: str = "DP4"; break;
  case 3: str = "D2A"; break;
  case 4: str = "MIN"; break;
  case 5: str = "MAX"; break;
  case 6: str = "Reserved"; break;
  case 7: str = "CND"; break;
  case 8: str = "CMP"; break;
  case 9: str = "FRC"; break;
  case 10: str = "SOP"; break;
  case 11: str = "MDH"; break;
  case 12: str = "MDV"; break;
  }
  return str;
}

static char *to_alpha_op(int op_val)
{
  char *str = NULL;
  switch (op_val) {
  case 0: str = "MAD"; break;
  case 1: str = "DP"; break;
  case 2: str = "MIN"; break;
  case 3: str = "MAX"; break;
  case 4: str = "Reserved"; break;
  case 5: str = "CND"; break;
  case 6: str = "CMP"; break;
  case 7: str = "FRC"; break;
  case 8: str = "EX2"; break;
  case 9: str = "LN2"; break;
  case 10: str = "RCP"; break;
  case 11: str = "RSQ"; break;
  case 12: str = "SIN"; break;
  case 13: str = "COS"; break;
  case 14: str = "MDH"; break;
  case 15: str = "MDV"; break;
  }
  return str;
}

static char *to_mask(int val)
{
  char *str = NULL;
  switch(val) {
  case 0: str = "NONE"; break;
  case 1: str = "R"; break;
  case 2: str = "G"; break;
  case 3: str = "RG"; break;
  case 4: str = "B"; break;
  case 5: str = "RB"; break;
  case 6: str = "GB"; break;
  case 7: str = "RGB"; break;
  case 8: str = "A"; break;
  case 9: str = "AR"; break;
  case 10: str = "AG"; break;
  case 11: str = "ARG"; break;
  case 12: str = "AB"; break;
  case 13: str = "ARB"; break;
  case 14: str = "AGB"; break;
  case 15: str = "ARGB"; break;
  }
  return str;
}

static char *to_texop(int val)
{
  switch(val) {
  case 0: return "NOP";
  case 1: return "LD";
  case 2: return "TEXKILL";
  case 3: return "PROJ";
  case 4: return "LODBIAS";
  case 5: return "LOD";
  case 6: return "DXDY";
  }
  return NULL;
}

void r500FragmentProgramDump(struct rX00_fragment_program_code *c)
{
  struct r500_fragment_program_code *code = &c->code.r500;
  fprintf(stderr, "R500 Fragment Program:\n--------\n");

  int n;
  uint32_t inst;
  uint32_t inst0;
  char *str = NULL;

  for (n = 0; n < code->inst_end+1; n++) {
    inst0 = inst = code->inst[n].inst0;
    fprintf(stderr,"%d\t0:CMN_INST   0x%08x:", n, inst);
    switch(inst & 0x3) {
    case R500_INST_TYPE_ALU: str = "ALU"; break;
    case R500_INST_TYPE_OUT: str = "OUT"; break;
    case R500_INST_TYPE_FC: str = "FC"; break;
    case R500_INST_TYPE_TEX: str = "TEX"; break;
    };
    fprintf(stderr,"%s %s %s %s %s ", str,
	    inst & R500_INST_TEX_SEM_WAIT ? "TEX_WAIT" : "",
	    inst & R500_INST_LAST ? "LAST" : "",
	    inst & R500_INST_NOP ? "NOP" : "",
	    inst & R500_INST_ALU_WAIT ? "ALU WAIT" : "");
    fprintf(stderr,"wmask: %s omask: %s\n", to_mask((inst >> 11) & 0xf),
	    to_mask((inst >> 15) & 0xf));

    switch(inst0 & 0x3) {
    case 0:
    case 1:
      fprintf(stderr,"\t1:RGB_ADDR   0x%08x:", code->inst[n].inst1);
      inst = code->inst[n].inst1;

      fprintf(stderr,"Addr0: %d%c, Addr1: %d%c, Addr2: %d%c, srcp:%d\n",
	      inst & 0xff, (inst & (1<<8)) ? 'c' : 't',
	      (inst >> 10) & 0xff, (inst & (1<<18)) ? 'c' : 't',
	      (inst >> 20) & 0xff, (inst & (1<<28)) ? 'c' : 't',
	      (inst >> 30));

      fprintf(stderr,"\t2:ALPHA_ADDR 0x%08x:", code->inst[n].inst2);
      inst = code->inst[n].inst2;
      fprintf(stderr,"Addr0: %d%c, Addr1: %d%c, Addr2: %d%c, srcp:%d\n",
	      inst & 0xff, (inst & (1<<8)) ? 'c' : 't',
	      (inst >> 10) & 0xff, (inst & (1<<18)) ? 'c' : 't',
	      (inst >> 20) & 0xff, (inst & (1<<28)) ? 'c' : 't',
	      (inst >> 30));
      fprintf(stderr,"\t3 RGB_INST:  0x%08x:", code->inst[n].inst3);
      inst = code->inst[n].inst3;
      fprintf(stderr,"rgb_A_src:%d %s/%s/%s %d rgb_B_src:%d %s/%s/%s %d\n",
	      (inst) & 0x3, toswiz((inst >> 2) & 0x7), toswiz((inst >> 5) & 0x7), toswiz((inst >> 8) & 0x7),
	      (inst >> 11) & 0x3,
	      (inst >> 13) & 0x3, toswiz((inst >> 15) & 0x7), toswiz((inst >> 18) & 0x7), toswiz((inst >> 21) & 0x7),
	      (inst >> 24) & 0x3);


      fprintf(stderr,"\t4 ALPHA_INST:0x%08x:", code->inst[n].inst4);
      inst = code->inst[n].inst4;
      fprintf(stderr,"%s dest:%d%s alp_A_src:%d %s %d alp_B_src:%d %s %d w:%d\n", to_alpha_op(inst & 0xf),
	      (inst >> 4) & 0x7f, inst & (1<<11) ? "(rel)":"",
	      (inst >> 12) & 0x3, toswiz((inst >> 14) & 0x7), (inst >> 17) & 0x3,
	      (inst >> 19) & 0x3, toswiz((inst >> 21) & 0x7), (inst >> 24) & 0x3,
	      (inst >> 31) & 0x1);

      fprintf(stderr,"\t5 RGBA_INST: 0x%08x:", code->inst[n].inst5);
      inst = code->inst[n].inst5;
      fprintf(stderr,"%s dest:%d%s rgb_C_src:%d %s/%s/%s %d alp_C_src:%d %s %d\n", toop(inst & 0xf),
	      (inst >> 4) & 0x7f, inst & (1<<11) ? "(rel)":"",
	      (inst >> 12) & 0x3, toswiz((inst >> 14) & 0x7), toswiz((inst >> 17) & 0x7), toswiz((inst >> 20) & 0x7),
	      (inst >> 23) & 0x3,
	      (inst >> 25) & 0x3, toswiz((inst >> 27) & 0x7), (inst >> 30) & 0x3);
      break;
    case 2:
      break;
    case 3:
      inst = code->inst[n].inst1;
      fprintf(stderr,"\t1:TEX_INST:  0x%08x: id: %d op:%s, %s, %s %s\n", inst, (inst >> 16) & 0xf,
	      to_texop((inst >> 22) & 0x7), (inst & (1<<25)) ? "ACQ" : "",
	      (inst & (1<<26)) ? "IGNUNC" : "", (inst & (1<<27)) ? "UNSCALED" : "SCALED");
      inst = code->inst[n].inst2;
      fprintf(stderr,"\t2:TEX_ADDR:  0x%08x: src: %d%s %s/%s/%s/%s dst: %d%s %s/%s/%s/%s\n", inst,
	      inst & 127, inst & (1<<7) ? "(rel)" : "",
	      toswiz((inst >> 8) & 0x3), toswiz((inst >> 10) & 0x3),
	      toswiz((inst >> 12) & 0x3), toswiz((inst >> 14) & 0x3),
	      (inst >> 16) & 127, inst & (1<<23) ? "(rel)" : "",
	      toswiz((inst >> 24) & 0x3), toswiz((inst >> 26) & 0x3),
	      toswiz((inst >> 28) & 0x3), toswiz((inst >> 30) & 0x3));

      fprintf(stderr,"\t3:TEX_DXDY:  0x%08x\n", code->inst[n].inst3);
      break;
    }
    fprintf(stderr,"\n");
  }

}
