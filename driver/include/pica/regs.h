/*
 * pica/regs.h - PICA200 register map.
 *
 * Two register spaces are described here:
 *
 *  1. "Internal" (P3D) registers, which are only written through GPU command
 *     lists.  IDs are 16-bit indices; a command list entry targets one or
 *     more of them.  (On real hardware they are also mapped at
 *     0x10401000 + id*4, but writes must go through the command processor
 *     for the rasterizer state to be latched correctly.)
 *
 *  2. "External" MMIO registers at physical 0x10400000 (memory fill engines,
 *     the display transfer / texture copy engine, LCD framebuffer setup and
 *     the command-list DMA kick).  Under Horizon these are owned by the
 *     gsp::Gpu sysmodule; bare-metal code may poke them directly.
 *
 * Sources: Corgi3DS's PICA200 implementation (src/core/arm11/gpu.cpp),
 * libctru's register list and 3dbrew's GPU documentation.
 */
#ifndef PICA_REGS_H
#define PICA_REGS_H

/* ---------------------------------------------------------------------------
 * Command list header encoding
 *
 *   word 0: first parameter
 *   word 1: header
 *            bits  0-15  register ID
 *            bits 16-19  byte-enable mask (bit n enables byte n of the value)
 *            bits 20-27  number of *extra* parameters that follow
 *            bit  31     1 = extra parameters go to consecutive registers,
 *                        0 = all parameters are written to the same register
 *   extra parameters, then one padding word if needed to keep 8-byte alignment.
 * ------------------------------------------------------------------------- */
#define PICA_CMD_HEADER(reg, mask, extra, incremental) \
    (((reg) & 0xFFFF) | (((mask) & 0xF) << 16) | (((extra) & 0xFF) << 20) | ((incremental) ? 0x80000000u : 0))

#define PICA_CMD_MAX_PARAMS 256

/* ---------------------------------------------------------------------------
 * Internal registers
 * ------------------------------------------------------------------------- */
#define PICA_REG_IRQ_ACK0                0x0000 /* 0x0000-0x000F: IRQ acknowledge/compare */
#define PICA_REG_FINALIZE                0x0010 /* writing raises the "command list done" IRQ */

/* Rasterizer */
#define PICA_REG_FACECULLING_CONFIG      0x0040
#define PICA_REG_VIEWPORT_WIDTH          0x0041 /* f24: width / 2 */
#define PICA_REG_VIEWPORT_INVW           0x0042 /* f31 << 1: 2 / width */
#define PICA_REG_VIEWPORT_HEIGHT         0x0043 /* f24: height / 2 */
#define PICA_REG_VIEWPORT_INVH           0x0044 /* f31 << 1: 2 / height */
#define PICA_REG_FRAGOP_CLIP             0x0047
#define PICA_REG_FRAGOP_CLIP_DATA0       0x0048
#define PICA_REG_DEPTHMAP_SCALE          0x004D /* f24 */
#define PICA_REG_DEPTHMAP_OFFSET         0x004E /* f24 */
#define PICA_REG_SH_OUTMAP_TOTAL         0x004F
#define PICA_REG_SH_OUTMAP_O0            0x0050 /* 0x0050-0x0056 */
#define PICA_REG_EARLYDEPTH_FUNC         0x0061
#define PICA_REG_EARLYDEPTH_TEST1        0x0062
#define PICA_REG_EARLYDEPTH_CLEAR        0x0063
#define PICA_REG_SH_OUTATTR_MODE         0x0064
#define PICA_REG_SCISSORTEST_MODE        0x0065
#define PICA_REG_SCISSORTEST_POS         0x0066
#define PICA_REG_SCISSORTEST_DIM         0x0067
#define PICA_REG_VIEWPORT_XY             0x0068
#define PICA_REG_EARLYDEPTH_DATA         0x006A
#define PICA_REG_DEPTHMAP_ENABLE         0x006D
#define PICA_REG_RENDERBUF_DIM           0x006E
#define PICA_REG_SH_OUTATTR_CLOCK        0x006F

