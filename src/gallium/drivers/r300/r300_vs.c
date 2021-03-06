/*
 * Copyright 2009 Corbin Simpson <MostAwesomeDude@gmail.com>
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
 * USE OR OTHER DEALINGS IN THE SOFTWARE. */

#include "r300_vs.h"

#include "r300_context.h"
#include "r300_tgsi_to_rc.h"

#include "tgsi/tgsi_dump.h"
#include "tgsi/tgsi_parse.h"

#include "radeon_compiler.h"


static void set_vertex_inputs_outputs(struct r300_vertex_program_compiler * c)
{
    struct r300_vertex_shader * vs = c->UserData;
    struct tgsi_shader_info* info = &vs->info;
    boolean pointsize = false;
    int out_colors = 0;
    int colors = 0;
    int out_generic = 0;
    int generic = 0;
    int i;

    /* Fill in the input mapping */
    for (i = 0; i < info->num_inputs; i++)
        c->code->inputs[i] = i;

    /* Fill in the output mapping */
    for (i = 0; i < info->num_outputs; i++) {
        switch (info->output_semantic_name[i]) {
            case TGSI_SEMANTIC_PSIZE:
                pointsize = true;
                break;
            case TGSI_SEMANTIC_COLOR:
                out_colors++;
                break;
            case TGSI_SEMANTIC_FOG:
            case TGSI_SEMANTIC_GENERIC:
                out_generic++;
                break;
        }
    }

    struct tgsi_parse_context parser;

    tgsi_parse_init(&parser, vs->state.tokens);

    while (!tgsi_parse_end_of_tokens(&parser)) {
        tgsi_parse_token(&parser);

        if (parser.FullToken.Token.Type != TGSI_TOKEN_TYPE_DECLARATION)
            continue;

        struct tgsi_full_declaration * decl = &parser.FullToken.FullDeclaration;

        if (decl->Declaration.File != TGSI_FILE_OUTPUT)
            continue;

        switch (decl->Semantic.SemanticName) {
            case TGSI_SEMANTIC_POSITION:
                c->code->outputs[decl->DeclarationRange.First] = 0;
                break;
            case TGSI_SEMANTIC_PSIZE:
                c->code->outputs[decl->DeclarationRange.First] = 1;
                break;
            case TGSI_SEMANTIC_COLOR:
                c->code->outputs[decl->DeclarationRange.First] = 1 +
                    (pointsize ? 1 : 0) +
                    colors++;
                break;
            case TGSI_SEMANTIC_FOG:
            case TGSI_SEMANTIC_GENERIC:
                c->code->outputs[decl->DeclarationRange.First] = 1 +
                    (pointsize ? 1 : 0) +
                    out_colors +
                    generic++;
                break;
            default:
                debug_printf("r300: vs: Bad semantic declaration %d\n",
                    decl->Semantic.SemanticName);
                break;
        }
    }

    tgsi_parse_free(&parser);
}


void r300_translate_vertex_shader(struct r300_context* r300,
                                  struct r300_vertex_shader* vs)
{
    struct r300_vertex_program_compiler compiler;
    struct tgsi_to_rc ttr;

    /* Setup the compiler */
    rc_init(&compiler.Base);

    compiler.Base.Debug = DBG_ON(r300, DBG_VP);
    compiler.code = &vs->code;
    compiler.UserData = vs;

    if (compiler.Base.Debug) {
        debug_printf("r300: Initial vertex program\n");
        tgsi_dump(vs->state.tokens, 0);
    }

    /* Translate TGSI to our internal representation */
    ttr.compiler = &compiler.Base;
    ttr.info = &vs->info;

    r300_tgsi_to_rc(&ttr, vs->state.tokens);

    compiler.RequiredOutputs = ~(~0 << vs->info.num_outputs);
    compiler.SetHwInputOutput = &set_vertex_inputs_outputs;

    /* Invoke the compiler */
    r3xx_compile_vertex_program(&compiler);
    if (compiler.Base.Error) {
        /* Todo: Fail gracefully */
        fprintf(stderr, "r300 VP: Compiler error\n");
        abort();
    }

    /* And, finally... */
    rc_destroy(&compiler.Base);
    vs->translated = TRUE;
}


