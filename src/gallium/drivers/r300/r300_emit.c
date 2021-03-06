/*
 * Copyright 2008 Corbin Simpson <MostAwesomeDude@gmail.com>
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

/* r300_emit: Functions for emitting state. */

#include "r300_emit.h"

#include "r300_fs.h"
#include "r300_state_derived.h"
#include "r300_vs.h"

void r300_emit_blend_state(struct r300_context* r300,
                           struct r300_blend_state* blend)
{
    CS_LOCALS(r300);
    BEGIN_CS(7);
    OUT_CS_REG_SEQ(R300_RB3D_CBLEND, 2);
    OUT_CS(blend->blend_control);
    OUT_CS(blend->alpha_blend_control);
    OUT_CS_REG(R300_RB3D_ROPCNTL, blend->rop);
    OUT_CS_REG(R300_RB3D_DITHER_CTL, blend->dither);
    END_CS;
}

void r300_emit_blend_color_state(struct r300_context* r300,
                                 struct r300_blend_color_state* bc)
{
    struct r300_screen* r300screen = r300_screen(r300->context.screen);
    CS_LOCALS(r300);

    if (r300screen->caps->is_r500) {
        BEGIN_CS(3);
        OUT_CS_REG_SEQ(R500_RB3D_CONSTANT_COLOR_AR, 2);
        OUT_CS(bc->blend_color_red_alpha);
        OUT_CS(bc->blend_color_green_blue);
        END_CS;
    } else {
        BEGIN_CS(2);
        OUT_CS_REG(R300_RB3D_BLEND_COLOR, bc->blend_color);
        END_CS;
    }
}

void r300_emit_clip_state(struct r300_context* r300,
                          struct pipe_clip_state* clip)
{
    int i;
    struct r300_screen* r300screen = r300_screen(r300->context.screen);
    CS_LOCALS(r300);

    if (r300screen->caps->has_tcl) {
        BEGIN_CS(5 + (6 * 4));
        OUT_CS_REG(R300_VAP_PVS_VECTOR_INDX_REG,
                (r300screen->caps->is_r500 ?
                 R500_PVS_UCP_START : R300_PVS_UCP_START));
        OUT_CS_ONE_REG(R300_VAP_PVS_UPLOAD_DATA, 6 * 4);
        for (i = 0; i < 6; i++) {
            OUT_CS_32F(clip->ucp[i][0]);
            OUT_CS_32F(clip->ucp[i][1]);
            OUT_CS_32F(clip->ucp[i][2]);
            OUT_CS_32F(clip->ucp[i][3]);
        }
        OUT_CS_REG(R300_VAP_CLIP_CNTL, ((1 << clip->nr) - 1) |
                R300_PS_UCP_MODE_CLIP_AS_TRIFAN);
        END_CS;
    } else {
        BEGIN_CS(2);
        OUT_CS_REG(R300_VAP_CLIP_CNTL, R300_CLIP_DISABLE);
        END_CS;
    }

}

void r300_emit_dsa_state(struct r300_context* r300,
                           struct r300_dsa_state* dsa)
{
    struct r300_screen* r300screen = r300_screen(r300->context.screen);
    CS_LOCALS(r300);

    BEGIN_CS(r300screen->caps->is_r500 ? 8 : 8);
    OUT_CS_REG(R300_FG_ALPHA_FUNC, dsa->alpha_function);
    /* XXX figure out the r300 counterpart for this */
    if (r300screen->caps->is_r500) {
        /* OUT_CS_REG(R500_FG_ALPHA_VALUE, dsa->alpha_reference); */
    }
    OUT_CS_REG_SEQ(R300_ZB_CNTL, 3);
    OUT_CS(dsa->z_buffer_control);
    OUT_CS(dsa->z_stencil_control);
    OUT_CS(dsa->stencil_ref_mask);
    OUT_CS_REG(R300_ZB_ZTOP, dsa->z_buffer_top);
    if (r300screen->caps->is_r500) {
        /* OUT_CS_REG(R500_ZB_STENCILREFMASK_BF, dsa->stencil_ref_bf); */
    }
    END_CS;
}