/* Texturing */
#define PICA_REG_TEXUNIT_CONFIG          0x0080
#define PICA_REG_TEXUNIT0_BORDER_COLOR   0x0081
#define PICA_REG_TEXUNIT0_DIM            0x0082
#define PICA_REG_TEXUNIT0_PARAM          0x0083
#define PICA_REG_TEXUNIT0_LOD            0x0084
#define PICA_REG_TEXUNIT0_ADDR1          0x0085 /* 0x0085-0x008A: 2D / cube faces */
#define PICA_REG_TEXUNIT0_SHADOW         0x008B
#define PICA_REG_TEXUNIT0_TYPE           0x008E
#define PICA_REG_LIGHTING_ENABLE0        0x008F
#define PICA_REG_TEXUNIT1_BORDER_COLOR   0x0091
#define PICA_REG_TEXUNIT1_DIM            0x0092
#define PICA_REG_TEXUNIT1_PARAM          0x0093
#define PICA_REG_TEXUNIT1_LOD            0x0094
#define PICA_REG_TEXUNIT1_ADDR           0x0095
#define PICA_REG_TEXUNIT1_TYPE           0x0096
#define PICA_REG_TEXUNIT2_BORDER_COLOR   0x0099
#define PICA_REG_TEXUNIT2_DIM            0x009A
#define PICA_REG_TEXUNIT2_PARAM          0x009B
#define PICA_REG_TEXUNIT2_LOD            0x009C
#define PICA_REG_TEXUNIT2_ADDR           0x009D
#define PICA_REG_TEXUNIT2_TYPE           0x009E

/* Texture combiners ("TexEnv").  Stages 0-3 are at 0xC0 + 8*n, 4-5 at 0xF0 + 8*(n-4). */
#define PICA_REG_TEXENV0_SOURCE          0x00C0
#define PICA_REG_TEXENV4_SOURCE          0x00F0
#define PICA_TEXENV_REG(stage)           ((stage) < 4 ? 0x00C0 + (stage) * 8 : 0x00F0 + ((stage) - 4) * 8)
#define PICA_REG_TEXENV_UPDATE_BUFFER    0x00E0
#define PICA_REG_FOG_COLOR               0x00E1
#define PICA_REG_FOG_LUT_INDEX           0x00E6
#define PICA_REG_FOG_LUT_DATA0           0x00E8
#define PICA_REG_TEXENV_BUFFER_COLOR     0x00FD

/* Framebuffer / per-fragment operations */
#define PICA_REG_COLOR_OPERATION         0x0100
#define PICA_REG_BLEND_FUNC              0x0101
#define PICA_REG_LOGIC_OP                0x0102
#define PICA_REG_BLEND_COLOR             0x0103
#define PICA_REG_FRAGOP_ALPHA_TEST       0x0104
#define PICA_REG_STENCIL_TEST            0x0105
#define PICA_REG_STENCIL_OP              0x0106
#define PICA_REG_DEPTH_COLOR_MASK        0x0107
#define PICA_REG_FRAMEBUFFER_INVALIDATE  0x0110
#define PICA_REG_FRAMEBUFFER_FLUSH       0x0111
#define PICA_REG_COLORBUFFER_READ        0x0112
#define PICA_REG_COLORBUFFER_WRITE       0x0113
#define PICA_REG_DEPTHBUFFER_READ        0x0114
#define PICA_REG_DEPTHBUFFER_WRITE       0x0115
#define PICA_REG_DEPTHBUFFER_FORMAT      0x0116
#define PICA_REG_COLORBUFFER_FORMAT      0x0117
#define PICA_REG_EARLYDEPTH_TEST2        0x0118
#define PICA_REG_FRAMEBUFFER_BLOCK32     0x011B
#define PICA_REG_DEPTHBUFFER_LOC         0x011C
#define PICA_REG_COLORBUFFER_LOC         0x011D
#define PICA_REG_FRAMEBUFFER_DIM         0x011E
#define PICA_REG_GAS_DELTAZ_DEPTH        0x0126
#define PICA_REG_FRAGOP_SHADOW           0x0130

