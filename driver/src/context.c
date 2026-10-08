/*
 * context.c - driver initialisation, memory helpers and fixed-function state.
 */
#include <string.h>
#include "internal.h"

#define PICA_DEFAULT_CMDBUF_BYTES (256 * 1024)

/* COLOR_OPERATION: bits 0-1 fragment mode, bit 8 blend (1) vs logic op (0),
 * bits 16-23 must be 0xE4 (as programmed by the official SDK). */
#define COLOR_OP_BASE   0x00E40000u
#define COLOR_OP_BLEND  0x00000100u

static void emit_default_state(pica_context* ctx)
{
    pica_texenv env;

    /* Rasterizer */
    pica_cull(ctx, PICA_CULL_NONE);
    pica_depth_map(ctx, true, -1.0f, 0.0f);
    pica_scissor(ctx, PICA_SCISSOR_DISABLE, 0, 0, 0, 0);

    /* Fragment operations */
    ctx->depth_color_mask = 0;
    pica_depth_test(ctx, false, PICA_GREATER, PICA_WRITE_ALL);
    pica_alpha_test(ctx, false, PICA_ALWAYS, 0);
    pica_stencil_test(ctx, false, PICA_ALWAYS, 0, 0xFF, 0);
    pica_stencil_op(ctx, PICA_STENCIL_KEEP, PICA_STENCIL_KEEP, PICA_STENCIL_KEEP);
    pica_blend_color(ctx, 0);
    ctx->logic_op = PICA_LOGICOP_COPY;
    pica_blend(ctx, PICA_BLEND_ADD, PICA_BLEND_ADD, PICA_SRC_ALPHA, PICA_ONE_MINUS_SRC_ALPHA, PICA_SRC_ALPHA,
               PICA_ONE_MINUS_SRC_ALPHA);

    if (pica__reserve(ctx, 16))
    {
        /* Early depth off */
        pica__write_masked(ctx, PICA_REG_EARLYDEPTH_TEST1, 0x1, 0);
        pica__write(ctx, PICA_REG_EARLYDEPTH_TEST2, 0);
        /* Shadow fragment op parameters: scale 0, bias 1 */
        pica__write(ctx, PICA_REG_FRAGOP_SHADOW, pica_f32_to_f16(1.0f));
        /* Fragment lighting off */
        pica__write(ctx, PICA_REG_LIGHTING_ENABLE0, 0);
        pica__write(ctx, PICA_REG_LIGHTING_ENABLE1, 1);
    }

    /* Texture combiners: stage 0 outputs the vertex color, the rest pass
     * the previous stage through. */
    for (int i = 0; i < 6; i++)
    {
        pica_texenv_init(&env);
        if (i == 0)
            pica_texenv_src(&env, PICA_TEV_BOTH, PICA_SRC_PRIMARY_COLOR, 0, 0);
        pica_set_texenv(ctx, i, &env);
    }
    ctx->texenv_update = 0;
    pica_texenv_buffer(ctx, 0, 0, 0xFFFFFFFF);
    pica_fog(ctx, NULL, 0, false);

    /* Texture units off */
    ctx->texunit_config = 1u << 12;
    for (int i = 0; i < 3; i++)
        pica_tex_bind(ctx, i, NULL);
    pica_tex_cache_clear(ctx);
}

int pica_init(pica_context* ctx, const pica_platform* plat, size_t cmdbuf_bytes)
{
    if (!ctx || !plat || !plat->run_cmdlist || !plat->alloc_linear || !plat->virt_to_phys)
        return PICA_ERR_ARG;

    memset(ctx, 0, sizeof(*ctx));
    ctx->plat = *plat;

    if (!cmdbuf_bytes)
        cmdbuf_bytes = PICA_DEFAULT_CMDBUF_BYTES;
    cmdbuf_bytes = (cmdbuf_bytes + 15) & ~(size_t)15;
    if (cmdbuf_bytes < 32 * 1024)
        cmdbuf_bytes = 32 * 1024; /* must hold a full 4096-word shader upload */

    ctx->cmd.buf = (uint32_t*)plat->alloc_linear(plat->user, cmdbuf_bytes, 16);
    if (!ctx->cmd.buf)
        return PICA_ERR_NOMEM;
    ctx->cmd.capacity = (uint32_t)(cmdbuf_bytes / sizeof(uint32_t));
    ctx->cmd.pos = 0;

    emit_default_state(ctx);
    int res = pica_flush(ctx);
    if (res)
        return res;
    return pica_get_error(ctx);
}