static const float * get_shader_constant(
    struct r300_context * r300,
    struct rc_constant * constant,
    struct r300_constant_buffer * externals)
{
    static const float zero[4] = { 0.0, 0.0, 0.0, 0.0 };
    switch(constant->Type) {
        case RC_CONSTANT_EXTERNAL:
            return externals->constants[constant->u.External];

        case RC_CONSTANT_IMMEDIATE:
            return constant->u.Immediate;

        default:
            debug_printf("r300: Implementation error: Unhandled constant type %i\n",
                constant->Type);
            return zero;
    }
}

/* Convert a normal single-precision float into the 7.16 format
 * used by the R300 fragment shader.
 */
static uint32_t pack_float24(float f)
{
    union {
        float fl;
        uint32_t u;
    } u;
    float mantissa;
    int exponent;
    uint32_t float24 = 0;

    if (f == 0.0)
        return 0;

    u.fl = f;

    mantissa = frexpf(f, &exponent);

    /* Handle -ve */
    if (mantissa < 0) {
        float24 |= (1 << 23);
        mantissa = mantissa * -1.0;
    }
    /* Handle exponent, bias of 63 */
    exponent += 62;
    float24 |= (exponent << 16);
    /* Kill 7 LSB of mantissa */
    float24 |= (u.u & 0x7FFFFF) >> 7;

    return float24;
}

void r300_emit_fragment_program_code(struct r300_context* r300,
                                     struct rX00_fragment_program_code* generic_code,
                                     struct r300_constant_buffer* externals)
{
    struct r300_fragment_program_code * code = &generic_code->code.r300;
    struct rc_constant_list * constants = &generic_code->constants;
    int i;
    CS_LOCALS(r300);

    BEGIN_CS(15 +
             code->alu.length * 4 +
             (code->tex.length ? (1 + code->tex.length) : 0) +
             (constants->Count ? (1 + constants->Count * 4) : 0));

    OUT_CS_REG(R300_US_CONFIG, code->config);
    OUT_CS_REG(R300_US_PIXSIZE, code->pixsize);
    OUT_CS_REG(R300_US_CODE_OFFSET, code->code_offset);

    OUT_CS_REG_SEQ(R300_US_CODE_ADDR_0, 4);
    for(i = 0; i < 4; ++i)
        OUT_CS(code->code_addr[i]);

    OUT_CS_REG_SEQ(R300_US_ALU_RGB_INST_0, code->alu.length);
    for (i = 0; i < code->alu.length; i++)
        OUT_CS(code->alu.inst[i].rgb_inst);

    OUT_CS_REG_SEQ(R300_US_ALU_RGB_ADDR_0, code->alu.length);
    for (i = 0; i < code->alu.length; i++)
        OUT_CS(code->alu.inst[i].rgb_addr);

    OUT_CS_REG_SEQ(R300_US_ALU_ALPHA_INST_0, code->alu.length);
    for (i = 0; i < code->alu.length; i++)
        OUT_CS(code->alu.inst[i].alpha_inst);

    OUT_CS_REG_SEQ(R300_US_ALU_ALPHA_ADDR_0, code->alu.length);
    for (i = 0; i < code->alu.length; i++)
        OUT_CS(code->alu.inst[i].alpha_addr);

    if (code->tex.length) {
        OUT_CS_REG_SEQ(R300_US_TEX_INST_0, code->tex.length);
        for(i = 0; i < code->tex.length; ++i)
            OUT_CS(code->tex.inst[i]);
    }

    if (constants->Count) {
        OUT_CS_ONE_REG(R300_PFS_PARAM_0_X, constants->Count * 4);
        for(i = 0; i < constants->Count; ++i) {
            const float * data = get_shader_constant(r300, &constants->Constants[i], externals);
            OUT_CS(pack_float24(data[0]));
            OUT_CS(pack_float24(data[1]));
            OUT_CS(pack_float24(data[2]));
            OUT_CS(pack_float24(data[3]));
        }
    }

    END_CS;
}

void r500_emit_fragment_program_code(struct r300_context* r300,
                                     struct rX00_fragment_program_code* generic_code,
                                     struct r300_constant_buffer* externals)
{
    struct r500_fragment_program_code * code = &generic_code->code.r500;
    struct rc_constant_list * constants = &generic_code->constants;
    int i;
    CS_LOCALS(r300);

