# PICA probe data

Measurements of PICA200 behaviour, taken with `examples/probe`. Each
subdirectory is one run: the CSVs and `meta.txt` exactly as the probe wrote
them to `sdmc:/pica_probe/`.

| Directory | Source |
|---|---|
| `azahar-2126.1.2-opengl/` | Azahar 2126.1.2, OpenGL renderer (llvmpipe), New 3DS mode |
| `azahar-2126.1.2-software/` | Azahar 2126.1.2, software renderer, New 3DS mode |
| `hw-*/` | Real hardware runs, once collected (see below) |

Emulator runs are **not** ground truth. They are kept so a hardware run can
be compared against them: every difference is either an emulator bug or an
undocumented hardware rule.

## Collecting a hardware run

1. Build `examples/probe` (`make -C driver examples`) and copy `probe.3dsx`
   to the SD card's `/3ds/` folder.
2. Run it from the Homebrew Launcher. It takes a few seconds and prints
   `Finished (0 errors)` on the top screen.
3. Copy `sdmc:/pica_probe/` into a new directory here named
   `hw-<model>-<your handle>-<date>/` (e.g. `hw-n3dsxl-alice-2026-10-09`).
4. Compare:

   ```sh
   python3 driver/tools/probe_compare.py driver/probe-data/hw-... driver/probe-data/azahar-2126.1.2-opengl --report report.md
   ```

Only real hardware runs belong in `hw-*` directories. Results must come from
running this probe, not from any other software's internals.

## Experiments (probe v1)

Every sample is rendered twice; `pr..pa` is the primary (diffuse) lighting
output and `sr..sa` the secondary (specular) output.

| CSV | Question |
|---|---|
| `diffuse_ln` | Diffuse response vs N.L over 0-180 degrees: precision and rounding of `diffuse * max(N.L, 0)` |
| `diffuse_two_sided` | Effect of light config bit 1 (two-sided diffuse) on back-facing normals |
| `lut_value`, `lut_value_ends` | How a 12-bit LUT value becomes an 8-bit color (is 0xFFF exactly 1.0? round or truncate?) |
| `lut_index_abs`, `lut_index_abs_zoom` | Which LUT entry an unsigned input selects; precision of N.H |
| `lut_index_signed` | Indexing with signed inputs (two's complement byte?) |
| `lut_interp_pos/neg/half` | Whether and how the per-entry difference field interpolates, incl. behaviour exactly at entry boundaries |
| `lut_scale` | All 8 values of a LUT scale selector, including the undocumented 4 and 5 |
| `layer_config` | Which LUTs each of the 16 layer configurations applies (8-15 mostly undocumented) |
| `config0_bits`, `config1_bits` | Every bit of LIGHTING_CONFIG0/1 flipped one at a time from a reference scene |
| `light_config_bits` | Every bit of a light's CONFIG register flipped one at a time |
| `dist_atten` | Distance attenuation LUT addressing (scale/bias, distance precision) |
| `spot` | Spotlight LUT input vs spot direction angle |
| `quat_norm` | Whether the normal quaternion is renormalised (same rotation scaled 0.25x-2x) |

## Open questions

What the two emulator runs already show:

- **Rounding.** The OpenGL and software renderers differ by 1 in most
  outputs (OpenGL rounds, software truncates). Which one matches hardware?
- **LUT entry boundaries.** When an input lands exactly on an entry
  boundary (`lut_interp_pos` samples 8, 24, ...) OpenGL uses the next entry
  with delta 0 (output 0) while software uses the previous entry with delta
  ~1 (output 254).
- **Everything that only has one emulator answer:** the undocumented
  layer configs 8-15 and scale selectors 4/5, unknown CONFIG0/CONFIG1 bits,
  quaternion renormalisation, and the precision of N.H / distance inputs.

Add confirmed rules below, each with the run directory and CSV rows that
show it.

## Confirmed findings

_None yet — waiting for the first hardware run._
