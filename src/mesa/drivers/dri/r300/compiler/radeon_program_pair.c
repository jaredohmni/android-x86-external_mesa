/*
 * Copyright (C) 2008 Nicolai Haehnle.
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

/**
 * @file
 *
 * Perform temporary register allocation and attempt to pair off instructions
 * in RGB and Alpha pairs. Also attempts to optimize the TEX instruction
 * vs. ALU instruction scheduling.
 */

#include "radeon_program_pair.h"

#include "memory_pool.h"
#include "radeon_compiler.h"
#include "shader/prog_print.h"

#define error(fmt, args...) do { \
	rc_error(&s->Compiler->Base, "%s::%s(): " fmt "\n",	\
		__FILE__, __FUNCTION__, ##args);	\
} while(0)

struct pair_state_instruction {
	struct prog_instruction Instruction;
	GLuint IP; /**< Position of this instruction in original program */

	GLuint IsTex:1; /**< Is a texture instruction */
	GLuint NeedRGB:1; /**< Needs the RGB ALU */
	GLuint NeedAlpha:1; /**< Needs the Alpha ALU */
	GLuint IsTranscendent:1; /**< Is a special transcendent instruction */

	/**
	 * Number of (read and write) dependencies that must be resolved before
	 * this instruction can be scheduled.
	 */
	GLuint NumDependencies:5;

	/**
	 * Next instruction in the linked list of ready instructions.
	 */
	struct pair_state_instruction *NextReady;

	/**
	 * Values that this instruction writes
	 */
	struct reg_value *Values[4];
};


/**
 * Used to keep track of which instructions read a value.
 */
struct reg_value_reader {
	struct pair_state_instruction *Reader;
	struct reg_value_reader *Next;
};

/**
 * Used to keep track which values are stored in each component of a
 * PROGRAM_TEMPORARY.
 */
struct reg_value {
	struct pair_state_instruction *Writer;
	struct reg_value *Next; /**< Pointer to the next value to be written to the same PROGRAM_TEMPORARY component */

	/**
	 * Unordered linked list of instructions that read from this value.
	 */
	struct reg_value_reader *Readers;

	/**
	 * Number of readers of this value. This is calculated during @ref scan_instructions
	 * and continually decremented during code emission.
	 * When this count reaches zero, the instruction that writes the @ref Next value
	 * can be scheduled.
	 */
	GLuint NumReaders;
};

/**
 * Used to translate a PROGRAM_INPUT or PROGRAM_TEMPORARY Mesa register
 * to the proper hardware temporary.
 */
struct pair_register_translation {
	GLuint Allocated:1;
	GLuint HwIndex:8;
	GLuint RefCount:23; /**< # of times this occurs in an unscheduled instruction SrcReg or DstReg */

	/**
	 * Notes the value that is currently contained in each component
	 * (only used for PROGRAM_TEMPORARY registers).
	 */
	struct reg_value *Value[4];
};

struct pair_state {
	struct r300_fragment_program_compiler * Compiler;
	const struct radeon_pair_handler *Handler;
	GLboolean Verbose;
	void *UserData;

	/**
	 * Translate Mesa registers to hardware registers
	 */
	struct pair_register_translation Inputs[FRAG_ATTRIB_MAX];
	struct pair_register_translation Temps[MAX_PROGRAM_TEMPS];

	struct {
		GLuint RefCount; /**< # of times this occurs in an unscheduled SrcReg or DstReg */
	} HwTemps[128];

	/**
	 * Linked list of instructions that can be scheduled right now,
	 * based on which ALU/TEX resources they require.
	 */
	struct pair_state_instruction *ReadyFullALU;
	struct pair_state_instruction *ReadyRGB;
	struct pair_state_instruction *ReadyAlpha;
	struct pair_state_instruction *ReadyTEX;
};


static struct pair_register_translation *get_register(struct pair_state *s, GLuint file, GLuint index)
{
	switch(file) {
	case PROGRAM_TEMPORARY: return &s->Temps[index];
	case PROGRAM_INPUT: return &s->Inputs[index];
	default: return 0;
	}
}

static void alloc_hw_reg(struct pair_state *s, GLuint file, GLuint index, GLuint hwindex)
{
	struct pair_register_translation *t = get_register(s, file, index);
	ASSERT(!s->HwTemps[hwindex].RefCount);
	ASSERT(!t->Allocated);
	s->HwTemps[hwindex].RefCount = t->RefCount;
	t->Allocated = 1;
	t->HwIndex = hwindex;
}

