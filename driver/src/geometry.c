/*
 * geometry.c - vertex input configuration and draw calls.
 */
#include <string.h>
#include "internal.h"

/* All vertex and index buffers are addressed relative to this base, which
 * covers both VRAM (0x18000000) and FCRAM (0x20000000+) with the 28-bit
 * offsets the hardware accepts. */
#define PICA_BUFFER_BASE 0x18000000u

void pica_attr_init(pica_attr_info* info)
{
    memset(info, 0, sizeof(*info));
    /* All 12 attributes start out "fixed" until a loader claims them */
    info->format[1] = 0xFFFu << 16;
}

static int attr_add(pica_attr_info* info, int reg)
{
    if (info->count >= 12)
        return PICA_ERR_RANGE;
    int id = info->count++;
    if (reg < 0)
        reg = id;
    if (reg > 15)
        return PICA_ERR_RANGE;

    info->format[1] = (info->format[1] & ~0xF0000000u) | ((uint32_t)id << 28);
    info->permutation[id / 8] |= (uint32_t)reg << ((id % 8) * 4);
    return id;
}

int pica_attr_add_loader(pica_attr_info* info, int reg, pica_attr_type_t type, int components)
{
    if (components < 1 || components > 4)
        return PICA_ERR_ARG;
    int id = attr_add(info, reg);
    if (id < 0)
        return id;

    uint32_t fmt = ((uint32_t)(components - 1) << 2) | (type & 3u);
    if (id < 8)
        info->format[0] |= fmt << (id * 4);
    else
        info->format[1] |= fmt << ((id - 8) * 4);
    info->format[1] &= ~(1u << (id + 16));
    return id;
}

int pica_attr_add_fixed(pica_attr_info* info, int reg)
{
    return attr_add(info, reg);
}

void pica_buf_init(pica_buf_info* info)
{
    memset(info, 0, sizeof(*info));
    info->base_phys = PICA_BUFFER_BASE;
}

int pica_buf_add(pica_context* ctx, pica_buf_info* info, const void* data, unsigned stride, unsigned attr_count,
                 uint64_t permutation)
{
    if (info->count >= 12)
        return PICA_ERR_RANGE;
    if (!data || stride > 0xFF || attr_count > 12)
        return PICA_ERR_ARG;

    uint32_t pa = pica_virt_to_phys(ctx, data);
    if (pa < info->base_phys || pa - info->base_phys >= 0x10000000u)
        return PICA_ERR_RANGE;

    int id = info->count++;
    info->buf[id].offset = pa - info->base_phys;
    info->buf[id].config1 = (uint32_t)permutation;
    info->buf[id].config2 = (uint32_t)(permutation >> 32) & 0xFFFF;
    info->buf[id].config2 |= (stride << 16) | ((uint32_t)attr_count << 28);
    return id;
}

void pica_bind_attrs(pica_context* ctx, const pica_attr_info* info)
{
    if (!info || !info->count)
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return;
    }
    if (!pica__reserve(ctx, pica__cmd_words(2) * 2 + 4))
        return;
    pica__write_inc(ctx, PICA_REG_ATTRIBBUFFERS_FORMAT_LOW, info->format, 2);
    pica__write_masked(ctx, PICA_REG_VSH_INPUTBUFFER_CONFIG, 0xB, 0xA0000000u | (info->count - 1u));
    pica__write(ctx, PICA_REG_VSH_NUM_ATTR, info->count - 1u);
    pica__write_inc(ctx, PICA_REG_VSH_ATTRIBUTES_PERMUTATION_LOW, info->permutation, 2);
}

void pica_bind_buffers(pica_context* ctx, const pica_buf_info* info)
{
    if (!info)
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return;
    }
    if (!pica__reserve(ctx, 2 + pica__cmd_words(36)))
        return;
    pica__write(ctx, PICA_REG_ATTRIBBUFFERS_LOC, info->base_phys >> 3);
    pica__write_inc(ctx, PICA_REG_ATTRIBBUFFER0_OFFSET, &info->buf[0].offset, 36);
}

static void send_attrib(pica_context* ctx, float x, float y, float z, float w)
{
    uint32_t packed[3];
    pica_pack_f24x4(packed, x, y, z, w);
    pica__write_inc(ctx, PICA_REG_FIXEDATTRIB_DATA0, packed, 3);
}

void pica_fixed_attrib(pica_context* ctx, unsigned index, float x, float y, float z, float w)
{
    if (index > 11)
    {
        pica__set_error(ctx, PICA_ERR_RANGE);
        return;
    }
    if (!pica__reserve(ctx, 2 + pica__cmd_words(3)))
        return;
    pica__write(ctx, PICA_REG_FIXEDATTRIB_INDEX, index);
    send_attrib(ctx, x, y, z, w);
}

static bool check_draw(pica_context* ctx)
{
    if (!ctx->fb || !ctx->vsh)
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return false;
    }
    return true;
}

