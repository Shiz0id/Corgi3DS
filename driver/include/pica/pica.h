/*
 * pica/pica.h - Corgi PICA200 driver: public API.
 *
 * A small, self-contained driver for the Nintendo 3DS GPU (DMP PICA200).
 * It builds GPU command lists, manages render targets, textures, vertex
 * input and shader programs, and hands work to the hardware through a
 * pluggable platform backend:
 *
 *   - pica_platform_ctru():  libctru / gsp::Gpu (3DS homebrew, .3dsx)
 *   - pica_platform_mmio():  direct register access (bare-metal ARM11 code,
 *                            and the Corgi3DS-based host test harness)
 *
 * All GPU operations issued by the driver are synchronous: when a call that
 * submits work (pica_flush, pica_clear, pica_transfer, pica_tex_upload, ...)
 * returns, the GPU has finished it.  State setters only append to the
 * command buffer; the buffer is submitted automatically when it fills up or
 * when an operation needs the GPU to be idle.
 *
 * Conventions:
 *   - Pointers handed to the GPU must be in GPU-visible memory: allocate them
 *     with pica_alloc_linear() / pica_alloc_vram().
 *   - Colors are 0xRRGGBBAA unless stated otherwise.
 *   - Matrices are row-major float[16]; shaders compute dp4(row_i, v).
 *   - The 3DS LCDs are mounted rotated: a top-screen render target is 240
 *     pixels wide and 400 tall.  pica_mtx_ortho_tilt / pica_mtx_persp_tilt
 *     build projections that compensate for this.
 */
#ifndef PICA_PICA_H
#define PICA_PICA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "regs.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * Platform backend
 * ======================================================================== */

typedef struct pica_platform
{
    void* user;

    /* Memory management.  Linear memory is physically contiguous FCRAM. */
    uint32_t (*virt_to_phys)(void* user, const void* ptr);
    void*    (*alloc_linear)(void* user, size_t size, size_t align);
    void     (*free_linear)(void* user, void* ptr);
    void*    (*alloc_vram)(void* user, size_t size);
    void     (*free_vram)(void* user, void* ptr);

    /* Cache maintenance: make CPU writes visible to the GPU, and GPU writes
     * visible to the CPU. */
    void (*flush_dcache)(void* user, const void* ptr, size_t size);
    void (*invalidate_dcache)(void* user, const void* ptr, size_t size);

    /* GPU engines.  Each call must block until the operation has finished. */
    int (*run_cmdlist)(void* user, const uint32_t* buf, size_t size_bytes);
    int (*memory_fill)(void* user, void* start, size_t size_bytes, uint32_t value, unsigned width_bits);
    int (*display_transfer)(void* user, const void* src, uint32_t src_dim, void* dst, uint32_t dst_dim, uint32_t flags);
    int (*texture_copy)(void* user, const void* src, uint32_t src_line, void* dst, uint32_t dst_line,
                        size_t size_bytes, uint32_t flags);
} pica_platform;

#if defined(__3DS__)
/* libctru backend.  Call gfxInit*() before pica_init().
 *
 * By default the whole linear heap is written back from the CPU cache before
 * every command list, so vertex/index/texture data never needs explicit
 * pica_flush_dcache() calls (this is what citro3d does too).  Disable it with
 * pica_ctru_auto_flush(false) if you manage cache maintenance yourself. */
const pica_platform* pica_platform_ctru(void);
void pica_ctru_auto_flush(bool enable);
#endif

/* Direct-MMIO backend.  The caller supplies register accessors and memory
 * arenas; see pica_mmio.h. */
typedef struct pica_mmio_desc pica_mmio_desc;
int pica_platform_mmio(pica_platform* out, pica_mmio_desc* desc);

/* ===========================================================================
 * Command buffer
 * ======================================================================== */

typedef struct pica_cmdbuf
{
    uint32_t* buf;
    uint32_t  capacity; /* in words */
    uint32_t  pos;      /* in words */
} pica_cmdbuf;

/* ===========================================================================
 * Render targets
 * ======================================================================== */

typedef struct pica_framebuffer
{
    void*    color;       /* VRAM, or NULL */
    void*    depth;       /* VRAM, or NULL */
    uint16_t width, height;
    uint8_t  color_fmt;   /* pica_color_format */
    uint8_t  depth_fmt;   /* pica_depth_format */
    bool     owns_memory;
} pica_framebuffer;

/* ===========================================================================
 * Textures
 * ======================================================================== */