    BEGIN_CS(13 +
             ((code->inst_end + 1) * 6) +
             (constants->Count ? (3 + (constants->Count * 4)) : 0));
    OUT_CS_REG(R500_US_CONFIG, 0);
    OUT_CS_REG(R500_US_PIXSIZE, code->max_temp_idx);
    OUT_CS_REG(R500_US_CODE_RANGE,
               R500_US_CODE_RANGE_ADDR(0) | R500_US_CODE_RANGE_SIZE(code->inst_end));
    OUT_CS_REG(R500_US_CODE_OFFSET, 0);
    OUT_CS_REG(R500_US_CODE_ADDR,
               R500_US_CODE_START_ADDR(0) | R500_US_CODE_END_ADDR(code->inst_end));

    OUT_CS_REG(R500_GA_US_VECTOR_INDEX, R500_GA_US_VECTOR_INDEX_TYPE_INSTR);
    OUT_CS_ONE_REG(R500_GA_US_VECTOR_DATA, (code->inst_end + 1) * 6);
    for (i = 0; i <= code->inst_end; i++) {
        OUT_CS(code->inst[i].inst0);
        OUT_CS(code->inst[i].inst1);
        OUT_CS(code->inst[i].inst2);
        OUT_CS(code->inst[i].inst3);
        OUT_CS(code->inst[i].inst4);
        OUT_CS(code->inst[i].inst5);
    }

    if (constants->Count) {
        OUT_CS_REG(R500_GA_US_VECTOR_INDEX, R500_GA_US_VECTOR_INDEX_TYPE_CONST);
        OUT_CS_ONE_REG(R500_GA_US_VECTOR_DATA, constants->Count * 4);
        for (i = 0; i < constants->Count; i++) {
            const float * data = get_shader_constant(r300, &constants->Constants[i], externals);
            OUT_CS_32F(data[0]);
            OUT_CS_32F(data[1]);
            OUT_CS_32F(data[2]);
            OUT_CS_32F(data[3]);
        }
    }

    END_CS;
}

void r300_emit_fb_state(struct r300_context* r300,
                        struct pipe_framebuffer_state* fb)
{
    struct r300_texture* tex;
    unsigned pixpitch;
    int i;
    CS_LOCALS(r300);

    BEGIN_CS((10 * fb->nr_cbufs) + (fb->zsbuf ? 10 : 0) + 4);
    for (i = 0; i < fb->nr_cbufs; i++) {
        tex = (struct r300_texture*)fb->cbufs[i]->texture;
        assert(tex && tex->buffer && "cbuf is marked, but NULL!");
        pixpitch = tex->stride / tex->tex.block.size;

        OUT_CS_REG_SEQ(R300_RB3D_COLOROFFSET0 + (4 * i), 1);
        OUT_CS_RELOC(tex->buffer, 0, 0, RADEON_GEM_DOMAIN_VRAM, 0);

        OUT_CS_REG_SEQ(R300_RB3D_COLORPITCH0 + (4 * i), 1);
        OUT_CS_RELOC(tex->buffer, pixpitch |
                     r300_translate_colorformat(tex->tex.format), 0,
                     RADEON_GEM_DOMAIN_VRAM, 0);

        OUT_CS_REG(R300_US_OUT_FMT_0 + (4 * i),
            r300_translate_out_fmt(fb->cbufs[i]->format));
    }

    if (fb->zsbuf) {
        tex = (struct r300_texture*)fb->zsbuf->texture;
        assert(tex && tex->buffer && "zsbuf is marked, but NULL!");
        pixpitch = tex->stride / tex->tex.block.size;

        OUT_CS_REG_SEQ(R300_ZB_DEPTHOFFSET, 1);
        OUT_CS_RELOC(tex->buffer, 0, 0, RADEON_GEM_DOMAIN_VRAM, 0);

        OUT_CS_REG(R300_ZB_FORMAT, r300_translate_zsformat(tex->tex.format));

        OUT_CS_REG_SEQ(R300_ZB_DEPTHPITCH, 1);
        OUT_CS_RELOC(tex->buffer, pixpitch, 0, RADEON_GEM_DOMAIN_VRAM, 0);
    }

    OUT_CS_REG(R300_RB3D_DSTCACHE_CTLSTAT,
        R300_RB3D_DSTCACHE_CTLSTAT_DC_FREE_FREE_3D_TAGS |
        R300_RB3D_DSTCACHE_CTLSTAT_DC_FLUSH_FLUSH_DIRTY_3D);
    OUT_CS_REG(R300_ZB_ZCACHE_CTLSTAT,
        R300_ZB_ZCACHE_CTLSTAT_ZC_FLUSH_FLUSH_AND_FREE |
        R300_ZB_ZCACHE_CTLSTAT_ZC_FREE_FREE);
    END_CS;
}

