/*
 * pica/types.h - PICA200 enumerations shared by the driver API.
 */
#ifndef PICA_TYPES_H
#define PICA_TYPES_H

#include <stdint.h>

/* Render buffer color formats (also valid texture formats with the same ID) */
typedef enum
{
    PICA_COLOR_RGBA8    = 0,
    PICA_COLOR_RGB8     = 1,
    PICA_COLOR_RGBA5551 = 2,
    PICA_COLOR_RGB565   = 3,
    PICA_COLOR_RGBA4    = 4,
} pica_color_format_t;

typedef enum
{
    PICA_DEPTH16        = 0,
    PICA_DEPTH24        = 2,
    PICA_DEPTH24_STENCIL8 = 3,
    PICA_DEPTH_NONE     = 0xFF, /* no depth buffer */
} pica_depth_format_t;

/* Texture formats */
typedef enum
{
    PICA_TEX_RGBA8    = 0x0,
    PICA_TEX_RGB8     = 0x1,
    PICA_TEX_RGBA5551 = 0x2,
    PICA_TEX_RGB565   = 0x3,
    PICA_TEX_RGBA4    = 0x4,
    PICA_TEX_LA8      = 0x5,
    PICA_TEX_HILO8    = 0x6,
    PICA_TEX_L8       = 0x7,
    PICA_TEX_A8       = 0x8,
    PICA_TEX_LA4      = 0x9,
    PICA_TEX_L4       = 0xA,
    PICA_TEX_A4       = 0xB,
    PICA_TEX_ETC1     = 0xC,
    PICA_TEX_ETC1A4   = 0xD,
} pica_tex_format_t;

typedef enum
{
    PICA_TEXTYPE_2D          = 0,
    PICA_TEXTYPE_CUBE        = 1,
    PICA_TEXTYPE_SHADOW_2D   = 2,
    PICA_TEXTYPE_PROJECTION  = 3,
    PICA_TEXTYPE_SHADOW_CUBE = 4,
    PICA_TEXTYPE_DISABLED    = 5,
} pica_tex_type_t;

typedef enum
{
    PICA_NEAREST = 0,
    PICA_LINEAR  = 1,
} pica_tex_filter_t;

typedef enum
{
    PICA_CLAMP_TO_EDGE   = 0,
    PICA_CLAMP_TO_BORDER = 1,
    PICA_REPEAT          = 2,
    PICA_MIRRORED_REPEAT = 3,
} pica_tex_wrap_t;

typedef enum
{
    PICA_TRIANGLES      = 0x0000,
    PICA_TRIANGLE_STRIP = 0x0100,
    PICA_TRIANGLE_FAN   = 0x0200,
    PICA_GEOMETRY_PRIM  = 0x0300, /* geometry shader primitives */
} pica_primitive_t;

typedef enum
{
    PICA_CULL_NONE      = 0,
    PICA_CULL_FRONT_CCW = 1,
    PICA_CULL_BACK_CCW  = 2,
} pica_cull_mode_t;

typedef enum
{
    PICA_NEVER    = 0,
    PICA_ALWAYS   = 1,
    PICA_EQUAL    = 2,
    PICA_NOTEQUAL = 3,
    PICA_LESS     = 4,
    PICA_LEQUAL   = 5,
    PICA_GREATER  = 6,
    PICA_GEQUAL   = 7,
} pica_test_func_t;

typedef enum
{
    PICA_STENCIL_KEEP      = 0,
    PICA_STENCIL_ZERO      = 1,
    PICA_STENCIL_REPLACE   = 2,
    PICA_STENCIL_INCR      = 3,
    PICA_STENCIL_DECR      = 4,
    PICA_STENCIL_INVERT    = 5,
    PICA_STENCIL_INCR_WRAP = 6,
    PICA_STENCIL_DECR_WRAP = 7,
} pica_stencil_op_t;

typedef enum
{
    PICA_WRITE_RED   = 0x01,
    PICA_WRITE_GREEN = 0x02,
    PICA_WRITE_BLUE  = 0x04,
    PICA_WRITE_ALPHA = 0x08,
    PICA_WRITE_DEPTH = 0x10,
    PICA_WRITE_COLOR = 0x0F,
    PICA_WRITE_ALL   = 0x1F,
} pica_write_mask_t;

