# HDRAGC-Next

Adaptive shadow brightening / local tone mapping filter for AviSynth+.
Modern successor to the classic HDRAGC: Laplacian-pyramid decomposition,
spatially-adaptive shadow gain, temporal parameter smoothing with robust
scene-cut detection, hue-preserving chroma handling, and high-bit-depth
pipeline with blue-noise dithering.

## Features

- **Local tone mapping** — tone curve applied on the coarsest Laplacian level,
  modulated per-pixel by a shadow mask (no halo, no lifted blacks in bright scenes)
- **Asymmetric lift rule** — positive (shadow) lift is mask-modulated, negative
  (highlight compression) is always fully applied
- **Temporal stabilization** — IIR smoothing of curve parameters (not pixels),
  two-threshold scene-cut hysteresis (flash/fade safe), deterministic window mode
- **Auto points with guard band + stretch blending** — percentile stretch that
  does not crush shadows and does not re-gamma the image into haze
- **Chroma** — luma-ratio saturation compensation, hue-preserving soft clamp
- **Quality** — float32 internal, 8–16 bit I/O, ordered/blue-noise dithering,
  ~66 fps at 640x480 (2-core, AVX2)
- **Debug** — `show="mask"|"base"|"lift"` visualizations

## Parameters (abridged)

| Parameter | Default | Notes |
|---|---|---|
| `strength` | 0.8 | shadow lift strength |
| `protect_highlights` | 0.8 | highlight compression |
| `auto_points` | true | auto black/white point (p01/p99 + guard band) |
| `temporal` / `temporal_mode` | 0.85 / "iir" | parameter smoothing; "window" = deterministic |
| `scene_cut` / `scene_cut_low` / `scene_cooldown` | 0.35 / 0.20 / 3 | cut hysteresis |
| `local_mix` | 1.0 | 0 = global curve, 1 = fully local |
| `shadow_threshold` | 0.35 | shadow mask threshold; <=0 = auto from p50 |
| `mask_gamma` | 2.0 | mask rolloff; use ~1.2 for "recovery" look |
| `levels` | 4 | Laplacian pyramid levels |
| `chroma_mode` | "luma_ratio" | or "simple" |
| `output_bits` | 0 | 0 = same as input; 8/10/12/14/16 |
| `dither` | "blue" | "none" / "ordered" / "blue" |
| `show` | "none" | "mask" / "base" / "lift" |

Recovery preset for underexposed content: `strength=1.0, protect_highlights=0.9,
shadow_threshold=0.45, mask_gamma=1.2`.

## Build (Linux)

```bash
# 1. AviSynth+ core (no official Linux binary; v3.7.5 pinned in CI)
git clone --depth 1 --branch v3.7.5 https://github.com/AviSynth/AviSynthPlus.git avs
cmake -S avs -B avsbuild -GNinja -DCMAKE_BUILD_TYPE=Release
ninja -C avsbuild AvsCore

# 2. this plugin
cmake -S . -B pbuild -GNinja -DAVISYNTH_INCLUDE_DIR=$PWD/avs/avs_core/include
ninja -C pbuild          # -> hdragc_next.so (+ rawsrc.so test input)

# 3. test suite (16 groups: T1-T16 incl. real dark frames)
gcc -O1 -o host test/host.c -I avs/avs_core/include -L avsbuild/avs_core -lavisynth -lm
LD_LIBRARY_PATH=$PWD/avsbuild/avs_core ./host pbuild/hdragc_next.so
```

Windows (MSVC): see `.github/workflows/build.yml`.

## Usage

```avs
LoadPlugin("hdragc_next.dll")   # .so on Linux
ConvertToYUV420P16()
HDRAGCNext(strength=0.8, temporal=0.85, output_bits=8, dither="blue")
```

## Repository layout

```
src/hdragc_next.cpp   filter implementation (M1-M7)
src/rawsrc.cpp        raw YV12 reader (enables real-content testing)
test/host.c           C-API verification suite (16 test groups)
docs/DESIGN.md        full design document v2.2
.github/workflows/    CI: Linux full pipeline + Windows MSVC build
```

## Design notes

Five findings from real-world dark frames shaped the current algorithm —
percentile guard band, anti-ring detail rolloff, stretch blending (anti-haze),
default tau, and adaptive test assertions. See `docs/DESIGN.md` SS10.5, SS15.7, SS28.

## Acknowledgements

Plugin skeleton follows the AVS_linkage pattern from
[denmase/colorrestore](https://github.com/denmase/colorrestore).
