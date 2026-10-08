/*
 * texture.c - texture layout, upload and binding.
 */
#include <string.h>
#include "internal.h"

/* Position of pixel (x, y) inside its 8x8 tile: x and y bits interleaved,
 * x in the even bits. */
static inline unsigned morton8(unsigned x, unsigned y)
{
    static const uint8_t xlut[8] = { 0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15 };
    static const uint8_t ylut[8] = { 0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A };
    return xlut[x & 7] + ylut[y & 7];
}

/* Index (in pixels) of (x, y) in a tiled image `w` pixels wide. */
static inline size_t tiled_index(unsigned x, unsigned y, unsigned w)
{
    return (size_t)(y & ~7u) * w + (size_t)(x & ~7u) * 8 + morton8(x, y);
}

unsigned pica_tex_bpp(pica_tex_format_t fmt)
{
    switch (fmt)
    {
        case PICA_TEX_RGBA8: return 32;
        case PICA_TEX_RGB8: return 24;
        case PICA_TEX_RGBA5551:
        case PICA_TEX_RGB565:
        case PICA_TEX_RGBA4:
        case PICA_TEX_LA8:
        case PICA_TEX_HILO8: return 16;
        case PICA_TEX_L8:
        case PICA_TEX_A8:
        case PICA_TEX_LA4:
        case PICA_TEX_ETC1A4: return 8;
        case PICA_TEX_L4:
        case PICA_TEX_A4:
        case PICA_TEX_ETC1: return 4;
        default: return 0;
    }
}

size_t pica_tex_level_size(pica_tex_format_t fmt, unsigned w, unsigned h)
{
    return (size_t)w * h * pica_tex_bpp(fmt) / 8;
}

static int tile_convert(void* dst, const void* src, unsigned w, unsigned h, pica_tex_format_t fmt, bool to_tiled)
{
    unsigned bpp = pica_tex_bpp(fmt);
    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = (uint8_t*)dst;

    if (!dst || !src || !bpp || (w & 7) || (h & 7))
        return PICA_ERR_ARG;
    if (fmt == PICA_TEX_ETC1 || fmt == PICA_TEX_ETC1A4)
        return PICA_ERR_FORMAT;

    if (bpp == 4)
    {
        /* Two pixels per byte, even pixel in the low nibble */
        memset(dst, 0, (size_t)w * h / 2);
        for (unsigned y = 0; y < h; y++)
        {
            for (unsigned x = 0; x < w; x++)
            {
                size_t lin = (size_t)y * w + x, til = tiled_index(x, y, w);
                size_t from = to_tiled ? lin : til, to = to_tiled ? til : lin;
                uint8_t v = (s[from >> 1] >> ((from & 1) * 4)) & 0xF;
                d[to >> 1] |= (uint8_t)(v << ((to & 1) * 4));
            }
        }
        return PICA_OK;
    }

    unsigned bytes = bpp / 8;
    for (unsigned y = 0; y < h; y++)
    {
        for (unsigned x = 0; x < w; x++)
        {
            size_t lin = ((size_t)y * w + x) * bytes, til = tiled_index(x, y, w) * bytes;
            if (to_tiled)
                memcpy(d + til, s + lin, bytes);
            else
                memcpy(d + lin, s + til, bytes);
        }
    }
    return PICA_OK;
}

int pica_tex_tile(void* dst, const void* src, unsigned w, unsigned h, pica_tex_format_t fmt)
{
    return tile_convert(dst, src, w, h, fmt, true);
}

int pica_tex_untile(void* dst, const void* src, unsigned w, unsigned h, pica_tex_format_t fmt)
{
    return tile_convert(dst, src, w, h, fmt, false);
}

static bool valid_dim(unsigned v)
{
    return v >= 8 && v <= 1024 && !(v & (v - 1));
}

int pica_tex_create(pica_context* ctx, pica_texture* tex, unsigned width, unsigned height,
                    pica_tex_format_t fmt, unsigned max_level, bool in_vram)
{
    if (!tex || !valid_dim(width) || !valid_dim(height) || !pica_tex_bpp(fmt))
        return PICA_ERR_ARG;

    /* The smallest mip level must still be at least 8x8 */
    unsigned w = width, h = height, levels = 0;
    size_t total = 0;
    while (true)
    {
        total += pica_tex_level_size(fmt, w, h);
        if (levels == max_level)
            break;
        w >>= 1;
        h >>= 1;
        levels++;
        if (w < 8 || h < 8)
            return PICA_ERR_ARG;
    }

    memset(tex, 0, sizeof(*tex));
    tex->data = in_vram ? pica_alloc_vram(ctx, total) : pica_alloc_linear(ctx, total);
    if (!tex->data)
        return PICA_ERR_NOMEM;
    tex->size = total;
    tex->width = (uint16_t)width;
    tex->height = (uint16_t)height;
    tex->fmt = (uint8_t)fmt;
    tex->max_level = (uint8_t)max_level;
    tex->in_vram = in_vram;
    tex->param = (uint32_t)PICA_TEXTYPE_2D << 28;
    if (fmt == PICA_TEX_ETC1)
        tex->param |= 1u << 5;
    tex->border = 0;
    pica_tex_lod(tex, 0.0f, 0, max_level);
    return PICA_OK;
}

void pica_tex_destroy(pica_context* ctx, pica_texture* tex)
{
    if (!tex || !tex->data)
        return;
    pica_flush(ctx);
    if (tex->in_vram)
        pica_free_vram(ctx, tex->data);
    else
        pica_free_linear(ctx, tex->data);
    memset(tex, 0, sizeof(*tex));
}