/* Fragment lighting (registers only; see pica_write_reg for raw access) */
#define PICA_REG_LIGHT0_SPECULAR0        0x0140 /* light n at 0x140 + 0x10*n */
#define PICA_REG_LIGHTING_AMBIENT        0x01C0
#define PICA_REG_LIGHTING_NUM_LIGHTS     0x01C2
#define PICA_REG_LIGHTING_CONFIG0        0x01C3
#define PICA_REG_LIGHTING_CONFIG1        0x01C4
#define PICA_REG_LIGHTING_LUT_INDEX      0x01C5
#define PICA_REG_LIGHTING_ENABLE1        0x01C6
#define PICA_REG_LIGHTING_LUT_DATA0      0x01C8
#define PICA_REG_LIGHTING_LUTINPUT_ABS   0x01D0
#define PICA_REG_LIGHTING_LUTINPUT_SELECT 0x01D1
#define PICA_REG_LIGHTING_LUTINPUT_SCALE 0x01D2
#define PICA_REG_LIGHTING_LIGHT_PERMUTATION 0x01D9

/* Geometry pipeline */
#define PICA_REG_ATTRIBBUFFERS_LOC       0x0200
#define PICA_REG_ATTRIBBUFFERS_FORMAT_LOW  0x0201
#define PICA_REG_ATTRIBBUFFERS_FORMAT_HIGH 0x0202
#define PICA_REG_ATTRIBBUFFER0_OFFSET    0x0203 /* buffer n at 0x203 + 3*n: OFFSET, CONFIG1, CONFIG2 */
#define PICA_REG_INDEXBUFFER_CONFIG      0x0227
#define PICA_REG_NUMVERTICES             0x0228
#define PICA_REG_GEOSTAGE_CONFIG         0x0229
#define PICA_REG_VERTEX_OFFSET           0x022A
#define PICA_REG_POST_VERTEX_CACHE_NUM   0x022D
#define PICA_REG_DRAWARRAYS              0x022E
#define PICA_REG_DRAWELEMENTS            0x022F
#define PICA_REG_VTX_FUNC                0x0231 /* write 1: clear post-vertex cache */
#define PICA_REG_FIXEDATTRIB_INDEX       0x0232
#define PICA_REG_FIXEDATTRIB_DATA0       0x0233
#define PICA_REG_CMDBUF_SIZE0            0x0238
#define PICA_REG_CMDBUF_SIZE1            0x0239
#define PICA_REG_CMDBUF_ADDR0            0x023A
#define PICA_REG_CMDBUF_ADDR1            0x023B
#define PICA_REG_CMDBUF_JUMP0            0x023C
#define PICA_REG_CMDBUF_JUMP1            0x023D
#define PICA_REG_VSH_NUM_ATTR            0x0242
#define PICA_REG_VSH_COM_MODE            0x0244
#define PICA_REG_START_DRAW_FUNC0        0x0245
#define PICA_REG_VSH_OUTMAP_TOTAL1       0x024A
#define PICA_REG_VSH_OUTMAP_TOTAL2       0x0251
#define PICA_REG_GSH_MISC0               0x0252
#define PICA_REG_GEOSTAGE_CONFIG2        0x0253
#define PICA_REG_GSH_MISC1               0x0254
#define PICA_REG_PRIMITIVE_CONFIG        0x025E
#define PICA_REG_RESTART_PRIMITIVE       0x025F

/* Shader units.  The geometry shader block (0x280) mirrors the vertex shader
 * block (0x2B0); PICA_SH_REG_OFFSET(gsh) gives the difference. */
#define PICA_REG_GSH_BOOLUNIFORM         0x0280
#define PICA_REG_GSH_INTUNIFORM_I0       0x0281
#define PICA_REG_GSH_INPUTBUFFER_CONFIG  0x0289
#define PICA_REG_GSH_ENTRYPOINT          0x028A
#define PICA_REG_GSH_ATTRIBUTES_PERMUTATION_LOW 0x028B
#define PICA_REG_GSH_OUTMAP_MASK         0x028D
#define PICA_REG_GSH_CODETRANSFER_END    0x028F
#define PICA_REG_GSH_FLOATUNIFORM_CONFIG 0x0290
#define PICA_REG_GSH_FLOATUNIFORM_DATA   0x0291
#define PICA_REG_GSH_CODETRANSFER_CONFIG 0x029B
#define PICA_REG_GSH_CODETRANSFER_DATA   0x029C
#define PICA_REG_GSH_OPDESCS_CONFIG      0x02A5
#define PICA_REG_GSH_OPDESCS_DATA        0x02A6