static GLuint get_hw_reg(struct pair_state *s, GLuint file, GLuint index)
{
	GLuint hwindex;

	struct pair_register_translation *t = get_register(s, file, index);
	if (!t) {
		error("get_hw_reg: %i[%i]\n", file, index);
		return 0;
	}

	if (t->Allocated)
		return t->HwIndex;

	for(hwindex = 0; hwindex < s->Handler->MaxHwTemps; ++hwindex)
		if (!s->HwTemps[hwindex].RefCount)
			break;

	if (hwindex >= s->Handler->MaxHwTemps) {
		error("Ran out of hardware temporaries");
		return 0;
	}

	alloc_hw_reg(s, file, index, hwindex);
	return hwindex;
}


static void deref_hw_reg(struct pair_state *s, GLuint hwindex)
{
	if (!s->HwTemps[hwindex].RefCount) {
		error("Hwindex %i refcount error", hwindex);
		return;
	}

	s->HwTemps[hwindex].RefCount--;
}

static void add_pairinst_to_list(struct pair_state_instruction **list, struct pair_state_instruction *pairinst)
{
	pairinst->NextReady = *list;
	*list = pairinst;
}

/**
 * The given instruction has become ready. Link it into the ready
 * instructions.
 */
static void instruction_ready(struct pair_state *s, struct pair_state_instruction *pairinst)
{
	if (s->Verbose)
		_mesa_printf("instruction_ready(%i)\n", pairinst->IP);

	if (pairinst->IsTex)
		add_pairinst_to_list(&s->ReadyTEX, pairinst);
	else if (!pairinst->NeedAlpha)
		add_pairinst_to_list(&s->ReadyRGB, pairinst);
	else if (!pairinst->NeedRGB)
		add_pairinst_to_list(&s->ReadyAlpha, pairinst);
	else
		add_pairinst_to_list(&s->ReadyFullALU, pairinst);
}


/**
 * Finally rewrite ADD, MOV, MUL as the appropriate native instruction
 * and reverse the order of arguments for CMP.
 */
static void final_rewrite(struct pair_state *s, struct prog_instruction *inst)
{
	struct prog_src_register tmp;

	switch(inst->Opcode) {
	case OPCODE_ADD:
		inst->SrcReg[2] = inst->SrcReg[1];
		inst->SrcReg[1].File = PROGRAM_BUILTIN;
		inst->SrcReg[1].Swizzle = SWIZZLE_1111;
		inst->SrcReg[1].Negate = NEGATE_NONE;
		inst->Opcode = OPCODE_MAD;
		break;
	case OPCODE_CMP:
		tmp = inst->SrcReg[2];
		inst->SrcReg[2] = inst->SrcReg[0];
		inst->SrcReg[0] = tmp;
		break;
	case OPCODE_MOV:
		/* AMD say we should use CMP.
		 * However, when we transform
		 *  KIL -r0;
		 * into
		 *  CMP tmp, -r0, -r0, 0;
		 *  KIL tmp;
		 * we get incorrect behaviour on R500 when r0 == 0.0.
		 * It appears that the R500 KIL hardware treats -0.0 as less
		 * than zero.
		 */
		inst->SrcReg[1].File = PROGRAM_BUILTIN;
		inst->SrcReg[1].Swizzle = SWIZZLE_1111;
		inst->SrcReg[2].File = PROGRAM_BUILTIN;
		inst->SrcReg[2].Swizzle = SWIZZLE_0000;
		inst->Opcode = OPCODE_MAD;
		break;
	case OPCODE_MUL:
		inst->SrcReg[2].File = PROGRAM_BUILTIN;
		inst->SrcReg[2].Swizzle = SWIZZLE_0000;
		inst->Opcode = OPCODE_MAD;
		break;
	default:
		/* nothing to do */
		break;
	}
}


/**
 * Classify an instruction according to which ALUs etc. it needs
 */
static void classify_instruction(struct pair_state *s,
	struct pair_state_instruction *psi)
{
	psi->NeedRGB = (psi->Instruction.DstReg.WriteMask & WRITEMASK_XYZ) ? 1 : 0;
	psi->NeedAlpha = (psi->Instruction.DstReg.WriteMask & WRITEMASK_W) ? 1 : 0;