void r300_emit_query_begin(struct r300_context* r300,
                           struct r300_query* query)
{
    CS_LOCALS(r300);

    /* XXX This will almost certainly not return good results
     * for overlapping queries. */
    BEGIN_CS(2);
    OUT_CS_REG(R300_ZB_ZPASS_DATA, 0);
    END_CS;
}

void r300_emit_query_end(struct r300_context* r300,
                         struct r300_query* query)
{
    struct r300_capabilities* caps = r300_screen(r300->context.screen)->caps;
    CS_LOCALS(r300);

    if (!r300->winsys->add_buffer(r300->winsys, r300->oqbo,
                0, RADEON_GEM_DOMAIN_GTT)) {
        debug_printf("r300: There wasn't room for the OQ buffer!?"
                " Oh noes!\n");
    }

    assert(caps->num_frag_pipes);
    BEGIN_CS(6 * caps->num_frag_pipes + 2);
    /* I'm not so sure I like this switch, but it's hard to be elegant
     * when there's so many special cases...
     *
     * So here's the basic idea. For each pipe, enable writes to it only,
     * then put out the relocation for ZPASS_ADDR, taking into account a
     * 4-byte offset for each pipe. RV380 and older are special; they have
     * only two pipes, and the second pipe's enable is on bit 3, not bit 1,
     * so there's a chipset cap for that. */
    switch (caps->num_frag_pipes) {
        case 4:
            /* pipe 3 only */
            OUT_CS_REG(R300_SU_REG_DEST, 1 << 3);
            OUT_CS_REG_SEQ(R300_ZB_ZPASS_ADDR, 1);
            OUT_CS_RELOC(r300->oqbo, query->offset + (sizeof(uint32_t) * 3),
                    0, RADEON_GEM_DOMAIN_GTT, 0);
        case 3:
            /* pipe 2 only */
            OUT_CS_REG(R300_SU_REG_DEST, 1 << 2);
            OUT_CS_REG_SEQ(R300_ZB_ZPASS_ADDR, 1);
            OUT_CS_RELOC(r300->oqbo, query->offset + (sizeof(uint32_t) * 2),
                    0, RADEON_GEM_DOMAIN_GTT, 0);
        case 2:
            /* pipe 1 only */
            /* As mentioned above, accomodate RV380 and older. */
            OUT_CS_REG(R300_SU_REG_DEST,
                    1 << (caps->high_second_pipe ? 3 : 1));
            OUT_CS_REG_SEQ(R300_ZB_ZPASS_ADDR, 1);
            OUT_CS_RELOC(r300->oqbo, query->offset + (sizeof(uint32_t) * 1),
                    0, RADEON_GEM_DOMAIN_GTT, 0);
        case 1:
            /* pipe 0 only */
            OUT_CS_REG(R300_SU_REG_DEST, 1 << 0);
            OUT_CS_REG_SEQ(R300_ZB_ZPASS_ADDR, 1);
            OUT_CS_RELOC(r300->oqbo, query->offset + (sizeof(uint32_t) * 0),
                    0, RADEON_GEM_DOMAIN_GTT, 0);
        default:
            debug_printf("r300: Implementation error: Chipset reports %d"
                    " pixel pipes!\n", caps->num_frag_pipes);
            assert(0);
    }

    /* And, finally, reset it to normal... */
    OUT_CS_REG(R300_SU_REG_DEST, 0xF);
    END_CS;

}

void r300_emit_rs_state(struct r300_context* r300, struct r300_rs_state* rs)
{
    CS_LOCALS(r300);

    BEGIN_CS(20);
    OUT_CS_REG(R300_VAP_CNTL_STATUS, rs->vap_control_status);
    OUT_CS_REG(R300_GA_POINT_SIZE, rs->point_size);
    OUT_CS_REG_SEQ(R300_GA_POINT_MINMAX, 2);
    OUT_CS(rs->point_minmax);
    OUT_CS(rs->line_control);
    OUT_CS_REG_SEQ(R300_SU_POLY_OFFSET_FRONT_SCALE, 6);
    OUT_CS(rs->depth_scale_front);
    OUT_CS(rs->depth_offset_front);
    OUT_CS(rs->depth_scale_back);
    OUT_CS(rs->depth_offset_back);
    OUT_CS(rs->polygon_offset_enable);
    OUT_CS(rs->cull_mode);
    OUT_CS_REG(R300_GA_LINE_STIPPLE_CONFIG, rs->line_stipple_config);
    OUT_CS_REG(R300_GA_LINE_STIPPLE_VALUE, rs->line_stipple_value);
    OUT_CS_REG(R300_GA_COLOR_CONTROL, rs->color_control);
    END_CS;
}

