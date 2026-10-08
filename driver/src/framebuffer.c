/*
 * framebuffer.c - render targets, clears and the transfer engines.
 */
#include <string.h>
#include "internal.h"

static unsigned color_bytes(pica_color_format_t fmt)
{
    switch (fmt)
    {
        case PICA_COLOR_RGBA8: return 4;
        case PICA_COLOR_RGB8: return 3;
        case PICA_COLOR_RGBA5551:
        case PICA_COLOR_RGB565:
        case PICA_COLOR_RGBA4: return 2;
        default: return 0;
    }
}

static unsigned depth_bytes(pica_depth_format_t fmt)
{
    switch (fmt)
    {
        case PICA_DEPTH16: return 2;
        case PICA_DEPTH24: return 3;
        case PICA_DEPTH24_STENCIL8: return 4;
        default: return 0;
    }
}

size_t pica_color_buffer_size(unsigned width, unsigned height, pica_color_format_t fmt)
{
    return (size_t)width * height * color_bytes(fmt);
}

size_t pica_depth_buffer_size(unsigned width, unsigned height, pica_depth_format_t fmt)
{
    return (size_t)width * height * depth_bytes(fmt);
}

int pica_framebuffer_create(pica_context* ctx, pica_framebuffer* fb, unsigned width, unsigned height,
                            pica_color_format_t color_fmt, pica_depth_format_t depth_fmt)
{
    if (!fb || !width || !height || (width & 7) || (height & 7) || width > 1024 || height > 1024 ||
        !color_bytes(color_fmt) || (depth_fmt != PICA_DEPTH_NONE && !depth_bytes(depth_fmt)))
        return PICA_ERR_ARG;

    memset(fb, 0, sizeof(*fb));
    fb->width = (uint16_t)width;
    fb->height = (uint16_t)height;
    fb->color_fmt = (uint8_t)color_fmt;
    fb->depth_fmt = (uint8_t)depth_fmt;
    fb->owns_memory = true;

    fb->color = pica_alloc_vram(ctx, pica_color_buffer_size(width, height, color_fmt));
    if (!fb->color)
        return PICA_ERR_NOMEM;

    if (depth_fmt != PICA_DEPTH_NONE)
    {
        fb->depth = pica_alloc_vram(ctx, pica_depth_buffer_size(width, height, depth_fmt));
        if (!fb->depth)
        {
            pica_free_vram(ctx, fb->color);
            fb->color = NULL;
            return PICA_ERR_NOMEM;
        }
    }
    return PICA_OK;
}

void pica_framebuffer_destroy(pica_context* ctx, pica_framebuffer* fb)
{
    if (!fb)
        return;
    if (ctx->fb == fb)
    {
        pica_flush(ctx);
        ctx->fb = NULL;
    }
    if (fb->owns_memory)
    {
        pica_free_vram(ctx, fb->color);
        pica_free_vram(ctx, fb->depth);
    }
    memset(fb, 0, sizeof(*fb));
}

void pica_bind_framebuffer(pica_context* ctx, const pica_framebuffer* fb)
{
    uint32_t loc[3], masks[4];
    static const uint8_t color_size_code[] = { 2, 1, 0, 0, 0 };

    if (!fb || !fb->color)
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return;
    }
    if (!pica__reserve(ctx, 4 + 2 + pica__cmd_words(3) + 2 * 4 + pica__cmd_words(4)))
        return;

    if (ctx->draw_used)
    {
        /* Write back the old target before switching */
        pica__write(ctx, PICA_REG_FRAMEBUFFER_FLUSH, 1);
        pica__write(ctx, PICA_REG_EARLYDEPTH_CLEAR, 1);
        ctx->draw_used = false;
    }
    pica__write(ctx, PICA_REG_FRAMEBUFFER_INVALIDATE, 1);

    loc[0] = fb->depth ? pica_virt_to_phys(ctx, fb->depth) >> 3 : 0;
    loc[1] = pica_virt_to_phys(ctx, fb->color) >> 3;
    loc[2] = 0x01000000u | ((uint32_t)(fb->height - 1) << 12) | fb->width;
    pica__write_inc(ctx, PICA_REG_DEPTHBUFFER_LOC, loc, 3);
    pica__write(ctx, PICA_REG_RENDERBUF_DIM, loc[2]);
    pica__write(ctx, PICA_REG_DEPTHBUFFER_FORMAT, fb->depth ? fb->depth_fmt : PICA_DEPTH16);
    pica__write(ctx, PICA_REG_COLORBUFFER_FORMAT, color_size_code[fb->color_fmt] | ((uint32_t)fb->color_fmt << 16));
    pica__write(ctx, PICA_REG_FRAMEBUFFER_BLOCK32, 0);

    /* Color/depth buffer read & write enables */
    masks[0] = masks[1] = 0xF;
    masks[2] = masks[3] = fb->depth ? (fb->depth_fmt == PICA_DEPTH24_STENCIL8 ? 0x3 : 0x2) : 0;
    pica__write_inc(ctx, PICA_REG_COLORBUFFER_READ, masks, 4);

    ctx->fb = fb;
    pica_viewport(ctx, 0, 0, fb->width, fb->height);
}