/* XXX get these to r300_reg */
#define R300_PVS_DST_OPCODE(x)   ((x) << 0)
#   define R300_VE_DOT_PRODUCT            1
#   define R300_VE_MULTIPLY               2
#   define R300_VE_ADD                    3
#   define R300_VE_MAXIMUM                7
#   define R300_VE_SET_LESS_THAN          10
#define R300_PVS_DST_MATH_INST     (1 << 6)
#   define R300_ME_RECIP_DX               6
#define R300_PVS_DST_MACRO_INST    (1 << 7)
#   define R300_PVS_MACRO_OP_2CLK_MADD    0
#define R300_PVS_DST_REG_TYPE(x) ((x) << 8)
#   define R300_PVS_DST_REG_TEMPORARY     0
#   define R300_PVS_DST_REG_A0            1
#   define R300_PVS_DST_REG_OUT           2
#   define R300_PVS_DST_REG_OUT_REPL_X    3
#   define R300_PVS_DST_REG_ALT_TEMPORARY 4
#   define R300_PVS_DST_REG_INPUT         5
#define R300_PVS_DST_OFFSET(x)   ((x) << 13)
#define R300_PVS_DST_WE(x)       ((x) << 20)
#define R300_PVS_DST_WE_XYZW     (0xf << 20)

#define R300_PVS_SRC_REG_TYPE(x) ((x) << 0)
#   define R300_PVS_SRC_REG_TEMPORARY     0
#   define R300_PVS_SRC_REG_INPUT         1
#   define R300_PVS_SRC_REG_CONSTANT      2
#   define R300_PVS_SRC_REG_ALT_TEMPORARY 3
#define R300_PVS_SRC_OFFSET(x)   ((x) << 5)
#define R300_PVS_SRC_SWIZZLE(x)  ((x) << 13)
#   define R300_PVS_SRC_SELECT_X          0
#   define R300_PVS_SRC_SELECT_Y          1
#   define R300_PVS_SRC_SELECT_Z          2
#   define R300_PVS_SRC_SELECT_W          3
#   define R300_PVS_SRC_SELECT_FORCE_0    4
#   define R300_PVS_SRC_SELECT_FORCE_1    5
#   define R300_PVS_SRC_SWIZZLE_XYZW \
    ((R300_PVS_SRC_SELECT_X | (R300_PVS_SRC_SELECT_Y << 3) | \
     (R300_PVS_SRC_SELECT_Z << 6) | (R300_PVS_SRC_SELECT_W << 9)) << 13)
#   define R300_PVS_SRC_SWIZZLE_ZERO \
    ((R300_PVS_SRC_SELECT_FORCE_0 | (R300_PVS_SRC_SELECT_FORCE_0 << 3) | \
     (R300_PVS_SRC_SELECT_FORCE_0 << 6) | \
      (R300_PVS_SRC_SELECT_FORCE_0 << 9)) << 13)
#   define R300_PVS_SRC_SWIZZLE_ONE \
    ((R300_PVS_SRC_SELECT_FORCE_1 | (R300_PVS_SRC_SELECT_FORCE_1 << 3) | \
     (R300_PVS_SRC_SELECT_FORCE_1 << 6) | \
      (R300_PVS_SRC_SELECT_FORCE_1 << 9)) << 13)
#define R300_PVS_MODIFIER_X        (1 << 25)
#define R300_PVS_MODIFIER_Y        (1 << 26)
#define R300_PVS_MODIFIER_Z        (1 << 27)
#define R300_PVS_MODIFIER_W        (1 << 28)
#define R300_PVS_NEGATE_XYZW \
    (R300_PVS_MODIFIER_X | R300_PVS_MODIFIER_Y | \
     R300_PVS_MODIFIER_Z | R300_PVS_MODIFIER_W)

struct r300_vertex_program_code r300_passthrough_vertex_shader = {
    .length = 8, /* two instructions */

    /* MOV out[0], in[0] */
    .body.d[0] = R300_PVS_DST_OPCODE(R300_VE_ADD) |
        R300_PVS_DST_REG_TYPE(R300_PVS_DST_REG_OUT) |
        R300_PVS_DST_OFFSET(0) | R300_PVS_DST_WE_XYZW,
    .body.d[1] = R300_PVS_SRC_REG_TYPE(R300_PVS_SRC_REG_INPUT) |
        R300_PVS_SRC_OFFSET(0) | R300_PVS_SRC_SWIZZLE_XYZW,
    .body.d[2] = R300_PVS_SRC_SWIZZLE_ZERO,
    .body.d[3] = 0x0,

    /* MOV out[1], in[1] */
    .body.d[4] = R300_PVS_DST_OPCODE(R300_VE_ADD) |
        R300_PVS_DST_REG_TYPE(R300_PVS_DST_REG_OUT) |
        R300_PVS_DST_OFFSET(1) | R300_PVS_DST_WE_XYZW,
    .body.d[5] = R300_PVS_SRC_REG_TYPE(R300_PVS_SRC_REG_INPUT) |
        R300_PVS_SRC_OFFSET(1) | R300_PVS_SRC_SWIZZLE_XYZW,
    .body.d[6] = R300_PVS_SRC_SWIZZLE_ZERO,
    .body.d[7] = 0x0,

    .inputs[0] = 0,
    .inputs[1] = 1,
    .outputs[0] = 0,
    .outputs[1] = 1,

    .InputsRead = 3,
    .OutputsWritten = 3
};