void r300_emit_rs_block_state(struct r300_context* r300,
                              struct r300_rs_block* rs)
{
    int i;
    struct r300_screen* r300screen = r300_screen(r300->context.screen);
    CS_LOCALS(r300);

    BEGIN_CS(21);
    if (r300screen->caps->is_r500) {
        OUT_CS_REG_SEQ(R500_RS_IP_0, 8);
    } else {
        OUT_CS_REG_SEQ(R300_RS_IP_0, 8);
    }
    for (i = 0; i < 8; i++) {
        OUT_CS(rs->ip[i]);
        /* debug_printf("ip %d: 0x%08x\n", i, rs->ip[i]); */
    }

    OUT_CS_REG_SEQ(R300_RS_COUNT, 2);
    OUT_CS(rs->count);
    OUT_CS(rs->inst_count);

    if (r300screen->caps->is_r500) {
        OUT_CS_REG_SEQ(R500_RS_INST_0, 8);
    } else {
        OUT_CS_REG_SEQ(R300_RS_INST_0, 8);
    }
    for (i = 0; i < 8; i++) {
        OUT_CS(rs->inst[i]);
        /* debug_printf("inst %d: 0x%08x\n", i, rs->inst[i]); */
    }

    /* debug_printf("count: 0x%08x inst_count: 0x%08x\n", rs->count,
     *        rs->inst_count); */

    END_CS;
}

void r300_emit_scissor_state(struct r300_context* r300,
                             struct r300_scissor_state* scissor)
{
    CS_LOCALS(r300);

    BEGIN_CS(3);
    OUT_CS_REG_SEQ(R300_SC_SCISSORS_TL, 2);
    OUT_CS(scissor->scissor_top_left);
    OUT_CS(scissor->scissor_bottom_right);
    END_CS;
}

void r300_emit_texture(struct r300_context* r300,
                       struct r300_sampler_state* sampler,
                       struct r300_texture* tex,
                       unsigned offset)
{
    CS_LOCALS(r300);

    BEGIN_CS(16);
    OUT_CS_REG(R300_TX_FILTER0_0 + (offset * 4), sampler->filter0);
    OUT_CS_REG(R300_TX_FILTER1_0 + (offset * 4), sampler->filter1);
    OUT_CS_REG(R300_TX_BORDER_COLOR_0 + (offset * 4), sampler->border_color);

    OUT_CS_REG(R300_TX_FORMAT0_0 + (offset * 4), tex->state.format0);
    OUT_CS_REG(R300_TX_FORMAT1_0 + (offset * 4), tex->state.format1);
    OUT_CS_REG(R300_TX_FORMAT2_0 + (offset * 4), tex->state.format2);
    OUT_CS_REG_SEQ(R300_TX_OFFSET_0 + (offset * 4), 1);
    OUT_CS_RELOC(tex->buffer, 0,
            RADEON_GEM_DOMAIN_GTT | RADEON_GEM_DOMAIN_VRAM, 0, 0);
    END_CS;
}

void r300_emit_vertex_buffer(struct r300_context* r300)
{
    CS_LOCALS(r300);

    DBG(r300, DBG_DRAW, "r300: Preparing vertex buffer %p for render, "
            "vertex size %d\n", r300->vbo,
            r300->vertex_info.vinfo.size);
    /* Set the pointer to our vertex buffer. The emitted values are this:
     * PACKET3 [3D_LOAD_VBPNTR]
     * COUNT   [1]
     * FORMAT  [size | stride << 8]
     * OFFSET  [offset into BO]
     * VBPNTR  [relocated BO]
     */
    BEGIN_CS(7);
    OUT_CS_PKT3(R300_PACKET3_3D_LOAD_VBPNTR, 3);
    OUT_CS(1);
    OUT_CS(r300->vertex_info.vinfo.size |
            (r300->vertex_info.vinfo.size << 8));
    OUT_CS(r300->vbo_offset);
    OUT_CS_RELOC(r300->vbo, 0, RADEON_GEM_DOMAIN_GTT, 0, 0);
    END_CS;
}