	switch(psi->Instruction.Opcode) {
	case OPCODE_ADD:
	case OPCODE_CMP:
	case OPCODE_DDX:
	case OPCODE_DDY:
	case OPCODE_FRC:
	case OPCODE_MAD:
	case OPCODE_MAX:
	case OPCODE_MIN:
	case OPCODE_MOV:
	case OPCODE_MUL:
		break;
	case OPCODE_COS:
	case OPCODE_EX2:
	case OPCODE_LG2:
	case OPCODE_RCP:
	case OPCODE_RSQ:
	case OPCODE_SIN:
		psi->IsTranscendent = 1;
		psi->NeedAlpha = 1;
		break;
	case OPCODE_DP4:
		psi->NeedAlpha = 1;
		/* fall through */
	case OPCODE_DP3:
		psi->NeedRGB = 1;
		break;
	case OPCODE_KIL:
	case OPCODE_TEX:
	case OPCODE_TXB:
	case OPCODE_TXP:
	case OPCODE_END:
		psi->IsTex = 1;
		break;
	default:
		error("Unknown opcode %d\n", psi->Instruction.Opcode);
		break;
	}
}


/**
 * Count which (input, temporary) register is read and written how often,
 * and scan the instruction stream to find dependencies.
 */
static void scan_instructions(struct pair_state *s)
{
	struct rc_instruction *source;
	GLuint ip;

	for(source = s->Compiler->Base.Program.Instructions.Next, ip = 0;
	    source != &s->Compiler->Base.Program.Instructions;
	    source = source->Next, ++ip) {
		struct pair_state_instruction *pairinst = memory_pool_malloc(&s->Compiler->Base.Pool, sizeof(*pairinst));
		memset(pairinst, 0, sizeof(struct pair_state_instruction));

		pairinst->Instruction = source->I;
		pairinst->IP = ip;
		final_rewrite(s, &pairinst->Instruction);
		classify_instruction(s, pairinst);

		int nsrc = _mesa_num_inst_src_regs(pairinst->Instruction.Opcode);
		int j;
		for(j = 0; j < nsrc; j++) {
			struct pair_register_translation *t =
				get_register(s, pairinst->Instruction.SrcReg[j].File, pairinst->Instruction.SrcReg[j].Index);
			if (!t)
				continue;

			t->RefCount++;

			if (pairinst->Instruction.SrcReg[j].File == PROGRAM_TEMPORARY) {
				int i;
				for(i = 0; i < 4; ++i) {
					GLuint swz = GET_SWZ(pairinst->Instruction.SrcReg[j].Swizzle, i);
					if (swz >= 4)
						continue; /* constant or NIL swizzle */
					if (!t->Value[swz])
						continue; /* this is an undefined read */

					/* Do not add a dependency if this instruction
					 * also rewrites the value. The code below adds
					 * a dependency for the DstReg, which is a superset
					 * of the SrcReg dependency. */
					if (pairinst->Instruction.DstReg.File == PROGRAM_TEMPORARY &&
					    pairinst->Instruction.DstReg.Index == pairinst->Instruction.SrcReg[j].Index &&
					    GET_BIT(pairinst->Instruction.DstReg.WriteMask, swz))
						continue;

					struct reg_value_reader* r = memory_pool_malloc(&s->Compiler->Base.Pool, sizeof(*r));
					pairinst->NumDependencies++;
					t->Value[swz]->NumReaders++;
					r->Reader = pairinst;
					r->Next = t->Value[swz]->Readers;
					t->Value[swz]->Readers = r;
				}
			}
		}

		int ndst = _mesa_num_inst_dst_regs(pairinst->Instruction.Opcode);
		if (ndst) {
			struct pair_register_translation *t =
				get_register(s, pairinst->Instruction.DstReg.File, pairinst->Instruction.DstReg.Index);
			if (t) {
				t->RefCount++;

				if (pairinst->Instruction.DstReg.File == PROGRAM_TEMPORARY) {
					int j;
					for(j = 0; j < 4; ++j) {
						if (!GET_BIT(pairinst->Instruction.DstReg.WriteMask, j))
							continue;

						struct reg_value* v = memory_pool_malloc(&s->Compiler->Base.Pool, sizeof(*v));
						memset(v, 0, sizeof(struct reg_value));
						v->Writer = pairinst;
						if (t->Value[j]) {
							pairinst->NumDependencies++;
							t->Value[j]->Next = v;
						}
						t->Value[j] = v;
						pairinst->Values[j] = v;
					}
				}
			}
		}

		if (s->Verbose)
			_mesa_printf("scan(%i): NumDeps = %i\n", ip, pairinst->NumDependencies);

		if (!pairinst->NumDependencies)
			instruction_ready(s, pairinst);
	}

	/* Clear the PROGRAM_TEMPORARY state */
	int i, j;
	for(i = 0; i < MAX_PROGRAM_TEMPS; ++i) {
		for(j = 0; j < 4; ++j)
			s->Temps[i].Value[j] = 0;
	}
}