typedef struct pica_texture
{
    void*    data;
    size_t   size;        /* bytes, all mip levels */
    uint16_t width, height;
    uint8_t  fmt;         /* pica_tex_format */
    uint8_t  max_level;
    bool     in_vram;
    uint32_t param;       /* TEXUNITn_PARAM */
    uint32_t border;      /* TEXUNITn_BORDER_COLOR (0xAABBGGRR) */
    uint32_t lod;         /* TEXUNITn_LOD */
} pica_texture;

/* Bytes taken by one mip level of a w x h texture of the given format. */
size_t pica_tex_level_size(pica_tex_format_t fmt, unsigned w, unsigned h);
unsigned pica_tex_bpp(pica_tex_format_t fmt); /* bits per pixel, 0 if unknown */

/* Convert a linear image (row 0 = top) to the PICA's 8x8 Morton-tiled layout.
 * Pixels are expected in the texture's native byte order (see README).
 * Supports every format except ETC1/ETC1A4 (which are already block-based). */
int pica_tex_tile(void* dst, const void* src, unsigned w, unsigned h, pica_tex_format_t fmt);
/* Inverse of pica_tex_tile (useful for reading back render targets). */
int pica_tex_untile(void* dst, const void* src, unsigned w, unsigned h, pica_tex_format_t fmt);

/* ===========================================================================
 * Shaders
 * ======================================================================== */

typedef struct pica_dvlp
{
    const uint32_t* code;
    uint32_t        code_size;   /* words */
    uint32_t        opdesc[128];
    uint32_t        opdesc_size; /* entries */
} pica_dvlp;

typedef struct pica_shader_const
{
    uint16_t type; /* 0 = bool, 1 = int (u8 x4), 2 = float24 */
    uint16_t id;
    uint32_t data[4];
} pica_shader_const;

typedef struct pica_shader_output
{
    uint16_t type; /* 0 pos, 1 normalquat, 2 color, 3 texcoord0, 4 texcoord0w, 5 texcoord1, 6 texcoord2, 8 view */
    uint16_t reg;
    uint8_t  mask;
    uint8_t  pad[3];
} pica_shader_output;

typedef struct pica_shader_uniform
{
    uint32_t symbol_offset;
    uint16_t start_reg;
    uint16_t end_reg;
} pica_shader_uniform_entry;

typedef struct pica_dvle
{
    const pica_dvlp* dvlp;
    uint8_t  type;          /* pica_shader_type */
    bool     merge_outmaps;
    uint32_t main_offset;
    uint32_t endmain_offset;

    /* Geometry shader parameters */
    uint8_t  gsh_mode;
    uint8_t  gsh_fixed_vtx_start;
    uint8_t  gsh_variable_vtx_num;
    uint8_t  gsh_fixed_vtx_num;

    const pica_shader_const*   consts;
    uint32_t                   num_consts;
    const pica_shader_output*  outputs;
    uint32_t                   num_outputs;
    const pica_shader_uniform_entry* uniforms;
    uint32_t                   num_uniforms;
    const char*                symbols;
    uint32_t                   symbols_size;

    /* Derived output mapping */
    uint32_t outmap[8];     /* [0] = number of outputs, [1..7] = SH_OUTMAP_On */
    uint32_t outmap_mask;   /* output registers written */
    uint32_t outmap_mode;
    uint32_t outmap_clock;
} pica_dvle;

#define PICA_MAX_DVLE 8

typedef struct pica_shbin
{
    pica_dvlp dvlp;
    pica_dvle dvle[PICA_MAX_DVLE];
    uint32_t  num_dvle;
} pica_shbin;

/* Parse a compiled shader binary (.shbin, as produced by picasso).  The
 * parsed structure points into `data`, which must stay alive. */
int pica_shbin_parse(pica_shbin* out, const void* data, size_t size);

typedef enum
{
    PICA_UNIFORM_NONE  = 0,
    PICA_UNIFORM_FLOAT = 1, /* index into c0-c95 */
    PICA_UNIFORM_INT   = 2, /* index into i0-i3 */
    PICA_UNIFORM_BOOL  = 3, /* index into b0-b15 */
} pica_uniform_kind;

typedef struct pica_uniform_loc
{
    pica_uniform_kind kind;
    int index;
    int count;
} pica_uniform_loc;

/* Look up a uniform declared in the shader source (".fvec projection[4]"). */
pica_uniform_loc pica_shader_uniform(const pica_dvle* dvle, const char* name);
/* Shortcut: float register index of a uniform, or -1. */
int pica_shader_uniform_reg(const pica_dvle* dvle, const char* name);

/* ===========================================================================
 * Vertex input
 * ======================================================================== */