typedef enum
{
    PICA_BLEND_ADD              = 0,
    PICA_BLEND_SUBTRACT         = 1,
    PICA_BLEND_REVERSE_SUBTRACT = 2,
    PICA_BLEND_MIN              = 3,
    PICA_BLEND_MAX              = 4,
} pica_blend_equation_t;

typedef enum
{
    PICA_ZERO                     = 0,
    PICA_ONE                      = 1,
    PICA_SRC_COLOR                = 2,
    PICA_ONE_MINUS_SRC_COLOR      = 3,
    PICA_DST_COLOR                = 4,
    PICA_ONE_MINUS_DST_COLOR      = 5,
    PICA_SRC_ALPHA                = 6,
    PICA_ONE_MINUS_SRC_ALPHA      = 7,
    PICA_DST_ALPHA                = 8,
    PICA_ONE_MINUS_DST_ALPHA      = 9,
    PICA_CONSTANT_COLOR           = 10,
    PICA_ONE_MINUS_CONSTANT_COLOR = 11,
    PICA_CONSTANT_ALPHA           = 12,
    PICA_ONE_MINUS_CONSTANT_ALPHA = 13,
    PICA_SRC_ALPHA_SATURATE       = 14,
} pica_blend_factor_t;

typedef enum
{
    PICA_LOGICOP_CLEAR = 0,  PICA_LOGICOP_AND = 1,  PICA_LOGICOP_AND_REVERSE = 2, PICA_LOGICOP_COPY = 3,
    PICA_LOGICOP_SET = 4,    PICA_LOGICOP_COPY_INVERTED = 5, PICA_LOGICOP_NOOP = 6, PICA_LOGICOP_INVERT = 7,
    PICA_LOGICOP_NAND = 8,   PICA_LOGICOP_OR = 9,   PICA_LOGICOP_NOR = 10,         PICA_LOGICOP_XOR = 11,
    PICA_LOGICOP_EQUIV = 12, PICA_LOGICOP_AND_INVERTED = 13, PICA_LOGICOP_OR_REVERSE = 14, PICA_LOGICOP_OR_INVERTED = 15,
} pica_logic_op_t;

typedef enum
{
    PICA_SCISSOR_DISABLE = 0,
    PICA_SCISSOR_INVERT  = 1, /* discard pixels inside the box */
    PICA_SCISSOR_NORMAL  = 3, /* discard pixels outside the box */
} pica_scissor_mode_t;

/* Vertex attribute component types */
typedef enum
{
    PICA_BYTE          = 0,
    PICA_UNSIGNED_BYTE = 1,
    PICA_SHORT         = 2,
    PICA_FLOAT         = 3,
} pica_attr_type_t;

/* Texture combiner sources */
typedef enum
{
    PICA_SRC_PRIMARY_COLOR            = 0x0,
    PICA_SRC_FRAGMENT_PRIMARY_COLOR   = 0x1,
    PICA_SRC_FRAGMENT_SECONDARY_COLOR = 0x2,
    PICA_SRC_TEXTURE0                 = 0x3,
    PICA_SRC_TEXTURE1                 = 0x4,
    PICA_SRC_TEXTURE2                 = 0x5,
    PICA_SRC_TEXTURE3                 = 0x6,
    PICA_SRC_PREVIOUS_BUFFER          = 0xD,
    PICA_SRC_CONSTANT                 = 0xE,
    PICA_SRC_PREVIOUS                 = 0xF,
} pica_texenv_src_t;

/* Combiner RGB operands */
typedef enum
{
    PICA_OP_RGB_SRC_COLOR           = 0x0,
    PICA_OP_RGB_ONE_MINUS_SRC_COLOR = 0x1,
    PICA_OP_RGB_SRC_ALPHA           = 0x2,
    PICA_OP_RGB_ONE_MINUS_SRC_ALPHA = 0x3,
    PICA_OP_RGB_SRC_R               = 0x4,
    PICA_OP_RGB_ONE_MINUS_SRC_R     = 0x5,
    PICA_OP_RGB_SRC_G               = 0x8,
    PICA_OP_RGB_ONE_MINUS_SRC_G     = 0x9,
    PICA_OP_RGB_SRC_B               = 0xC,
    PICA_OP_RGB_ONE_MINUS_SRC_B     = 0xD,
} pica_texenv_op_rgb_t;

