# Corgi PICA200 driver

A small, self-contained C99 driver for the Nintendo 3DS GPU (DMP PICA200),
for 3DS homebrew. It programs the GPU directly through command lists:
render targets, clears, viewport/scissor, depth/stencil/alpha tests,
blending and logic ops, the six texture combiner stages, textures (all
formats, three units, filtering/wrapping/mipmaps), vertex buffers, indexed
and immediate-mode drawing, fixed attributes, vertex and geometry shaders
(picasso `.shbin` files), uniforms, fog and the display transfer / texture
copy engines.

The register-level knowledge comes from Corgi3DS's own PICA200 emulation
(`src/core/arm11/gpu.cpp`), cross-checked against libctru, citro3d and
Azahar (the Citra successor).

## Layout

| Path | What |
|---|---|
| `include/pica/pica.h` | Public API |
| `include/pica/regs.h` | Register map (internal command registers + MMIO) |
| `include/pica/types.h` | Enums (formats, blend factors, combiner sources, ...) |
| `include/pica/floats.h`, `matrix.h` | f24/f31 conversions, 4x4 matrix helpers incl. the rotated-screen projections |
| `include/pica/mmio.h` | Direct-register backend (bare metal / emulator) |
| `src/` | Implementation; `platform_ctru.c` and `platform_mmio.c` are the two backends |
| `examples/triangle` | Minimal app: perspective scene on the top screen |
| `examples/selftest` | Runs the test suite on the console/emulator, writes results to the SD card |
| `tests/suite.c` | The rendering test suite (23 tests) |
| `tests/host` | Runs the same suite on Corgi3DS's GPU emulation, on your PC |

## Backends

All driver operations are **synchronous**: when a call that submits work
returns (`pica_flush`, `pica_clear`, `pica_transfer`, texture uploads), the
GPU has finished it. State setters only append to the command buffer,
which is submitted automatically when it fills up or when the GPU must be
idle.

- **libctru** (`pica_platform_ctru()`): for normal `.3dsx` homebrew. Uses
  `gsp::Gpu` (GX commands + GSP events). Call `gfxInit*()` first. By default
  the whole linear heap is written back from the data cache before each
  command list (like citro3d), so you don't need explicit cache flushes;
  turn this off with `pica_ctru_auto_flush(false)`.
- **MMIO** (`pica_platform_mmio()`): pokes the external GPU registers
  (0x10400000) directly and includes a small allocator for memory arenas
  you hand it. It is what the host tests use. It can also target
  bare-metal ARM11 code, but it does not bring up the LCDs, and polling for
  completion via register reads has only been checked against emulation,
  not real hardware.

## Building

Needs devkitPro's `3ds-dev` (devkitARM, libctru, picasso).

```sh
cd driver
make            # lib/libpica.a and lib/libpicad.a
make examples   # examples/*/*.3dsx
make install    # optional: copy into $DEVKITPRO/portlibs/3ds
```

Link with `-lpica -lctru -lm`, or add `driver/src` to your project's
`SOURCES` and `driver/include` to `INCLUDES` like the examples do.

## Usage

```c
#include <3ds.h>
#include "pica/pica.h"
#include "vshader_shbin.h"   // from picasso, via the devkitPro Makefile template

gfxInitDefault();

pica_context ctx;
pica_init(&ctx, pica_platform_ctru(), 0);

pica_shbin shbin;
pica_shbin_parse(&shbin, vshader_shbin, vshader_shbin_size);
int u_proj = pica_shader_uniform_reg(&shbin.dvle[0], "projection");

pica_framebuffer fb;                       // top screen: 240 wide, 400 tall
pica_framebuffer_create(&ctx, &fb, 240, 400, PICA_COLOR_RGBA8, PICA_DEPTH24_STENCIL8);

// Vertex input: v0 = float3 position, v1 = float4 color, interleaved
pica_attr_info attrs;
pica_attr_init(&attrs);
pica_attr_add_loader(&attrs, 0, PICA_FLOAT, 3);
pica_attr_add_loader(&attrs, 1, PICA_FLOAT, 4);
pica_buf_info bufs;
pica_buf_init(&bufs);
pica_buf_add(&ctx, &bufs, vertices /* linear memory */, sizeof(vertex), 2, PICA_PERMUTATION_SEQ(2));

float proj[16];
pica_mtx_ortho_tilt(proj, 0, 400, 0, 240, 0.1f, 10.0f);

while (aptMainLoop()) {
    pica_bind_framebuffer(&ctx, &fb);
    pica_clear(&ctx, &fb, PICA_CLEAR_ALL, 0x203040FF, 0);
    pica_bind_shader(&ctx, &shbin.dvle[0], NULL, 0);
    pica_uniform_mtx4(&ctx, PICA_VERTEX_SHADER, u_proj, proj);
    pica_bind_attrs(&ctx, &attrs);
    pica_bind_buffers(&ctx, &bufs);
    pica_draw_arrays(&ctx, PICA_TRIANGLES, 0, 3);

    pica_transfer(&ctx, &fb, gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL),
                  240, 400, PICA_XFER_RGB8, 0);   // untile + convert to the LCD format
    gfxSwapBuffers();
    gspWaitForVBlank();
}
```