typedef struct pica_attr_info
{
    uint32_t format[2];     /* ATTRIBBUFFERS_FORMAT_LOW/HIGH */
    uint32_t permutation[2];/* VSH_ATTRIBUTES_PERMUTATION_LOW/HIGH */
    uint8_t  count;
} pica_attr_info;

void pica_attr_init(pica_attr_info* info);
/* Add an attribute fetched from a vertex buffer.  `reg` is the shader input
 * register (v0-v15) it feeds, or -1 for "same as attribute index".  Returns
 * the attribute index or a negative error. */
int pica_attr_add_loader(pica_attr_info* info, int reg, pica_attr_type_t type, int components);
/* Add an attribute whose value comes from pica_fixed_attrib(). */
int pica_attr_add_fixed(pica_attr_info* info, int reg);

typedef struct pica_buf_info
{
    uint32_t base_phys;
    uint8_t  count;
    struct
    {
        uint32_t offset;
        uint32_t config1;
        uint32_t config2;
    } buf[12];
} pica_buf_info;

void pica_buf_init(pica_buf_info* info);
/* Add a vertex buffer containing `attr_count` attributes per vertex, laid
 * out as described by `permutation` (nibble n = attribute index of the n-th
 * element; 0xC-0xF insert 4/8/12/16 bytes of padding).  For the common case
 * of attributes 0..n-1 stored in order, pass PICA_PERMUTATION_SEQ(n).
 * Returns the buffer index or a negative error. */
struct pica_context;
int pica_buf_add(struct pica_context* ctx, pica_buf_info* info, const void* data, unsigned stride,
                 unsigned attr_count, uint64_t permutation);
#define PICA_PERMUTATION_SEQ(n) (0xFEDCBA9876543210ull & ((n) >= 16 ? ~0ull : ((1ull << ((n) * 4)) - 1)))

/* ===========================================================================
 * Texture combiners
 * ======================================================================== */

typedef struct pica_texenv
{
    uint16_t src_rgb, src_alpha;
    uint16_t op_rgb, op_alpha;
    uint16_t func_rgb, func_alpha;
    uint32_t color;            /* constant, 0xAABBGGRR as the register wants it */
    uint16_t scale_rgb, scale_alpha;
} pica_texenv;

/* Reset to "pass the previous stage through unchanged". */
void pica_texenv_init(pica_texenv* env);
void pica_texenv_src(pica_texenv* env, pica_texenv_mode_t mode, int s0, int s1, int s2);
void pica_texenv_op_rgb(pica_texenv* env, int o0, int o1, int o2);
void pica_texenv_op_alpha(pica_texenv* env, int o0, int o1, int o2);
void pica_texenv_func(pica_texenv* env, pica_texenv_mode_t mode, pica_texenv_func_t func);
void pica_texenv_color(pica_texenv* env, uint32_t rgba);
void pica_texenv_scale(pica_texenv* env, pica_texenv_mode_t mode, pica_texenv_scale_t scale);

/* ===========================================================================
 * Context
 * ======================================================================== */

typedef struct pica_context
{
    pica_platform plat;
    pica_cmdbuf   cmd;
    int           error;          /* first error seen, sticky until pica_get_error */

    const pica_framebuffer* fb;
    bool          draw_used;      /* draws issued since the last framebuffer flush */

    const pica_dvle* vsh;
    const pica_dvle* gsh;
    const pica_dvlp* vsh_code_loaded;
    const pica_dvlp* gsh_code_loaded;
    uint16_t      bool_uniforms[2];

    uint32_t      texunit_config;
    uint32_t      color_operation;
    uint32_t      alpha_blend;
    uint32_t      logic_op;
    uint32_t      depth_color_mask;
    uint32_t      texenv_update;  /* TEXENV_UPDATE_BUFFER */
} pica_context;

/* Initialise the driver.  `cmdbuf_bytes` is the size of the command buffer
 * (0 = default 256 KiB).  Sends a full default state to the GPU. */
int  pica_init(pica_context* ctx, const pica_platform* plat, size_t cmdbuf_bytes);
void pica_fini(pica_context* ctx);

/* Return and clear the first error recorded by a void API call. */
int  pica_get_error(pica_context* ctx);

/* Submit all pending commands and wait for the GPU to finish them. */
int  pica_flush(pica_context* ctx);

/* GPU-visible memory */
void* pica_alloc_linear(pica_context* ctx, size_t size);
void  pica_free_linear(pica_context* ctx, void* ptr);
void* pica_alloc_vram(pica_context* ctx, size_t size);
void  pica_free_vram(pica_context* ctx, void* ptr);
/* Write back CPU caches for memory the GPU is about to read. */
void  pica_flush_dcache(pica_context* ctx, const void* ptr, size_t size);
/* Discard CPU caches for memory the GPU wrote, before the CPU reads it. */
void  pica_invalidate_dcache(pica_context* ctx, const void* ptr, size_t size);
uint32_t pica_virt_to_phys(pica_context* ctx, const void* ptr);