static void decrement_dependencies(struct pair_state *s, struct pair_state_instruction *pairinst)
{
	ASSERT(pairinst->NumDependencies > 0);
	if (!--pairinst->NumDependencies)
		instruction_ready(s, pairinst);
}

/**
 * Update the dependency tracking state based on what the instruction
 * at the given IP does.
 */
static void commit_instruction(struct pair_state *s, struct pair_state_instruction *pairinst)
{
	struct prog_instruction *inst = &pairinst->Instruction;

	if (s->Verbose)
		_mesa_printf("commit_instruction(%i)\n", pairinst->IP);

	if (inst->DstReg.File == PROGRAM_TEMPORARY) {
		struct pair_register_translation *t = &s->Temps[inst->DstReg.Index];
		deref_hw_reg(s, t->HwIndex);

		int i;
		for(i = 0; i < 4; ++i) {
			if (!GET_BIT(inst->DstReg.WriteMask, i))
				continue;

			t->Value[i] = pairinst->Values[i];
			if (t->Value[i]->NumReaders) {
				struct reg_value_reader *r;
				for(r = pairinst->Values[i]->Readers; r; r = r->Next)
					decrement_dependencies(s, r->Reader);
			} else if (t->Value[i]->Next) {
				/* This happens when the only reader writes
				 * the register at the same time */
				decrement_dependencies(s, t->Value[i]->Next->Writer);
			}
		}
	}

	int nsrc = _mesa_num_inst_src_regs(inst->Opcode);
	int i;
	for(i = 0; i < nsrc; i++) {
		struct pair_register_translation *t = get_register(s, inst->SrcReg[i].File, inst->SrcReg[i].Index);
		if (!t)
			continue;

		deref_hw_reg(s, get_hw_reg(s, inst->SrcReg[i].File, inst->SrcReg[i].Index));

		if (inst->SrcReg[i].File != PROGRAM_TEMPORARY)
			continue;

		int j;
		for(j = 0; j < 4; ++j) {
			GLuint swz = GET_SWZ(inst->SrcReg[i].Swizzle, j);
			if (swz >= 4)
				continue;
			if (!t->Value[swz])
				continue;

			/* Do not free a dependency if this instruction
			 * also rewrites the value. See scan_instructions. */
			if (inst->DstReg.File == PROGRAM_TEMPORARY &&
			    inst->DstReg.Index == inst->SrcReg[i].Index &&
			    GET_BIT(inst->DstReg.WriteMask, swz))
				continue;

			if (!--t->Value[swz]->NumReaders) {
				if (t->Value[swz]->Next)
					decrement_dependencies(s, t->Value[swz]->Next->Writer);
			}
		}
	}
}


/**
 * Emit all ready texture instructions in a single block.
 *
 * Emit as a single block to (hopefully) sample many textures in parallel,
 * and to avoid hardware indirections on R300.
 *
 * In R500, we don't really know when the result of a texture instruction
 * arrives. So allocate all destinations first, to make sure they do not
 * arrive early and overwrite a texture coordinate we're going to use later
 * in the block.
 */