See `examples/triangle/source/main.c` for a complete program with textures
and depth testing, and `tests/suite.c` for every feature in use.

### Conventions

- Memory the GPU reads (vertices, indices, textures, command buffers) must
  come from `pica_alloc_linear()` or `pica_alloc_vram()`. Render targets live
  in VRAM.
- Colors passed to the API are `0xRRGGBBAA`.
- Matrices are row-major `float[16]`; shaders do `dp4 out.x, M[0], v`.
- Clip-space z runs from -w (near) to 0 (far). The default depth map
  (`scale -1, offset 0`) turns that into depth 1 (near) .. 0 (far), so use
  `PICA_GREATER` and clear depth to 0. `pica_mtx_ortho_tilt` /
  `pica_mtx_persp_tilt` produce this range and also rotate for the
  sideways-mounted LCDs.
- Render targets and textures are stored top row first: texture
  coordinate t = 1 is the first row of the image you upload, and window
  row y = 0 (bottom) is the last row of the render target in memory.
- Texel byte order for `pica_tex_upload` (linear, row 0 = top): RGBA8 is a
  `uint32_t` `0xRRGGBBAA`; RGB8 is bytes B,G,R; 16-bit formats are native
  `uint16_t` (RGB565 `R5G6B5`, RGBA5551 `R5G5B5A1`, RGBA4 `R4G4B4A4`, LA8
  `L<<8|A`); L8/A8 one byte; LA4 `L<<4|A`; L4/A4 two pixels per byte, even
  pixel in the low nibble. ETC1 data must already be in the PICA layout
  (use `tex3ds`) and is uploaded with `pica_tex_upload_tiled`.
- Every API function that returns `void` records its first error in the
  context; read it with `pica_get_error()`.

### Not covered (yet)

Fragment lighting, procedural textures, gas rendering and shadow textures
have no helpers; their registers are in `regs.h` and can be programmed with
`pica_write_reg*()`. Cube map textures are not wrapped either.

## Testing

The same 23-test suite (`tests/suite.c`) runs in two places:

**On your PC, against Corgi3DS's GPU emulation** (no devkitPro needed):

```sh
cmake -S driver/tests/host -B build-pica && cmake --build build-pica
./build-pica/corgi_pica_tests              # --filter NAME, --dump DIR (PPM images)
```

**On a 3DS or in Azahar/Citra:** build `examples/selftest` and run
`selftest.3dsx`. It prints results on the bottom screen and writes
`sdmc:/pica_selftest/results.txt` plus a 64x64 PPM per test.

The tests check exact pixel values for: clears, framebuffer orientation,
color interpolation, triangle lists/strips/fans, depth testing, blending,
alpha test, scissor, stencil, color write masks, face culling, u8/u16
indexed draws, immediate mode, fixed attributes, logic ops, textures
(orientation, repeat/clamp, all 11 non-compressed formats, VRAM upload via
texture copy), texture combiners (modulate/add/interpolate, combiner
buffer), geometry shaders, tilted projection matrices, and command buffer
auto-splitting (1000+ draws through a 32 KiB buffer).

Status: all 23 pass in Azahar 2126.1.2 (OpenGL renderer; Azahar's
PICA emulation tracks hardware closely) and on Corgi3DS. Running on physical hardware has not been done yet; please run
`selftest.3dsx` on a console and report the results file if anything fails.

`tests/shaders/shaders.h` embeds the assembled test shaders; regenerate it
with `tests/gen_shaders.py` after editing `tests/shaders/*.pica`.