/* Raw register access (escape hatch for features without a helper). */
void pica_write_reg(pica_context* ctx, uint16_t reg, uint32_t value);
void pica_write_reg_masked(pica_context* ctx, uint16_t reg, uint32_t mask, uint32_t value);
void pica_write_regs(pica_context* ctx, uint16_t first_reg, const uint32_t* values, unsigned count);
void pica_write_reg_repeat(pica_context* ctx, uint16_t reg, const uint32_t* values, unsigned count);

/* ---- Render targets ---- */
int  pica_framebuffer_create(pica_context* ctx, pica_framebuffer* fb, unsigned width, unsigned height,
                             pica_color_format_t color_fmt, pica_depth_format_t depth_fmt);
void pica_framebuffer_destroy(pica_context* ctx, pica_framebuffer* fb);
size_t pica_color_buffer_size(unsigned width, unsigned height, pica_color_format_t fmt);
size_t pica_depth_buffer_size(unsigned width, unsigned height, pica_depth_format_t fmt);

/* Make `fb` the current render target and reset the viewport to cover it. */
void pica_bind_framebuffer(pica_context* ctx, const pica_framebuffer* fb);
/* Fill color and/or depth with the memory fill engine.  `color` is
 * 0xRRGGBBAA; `depth` is the raw depth value (24-bit depth in bits 0-23,
 * stencil in bits 24-31 for D24S8). */
int  pica_clear(pica_context* ctx, const pica_framebuffer* fb, unsigned flags, uint32_t color, uint32_t depth);
/* Copy (and convert/untile) the color buffer to linear memory, e.g. an LCD
 * framebuffer.  `dst_w`/`dst_h` are in the same rotated orientation. */
int  pica_transfer(pica_context* ctx, const pica_framebuffer* fb, void* dst, unsigned dst_w, unsigned dst_h,
                   pica_xfer_format_t dst_fmt, uint32_t extra_flags);
/* Raw access to the display transfer / texture copy engines. */
int  pica_display_transfer(pica_context* ctx, const void* src, uint32_t src_dim, void* dst, uint32_t dst_dim,
                           uint32_t flags);
int  pica_texture_copy(pica_context* ctx, const void* src, uint32_t src_line, void* dst, uint32_t dst_line,
                       size_t size, uint32_t flags);
int  pica_memory_fill(pica_context* ctx, void* start, size_t size, uint32_t value, unsigned width_bits);

/* ---- Rasterizer state ---- */
void pica_viewport(pica_context* ctx, unsigned x, unsigned y, unsigned w, unsigned h);
void pica_scissor(pica_context* ctx, pica_scissor_mode_t mode, unsigned left, unsigned top, unsigned right,
                  unsigned bottom);
void pica_cull(pica_context* ctx, pica_cull_mode_t mode);
/* Depth map: depth = z * scale + offset (z-buffer) or w-buffer when !use_z. */
void pica_depth_map(pica_context* ctx, bool use_z, float scale, float offset);

/* ---- Fragment operations ---- */
void pica_depth_test(pica_context* ctx, bool enable, pica_test_func_t func, unsigned write_mask);
void pica_alpha_test(pica_context* ctx, bool enable, pica_test_func_t func, uint8_t ref);
void pica_stencil_test(pica_context* ctx, bool enable, pica_test_func_t func, uint8_t ref, uint8_t input_mask,
                       uint8_t write_mask);
void pica_stencil_op(pica_context* ctx, pica_stencil_op_t fail, pica_stencil_op_t depth_fail,
                     pica_stencil_op_t pass);
void pica_blend(pica_context* ctx, pica_blend_equation_t eq_rgb, pica_blend_equation_t eq_alpha,
                pica_blend_factor_t src_rgb, pica_blend_factor_t dst_rgb,
                pica_blend_factor_t src_alpha, pica_blend_factor_t dst_alpha);
void pica_blend_color(pica_context* ctx, uint32_t rgba);
/* Switch from blending to a framebuffer logic operation. */
void pica_logic_op(pica_context* ctx, pica_logic_op_t op);

/* ---- Texture combiners ---- */
void pica_set_texenv(pica_context* ctx, int stage, const pica_texenv* env);
/* Select which of stages 0-3 write their result into the combiner buffer
 * (bit n = stage n), and the buffer's initial color. */
