/*
 * shader.c - SHBIN parsing, shader program binding and uniforms.
 *
 * SHBIN layout (all little-endian words):
 *   DVLB: "DVLB", number of DVLEs, offset of each DVLE (from file start)
 *   DVLP: "DVLP", version, code offset, code size (words),
 *         operand descriptor offset, operand descriptor count (8 bytes each)
 *         -- offsets relative to the DVLP header
 *   DVLE: "DVLE", version | type << 16 | merge_outmaps << 24, main offset,
 *         endmain offset, unused, geometry shader info, constant table
 *         offset/count, label table offset/count, output table offset/count,
 *         uniform table offset/count, symbol table offset/size
 *         -- offsets relative to the DVLE header
 */
#include <string.h>
#include "internal.h"

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Shader output semantics (DVLE output table "type") */
enum
{
    OUT_POSITION   = 0,
    OUT_NORMALQUAT = 1,
    OUT_COLOR      = 2,
    OUT_TEXCOORD0  = 3,
    OUT_TEXCOORD0W = 4,
    OUT_TEXCOORD1  = 5,
    OUT_TEXCOORD2  = 6,
    OUT_VIEW       = 8,
    OUT_DUMMY      = 9,
};

static inline uint32_t popcount32(uint32_t v)
{
    uint32_t c = 0;
    for (; v; v &= v - 1)
        c++;
    return c;
}

/* Build the SH_OUTMAP_On values.  The rasterizer sees the shader outputs
 * compacted: the n-th register set in the output mask becomes slot n. */
static void generate_outmap(pica_dvle* dvle)
{
    memset(dvle->outmap, 0x1F, sizeof(dvle->outmap));
    dvle->outmap_mask = 0;
    dvle->outmap_mode = 0;
    dvle->outmap_clock = 0;

    for (uint32_t i = 0; i < dvle->num_outputs; i++)
        if (dvle->outputs[i].reg < 16)
            dvle->outmap_mask |= 1u << dvle->outputs[i].reg;
    dvle->outmap[0] = popcount32(dvle->outmap_mask);

    for (uint32_t i = 0; i < dvle->num_outputs; i++)
    {
        const pica_shader_output* o = &dvle->outputs[i];
        if (o->reg >= 16)
            continue;
        uint32_t slot = popcount32(dvle->outmap_mask & ((1u << o->reg) - 1));
        if (slot >= 7)
            continue;
        uint32_t* out = &dvle->outmap[slot + 1];

        unsigned sem, num;
        switch (o->type)
        {
            case OUT_POSITION:   sem = 0x00; num = 4; break;
            case OUT_NORMALQUAT: sem = 0x04; num = 4; dvle->outmap_clock |= 1u << 24; break;
            case OUT_COLOR:      sem = 0x08; num = 4; dvle->outmap_clock |= 1u << 1; break;
            case OUT_TEXCOORD0:  sem = 0x0C; num = 2; dvle->outmap_clock |= 1u << 8; dvle->outmap_mode = 1; break;
            case OUT_TEXCOORD0W: sem = 0x10; num = 1; dvle->outmap_clock |= 1u << 16; dvle->outmap_mode = 1; break;
            case OUT_TEXCOORD1:  sem = 0x0E; num = 2; dvle->outmap_clock |= 1u << 9; dvle->outmap_mode = 1; break;
            case OUT_TEXCOORD2:  sem = 0x16; num = 2; dvle->outmap_clock |= 1u << 10; dvle->outmap_mode = 1; break;
            case OUT_VIEW:       sem = 0x12; num = 3; dvle->outmap_clock |= 1u << 24; break;
            default: continue;
        }

        for (unsigned j = 0, k = 0; j < 4 && k < num; j++)
        {
            if (!(o->mask & (1u << j)))
                continue;
            *out &= ~(0xFFu << (j * 8));
            *out |= (uint32_t)(sem++) << (j * 8);
            k++;
            if (o->type == OUT_POSITION && k == 3)
                dvle->outmap_clock |= 1u << 0;
        }
    }
}

static bool in_bounds(size_t size, size_t offset, size_t len)
{
    return offset <= size && len <= size - offset;
}