void pica_tex_filter(pica_texture* tex, pica_tex_filter_t mag, pica_tex_filter_t min)
{
    tex->param &= ~((1u << 1) | (1u << 2));
    tex->param |= ((mag & 1u) << 1) | ((min & 1u) << 2);
}

void pica_tex_filter_mip(pica_texture* tex, pica_tex_filter_t mip)
{
    tex->param &= ~(1u << 24);
    tex->param |= (mip & 1u) << 24;
}

void pica_tex_wrap(pica_texture* tex, pica_tex_wrap_t s, pica_tex_wrap_t t)
{
    tex->param &= ~((3u << 12) | (3u << 8));
    tex->param |= ((s & 3u) << 12) | ((t & 3u) << 8);
}

void pica_tex_border(pica_texture* tex, uint32_t rgba)
{
    tex->border = pica__rgba_to_reg(rgba);
}

void pica_tex_lod(pica_texture* tex, float bias, unsigned min_level, unsigned max_level)
{
    tex->lod = pica_f32_to_fixed13(bias) | ((max_level & 0xFu) << 16) | ((min_level & 0xFu) << 24);
}

void* pica_tex_level_ptr(const pica_texture* tex, unsigned level, size_t* size_out)
{
    size_t offset = 0;
    unsigned w = tex->width, h = tex->height;

    if (level > tex->max_level)
        return NULL;
    for (unsigned i = 0; i < level; i++)
    {
        offset += pica_tex_level_size((pica_tex_format_t)tex->fmt, w, h);
        w >>= 1;
        h >>= 1;
    }
    if (size_out)
        *size_out = pica_tex_level_size((pica_tex_format_t)tex->fmt, w, h);
    return (uint8_t*)tex->data + offset;
}

int pica_tex_upload_tiled(pica_context* ctx, pica_texture* tex, unsigned level, const void* data)
{
    size_t size;
    void* dst = pica_tex_level_ptr(tex, level, &size);
    if (!dst || !data)
        return PICA_ERR_ARG;

    if (!tex->in_vram)
    {
        if (dst != data)
            memcpy(dst, data, size);
        pica_flush_dcache(ctx, dst, size);
    }
    else
    {
        /* The CPU cannot write VRAM directly under Horizon: stage in linear
         * memory and let the transfer engine copy it. */
        void* staging = pica_alloc_linear(ctx, size);
        if (!staging)
            return PICA_ERR_NOMEM;
        memcpy(staging, data, size);
        pica_flush_dcache(ctx, staging, size);
        int res = pica_texture_copy(ctx, staging, 0, dst, 0, size, 0);
        pica_free_linear(ctx, staging);
        if (res)
            return res;
    }
    pica_tex_cache_clear(ctx);
    return PICA_OK;
}

int pica_tex_upload(pica_context* ctx, pica_texture* tex, unsigned level, const void* linear)
{
    size_t size;
    void* dst = pica_tex_level_ptr(tex, level, &size);
    if (!dst || !linear)
        return PICA_ERR_ARG;

    unsigned w = tex->width >> level, h = tex->height >> level;
    if (!tex->in_vram)
    {
        int res = pica_tex_tile(dst, linear, w, h, (pica_tex_format_t)tex->fmt);
        if (res)
            return res;
        return pica_tex_upload_tiled(ctx, tex, level, dst);
    }

    void* staging = pica_alloc_linear(ctx, size);
    if (!staging)
        return PICA_ERR_NOMEM;
    int res = pica_tex_tile(staging, linear, w, h, (pica_tex_format_t)tex->fmt);
    if (!res)
        res = pica_tex_upload_tiled(ctx, tex, level, staging);
    pica_free_linear(ctx, staging);
    return res;
}

static void emit_texunit_config(pica_context* ctx, bool clear_cache)
{
    if (!pica__reserve(ctx, 4))
        return;
    pica__write_masked(ctx, PICA_REG_TEXUNIT_CONFIG, 0xB, ctx->texunit_config);
    if (clear_cache)
        pica__write_masked(ctx, PICA_REG_TEXUNIT_CONFIG, 0x4, 1u << 16);
}

void pica_tex_bind(pica_context* ctx, int unit, const pica_texture* tex)
{
    static const uint16_t base_reg[3] = { PICA_REG_TEXUNIT0_BORDER_COLOR, PICA_REG_TEXUNIT1_BORDER_COLOR,
                                          PICA_REG_TEXUNIT2_BORDER_COLOR };
    static const uint16_t type_reg[3] = { PICA_REG_TEXUNIT0_TYPE, PICA_REG_TEXUNIT1_TYPE, PICA_REG_TEXUNIT2_TYPE };

    if (unit < 0 || unit > 2)
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return;
    }

    if (!tex)
    {
        ctx->texunit_config &= ~(1u << unit);
        emit_texunit_config(ctx, false);
        return;
    }

    uint32_t regs[5];
    regs[0] = tex->border;
    regs[1] = ((uint32_t)tex->width << 16) | tex->height;
    regs[2] = tex->param;
    regs[3] = tex->lod;
    regs[4] = pica_virt_to_phys(ctx, tex->data) >> 3;
    if (!pica__reserve(ctx, pica__cmd_words(5) + 2))
        return;
    pica__write_inc(ctx, base_reg[unit], regs, 5);
    pica__write(ctx, type_reg[unit], tex->fmt);

    ctx->texunit_config |= 1u << unit;
    emit_texunit_config(ctx, true);
}

void pica_tex_cache_clear(pica_context* ctx)
{
    emit_texunit_config(ctx, true);
}