#define PICA_REG_VSH_BOOLUNIFORM         0x02B0
#define PICA_REG_VSH_INTUNIFORM_I0       0x02B1
#define PICA_REG_VSH_INPUTBUFFER_CONFIG  0x02B9
#define PICA_REG_VSH_ENTRYPOINT          0x02BA
#define PICA_REG_VSH_ATTRIBUTES_PERMUTATION_LOW 0x02BB
#define PICA_REG_VSH_OUTMAP_MASK         0x02BD
#define PICA_REG_VSH_CODETRANSFER_END    0x02BF
#define PICA_REG_VSH_FLOATUNIFORM_CONFIG 0x02C0
#define PICA_REG_VSH_FLOATUNIFORM_DATA   0x02C1
#define PICA_REG_VSH_CODETRANSFER_CONFIG 0x02CB
#define PICA_REG_VSH_CODETRANSFER_DATA   0x02CC
#define PICA_REG_VSH_OPDESCS_CONFIG      0x02D5
#define PICA_REG_VSH_OPDESCS_DATA        0x02D6

#define PICA_SH_REG_OFFSET(is_gsh)       ((is_gsh) ? -0x30 : 0)

#define PICA_REG_COUNT                   0x0300

/* ---------------------------------------------------------------------------
 * External (MMIO) registers, as byte offsets from PICA_MMIO_BASE.
 * ------------------------------------------------------------------------- */
#define PICA_MMIO_BASE                   0x10400000u

/* Memory fill ("PSC") engines 0 and 1, 0x10 bytes apart */
#define PICA_MMIO_PSC_START(n)           (0x0010 + 0x10 * (n)) /* physical address >> 3 */
#define PICA_MMIO_PSC_END(n)             (0x0014 + 0x10 * (n)) /* physical address >> 3, exclusive */
#define PICA_MMIO_PSC_VALUE(n)           (0x0018 + 0x10 * (n))
#define PICA_MMIO_PSC_CONTROL(n)         (0x001C + 0x10 * (n))
#define   PICA_PSC_START                 (1u << 0)
#define   PICA_PSC_FINISHED              (1u << 1)
#define   PICA_PSC_WIDTH(w)              (((w) & 3u) << 8)   /* 0 = 16-bit, 1 = 24-bit, 2 = 32-bit */

/* LCD framebuffer setup: top screen at 0x400, bottom screen at 0x500 */
#define PICA_MMIO_LCD_TOP                0x0400
#define PICA_MMIO_LCD_BOTTOM             0x0500
#define PICA_MMIO_LCD_LEFT_ADDR_A        0x68
#define PICA_MMIO_LCD_LEFT_ADDR_B        0x6C
#define PICA_MMIO_LCD_FORMAT             0x70
#define PICA_MMIO_LCD_SELECT             0x78
#define PICA_MMIO_LCD_STRIDE             0x90
#define PICA_MMIO_LCD_RIGHT_ADDR_A       0x94
#define PICA_MMIO_LCD_RIGHT_ADDR_B       0x98

/* Transfer engine ("PPF"): display transfer and texture copy */
#define PICA_MMIO_DMA_INPUT_ADDR         0x0C00 /* physical >> 3 */
#define PICA_MMIO_DMA_OUTPUT_ADDR        0x0C04 /* physical >> 3 */
#define PICA_MMIO_DMA_OUTPUT_DIM         0x0C08 /* height << 16 | width */
#define PICA_MMIO_DMA_INPUT_DIM          0x0C0C
#define PICA_MMIO_DMA_FLAGS              0x0C10
#define PICA_MMIO_DMA_CONTROL            0x0C18 /* bit 0: start/busy, bit 8: finished */
#define PICA_MMIO_DMA_TEXCOPY_SIZE       0x0C20
#define PICA_MMIO_DMA_TEXCOPY_IN_LINE    0x0C24 /* gap << 16 | width, in 16-byte units */
#define PICA_MMIO_DMA_TEXCOPY_OUT_LINE   0x0C28

/* Command list DMA (P3D) */
#define PICA_MMIO_P3D_CMDBUF_SIZE        0x18E0 /* size in bytes >> 3 */
#define PICA_MMIO_P3D_CMDBUF_ADDR        0x18E8 /* physical >> 3 */
#define PICA_MMIO_P3D_CMDBUF_RUN         0x18F0 /* write 1 to start; reads busy */

#endif /* PICA_REGS_H */