int pica_shbin_parse(pica_shbin* out, const void* data, size_t size)
{
    const uint8_t* base = (const uint8_t*)data;
    const uint32_t* w = (const uint32_t*)data;

    if (!out || !data || size < 12 || ((uintptr_t)data & 3))
        return PICA_ERR_ARG;
    memset(out, 0, sizeof(*out));

    if (w[0] != FOURCC('D', 'V', 'L', 'B'))
        return PICA_ERR_FORMAT;
    uint32_t num = w[1];
    if (!num || num > PICA_MAX_DVLE || !in_bounds(size, 8, num * 4 + 24))
        return PICA_ERR_FORMAT;
    out->num_dvle = num;

    /* DVLP immediately follows the DVLE offset table */
    size_t dvlp_off = 8 + num * 4;
    const uint32_t* dvlp = (const uint32_t*)(base + dvlp_off);
    if (dvlp[0] != FOURCC('D', 'V', 'L', 'P'))
        return PICA_ERR_FORMAT;
    if (!in_bounds(size, dvlp_off + dvlp[2], (size_t)dvlp[3] * 4) ||
        !in_bounds(size, dvlp_off + dvlp[4], (size_t)dvlp[5] * 8) || dvlp[5] > 128 || dvlp[3] > 4096)
        return PICA_ERR_FORMAT;
    out->dvlp.code = (const uint32_t*)(base + dvlp_off + dvlp[2]);
    out->dvlp.code_size = dvlp[3];
    out->dvlp.opdesc_size = dvlp[5];
    for (uint32_t i = 0; i < dvlp[5]; i++)
        out->dvlp.opdesc[i] = ((const uint32_t*)(base + dvlp_off + dvlp[4]))[i * 2];

    for (uint32_t i = 0; i < num; i++)
    {
        size_t off = w[2 + i];
        if ((off & 3) || !in_bounds(size, off, 16 * 4))
            return PICA_ERR_FORMAT;
        const uint32_t* e = (const uint32_t*)(base + off);
        pica_dvle* d = &out->dvle[i];

        if (e[0] != FOURCC('D', 'V', 'L', 'E'))
            return PICA_ERR_FORMAT;
        d->dvlp = &out->dvlp;
        d->type = (e[1] >> 16) & 0xFF;
        d->merge_outmaps = (e[1] >> 24) & 1;
        d->main_offset = e[2];
        d->endmain_offset = e[3];
        if (d->type == PICA_GEOMETRY_SHADER)
        {
            d->gsh_mode = e[5] & 0xFF;
            d->gsh_fixed_vtx_start = (e[5] >> 8) & 0xFF;
            d->gsh_variable_vtx_num = (e[5] >> 16) & 0xFF;
            d->gsh_fixed_vtx_num = (e[5] >> 24) & 0xFF;
        }

        if (!in_bounds(size, off + e[6], (size_t)e[7] * sizeof(pica_shader_const)) ||
            !in_bounds(size, off + e[10], (size_t)e[11] * sizeof(pica_shader_output)) ||
            !in_bounds(size, off + e[12], (size_t)e[13] * sizeof(pica_shader_uniform_entry)) ||
            !in_bounds(size, off + e[14], e[15]))
            return PICA_ERR_FORMAT;

        d->consts = (const pica_shader_const*)(base + off + e[6]);
        d->num_consts = e[7];
        d->outputs = (const pica_shader_output*)(base + off + e[10]);
        d->num_outputs = e[11];
        d->uniforms = (const pica_shader_uniform_entry*)(base + off + e[12]);
        d->num_uniforms = e[13];
        d->symbols = (const char*)(base + off + e[14]);
        d->symbols_size = e[15];

        generate_outmap(d);
    }
    return PICA_OK;
}

pica_uniform_loc pica_shader_uniform(const pica_dvle* dvle, const char* name)
{
    pica_uniform_loc loc = { PICA_UNIFORM_NONE, -1, 0 };

    if (!dvle || !name)
        return loc;
    for (uint32_t i = 0; i < dvle->num_uniforms; i++)
    {
        const pica_shader_uniform_entry* u = &dvle->uniforms[i];
        if (u->symbol_offset >= dvle->symbols_size)
            continue;
        const char* sym = dvle->symbols + u->symbol_offset;
        size_t maxlen = dvle->symbols_size - u->symbol_offset;
        if (strncmp(sym, name, maxlen) != 0 || strlen(name) >= maxlen)
            continue;

        loc.count = u->end_reg - u->start_reg + 1;
        if (u->start_reg >= 0x10 && u->start_reg < 0x70)
        {
            loc.kind = PICA_UNIFORM_FLOAT;
            loc.index = u->start_reg - 0x10;
        }
        else if (u->start_reg >= 0x70 && u->start_reg < 0x74)
        {
            loc.kind = PICA_UNIFORM_INT;
            loc.index = u->start_reg - 0x70;
        }
        else if (u->start_reg >= 0x78 && u->start_reg < 0x88)
        {
            loc.kind = PICA_UNIFORM_BOOL;
            loc.index = u->start_reg - 0x78;
        }
        return loc;
    }
    return loc;
}