void r300_emit_vertex_format_state(struct r300_context* r300)
{
    int i;
    CS_LOCALS(r300);

    BEGIN_CS(26);
    OUT_CS_REG(R300_VAP_VTX_SIZE, r300->vertex_info.vinfo.size);

    OUT_CS_REG_SEQ(R300_VAP_VTX_STATE_CNTL, 2);
    OUT_CS(r300->vertex_info.vinfo.hwfmt[0]);
    OUT_CS(r300->vertex_info.vinfo.hwfmt[1]);
    OUT_CS_REG_SEQ(R300_VAP_OUTPUT_VTX_FMT_0, 2);
    OUT_CS(r300->vertex_info.vinfo.hwfmt[2]);
    OUT_CS(r300->vertex_info.vinfo.hwfmt[3]);
    /* for (i = 0; i < 4; i++) {
     *    debug_printf("hwfmt%d: 0x%08x\n", i,
     *            r300->vertex_info.vinfo.hwfmt[i]);
     * } */

    OUT_CS_REG_SEQ(R300_VAP_PROG_STREAM_CNTL_0, 8);
    for (i = 0; i < 8; i++) {
        OUT_CS(r300->vertex_info.vap_prog_stream_cntl[i]);
        /* debug_printf("prog_stream_cntl%d: 0x%08x\n", i,
         *        r300->vertex_info.vap_prog_stream_cntl[i]); */
    }
    OUT_CS_REG_SEQ(R300_VAP_PROG_STREAM_CNTL_EXT_0, 8);
    for (i = 0; i < 8; i++) {
        OUT_CS(r300->vertex_info.vap_prog_stream_cntl_ext[i]);
        /* debug_printf("prog_stream_cntl_ext%d: 0x%08x\n", i,
         *        r300->vertex_info.vap_prog_stream_cntl_ext[i]); */
    }
    END_CS;
}

void r300_emit_vertex_program_code(struct r300_context* r300,
                                   struct r300_vertex_program_code* code,
                                   struct r300_constant_buffer* constants)
{
    int i;
    struct r300_screen* r300screen = r300_screen(r300->context.screen);
    unsigned instruction_count = code->length / 4;
    CS_LOCALS(r300);

    if (!r300screen->caps->has_tcl) {
        debug_printf("r300: Implementation error: emit_vertex_shader called,"
                " but has_tcl is FALSE!\n");
        return;
    }

    if (code->constants.Count) {
        BEGIN_CS(14 + code->length + (code->constants.Count * 4));
    } else {
        BEGIN_CS(11 + code->length);
    }

    /* R300_VAP_PVS_CODE_CNTL_0
     * R300_VAP_PVS_CONST_CNTL
     * R300_VAP_PVS_CODE_CNTL_1
     * See the r5xx docs for instructions on how to use these.
     * XXX these could be optimized to select better values... */
    OUT_CS_REG_SEQ(R300_VAP_PVS_CODE_CNTL_0, 3);
    OUT_CS(R300_PVS_FIRST_INST(0) |
            R300_PVS_XYZW_VALID_INST(instruction_count - 1) |
            R300_PVS_LAST_INST(instruction_count - 1));
    OUT_CS(R300_PVS_MAX_CONST_ADDR(code->constants.Count - 1));
    OUT_CS(instruction_count - 1);

    OUT_CS_REG(R300_VAP_PVS_VECTOR_INDX_REG, 0);
    OUT_CS_ONE_REG(R300_VAP_PVS_UPLOAD_DATA, code->length);
    for (i = 0; i < code->length; i++)
        OUT_CS(code->body.d[i]);

    if (code->constants.Count) {
        OUT_CS_REG(R300_VAP_PVS_VECTOR_INDX_REG,
                (r300screen->caps->is_r500 ?
                 R500_PVS_CONST_START : R300_PVS_CONST_START));
        OUT_CS_ONE_REG(R300_VAP_PVS_UPLOAD_DATA, code->constants.Count * 4);
        for (i = 0; i < code->constants.Count; i++) {
            const float * data = get_shader_constant(r300, &code->constants.Constants[i], constants);
            OUT_CS_32F(data[0]);
            OUT_CS_32F(data[1]);
            OUT_CS_32F(data[2]);
            OUT_CS_32F(data[3]);
        }
    }

