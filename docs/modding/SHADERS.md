# Shaders

> Add your own screen filters: scanlines, CRT looks, color grading, and more.

A shader is a small program that redraws every frame before it reaches your screen. G-Diffuser
reads shaders from the `shaders/` folder next to the game executable. It understands the
**RetroArch slang format**, so you can write your own or use existing RetroArch shaders.

No pack tools and no Reload are needed. Save a file into `shaders/`, pick it in the menu, and
edit it while the game runs.

## Use existing RetroArch shaders

The [libretro/slang-shaders](https://github.com/libretro/slang-shaders) collection has hundreds of
ready-made filters.

1. On that page, click **Code**, then **Download ZIP**.
2. Extract it into the `shaders/` folder next to the game executable, so you get
   `shaders/slang-shaders/crt/`, `shaders/slang-shaders/scanlines/`, and so on.
3. Press **F1** and open **Settings > Shaders**.
4. Pick a preset from **Shader preset**. Presets in folders are listed as `folder/preset`. If the
   new presets do not appear, click **Refresh list**.

Each shader in that collection has its own license, written at the top of its file. That is why
G-Diffuser does not ship them.

Not every RetroArch shader works yet. See [Limitations](#limitations).

## Write your first shader

A `.slang` file placed directly in `shaders/` appears in **Shader preset** as its own one-pass
preset. One `.slang` file works on both OpenGL and DirectX 11.

Save this as `shaders/my-scanlines.slang`:

```glsl
#pragma parameter scanline_brightness "Scanline brightness" 0.80 0.0 1.0 0.01

void main() {
    vec3 color = texture(Source, vTexCoord).rgb;
    float scan = scanline_brightness + (1.0 - scanline_brightness) *
                 cos(6.2831853 * (fract(vTexCoord.y * SourceSize.y) - 0.5));
    FragColor = vec4(color * scan, 1.0);
}
```

Then:

1. Open **Settings > Shaders** and pick **my-scanlines** in **Shader preset**.
2. Change the shader and save the file. The game rebuilds it on the next frame.

The `#pragma parameter` line creates a slider. Open **Settings > Shaders > Shader parameters** to
move it while the game runs.

Do not add a `#version` line. The game adds the right one.

## What a shader can use

| Name | What it is |
| --- | --- |
| `Source` | The input image. Read it with `texture(Source, vTexCoord)`. Pass 0 gets the game's frame at N64 resolution; later passes get the previous pass's output. |
| `Original` | Same as `Source` in this version. |
| `vTexCoord` | The position of the pixel being drawn, from 0 to 1. |
| `SourceSize` | Size of the input image, in pixels. `OriginalSize` is the same value. |
| `OutputSize` | Size of this pass's output, in pixels. `FinalViewportSize` is the same value. |
| `FrameCount` | A frame counter, for animation. |
| `MVP` | The model-view-projection matrix. |
| `FragColor` | Where you write the final color. |
| `OriginalHistory1`, `OriginalHistory2`, ... | The game's frame 1, 2, ... frames ago. |
| `PassOutput0`, `PassOutput1`, ... | This frame's output of an earlier pass. |
| `PassFeedback0`, `PassFeedback1`, ... | Last frame's output of a pass. The pass needs `feedback_passN = "true"`. |
| `User0`, `User1`, ... | Extra images listed in the preset's `textures` key. |

A parameter is declared as:

```text
#pragma parameter <name> "<label>" <default> <min> <max> <step>
```

## Chain passes with a preset

A **preset** (`.slangp` file) runs several shaders in a row. Each one is a **pass**, and each pass
reads the output of the one before it.

Save the shader above as `shaders/pipelines/my-scanlines.slang`, then save this as
`shaders/pipelines/my-effect.slangp`:

```ini
shaders = 1

shader0 = "my-scanlines"
filter_linear0 = "false"
wrap_mode0 = "clamp_to_edge"
scale_type0 = "source"
scale0 = "1.0"

scanline_brightness = "0.65"
```

Pass paths are relative to the preset's own folder, and the extension is optional. A preset can
live in any folder under `shaders/`. The `shaders/include/` folder is not scanned: it holds shared
files that shaders pull in with `#include`.

### Preset keys

`N` is the pass number, starting at 0.

| Key | Meaning | Default |
| --- | --- | --- |
| `shaders` | Number of passes, 1 to 64. | |
| `shaderN` | The pass's shader file. | |
| `filter_linearN` | `true` for smooth sampling, `false` for sharp pixels. | `true` |
| `wrap_modeN` | `clamp_to_edge`, `clamp_to_border`, `repeat`, or `mirrored_repeat`. | `clamp_to_edge` |
| `scale_typeN` | `source` (times the input size), `absolute` (fixed pixels), or `viewport` (times the screen size). | `source` |
| `scale_type_xN`, `scale_type_yN` | `scale_typeN` for one axis. | |
| `scaleN` | Scale for both axes. | `1.0` |
| `scale_xN`, `scale_yN` | Scale for one axis. | |
| `float_framebufferN` | Render the pass in 16-bit float, for HDR-style effects. | `false` |
| `srgb_framebufferN` | Render the pass in sRGB. | `false` |
| `frame_count_modN` | Run the pass only every N frames. | |
| `feedback_passN` | Keep the pass's output for the next frame, as `PassFeedbackN`. | `false` |
| `textures` | Extra images, exposed as `User0`, `User1`, ... in order. | |

The last pass always draws at the screen size. Any other key sets a parameter, like
`scanline_brightness` above.

A preset can start from another one: put `#reference "other.slangp"` at the top, then override any
key below it. References can chain up to four levels.

### Save your slider settings

Slider changes are remembered per preset. **Reset parameters to defaults** discards them. **Save
custom preset** writes a new `.slangp` into `shaders/pipelines/` that references the active preset
with your slider values built in, so you can share it.

## Backend-specific passes

A pass can also be a `.glsl` file (OpenGL) or `.hlsl` file (DirectX 11). These only work on their
own backend, so prefer `.slang`. A pass named `shader0 = "my-pass"` uses `my-pass.slang` if it
exists, then `my-pass.glsl` on OpenGL or `my-pass.hlsl` on DirectX 11.

To use one, reference it from a `.slangp` preset. Loose `.glsl` and `.hlsl` files are not listed
in **Shader preset** by themselves.

For these files you write only the pixel code. The game adds the rest.

| | OpenGL `.glsl` | DirectX 11 `.hlsl` |
| --- | --- | --- |
| Input position | `vUV` | `input.vUV` |
| Input image | `texture(uTex, vUV)` | `uTex.Sample(uTexSampler, input.vUV)` |
| Input size | `uniform vec2 uSrcSize;` | `float2 uSrcSize;` in `cbuffer PostCB : register(b0)` |
| Output size | `uniform vec2 uOutSize;` | `float2 uOutSize;` in the same cbuffer |
| Output | write `gdxFragColor` | `float4 PSMain(PSInput input) : SV_Target { ... }` |

DirectX 11 shaders compile against shader model 4.0.

## Limitations

- Compute, geometry, and tessellation stages are not supported.
- Pass aliases (`aliasN`) are read but not resolved. Refer to earlier passes by number, as
  `PassOutputN`.
- Editing a file in `shaders/include/` does not trigger a rebuild. Save the pass file again, or
  restart.

## If something goes wrong

**The screen shows the game without my filter.**
The shader failed to compile, so the game falls back to the plain frame. The log names the error.
See [Logs](../MODDING_GUIDE.md#logs).

**I want to see what my `.slang` file turned into.**
Set the environment variable `GDX_DUMP_SLANG_TRANSLATION` before starting the game. Each
translated shader is written to `logs/slang-<name>-<backend>.txt`.

**My preset is not in the list.**
Check that the file ends in `.slangp` and is not inside `shaders/include/`. Click **Refresh
list**.

## Quick reference

```text
shaders/my-shader.slang                 one-pass preset, works on both backends
shaders/<any-folder>/<name>.slangp      multi-pass preset
shaders/include/                        shared #include files, not listed

Pick:     F1 > Settings > Shaders > Shader preset
Sliders:  F1 > Settings > Shaders > Shader parameters
Share:    F1 > Settings > Shaders > Save custom preset
Reload:   automatic when you save a file
```