int pica_shader_uniform_reg(const pica_dvle* dvle, const char* name)
{
    pica_uniform_loc loc = pica_shader_uniform(dvle, name);
    return loc.kind == PICA_UNIFORM_FLOAT ? loc.index : -1;
}

/* ---- Program binding ------------------------------------------------------- */

static void upload_code(pica_context* ctx, bool gsh, const pica_dvlp* dvlp, uint32_t max_words)
{
    int off = PICA_SH_REG_OFFSET(gsh);
    uint32_t words = dvlp->code_size < max_words ? dvlp->code_size : max_words;

    if (!pica__reserve(ctx, 2 + pica__cmd_words(words) + 2 + 2 + pica__cmd_words(dvlp->opdesc_size)))
        return;
    pica__write(ctx, (uint16_t)(PICA_REG_VSH_CODETRANSFER_CONFIG + off), 0);
    /* The DATA registers are a FIFO: write them repeatedly, 128 at a time */
    for (uint32_t i = 0; i < words; i += 128)
    {
        uint32_t n = words - i < 128 ? words - i : 128;
        pica__write_rep(ctx, (uint16_t)(PICA_REG_VSH_CODETRANSFER_DATA + off), dvlp->code + i, n);
    }
    pica__write(ctx, (uint16_t)(PICA_REG_VSH_CODETRANSFER_END + off), 1);

    pica__write(ctx, (uint16_t)(PICA_REG_VSH_OPDESCS_CONFIG + off), 0);
    if (dvlp->opdesc_size)
        pica__write_rep(ctx, (uint16_t)(PICA_REG_VSH_OPDESCS_DATA + off), dvlp->opdesc, dvlp->opdesc_size);
}

static void upload_consts(pica_context* ctx, pica_shader_type_t unit, const pica_dvle* dvle)
{
    int off = PICA_SH_REG_OFFSET(unit == PICA_GEOMETRY_SHADER);

    for (uint32_t i = 0; i < dvle->num_consts; i++)
    {
        const pica_shader_const* c = &dvle->consts[i];
        switch (c->type)
        {
            case 0: /* bool */
                if (c->id < 16)
                {
                    ctx->bool_uniforms[unit] &= ~(1u << c->id);
                    ctx->bool_uniforms[unit] |= (uint16_t)((c->data[0] & 1) << c->id);
                }
                break;
            case 1: /* int: x, y, z, w bytes */
                if (c->id < 4 && pica__reserve(ctx, 2))
                    pica__write(ctx, (uint16_t)(PICA_REG_VSH_INTUNIFORM_I0 + off + c->id), c->data[0]);
                break;
            case 2: /* float24: four 24-bit values */
            {
                uint32_t x = c->data[0] & 0xFFFFFF, y = c->data[1] & 0xFFFFFF;
                uint32_t z = c->data[2] & 0xFFFFFF, w = c->data[3] & 0xFFFFFF;
                uint32_t v[4];
                v[0] = c->id & 0xFF; /* f24 mode */
                v[1] = (w << 8) | (z >> 16);
                v[2] = ((z & 0xFFFF) << 16) | (y >> 8);
                v[3] = ((y & 0xFF) << 24) | x;
                if (pica__reserve(ctx, pica__cmd_words(4)))
                    pica__write_inc(ctx, (uint16_t)(PICA_REG_VSH_FLOATUNIFORM_CONFIG + off), v, 4);
                break;
            }
        }
    }
    if (pica__reserve(ctx, 2))
        pica__write(ctx, (uint16_t)(PICA_REG_VSH_BOOLUNIFORM + off), 0x7FFF0000u | ctx->bool_uniforms[unit]);
}

/* Merge vertex and geometry shader output maps (for geometry shaders that
 * declare a dummy output to request it). */
static void merge_outmaps(uint32_t* out, const uint32_t* vsh, const uint32_t* gsh)
{
    uint32_t vsh_common = 0, gsh_common = 0;
    int i, j;

    memset(out, 0x1F, sizeof(uint32_t) * 8);
    out[0] = 0;
    for (i = 1; i < 8 && gsh[i] != 0x1F1F1F1F; i++)
    {
        for (j = 1; j < 8; j++)
        {
            if (vsh[j] == gsh[i])
            {
                out[++out[0]] = gsh[i];
                vsh_common |= 1u << j;
                gsh_common |= 1u << i;
                break;
            }
        }
    }
    for (i = 1; i < 8 && gsh[i] != 0x1F1F1F1F && out[0] < 7; i++)
        if (!(gsh_common & (1u << i)))
            out[++out[0]] = gsh[i];
    for (i = 1; i < 8 && vsh[i] != 0x1F1F1F1F && out[0] < 7; i++)
        if (!(vsh_common & (1u << i)))
            out[++out[0]] = vsh[i];
}