    OUT_CS_REG(R300_VAP_CNTL, R300_PVS_NUM_SLOTS(10) |
            R300_PVS_NUM_CNTLRS(5) |
            R300_PVS_NUM_FPUS(r300screen->caps->num_vert_fpus) |
            R300_PVS_VF_MAX_VTX_NUM(12));
    OUT_CS_REG(R300_VAP_PVS_STATE_FLUSH_REG, 0x0);
    END_CS;
}

void r300_emit_vertex_shader(struct r300_context* r300,
                             struct r300_vertex_shader* vs)
{
    r300_emit_vertex_program_code(r300, &vs->code, &r300->shader_constants[PIPE_SHADER_VERTEX]);
}

void r300_emit_viewport_state(struct r300_context* r300,
                              struct r300_viewport_state* viewport)
{
    CS_LOCALS(r300);

    BEGIN_CS(9);
    OUT_CS_REG_SEQ(R300_SE_VPORT_XSCALE, 6);
    OUT_CS_32F(viewport->xscale);
    OUT_CS_32F(viewport->xoffset);
    OUT_CS_32F(viewport->yscale);
    OUT_CS_32F(viewport->yoffset);
    OUT_CS_32F(viewport->zscale);
    OUT_CS_32F(viewport->zoffset);

    if (r300->rs_state->enable_vte) {
        OUT_CS_REG(R300_VAP_VTE_CNTL, viewport->vte_control);
    } else {
        OUT_CS_REG(R300_VAP_VTE_CNTL, 0);
    }
    END_CS;
}

void r300_flush_textures(struct r300_context* r300)
{
    CS_LOCALS(r300);

    BEGIN_CS(4);
    OUT_CS_REG(R300_TX_INVALTAGS, 0);
    OUT_CS_REG(R300_TX_ENABLE, (1 << r300->texture_count) - 1);
    END_CS;
}