/* Combiner alpha operands */
typedef enum
{
    PICA_OP_A_SRC_ALPHA           = 0x0,
    PICA_OP_A_ONE_MINUS_SRC_ALPHA = 0x1,
    PICA_OP_A_SRC_R               = 0x2,
    PICA_OP_A_ONE_MINUS_SRC_R     = 0x3,
    PICA_OP_A_SRC_G               = 0x4,
    PICA_OP_A_ONE_MINUS_SRC_G     = 0x5,
    PICA_OP_A_SRC_B               = 0x6,
    PICA_OP_A_ONE_MINUS_SRC_B     = 0x7,
} pica_texenv_op_alpha_t;

/* Combiner functions */
typedef enum
{
    PICA_REPLACE      = 0x0,
    PICA_MODULATE     = 0x1,
    PICA_ADD          = 0x2,
    PICA_ADD_SIGNED   = 0x3,
    PICA_INTERPOLATE  = 0x4,
    PICA_SUBTRACT     = 0x5,
    PICA_DOT3_RGB     = 0x6,
    PICA_DOT3_RGBA    = 0x7,
    PICA_MULTIPLY_ADD = 0x8,
    PICA_ADD_MULTIPLY = 0x9,
} pica_texenv_func_t;

typedef enum
{
    PICA_TEVSCALE_1 = 0,
    PICA_TEVSCALE_2 = 1,
    PICA_TEVSCALE_4 = 2,
} pica_texenv_scale_t;

/* Which half of a combiner stage a setter applies to */
typedef enum
{
    PICA_TEV_RGB   = 1,
    PICA_TEV_ALPHA = 2,
    PICA_TEV_BOTH  = 3,
} pica_texenv_mode_t;

/* Display transfer pixel formats */
typedef enum
{
    PICA_XFER_RGBA8  = 0,
    PICA_XFER_RGB8   = 1,
    PICA_XFER_RGB565 = 2,
    PICA_XFER_RGB5A1 = 3,
    PICA_XFER_RGBA4  = 4,
} pica_xfer_format_t;

/* Display transfer flags */
#define PICA_XFER_FLIP_VERT        (1u << 0)
#define PICA_XFER_OUT_TILED        (1u << 1)  /* linear -> tiled (default is tiled -> linear) */
#define PICA_XFER_RAW_COPY         (1u << 3)  /* texture copy mode */
#define PICA_XFER_IN_FORMAT(f)     (((uint32_t)(f) & 7) << 8)
#define PICA_XFER_OUT_FORMAT(f)    (((uint32_t)(f) & 7) << 12)
#define PICA_XFER_BLOCK32          (1u << 16)
#define PICA_XFER_SCALE_NONE       (0u << 24)
#define PICA_XFER_SCALE_X          (1u << 24) /* 2x1 downscale (anti-aliasing) */
#define PICA_XFER_SCALE_XY         (2u << 24) /* 2x2 downscale (anti-aliasing) */
#define PICA_XFER_DIM(w, h)        ((((uint32_t)(h)) << 16) | ((uint32_t)(w) & 0xFFFF))

typedef enum
{
    PICA_VERTEX_SHADER   = 0,
    PICA_GEOMETRY_SHADER = 1,
} pica_shader_type_t;

/* Clear flags for pica_clear() */
#define PICA_CLEAR_COLOR 1u
#define PICA_CLEAR_DEPTH 2u
#define PICA_CLEAR_ALL   3u

/* Driver error codes (functions return 0 on success, a negative value on failure) */
#define PICA_OK              0
#define PICA_ERR_ARG        -1
#define PICA_ERR_NOMEM      -2
#define PICA_ERR_PLATFORM   -3
#define PICA_ERR_RANGE      -4
#define PICA_ERR_FORMAT     -5
#define PICA_ERR_CMDBUF     -6

#endif /* PICA_TYPES_H */