void pica_bind_shader(pica_context* ctx, const pica_dvle* vsh, const pica_dvle* gsh, unsigned gsh_stride)
{
    uint32_t outmap[8];

    if (!vsh || vsh->type != PICA_VERTEX_SHADER || (gsh && gsh->type != PICA_GEOMETRY_SHADER))
    {
        pica__set_error(ctx, PICA_ERR_ARG);
        return;
    }
    const pica_dvle* main = gsh ? gsh : vsh;
    uint32_t mode = main->outmap_mode, clock = main->outmap_clock;

    if (!pica__reserve(ctx, 6))
        return;
    /* Geometry stage routing must be configured before code is uploaded:
     * with VSH_COM_MODE = 0 vertex shader writes also reach the 4th shader
     * unit, which then acts as an extra vertex unit. */
    pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG, 0x3, gsh ? 2 : 0);
    pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG2, 0x3, 0);
    pica__write_masked(ctx, PICA_REG_VSH_COM_MODE, 0x1, gsh ? 1 : 0);

    if (gsh)
    {
        if (ctx->vsh_code_loaded != vsh->dvlp)
            upload_code(ctx, false, vsh->dvlp, 512);
        if (ctx->gsh_code_loaded != gsh->dvlp)
            upload_code(ctx, true, gsh->dvlp, 4096);
        ctx->vsh_code_loaded = vsh->dvlp;
        ctx->gsh_code_loaded = gsh->dvlp;
    }
    else
    {
        if (ctx->vsh_code_loaded != vsh->dvlp || ctx->gsh_code_loaded != vsh->dvlp)
            upload_code(ctx, false, vsh->dvlp, 512);
        ctx->vsh_code_loaded = vsh->dvlp;
        /* The geometry unit received a (possibly truncated) copy */
        ctx->gsh_code_loaded = vsh->dvlp->code_size > 512 ? NULL : vsh->dvlp;
    }

    if (!pica__reserve(ctx, 8 * 2 + 4 + pica__cmd_words(8) + 4))
        return;
    pica__write(ctx, PICA_REG_VSH_ENTRYPOINT, 0x7FFF0000u | (vsh->main_offset & 0xFFFF));
    pica__write(ctx, PICA_REG_VSH_OUTMAP_MASK, vsh->outmap_mask);
    pica__write(ctx, PICA_REG_VSH_OUTMAP_TOTAL1, vsh->outmap[0] - 1);
    pica__write(ctx, PICA_REG_VSH_OUTMAP_TOTAL2, vsh->outmap[0] - 1);
    if (gsh)
    {
        pica__write(ctx, PICA_REG_GSH_ENTRYPOINT, 0x7FFF0000u | (gsh->main_offset & 0xFFFF));
        pica__write(ctx, PICA_REG_GSH_OUTMAP_MASK, gsh->outmap_mask);
    }

    if (gsh && gsh->merge_outmaps)
    {
        merge_outmaps(outmap, vsh->outmap, gsh->outmap);
        mode |= vsh->outmap_mode;
        clock |= vsh->outmap_clock;
    }
    else
        memcpy(outmap, main->outmap, sizeof(outmap));

    pica__write_masked(ctx, PICA_REG_PRIMITIVE_CONFIG, 0x1, outmap[0] - 1);
    pica__write_inc(ctx, PICA_REG_SH_OUTMAP_TOTAL, outmap, 8);
    pica__write(ctx, PICA_REG_SH_OUTATTR_MODE, mode);
    pica__write(ctx, PICA_REG_SH_OUTATTR_CLOCK, clock);

    if (!pica__reserve(ctx, 10 + pica__cmd_words(2)))
        return;
    if (gsh)
    {
        unsigned stride = gsh_stride ? gsh_stride : vsh->outmap[0];
        uint32_t misc = gsh->gsh_mode;
        uint32_t perm[2] = { 0x76543210, 0xFEDCBA98 };

        pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG, 0xA, gsh->gsh_mode == 2 ? 0x80000000u : 0);
        if (gsh->gsh_mode == 1) /* fixed-size primitives */
            misc |= 0x01000000u | ((uint32_t)gsh->gsh_fixed_vtx_start << 16) | ((stride - 1) << 12) |
                    ((uint32_t)(gsh->gsh_fixed_vtx_num - 1) << 8);
        pica__write(ctx, PICA_REG_GSH_MISC0, misc);
        pica__write(ctx, PICA_REG_GSH_MISC1, gsh->gsh_mode == 2 ? gsh->gsh_variable_vtx_num - 1u : 0);
        pica__write(ctx, PICA_REG_GSH_INPUTBUFFER_CONFIG, 0x08000000u | (gsh->gsh_mode ? 0x0100u : 0) | (stride - 1));
        pica__write_inc(ctx, PICA_REG_GSH_ATTRIBUTES_PERMUTATION_LOW, perm, 2);
    }
    else
    {
        pica__write_masked(ctx, PICA_REG_GEOSTAGE_CONFIG, 0xA, 0);
        pica__write(ctx, PICA_REG_GSH_MISC0, 0);
        pica__write(ctx, PICA_REG_GSH_MISC1, 0);
        pica__write(ctx, PICA_REG_GSH_INPUTBUFFER_CONFIG, 0xA0000000u);
    }

    ctx->vsh = vsh;
    ctx->gsh = gsh;
    upload_consts(ctx, PICA_VERTEX_SHADER, vsh);
    if (gsh)
        upload_consts(ctx, PICA_GEOMETRY_SHADER, gsh);
}