static void emit_all_tex(struct pair_state *s)
{
	struct pair_state_instruction *readytex;
	struct pair_state_instruction *pairinst;

	ASSERT(s->ReadyTEX);

	// Don't let the ready list change under us!
	readytex = s->ReadyTEX;
	s->ReadyTEX = 0;

	// Allocate destination hardware registers in one block to avoid conflicts.
	for(pairinst = readytex; pairinst; pairinst = pairinst->NextReady) {
		struct prog_instruction *inst = &pairinst->Instruction;
		if (inst->Opcode != OPCODE_KIL)
			get_hw_reg(s, inst->DstReg.File, inst->DstReg.Index);
	}

	if (s->Compiler->Base.Debug)
		_mesa_printf(" BEGIN_TEX\n");

	if (s->Handler->BeginTexBlock)
		s->Compiler->Base.Error = s->Compiler->Base.Error || !s->Handler->BeginTexBlock(s->UserData);

	for(pairinst = readytex; pairinst; pairinst = pairinst->NextReady) {
		struct prog_instruction *inst = &pairinst->Instruction;
		commit_instruction(s, pairinst);

		if (inst->Opcode != OPCODE_KIL)
			inst->DstReg.Index = get_hw_reg(s, inst->DstReg.File, inst->DstReg.Index);
		inst->SrcReg[0].Index = get_hw_reg(s, inst->SrcReg[0].File, inst->SrcReg[0].Index);

		if (s->Compiler->Base.Debug) {
			_mesa_printf("   ");
			_mesa_print_instruction(inst);
			fflush(stderr);
		}

		struct radeon_pair_texture_instruction rpti;

		switch(inst->Opcode) {
		case OPCODE_TEX: rpti.Opcode = RADEON_OPCODE_TEX; break;
		case OPCODE_TXB: rpti.Opcode = RADEON_OPCODE_TXB; break;
		case OPCODE_TXP: rpti.Opcode = RADEON_OPCODE_TXP; break;
		default:
		case OPCODE_KIL: rpti.Opcode = RADEON_OPCODE_KIL; break;
		}

		rpti.DestIndex = inst->DstReg.Index;
		rpti.WriteMask = inst->DstReg.WriteMask;
		rpti.TexSrcUnit = inst->TexSrcUnit;
		rpti.TexSrcTarget = inst->TexSrcTarget;
		rpti.SrcIndex = inst->SrcReg[0].Index;
		rpti.SrcSwizzle = inst->SrcReg[0].Swizzle;

		s->Compiler->Base.Error = s->Compiler->Base.Error || !s->Handler->EmitTex(s->UserData, &rpti);
	}

	if (s->Compiler->Base.Debug)
		_mesa_printf(" END_TEX\n");
}


static int alloc_pair_source(struct pair_state *s, struct radeon_pair_instruction *pair,
	struct prog_src_register src, GLboolean rgb, GLboolean alpha)
{
	int candidate = -1;
	int candidate_quality = -1;
	int i;

	if (!rgb && !alpha)
		return 0;

	GLuint constant;
	GLuint index;

	if (src.File == PROGRAM_TEMPORARY || src.File == PROGRAM_INPUT) {
		constant = 0;
		index = get_hw_reg(s, src.File, src.Index);
	} else {
		constant = 1;
		index = src.Index;
	}

	for(i = 0; i < 3; ++i) {
		int q = 0;
		if (rgb) {
			if (pair->RGB.Src[i].Used) {
				if (pair->RGB.Src[i].Constant != constant ||
				    pair->RGB.Src[i].Index != index)
					continue;
				q++;
			}
		}
		if (alpha) {
			if (pair->Alpha.Src[i].Used) {
				if (pair->Alpha.Src[i].Constant != constant ||
				    pair->Alpha.Src[i].Index != index)
					continue;
				q++;
			}
		}
		if (q > candidate_quality) {
			candidate_quality = q;
			candidate = i;
		}
	}

	if (candidate >= 0) {
		if (rgb) {
			pair->RGB.Src[candidate].Used = 1;
			pair->RGB.Src[candidate].Constant = constant;
			pair->RGB.Src[candidate].Index = index;
		}
		if (alpha) {
			pair->Alpha.Src[candidate].Used = 1;
			pair->Alpha.Src[candidate].Constant = constant;
			pair->Alpha.Src[candidate].Index = index;
		}
	}

	return candidate;
}

/**
 * Fill the given ALU instruction's opcodes and source operands into the given pair,
 * if possible.
 */
static GLboolean fill_instruction_into_pair(
	struct pair_state *s,
	struct radeon_pair_instruction *pair,
	struct pair_state_instruction *pairinst)
{
	struct prog_instruction *inst = &pairinst->Instruction;

	ASSERT(!pairinst->NeedRGB || pair->RGB.Opcode == OPCODE_NOP);
	ASSERT(!pairinst->NeedAlpha || pair->Alpha.Opcode == OPCODE_NOP);