void pica_fini(pica_context* ctx)
{
    if (!ctx || !ctx->cmd.buf)
        return;
    pica_flush(ctx);
    ctx->plat.free_linear(ctx->plat.user, ctx->cmd.buf);
    ctx->cmd.buf = NULL;
}

int pica_get_error(pica_context* ctx)
{
    int err = ctx->error;
    ctx->error = 0;
    return err;
}

/* ---- Memory ---------------------------------------------------------------- */

void* pica_alloc_linear(pica_context* ctx, size_t size)
{
    return ctx->plat.alloc_linear(ctx->plat.user, size, 0x80);
}

void pica_free_linear(pica_context* ctx, void* ptr)
{
    if (ptr)
        ctx->plat.free_linear(ctx->plat.user, ptr);
}

void* pica_alloc_vram(pica_context* ctx, size_t size)
{
    if (!ctx->plat.alloc_vram)
        return NULL;
    return ctx->plat.alloc_vram(ctx->plat.user, size);
}

void pica_free_vram(pica_context* ctx, void* ptr)
{
    if (ptr && ctx->plat.free_vram)
        ctx->plat.free_vram(ctx->plat.user, ptr);
}

void pica_flush_dcache(pica_context* ctx, const void* ptr, size_t size)
{
    if (ctx->plat.flush_dcache && ptr && size)
        ctx->plat.flush_dcache(ctx->plat.user, ptr, size);
}

void pica_invalidate_dcache(pica_context* ctx, const void* ptr, size_t size)
{
    if (ctx->plat.invalidate_dcache && ptr && size)
        ctx->plat.invalidate_dcache(ctx->plat.user, ptr, size);
}

uint32_t pica_virt_to_phys(pica_context* ctx, const void* ptr)
{
    return ctx->plat.virt_to_phys(ctx->plat.user, ptr);
}

/* ---- Rasterizer -------------------------------------------------------------- */

void pica_viewport(pica_context* ctx, unsigned x, unsigned y, unsigned w, unsigned h)
{
    uint32_t vp[4];

    if (!w || !h)
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return;
    }
    vp[0] = pica_f32_to_f24(w / 2.0f);
    vp[1] = pica_f32_to_f31(2.0f / w) << 1;
    vp[2] = pica_f32_to_f24(h / 2.0f);
    vp[3] = pica_f32_to_f31(2.0f / h) << 1;

    if (!pica__reserve(ctx, pica__cmd_words(4) + 2))
        return;
    pica__write_inc(ctx, PICA_REG_VIEWPORT_WIDTH, vp, 4);
    pica__write(ctx, PICA_REG_VIEWPORT_XY, ((y & 0x3FF) << 16) | (x & 0x3FF));
}

void pica_scissor(pica_context* ctx, pica_scissor_mode_t mode, unsigned left, unsigned top, unsigned right,
                  unsigned bottom)
{
    uint32_t sc[3] = { (uint32_t)mode, 0, 0 };

    if (mode != PICA_SCISSOR_DISABLE)
    {
        if (right <= left || bottom <= top)
        {
            pica__set_error(ctx, PICA_ERR_ARG);
            return;
        }
        sc[1] = (top << 16) | (left & 0xFFFF);
        sc[2] = ((bottom - 1) << 16) | ((right - 1) & 0xFFFF);
    }
    if (pica__reserve(ctx, pica__cmd_words(3)))
        pica__write_inc(ctx, PICA_REG_SCISSORTEST_MODE, sc, 3);
}

void pica_cull(pica_context* ctx, pica_cull_mode_t mode)
{
    if (pica__reserve(ctx, 2))
        pica__write(ctx, PICA_REG_FACECULLING_CONFIG, mode & 3);
}

void pica_depth_map(pica_context* ctx, bool use_z, float scale, float offset)
{
    uint32_t v[2] = { pica_f32_to_f24(scale), pica_f32_to_f24(offset) };

    if (!pica__reserve(ctx, 2 + pica__cmd_words(2)))
        return;
    pica__write(ctx, PICA_REG_DEPTHMAP_ENABLE, use_z ? 1 : 0);
    pica__write_inc(ctx, PICA_REG_DEPTHMAP_SCALE, v, 2);
}

/* ---- Fragment operations ------------------------------------------------------- */

/* Gas depth function equivalent of a depth test function (from the
 * SDK's lookup table 0xAF02). */