void pica_draw_arrays(pica_context* ctx, pica_primitive_t prim, unsigned first, unsigned count)
{
    if (!check_draw(ctx) || !count)
        return;
    if (!pica__reserve(ctx, 11 * 2))
        return;

    pica__write_masked(ctx, PICA_REG_PRIMITIVE_CONFIG, 0x2, prim);
    pica__write(ctx, PICA_REG_RESTART_PRIMITIVE, 1);
    /* Not used for arrays, but the hardware wants it programmed */
    pica__write(ctx, PICA_REG_INDEXBUFFER_CONFIG, 0x80000000u);
    pica__write(ctx, PICA_REG_NUMVERTICES, count);
    pica__write(ctx, PICA_REG_VERTEX_OFFSET, first);
    pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG2, 0x1, 1);
    pica__write_masked(ctx, PICA_REG_START_DRAW_FUNC0, 0x1, 0);
    pica__write(ctx, PICA_REG_DRAWARRAYS, 1);
    pica__write_masked(ctx, PICA_REG_START_DRAW_FUNC0, 0x1, 1);
    pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG2, 0x1, 0);
    pica__write(ctx, PICA_REG_VTX_FUNC, 1);
    ctx->draw_used = true;
}

void pica_draw_elements(pica_context* ctx, pica_primitive_t prim, unsigned count, int index_type,
                        const void* indices)
{
    if (!check_draw(ctx) || !count)
        return;

    uint32_t pa = pica_virt_to_phys(ctx, indices);
    if (!indices || pa < PICA_BUFFER_BASE || pa - PICA_BUFFER_BASE >= 0x10000000u ||
        (index_type && (pa & 1)))
    {
        pica__set_error(ctx, PICA_ERR_RANGE);
        return;
    }
    if (!pica__reserve(ctx, 16 * 2))
        return;

    /* Independent triangles go through the geometry-primitive path, as the
     * official SDK does for indexed draws. */
    bool tris = prim == PICA_TRIANGLES;
    pica__write_masked(ctx, PICA_REG_PRIMITIVE_CONFIG, 0x2, tris ? PICA_GEOMETRY_PRIM : prim);
    pica__write(ctx, PICA_REG_RESTART_PRIMITIVE, 1);
    pica__write(ctx, PICA_REG_INDEXBUFFER_CONFIG, (pa - PICA_BUFFER_BASE) | ((uint32_t)(index_type ? 1 : 0) << 31));
    pica__write(ctx, PICA_REG_NUMVERTICES, count);
    pica__write(ctx, PICA_REG_VERTEX_OFFSET, 0);
    if (tris)
    {
        pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG, 0x2, 0x100);
        pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG2, 0x2, 0x100);
    }
    pica__write_masked(ctx, PICA_REG_START_DRAW_FUNC0, 0x1, 0);
    pica__write(ctx, PICA_REG_DRAWELEMENTS, 1);
    pica__write_masked(ctx, PICA_REG_START_DRAW_FUNC0, 0x1, 1);
    if (tris)
    {
        pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG, 0x2, 0);
        pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG2, 0x2, 0);
    }
    pica__write(ctx, PICA_REG_VTX_FUNC, 1);
    /* Hardware quirk workaround carried over from the official SDK */
    pica__write_masked(ctx, PICA_REG_PRIMITIVE_CONFIG, 0x8, 0);
    pica__write_masked(ctx, PICA_REG_PRIMITIVE_CONFIG, 0x8, 0);
    ctx->draw_used = true;
}

/* Immediate mode.  Keep a begin/attr/end batch small enough to fit in the
 * command buffer: a batch that overflows it gets split across two command
 * lists while the GPU is in drawing mode, which has not been verified on
 * hardware. */
void pica_imm_begin(pica_context* ctx, pica_primitive_t prim)
{
    if (!check_draw(ctx) || !pica__reserve(ctx, 6 * 2))
        return;
    pica__write_masked(ctx, PICA_REG_PRIMITIVE_CONFIG, 0x2, prim);
    pica__write(ctx, PICA_REG_RESTART_PRIMITIVE, 1);
    pica__write(ctx, PICA_REG_INDEXBUFFER_CONFIG, 0x80000000u);
    pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG2, 0x1, 1);
    pica__write_masked(ctx, PICA_REG_START_DRAW_FUNC0, 0x1, 0);
    pica__write(ctx, PICA_REG_FIXEDATTRIB_INDEX, 0xF);
}

void pica_imm_attr(pica_context* ctx, float x, float y, float z, float w)
{
    if (pica__reserve(ctx, pica__cmd_words(3)))
        send_attrib(ctx, x, y, z, w);
}

void pica_imm_end(pica_context* ctx)
{
    if (!pica__reserve(ctx, 3 * 2))
        return;
    pica__write_masked(ctx, PICA_REG_START_DRAW_FUNC0, 0x1, 1);
    pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG2, 0x1, 0);
    pica__write(ctx, PICA_REG_VTX_FUNC, 1);
    ctx->draw_used = true;
}