	if (pairinst->NeedRGB) {
		if (pairinst->IsTranscendent)
			pair->RGB.Opcode = OPCODE_REPL_ALPHA;
		else
			pair->RGB.Opcode = inst->Opcode;
		if (inst->SaturateMode == SATURATE_ZERO_ONE)
			pair->RGB.Saturate = 1;
	}
	if (pairinst->NeedAlpha) {
		pair->Alpha.Opcode = inst->Opcode;
		if (inst->SaturateMode == SATURATE_ZERO_ONE)
			pair->Alpha.Saturate = 1;
	}

	int nargs = _mesa_num_inst_src_regs(inst->Opcode);
	int i;

	/* Special case for DDX/DDY (MDH/MDV). */
	if (inst->Opcode == OPCODE_DDX || inst->Opcode == OPCODE_DDY) {
		if (pair->RGB.Src[0].Used || pair->Alpha.Src[0].Used)
			return GL_FALSE;
		else
			nargs++;
	}

	for(i = 0; i < nargs; ++i) {
		int source;
		if (pairinst->NeedRGB && !pairinst->IsTranscendent) {
			GLboolean srcrgb = GL_FALSE;
			GLboolean srcalpha = GL_FALSE;
			int j;
			for(j = 0; j < 3; ++j) {
				GLuint swz = GET_SWZ(inst->SrcReg[i].Swizzle, j);
				if (swz < 3)
					srcrgb = GL_TRUE;
				else if (swz < 4)
					srcalpha = GL_TRUE;
			}
			source = alloc_pair_source(s, pair, inst->SrcReg[i], srcrgb, srcalpha);
			if (source < 0)
				return GL_FALSE;
			pair->RGB.Arg[i].Source = source;
			pair->RGB.Arg[i].Swizzle = inst->SrcReg[i].Swizzle & 0x1ff;
			pair->RGB.Arg[i].Abs = inst->SrcReg[i].Abs;
			pair->RGB.Arg[i].Negate = !!(inst->SrcReg[i].Negate & (NEGATE_X | NEGATE_Y | NEGATE_Z));
		}
		if (pairinst->NeedAlpha) {
			GLboolean srcrgb = GL_FALSE;
			GLboolean srcalpha = GL_FALSE;
			GLuint swz = GET_SWZ(inst->SrcReg[i].Swizzle, pairinst->IsTranscendent ? 0 : 3);
			if (swz < 3)
				srcrgb = GL_TRUE;
			else if (swz < 4)
				srcalpha = GL_TRUE;
			source = alloc_pair_source(s, pair, inst->SrcReg[i], srcrgb, srcalpha);
			if (source < 0)
				return GL_FALSE;
			pair->Alpha.Arg[i].Source = source;
			pair->Alpha.Arg[i].Swizzle = swz;
			pair->Alpha.Arg[i].Abs = inst->SrcReg[i].Abs;
			pair->Alpha.Arg[i].Negate = !!(inst->SrcReg[i].Negate & NEGATE_W);
		}
	}

	return GL_TRUE;
}


/**
 * Fill in the destination register information.
 *
 * This is split from filling in source registers because we want
 * to avoid allocating hardware temporaries for destinations until
 * we are absolutely certain that we're going to emit a certain
 * instruction pairing.
 */
static void fill_dest_into_pair(
	struct pair_state *s,
	struct radeon_pair_instruction *pair,
	struct pair_state_instruction *pairinst)
{
	struct prog_instruction *inst = &pairinst->Instruction;

	if (inst->DstReg.File == PROGRAM_OUTPUT) {
		if (inst->DstReg.Index == s->Compiler->OutputColor) {
			pair->RGB.OutputWriteMask |= inst->DstReg.WriteMask & WRITEMASK_XYZ;
			pair->Alpha.OutputWriteMask |= GET_BIT(inst->DstReg.WriteMask, 3);
		} else if (inst->DstReg.Index == s->Compiler->OutputDepth) {
			pair->Alpha.DepthWriteMask |= GET_BIT(inst->DstReg.WriteMask, 3);
		}
	} else {
		GLuint hwindex = get_hw_reg(s, inst->DstReg.File, inst->DstReg.Index);
		if (pairinst->NeedRGB) {
			pair->RGB.DestIndex = hwindex;
			pair->RGB.WriteMask |= inst->DstReg.WriteMask & WRITEMASK_XYZ;
		}
		if (pairinst->NeedAlpha) {
			pair->Alpha.DestIndex = hwindex;
			pair->Alpha.WriteMask |= GET_BIT(inst->DstReg.WriteMask, 3);
		}
	}
}