/* Convert 0xRRGGBBAA to the raw fill value for a color buffer format. */
static uint32_t fill_value(pica_color_format_t fmt, uint32_t rgba, unsigned* width_bits)
{
    uint32_t r = rgba >> 24, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF, a = rgba & 0xFF;
    switch (fmt)
    {
        case PICA_COLOR_RGBA8:
            *width_bits = 32;
            return rgba;
        case PICA_COLOR_RGB8:
            *width_bits = 24;
            return rgba >> 8;
        case PICA_COLOR_RGBA5551:
            *width_bits = 16;
            return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7);
        case PICA_COLOR_RGB565:
            *width_bits = 16;
            return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
        case PICA_COLOR_RGBA4:
        default:
            *width_bits = 16;
            return ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
    }
}

int pica_memory_fill(pica_context* ctx, void* start, size_t size, uint32_t value, unsigned width_bits)
{
    if (!ctx->plat.memory_fill)
        return PICA_ERR_PLATFORM;
    if (!start || !size || (size & 7) || (pica_virt_to_phys(ctx, start) & 7))
        return PICA_ERR_ARG;
    if (ctx->plat.memory_fill(ctx->plat.user, start, size, value, width_bits))
        return PICA_ERR_PLATFORM;
    return PICA_OK;
}

int pica_clear(pica_context* ctx, const pica_framebuffer* fb, unsigned flags, uint32_t color, uint32_t depth)
{
    int res;
    unsigned width;

    if (!fb)
        return PICA_ERR_ARG;

    /* Everything drawn so far must land before the fill overwrites it, and
     * the framebuffer cache gets invalidated at the end of the flushed list. */
    res = pica_flush(ctx);
    if (res)
        return res;

    if ((flags & PICA_CLEAR_COLOR) && fb->color)
    {
        uint32_t v = fill_value((pica_color_format_t)fb->color_fmt, color, &width);
        res = pica_memory_fill(ctx, fb->color, pica_color_buffer_size(fb->width, fb->height,
                               (pica_color_format_t)fb->color_fmt), v, width);
        if (res)
            return res;
    }
    if ((flags & PICA_CLEAR_DEPTH) && fb->depth)
    {
        width = depth_bytes((pica_depth_format_t)fb->depth_fmt) * 8;
        res = pica_memory_fill(ctx, fb->depth, pica_depth_buffer_size(fb->width, fb->height,
                               (pica_depth_format_t)fb->depth_fmt), depth, width);
        if (res)
            return res;
    }
    return PICA_OK;
}

int pica_display_transfer(pica_context* ctx, const void* src, uint32_t src_dim, void* dst, uint32_t dst_dim,
                          uint32_t flags)
{
    if (!ctx->plat.display_transfer)
        return PICA_ERR_PLATFORM;
    int res = pica_flush(ctx);
    if (res)
        return res;
    if (ctx->plat.display_transfer(ctx->plat.user, src, src_dim, dst, dst_dim, flags))
        return PICA_ERR_PLATFORM;
    return PICA_OK;
}

int pica_texture_copy(pica_context* ctx, const void* src, uint32_t src_line, void* dst, uint32_t dst_line,
                      size_t size, uint32_t flags)
{
    if (!ctx->plat.texture_copy)
        return PICA_ERR_PLATFORM;
    int res = pica_flush(ctx);
    if (res)
        return res;
    if (ctx->plat.texture_copy(ctx->plat.user, src, src_line, dst, dst_line, size, flags | PICA_XFER_RAW_COPY))
        return PICA_ERR_PLATFORM;
    return PICA_OK;
}

int pica_transfer(pica_context* ctx, const pica_framebuffer* fb, void* dst, unsigned dst_w, unsigned dst_h,
                  pica_xfer_format_t dst_fmt, uint32_t extra_flags)
{
    if (!fb || !fb->color || !dst)
        return PICA_ERR_ARG;
    /* Render buffer color format IDs match the transfer engine's. */
    uint32_t flags = PICA_XFER_IN_FORMAT(fb->color_fmt) | PICA_XFER_OUT_FORMAT(dst_fmt) | extra_flags;
    int res = pica_display_transfer(ctx, fb->color, PICA_XFER_DIM(fb->width, fb->height), dst,
                                    PICA_XFER_DIM(dst_w, dst_h), flags);
    if (res)
        return res;

    static const unsigned out_bytes[] = { 4, 3, 2, 2, 2 };
    pica_invalidate_dcache(ctx, dst, (size_t)dst_w * dst_h * out_bytes[(dst_fmt & 7) < 5 ? (dst_fmt & 7) : 0]);
    return PICA_OK;
}