void pica_texenv_buffer(pica_context* ctx, unsigned rgb_mask, unsigned alpha_mask, uint32_t initial_rgba);

/* ---- Textures ---- */
int  pica_tex_create(pica_context* ctx, pica_texture* tex, unsigned width, unsigned height,
                     pica_tex_format_t fmt, unsigned max_level, bool in_vram);
void pica_tex_destroy(pica_context* ctx, pica_texture* tex);
void pica_tex_filter(pica_texture* tex, pica_tex_filter_t mag, pica_tex_filter_t min);
void pica_tex_filter_mip(pica_texture* tex, pica_tex_filter_t mip);
void pica_tex_wrap(pica_texture* tex, pica_tex_wrap_t s, pica_tex_wrap_t t);
void pica_tex_border(pica_texture* tex, uint32_t rgba);
void pica_tex_lod(pica_texture* tex, float bias, unsigned min_level, unsigned max_level);
/* Pointer to the start of mip level `level` inside tex->data. */
void* pica_tex_level_ptr(const pica_texture* tex, unsigned level, size_t* size_out);
/* Upload already-tiled texel data for one mip level. */
int  pica_tex_upload_tiled(pica_context* ctx, pica_texture* tex, unsigned level, const void* data);
/* Tile a linear image (row 0 = top, native texel byte order) and upload it. */
int  pica_tex_upload(pica_context* ctx, pica_texture* tex, unsigned level, const void* linear);
/* Bind a texture to unit 0-2 (NULL disables the unit). */
void pica_tex_bind(pica_context* ctx, int unit, const pica_texture* tex);
/* Invalidate the GPU's texture cache (after changing texture memory). */
void pica_tex_cache_clear(pica_context* ctx);

/* ---- Shaders ---- */
/* Bind a vertex shader and optional geometry shader.  `gsh_stride` is the
 * number of vertex shader outputs per geometry shader input vertex
 * (0 = number of vertex shader outputs). Constants declared in the shaders
 * are uploaded. */
void pica_bind_shader(pica_context* ctx, const pica_dvle* vsh, const pica_dvle* gsh, unsigned gsh_stride);
/* Upload `count` vec4 float uniforms starting at c[reg]; `xyzw` holds
 * x,y,z,w for each vector. */
void pica_uniform_f(pica_context* ctx, pica_shader_type_t unit, unsigned reg, const float* xyzw, unsigned count);
void pica_uniform_4f(pica_context* ctx, pica_shader_type_t unit, unsigned reg, float x, float y, float z, float w);
/* Upload a row-major 4x4 matrix to c[reg]..c[reg+3]. */
void pica_uniform_mtx4(pica_context* ctx, pica_shader_type_t unit, unsigned reg, const float m[16]);
void pica_uniform_i(pica_context* ctx, pica_shader_type_t unit, unsigned idx, uint8_t x, uint8_t y, uint8_t z,
                    uint8_t w);
void pica_uniform_b(pica_context* ctx, pica_shader_type_t unit, unsigned idx, bool value);

/* ---- Geometry ---- */
void pica_bind_attrs(pica_context* ctx, const pica_attr_info* info);
void pica_bind_buffers(pica_context* ctx, const pica_buf_info* info);
/* Value of a fixed (non-buffer) attribute. */
void pica_fixed_attrib(pica_context* ctx, unsigned index, float x, float y, float z, float w);

void pica_draw_arrays(pica_context* ctx, pica_primitive_t prim, unsigned first, unsigned count);
/* `indices` must lie inside GPU memory at or above the buffer base (i.e.
 * linear memory or VRAM); index_type 0 = u8, 1 = u16. */
void pica_draw_elements(pica_context* ctx, pica_primitive_t prim, unsigned count, int index_type,
                        const void* indices);

/* Immediate mode: send every attribute of every vertex in order. */
void pica_imm_begin(pica_context* ctx, pica_primitive_t prim);
void pica_imm_attr(pica_context* ctx, float x, float y, float z, float w);
void pica_imm_end(pica_context* ctx);

/* ---- Fog ---- */
/* Enable fog.  `lut129` holds 129 fog factors (1 = no fog, 0 = fully fogged)
 * sampled uniformly over the fragment depth range 0..1 (z_flip inverts the
 * depth used for the lookup).  `color` is 0xRRGGBBAA (alpha ignored).
 * Pass NULL to disable fog. */
void pica_fog(pica_context* ctx, const float* lut129, uint32_t color, bool z_flip);

#ifdef __cplusplus
}
#endif

#include "floats.h"
#include "matrix.h"

#endif /* PICA_PICA_H */