/**
 * Find a good ALU instruction or pair of ALU instruction and emit it.
 *
 * Prefer emitting full ALU instructions, so that when we reach a point
 * where no full ALU instruction can be emitted, we have more candidates
 * for RGB/Alpha pairing.
 */
static void emit_alu(struct pair_state *s)
{
	struct radeon_pair_instruction pair;
	struct pair_state_instruction *psi;

	if (s->ReadyFullALU || !(s->ReadyRGB && s->ReadyAlpha)) {
		if (s->ReadyFullALU) {
			psi = s->ReadyFullALU;
			s->ReadyFullALU = s->ReadyFullALU->NextReady;
		} else if (s->ReadyRGB) {
			psi = s->ReadyRGB;
			s->ReadyRGB = s->ReadyRGB->NextReady;
		} else {
			psi = s->ReadyAlpha;
			s->ReadyAlpha = s->ReadyAlpha->NextReady;
		}

		_mesa_bzero(&pair, sizeof(pair));
		fill_instruction_into_pair(s, &pair, psi);
		fill_dest_into_pair(s, &pair, psi);
		commit_instruction(s, psi);
	} else {
		struct pair_state_instruction **prgb;
		struct pair_state_instruction **palpha;

		/* Some pairings might fail because they require too
		 * many source slots; try all possible pairings if necessary */
		for(prgb = &s->ReadyRGB; *prgb; prgb = &(*prgb)->NextReady) {
			for(palpha = &s->ReadyAlpha; *palpha; palpha = &(*palpha)->NextReady) {
				struct pair_state_instruction * psirgb = *prgb;
				struct pair_state_instruction * psialpha = *palpha;
				_mesa_bzero(&pair, sizeof(pair));
				fill_instruction_into_pair(s, &pair, psirgb);
				if (!fill_instruction_into_pair(s, &pair, psialpha))
					continue;
				*prgb = (*prgb)->NextReady;
				*palpha = (*palpha)->NextReady;
				fill_dest_into_pair(s, &pair, psirgb);
				fill_dest_into_pair(s, &pair, psialpha);
				commit_instruction(s, psirgb);
				commit_instruction(s, psialpha);
				goto success;
			}
		}

		/* No success in pairing; just take the first RGB instruction */
		psi = s->ReadyRGB;
		s->ReadyRGB = s->ReadyRGB->NextReady;

		_mesa_bzero(&pair, sizeof(pair));
		fill_instruction_into_pair(s, &pair, psi);
		fill_dest_into_pair(s, &pair, psi);
		commit_instruction(s, psi);
	success: ;
	}

	if (s->Compiler->Base.Debug)
		radeonPrintPairInstruction(&pair);

	s->Compiler->Base.Error = s->Compiler->Base.Error || !s->Handler->EmitPaired(s->UserData, &pair);
}

/* Callback function for assigning input registers to hardware registers */
static void alloc_helper(void * data, unsigned input, unsigned hwreg)
{
	struct pair_state * s = data;
	alloc_hw_reg(s, PROGRAM_INPUT, input, hwreg);
}

void radeonPairProgram(
	struct r300_fragment_program_compiler * compiler,
	const struct radeon_pair_handler* handler, void *userdata)
{
	struct pair_state s;

	_mesa_bzero(&s, sizeof(s));
	s.Compiler = compiler;
	s.Handler = handler;
	s.UserData = userdata;
	s.Verbose = GL_FALSE && s.Compiler->Base.Debug;

	if (s.Compiler->Base.Debug)
		_mesa_printf("Emit paired program\n");

	scan_instructions(&s);
	s.Compiler->AllocateHwInputs(s.Compiler, &alloc_helper, &s);

	while(!s.Compiler->Base.Error &&
	      (s.ReadyTEX || s.ReadyRGB || s.ReadyAlpha || s.ReadyFullALU)) {
		if (s.ReadyTEX)
			emit_all_tex(&s);

		while(s.ReadyFullALU || s.ReadyRGB || s.ReadyAlpha)
			emit_alu(&s);
	}

	if (s.Compiler->Base.Debug)
		_mesa_printf(" END\n");
}