static uint32_t gas_depth_func(unsigned func)
{
    return (0xAF02u >> ((func & 7) * 2)) & 3;
}

void pica_depth_test(pica_context* ctx, bool enable, pica_test_func_t func, unsigned write_mask)
{
    ctx->depth_color_mask = (enable ? 1u : 0u) | ((func & 7u) << 4) | ((write_mask & 0x1Fu) << 8);
    if (!pica__reserve(ctx, 4))
        return;
    pica__write(ctx, PICA_REG_DEPTH_COLOR_MASK, ctx->depth_color_mask);
    pica__write_masked(ctx, PICA_REG_GAS_DELTAZ_DEPTH, 0x8, gas_depth_func(func) << 24);
}

void pica_alpha_test(pica_context* ctx, bool enable, pica_test_func_t func, uint8_t ref)
{
    if (pica__reserve(ctx, 2))
        pica__write(ctx, PICA_REG_FRAGOP_ALPHA_TEST, (enable ? 1u : 0u) | ((func & 7u) << 4) | ((uint32_t)ref << 8));
}

void pica_stencil_test(pica_context* ctx, bool enable, pica_test_func_t func, uint8_t ref, uint8_t input_mask,
                       uint8_t write_mask)
{
    uint32_t v = (enable ? 1u : 0u) | ((func & 7u) << 4) | ((uint32_t)write_mask << 8) | ((uint32_t)ref << 16) |
                 ((uint32_t)input_mask << 24);
    if (pica__reserve(ctx, 2))
        pica__write(ctx, PICA_REG_STENCIL_TEST, v);
}

void pica_stencil_op(pica_context* ctx, pica_stencil_op_t fail, pica_stencil_op_t depth_fail,
                     pica_stencil_op_t pass)
{
    if (pica__reserve(ctx, 2))
        pica__write(ctx, PICA_REG_STENCIL_OP, (fail & 7u) | ((depth_fail & 7u) << 4) | ((pass & 7u) << 8));
}

void pica__emit_fragment_state(pica_context* ctx)
{
    if (!pica__reserve(ctx, 6))
        return;
    pica__write_masked(ctx, PICA_REG_COLOR_OPERATION, 0x7, ctx->color_operation);
    pica__write(ctx, PICA_REG_BLEND_FUNC, ctx->alpha_blend);
    pica__write(ctx, PICA_REG_LOGIC_OP, ctx->logic_op);
}

void pica_blend(pica_context* ctx, pica_blend_equation_t eq_rgb, pica_blend_equation_t eq_alpha,
                pica_blend_factor_t src_rgb, pica_blend_factor_t dst_rgb,
                pica_blend_factor_t src_alpha, pica_blend_factor_t dst_alpha)
{
    ctx->alpha_blend = (eq_rgb & 7u) | ((eq_alpha & 7u) << 8) | ((src_rgb & 0xFu) << 16) |
                       ((dst_rgb & 0xFu) << 20) | ((src_alpha & 0xFu) << 24) | ((uint32_t)(dst_alpha & 0xFu) << 28);
    ctx->color_operation = COLOR_OP_BASE | COLOR_OP_BLEND;
    pica__emit_fragment_state(ctx);
}

void pica_blend_color(pica_context* ctx, uint32_t rgba)
{
    if (pica__reserve(ctx, 2))
        pica__write(ctx, PICA_REG_BLEND_COLOR, pica__rgba_to_reg(rgba));
}

void pica_logic_op(pica_context* ctx, pica_logic_op_t op)
{
    ctx->logic_op = op & 0xFu;
    ctx->color_operation = COLOR_OP_BASE;
    pica__emit_fragment_state(ctx);
}

/* ---- Texture combiners ------------------------------------------------------------ */

void pica_texenv_init(pica_texenv* env)
{
    env->src_rgb = PICA_SRC_PREVIOUS;
    env->src_alpha = PICA_SRC_PREVIOUS;
    env->op_rgb = 0;
    env->op_alpha = 0;
    env->func_rgb = PICA_REPLACE;
    env->func_alpha = PICA_REPLACE;
    env->color = 0xFFFFFFFF;
    env->scale_rgb = PICA_TEVSCALE_1;
    env->scale_alpha = PICA_TEVSCALE_1;
}