/* ---- Uniforms ------------------------------------------------------------- */

void pica_uniform_f(pica_context* ctx, pica_shader_type_t unit, unsigned reg, const float* xyzw, unsigned count)
{
    int off = PICA_SH_REG_OFFSET(unit == PICA_GEOMETRY_SHADER);

    if (!xyzw || reg + count > 96)
    {
        pica__set_error(ctx, PICA_ERR_RANGE);
        return;
    }
    while (count)
    {
        /* 64 vectors (256 words) per command */
        unsigned n = count > 64 ? 64 : count;
        uint32_t words[256];
        for (unsigned i = 0; i < n; i++)
        {
            /* Float32 mode takes components in w, z, y, x order */
            for (unsigned c = 0; c < 4; c++)
                memcpy(&words[i * 4 + c], &xyzw[i * 4 + (3 - c)], sizeof(uint32_t));
        }
        if (!pica__reserve(ctx, 2 + pica__cmd_words(n * 4)))
            return;
        pica__write(ctx, (uint16_t)(PICA_REG_VSH_FLOATUNIFORM_CONFIG + off), 0x80000000u | reg);
        pica__write_rep(ctx, (uint16_t)(PICA_REG_VSH_FLOATUNIFORM_DATA + off), words, n * 4);
        reg += n;
        xyzw += n * 4;
        count -= n;
    }
}

void pica_uniform_4f(pica_context* ctx, pica_shader_type_t unit, unsigned reg, float x, float y, float z, float w)
{
    float v[4] = { x, y, z, w };
    pica_uniform_f(ctx, unit, reg, v, 1);
}

void pica_uniform_mtx4(pica_context* ctx, pica_shader_type_t unit, unsigned reg, const float m[16])
{
    pica_uniform_f(ctx, unit, reg, m, 4);
}

void pica_uniform_i(pica_context* ctx, pica_shader_type_t unit, unsigned idx, uint8_t x, uint8_t y, uint8_t z,
                    uint8_t w)
{
    int off = PICA_SH_REG_OFFSET(unit == PICA_GEOMETRY_SHADER);
    if (idx > 3)
    {
        pica__set_error(ctx, PICA_ERR_RANGE);
        return;
    }
    if (pica__reserve(ctx, 2))
        pica__write(ctx, (uint16_t)(PICA_REG_VSH_INTUNIFORM_I0 + off + idx),
                    x | ((uint32_t)y << 8) | ((uint32_t)z << 16) | ((uint32_t)w << 24));
}

void pica_uniform_b(pica_context* ctx, pica_shader_type_t unit, unsigned idx, bool value)
{
    int off = PICA_SH_REG_OFFSET(unit == PICA_GEOMETRY_SHADER);
    if (idx > 15 || unit > PICA_GEOMETRY_SHADER)
    {
        pica__set_error(ctx, PICA_ERR_RANGE);
        return;
    }
    ctx->bool_uniforms[unit] &= ~(1u << idx);
    ctx->bool_uniforms[unit] |= (uint16_t)((value ? 1u : 0u) << idx);
    if (pica__reserve(ctx, 2))
        pica__write(ctx, (uint16_t)(PICA_REG_VSH_BOOLUNIFORM + off), 0x7FFF0000u | ctx->bool_uniforms[unit]);
}