static void print_pair_src(int i, struct radeon_pair_instruction_source* src)
{
	_mesa_printf("  Src%i = %s[%i]", i, src->Constant ? "CNST" : "TEMP", src->Index);
}

static const char* opcode_string(GLuint opcode)
{
	if (opcode == OPCODE_REPL_ALPHA)
		return "SOP";
	else
		return _mesa_opcode_string(opcode);
}

static int num_pairinst_args(GLuint opcode)
{
	if (opcode == OPCODE_REPL_ALPHA)
		return 0;
	else
		return _mesa_num_inst_src_regs(opcode);
}

static char swizzle_char(GLuint swz)
{
	switch(swz) {
	case SWIZZLE_X: return 'x';
	case SWIZZLE_Y: return 'y';
	case SWIZZLE_Z: return 'z';
	case SWIZZLE_W: return 'w';
	case SWIZZLE_ZERO: return '0';
	case SWIZZLE_ONE: return '1';
	case SWIZZLE_NIL: return '_';
	default: return '?';
	}
}

void radeonPrintPairInstruction(struct radeon_pair_instruction *inst)
{
	int nargs;
	int i;

	_mesa_printf("       RGB:  ");
	for(i = 0; i < 3; ++i) {
		if (inst->RGB.Src[i].Used)
			print_pair_src(i, inst->RGB.Src + i);
	}
	_mesa_printf("\n");
	_mesa_printf("       Alpha:");
	for(i = 0; i < 3; ++i) {
		if (inst->Alpha.Src[i].Used)
			print_pair_src(i, inst->Alpha.Src + i);
	}
	_mesa_printf("\n");

	_mesa_printf("  %s%s", opcode_string(inst->RGB.Opcode), inst->RGB.Saturate ? "_SAT" : "");
	if (inst->RGB.WriteMask)
		_mesa_printf(" TEMP[%i].%s%s%s", inst->RGB.DestIndex,
			(inst->RGB.WriteMask & 1) ? "x" : "",
			(inst->RGB.WriteMask & 2) ? "y" : "",
			(inst->RGB.WriteMask & 4) ? "z" : "");
	if (inst->RGB.OutputWriteMask)
		_mesa_printf(" COLOR.%s%s%s",
			(inst->RGB.OutputWriteMask & 1) ? "x" : "",
			(inst->RGB.OutputWriteMask & 2) ? "y" : "",
			(inst->RGB.OutputWriteMask & 4) ? "z" : "");
	nargs = num_pairinst_args(inst->RGB.Opcode);
	for(i = 0; i < nargs; ++i) {
		const char* abs = inst->RGB.Arg[i].Abs ? "|" : "";
		const char* neg = inst->RGB.Arg[i].Negate ? "-" : "";
		_mesa_printf(", %s%sSrc%i.%c%c%c%s", neg, abs, inst->RGB.Arg[i].Source,
			swizzle_char(GET_SWZ(inst->RGB.Arg[i].Swizzle, 0)),
			swizzle_char(GET_SWZ(inst->RGB.Arg[i].Swizzle, 1)),
			swizzle_char(GET_SWZ(inst->RGB.Arg[i].Swizzle, 2)),
			abs);
	}
	_mesa_printf("\n");

	_mesa_printf("  %s%s", opcode_string(inst->Alpha.Opcode), inst->Alpha.Saturate ? "_SAT" : "");
	if (inst->Alpha.WriteMask)
		_mesa_printf(" TEMP[%i].w", inst->Alpha.DestIndex);
	if (inst->Alpha.OutputWriteMask)
		_mesa_printf(" COLOR.w");
	if (inst->Alpha.DepthWriteMask)
		_mesa_printf(" DEPTH.w");
	nargs = num_pairinst_args(inst->Alpha.Opcode);
	for(i = 0; i < nargs; ++i) {
		const char* abs = inst->Alpha.Arg[i].Abs ? "|" : "";
		const char* neg = inst->Alpha.Arg[i].Negate ? "-" : "";
		_mesa_printf(", %s%sSrc%i.%c%s", neg, abs, inst->Alpha.Arg[i].Source,
			swizzle_char(inst->Alpha.Arg[i].Swizzle), abs);
	}
	_mesa_printf("\n");
}