void pica_texenv_src(pica_texenv* env, pica_texenv_mode_t mode, int s0, int s1, int s2)
{
    uint16_t v = (uint16_t)((s0 & 0xF) | ((s1 & 0xF) << 4) | ((s2 & 0xF) << 8));
    if (mode & PICA_TEV_RGB)
        env->src_rgb = v;
    if (mode & PICA_TEV_ALPHA)
        env->src_alpha = v;
}

void pica_texenv_op_rgb(pica_texenv* env, int o0, int o1, int o2)
{
    env->op_rgb = (uint16_t)((o0 & 0xF) | ((o1 & 0xF) << 4) | ((o2 & 0xF) << 8));
}

void pica_texenv_op_alpha(pica_texenv* env, int o0, int o1, int o2)
{
    env->op_alpha = (uint16_t)((o0 & 0x7) | ((o1 & 0x7) << 4) | ((o2 & 0x7) << 8));
}

void pica_texenv_func(pica_texenv* env, pica_texenv_mode_t mode, pica_texenv_func_t func)
{
    if (mode & PICA_TEV_RGB)
        env->func_rgb = func;
    if (mode & PICA_TEV_ALPHA)
        env->func_alpha = func;
}

void pica_texenv_color(pica_texenv* env, uint32_t rgba)
{
    env->color = pica__rgba_to_reg(rgba);
}

void pica_texenv_scale(pica_texenv* env, pica_texenv_mode_t mode, pica_texenv_scale_t scale)
{
    if (mode & PICA_TEV_RGB)
        env->scale_rgb = scale;
    if (mode & PICA_TEV_ALPHA)
        env->scale_alpha = scale;
}

void pica_set_texenv(pica_context* ctx, int stage, const pica_texenv* env)
{
    uint32_t regs[5];

    if (stage < 0 || stage > 5 || !env)
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return;
    }
    regs[0] = env->src_rgb | ((uint32_t)env->src_alpha << 16);
    regs[1] = env->op_rgb | ((uint32_t)env->op_alpha << 12);
    regs[2] = env->func_rgb | ((uint32_t)env->func_alpha << 16);
    regs[3] = env->color;
    regs[4] = env->scale_rgb | ((uint32_t)env->scale_alpha << 16);
    if (pica__reserve(ctx, pica__cmd_words(5)))
        pica__write_inc(ctx, (uint16_t)PICA_TEXENV_REG(stage), regs, 5);
}

void pica_texenv_buffer(pica_context* ctx, unsigned rgb_mask, unsigned alpha_mask, uint32_t initial_rgba)
{
    ctx->texenv_update &= ~0xFF00u;
    ctx->texenv_update |= ((rgb_mask & 0xFu) << 8) | ((alpha_mask & 0xFu) << 12);
    if (!pica__reserve(ctx, 4))
        return;
    pica__write_masked(ctx, PICA_REG_TEXENV_UPDATE_BUFFER, 0x7, ctx->texenv_update);
    pica__write(ctx, PICA_REG_TEXENV_BUFFER_COLOR, pica__rgba_to_reg(initial_rgba));
}

/* ---- Fog ---------------------------------------------------------------------------- */

void pica_fog(pica_context* ctx, const float* lut129, uint32_t color, bool z_flip)
{
    ctx->texenv_update &= ~0x100FFu;
    if (lut129)
        ctx->texenv_update |= 5u | (z_flip ? (1u << 16) : 0);

    if (!pica__reserve(ctx, 4 + 2 + pica__cmd_words(128)))
        return;
    pica__write_masked(ctx, PICA_REG_TEXENV_UPDATE_BUFFER, 0x7, ctx->texenv_update);
    pica__write(ctx, PICA_REG_FOG_COLOR, pica__rgba_to_reg(color) & 0xFFFFFF);

    if (lut129)
    {
        uint32_t lut[128];
        for (int i = 0; i < 128; i++)
        {
            float in = lut129[i] * 2048.0f;
            float diff = (lut129[i + 1] - lut129[i]) * 2048.0f;
            uint32_t val = in <= 0.0f ? 0 : (in >= 2047.0f ? 0x7FF : (uint32_t)in);
            if (diff < -4096.0f) diff = -4096.0f;
            if (diff > 4095.0f) diff = 4095.0f;
            lut[i] = ((uint32_t)(int32_t)diff & 0x1FFF) | (val << 13);
        }
        pica__write(ctx, PICA_REG_FOG_LUT_INDEX, 0);
        pica__write_rep(ctx, PICA_REG_FOG_LUT_DATA0, lut, 128);
    }
}