/* Emit all dirty state. */
void r300_emit_dirty_state(struct r300_context* r300)
{
    struct r300_screen* r300screen = r300_screen(r300->context.screen);
    struct r300_texture* tex;
    int i, dirty_tex = 0;
    boolean invalid = FALSE;

    if (!(r300->dirty_state)) {
        return;
    }

    r300_update_derived_state(r300);

    /* XXX check size */
validate:
    /* Color buffers... */
    for (i = 0; i < r300->framebuffer_state.nr_cbufs; i++) {
        tex = (struct r300_texture*)r300->framebuffer_state.cbufs[i]->texture;
        assert(tex && tex->buffer && "cbuf is marked, but NULL!");
        if (!r300->winsys->add_buffer(r300->winsys, tex->buffer,
                    0, RADEON_GEM_DOMAIN_VRAM)) {
            r300->context.flush(&r300->context, 0, NULL);
            goto validate;
        }
    }
    /* ...depth buffer... */
    if (r300->framebuffer_state.zsbuf) {
        tex = (struct r300_texture*)r300->framebuffer_state.zsbuf->texture;
        assert(tex && tex->buffer && "zsbuf is marked, but NULL!");
        if (!r300->winsys->add_buffer(r300->winsys, tex->buffer,
                    0, RADEON_GEM_DOMAIN_VRAM)) {
            r300->context.flush(&r300->context, 0, NULL);
            goto validate;
        }
    }
    /* ...textures... */
    for (i = 0; i < r300->texture_count; i++) {
        tex = r300->textures[i];
        assert(tex && tex->buffer && "texture is marked, but NULL!");
        if (!r300->winsys->add_buffer(r300->winsys, tex->buffer,
                    RADEON_GEM_DOMAIN_GTT | RADEON_GEM_DOMAIN_VRAM, 0)) {
            r300->context.flush(&r300->context, 0, NULL);
            goto validate;
        }
    }
    /* ...occlusion query buffer... */
    if (!r300->winsys->add_buffer(r300->winsys, r300->oqbo,
                0, RADEON_GEM_DOMAIN_GTT)) {
        r300->context.flush(&r300->context, 0, NULL);
        goto validate;
    }
    /* ...and vertex buffer. */
    if (r300->vbo) {
        if (!r300->winsys->add_buffer(r300->winsys, r300->vbo,
                    RADEON_GEM_DOMAIN_GTT, 0)) {
            r300->context.flush(&r300->context, 0, NULL);
            goto validate;
        }
    } else {
        debug_printf("No VBO while emitting dirty state!\n");
    }
    if (!r300->winsys->validate(r300->winsys)) {
        r300->context.flush(&r300->context, 0, NULL);
        if (invalid) {
            /* Well, hell. */
            debug_printf("r300: Stuck in validation loop, gonna quit now.");
            exit(1);
        }
        invalid = TRUE;
        goto validate;
    }

    if (r300->dirty_state & R300_NEW_BLEND) {
        r300_emit_blend_state(r300, r300->blend_state);
        r300->dirty_state &= ~R300_NEW_BLEND;
    }

    if (r300->dirty_state & R300_NEW_BLEND_COLOR) {
        r300_emit_blend_color_state(r300, r300->blend_color_state);
        r300->dirty_state &= ~R300_NEW_BLEND_COLOR;
    }

    if (r300->dirty_state & R300_NEW_CLIP) {
        r300_emit_clip_state(r300, &r300->clip_state);
        r300->dirty_state &= ~R300_NEW_CLIP;
    }

    if (r300->dirty_state & R300_NEW_DSA) {
        r300_emit_dsa_state(r300, r300->dsa_state);
        r300->dirty_state &= ~R300_NEW_DSA;
    }

    if (r300->dirty_state & R300_NEW_FRAGMENT_SHADER) {
        if (r300screen->caps->is_r500) {
            r500_emit_fragment_program_code(r300, &r300->fs->code, &r300->shader_constants[PIPE_SHADER_FRAGMENT]);
        } else {
            r300_emit_fragment_program_code(r300, &r300->fs->code, &r300->shader_constants[PIPE_SHADER_FRAGMENT]);
        }
        r300->dirty_state &= ~R300_NEW_FRAGMENT_SHADER;
    }

    if (r300->dirty_state & R300_NEW_FRAMEBUFFERS) {
        r300_emit_fb_state(r300, &r300->framebuffer_state);
        r300->dirty_state &= ~R300_NEW_FRAMEBUFFERS;
    }

    if (r300->dirty_state & R300_NEW_RASTERIZER) {
        r300_emit_rs_state(r300, r300->rs_state);
        r300->dirty_state &= ~R300_NEW_RASTERIZER;
    }

    if (r300->dirty_state & R300_NEW_RS_BLOCK) {
        r300_emit_rs_block_state(r300, r300->rs_block);
        r300->dirty_state &= ~R300_NEW_RS_BLOCK;
    }

    if (r300->dirty_state & R300_NEW_SCISSOR) {
        r300_emit_scissor_state(r300, r300->scissor_state);
        r300->dirty_state &= ~R300_NEW_SCISSOR;
    }

    /* Samplers and textures are tracked separately but emitted together. */
    if (r300->dirty_state &
            (R300_ANY_NEW_SAMPLERS | R300_ANY_NEW_TEXTURES)) {
        for (i = 0; i < MIN2(r300->sampler_count, r300->texture_count); i++) {
            if (r300->dirty_state &
                    ((R300_NEW_SAMPLER << i) | (R300_NEW_TEXTURE << i))) {
                r300_emit_texture(r300,
                        r300->sampler_states[i],
                        r300->textures[i],
                        i);
                r300->dirty_state &=
                    ~((R300_NEW_SAMPLER << i) | (R300_NEW_TEXTURE << i));
                dirty_tex++;
            }
        }
        r300->dirty_state &= ~(R300_ANY_NEW_SAMPLERS | R300_ANY_NEW_TEXTURES);
    }

    if (r300->dirty_state & R300_NEW_VIEWPORT) {
        r300_emit_viewport_state(r300, r300->viewport_state);
        r300->dirty_state &= ~R300_NEW_VIEWPORT;
    }

    if (dirty_tex) {
        r300_flush_textures(r300);
    }

    if (r300->dirty_state & R300_NEW_VERTEX_FORMAT) {
        r300_emit_vertex_format_state(r300);
        r300->dirty_state &= ~R300_NEW_VERTEX_FORMAT;
    }

    if (r300->dirty_state & R300_NEW_VERTEX_SHADER) {
        r300_emit_vertex_shader(r300, r300->vs);
        r300->dirty_state &= ~R300_NEW_VERTEX_SHADER;
    }

    /* XXX
    assert(r300->dirty_state == 0);
    */

    /* Finally, emit the VBO. */
    r300_emit_vertex_buffer(r300);

    r300->dirty_hw++;
}
