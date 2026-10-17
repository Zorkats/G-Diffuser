# G-Diffuser — Architecture and Porting Model

G-Diffuser is a native PC source port of F-Zero X (N64) with 64DD Expansion Kit support. It is an
independent repository that composes three submodules with a host glue layer, and the porting model
is genuinely non-obvious: **the game logic must simultaneously remain a byte-matching N64
decompilation and run correctly on a little-endian 64-bit host.** Nearly every recurring defect in
this project comes from that tension.

Audience: a new contributor, or the maintainer six months from now.

Read the last section first if you are here because something renders wrong.

---

## 1. Repository structure and ownership

| Path | Kind | Owns |
|---|---|---|
| `decomp/` | submodule → `Zorkats/fzerox`, branch `g-diffuser` | The matching F-Zero X decompilation: `src/`, `include/`, its own `Makefile`, splat yamls, linker scripts, expected ROM checksums, and its own CI. **The game logic.** |
| `port/` | in-tree | The PC host layer: entry point, CMake build, graphics bridge, audio, the fiber scheduler, VI, ROM/SRAM/disk buffers, generated bindings under `port/gen/`, resource factories, tests. |
| `libultraship/` | submodule → `Zorkats/libultraship` | Runtime and renderer: window/input/audio via SDL, the **Fast3D** display-list interpreter, libultra HLE, the O2R resource system, ImGui. |
| `torch/` | submodule → `Zorkats/Torch` | Build-time asset extraction from the user's ROM into a `.o2r` archive. Also supplies `torch/lib/libmio0`. |
| `fzerox-expansion-kit/` | submodule → `Zorkats/fzerox-expansion-kit` | 64DD Expansion Kit reference material. Not initialised in every checkout. |
| `tools/` | in-tree | Python generators and validators: `gen_asset_bindings.py`, `gen_link_stubs.py`, `gen_ek_translated_strings.py`, `gen_f3d_o2r.py`, and the harnesses. |
| `include/` | in-tree | Torch-generated asset headers (arrays plus `_WIDTH`/`_HEIGHT`/`_COMPRESSED_SIZE` defines). |
| `docs/` | in-tree, tracked | The documents that ship with a release: this file, `DIAGNOSTICS.md`, `MODDING_GUIDE.md` and the per-task guides in `modding/`. |
| `devdocs/` | in-tree, **gitignored** | Internal working notes: scopes, plans, audits, investigation write-ups. Local-only; not part of the repo or the release. |

All four submodules point at project-owned forks rather than upstream. If you have read an older note
claiming `libultraship` tracks Kenix3 upstream directly or `decomp` tracks `inspectredc/fzerox`
directly, that is out of date.

### Dependency direction is one-way, and the build enforces it

```
                 ┌──────────────┐
                 │   port/      │──────┐
                 └──────┬───────┘      │
                        │              │
                        ▼              ▼
                 ┌──────────────┐  ┌───────────────┐
                 │   decomp/    │  │ libultraship/ │
                 └──────────────┘  └───────────────┘
```

`port/CMakeLists.txt` compiles the decomp's C into its own object library, `gdiffuser_game`, and gives
that target an include path containing `decomp/`, `decomp/include`, `decomp/src` and the Torch asset
headers — **but not `port/`**. Decomp translation units therefore *cannot* `#include` a port header.
That is the enforcement mechanism, not a guideline. `libultraship` is linked to the executable rather
than to `gdiffuser_game`.

Two deliberate mechanisms cross the boundary anyway:

1. **PORT-gated local `extern` declarations**, resolved at link time. A decomp source that needs a host
   helper forward-declares it rather than including anything. The convention is stated in the code —
   `decomp/src/sys/dma.c` explains that it forward-declares because "this decomp TU's include path
   does not carry `port/`", and `decomp/src/overlays/course_edit/191080.c` repeats it. Bulk
   declarations live in `decomp/include/global.h`.
2. **Two port-generated headers vendored into `decomp/include/`** so the include stays inside the
   decomp tree: `port_segment_addrs.h` and `port_disk_segments.h`, both included under `#ifdef PORT`
   from `decomp/include/segment_symbols.h`. No generator in `tools/` writes `port_segment_addrs.h`, so
   treat it as hand-maintained.

### Two build workflows, easily confused

- **Building the port** happens natively on the host (CMake plus MSVC/clang on Windows, gcc/clang on
  Linux). It does **not** need the N64 IDO toolchain.
- **Verifying the matching decompilation** is a separate workflow inside `decomp/`, using its own
  Makefile and the MIPS toolchain. This is what proves the decomp still matches; it is not part of
  building the port. It needs a baserom that is not in this repository, so follow `decomp/README.md`
  for the setup and the exact invocation rather than guessing from the Makefile — and see section 2
  for what that build actually checks.

The renderer is Fast3D, libultraship's default. F-Zero X uses stock SDK F3DEX2 microcode including the
reject variants (`gspF3DEX2`, `gspF3DLX2_Rej`, `gspF3DFLX2_Rej`), which Fast3D already interprets. The
renderer is swappable; this choice is reversible.

---

## 2. The `PORT` gating convention

**Every host-side behavioural change to decomp code is wrapped in `#ifdef PORT`, because the non-PORT
build must stay byte-identical to retail.** There are currently **522** such preprocessor directives
across **67 files** in `decomp/src` (503 `#ifdef PORT`, 16 `#ifndef PORT`, 3 `defined(PORT)`), plus 12
more in `decomp/include`.

### Why: the decomp is checked against real ROM checksums

`decomp/` is a work-in-progress *matching* decompilation. Its Makefile defaults to `COMPARE ?= 1` and
`NON_MATCHING ?= 0`, and the `compressed` target verifies the built ROM against a checked-in md5:

- `decomp/fzerox.us.rev0.md5`
- `decomp/fzerox.jp.rev0.md5`
- `decomp/fzerox-expansion.jp.ek.md5`

Its CI builds both `us` and `jp` against a private baserom and produces an objdiff progress report.

Two things about that check are worth knowing. First, the md5 step is written as
`md5sum … && print OK || print FAILED`, so **a mismatch prints FAILED but does not fail the make
target** — you have to read the output. Second, `decomp/Makefile` never defines `PORT` at all, so
"non-PORT" simply *means* the decomp's own N64 build, i.e. the retail configuration.

### Where the macros live

The port build defines these on the `gdiffuser_game` target in `port/CMakeLists.txt`:

```cmake
PORT=1  GDIFFUSER_PORT=1  NON_MATCHING=1  NON_EQUIVALENT=1  AVOID_UB=1
F3DEX_GBI_2=1  _LANGUAGE_C=1  VERSION_US=1  ASSET_VERSION=us  ASSET_REVISION=rev0
```

Note that the port build is explicitly `NON_MATCHING` — matching is a property of the *other* build.

The shared macros are split across two headers, and it is worth knowing which is which:

- **`decomp/include/macros.h`** gates only two macros. `ROM_READ(addr)` becomes a call to
  `gdx_rom_read32()` under PORT, versus a dereference of an uncached KSEG1 cart address in retail.
  `GDX_EK_TEXT_CAP` becomes `48` under PORT and **expands to nothing** in retail, so a plain decomp
  build keeps the original array layout. Everything else in that header is configuration-independent.
- **`decomp/include/segment_symbols.h`** is where the segment and ROM addressing gating actually
  lives: under `#ifdef PORT` it includes `port_segment_addrs.h` and redefines
  `SEGMENT_ROM_START/END/SIZE` to `PORT_##segment##_ROM_START` constants instead of linker symbols.

### The consequence: PORT-clean does not mean retail-clean

**A change that compiles under PORT can still break the retail configuration, and the port's own build
will never tell you.** The superproject has no CI at all; the matching build is verified only by
`decomp/`'s CI, against `decomp/`'s committed submodule pointer. So a decomp edit made for the port
can pass every check you run locally and only surface later.

The sharpest form of this hazard is a **PORT-only construct referenced from ungated code**: the symbol
does not exist in the retail configuration, so the file does not compile at all.
`decomp/src/overlays/course_edit/191080.c` is the clearest worked example of how the codebase resolves
it. That file opens a large `#ifdef PORT` block at `:10`; inside it, `:32` defines the macro the
Expansion Kit editor uses to choose a label — the translated one off the loaded disk if there is one,
otherwise the retail Japanese string:

```c
/* :32, inside #ifdef PORT */
#define GDX_EK_LABEL(bound, jp) ((bound)[0] != 0 ? (u8*) (bound) : (u8*) (jp))
```

Its five call sites (`:1133`, `:1609`, `:1654`, `:1694`, `:1807`) are deliberately **not** gated, and
each one names a `gdx_ek_label_*` symbol declared only inside the PORT block (`:24-31`). The `#else` at
`:114` therefore has to supply a retail definition, and it does so with a macro that **discards `bound`
unexpanded**:

```c
/* :115, inside the #else */
#define GDX_EK_LABEL(bound, jp) ((u8*) (jp))
```

Because the parameter is never expanded, the port-only symbol written at the call site never appears in
the retail translation unit at all — it is not merely unused, it is never referenced. That is the whole
trick, and it is what lets those `extern` declarations live inside the PORT block without breaking
retail.

Two conventions follow, and both are load-bearing:

- If a PORT-only construct is referenced from ungated code, it **must** have a paired non-PORT
  definition that reproduces retail behaviour exactly. The codebase does this consistently — `GDX_CK`
  (`decomp/include/global.h:18` retail, `:40` PORT), `GDX_CKI`, `GDX_SEGMENTED_TO_HOST`, `GDX_IS_MIO0`
  and `GDX_AVOID_UB_RETURN` all have empty or pass-through retail definitions.
- Whatever the PORT construct yields in its inert state must equal what retail sees unconditionally.
  `GDX_EK_LABEL` falls back to `jp` when the bound label is empty, and retail's definition *is* `jp` —
  so a port build with no translated disk loaded draws exactly what retail draws.

> An audit of `decomp/src` for `gdx_*`, `CVarGet*` and `PORT_*` identifiers appearing outside any
> PORT-true preprocessor region found no genuinely unguarded call sites at the time of writing — the
> only hits were the intentional discarded macro arguments above and two unrelated `PORT_DISCONNECTED`
> constants. The hazard is real and documented; it is not currently outstanding.

---

## 3. The asset pipeline, end to end

```
user's ROM
   │
   ├─► decomp/assets/yaml/us/rev0/*.yaml          asset recipes (offset, type, symbol)
   │        │
   │        ├─► tools/gen_asset_bindings.py  ──►  port/gen/AssetBindings.c
   │        │        (symbol / segment / offset / fixup tables)
   │        │
   │        └─► gdx-extract (Torch)  ──────────►  fzerox.o2r
   │                                                  │
   └──────────────────────────────────────────────────┴─► runtime
```

Torch, built and installed as the standalone `gdx-extract` executable, turns the user's ROM into
`fzerox.o2r`. A separate `gdiffuser.o2r` carries only the MIT-licensed Fast3D shaders and is generated
by `tools/gen_f3d_o2r.py`. First boot spawns `gdx-extract` as a child process.

### What `AssetBindings.c` actually contains

The generator's job is subtle: it **defines** every decomp asset symbol as an array of its declared C
type. That matters because the game's static tables and display lists reference assets by address, and
a runtime pointer cannot be a static initializer. So the symbols must exist at link time with
compile-time-constant addresses.

- **Placeholder symbols.** Most generated arrays are `[1]` — one element. They exist to satisfy the
  linker and to give the address a value; the real bytes arrive from the `.o2r` at runtime. A
  one-element placeholder is why masking its address produces garbage (see defect class 2) and why
  interior pointer arithmetic against it aliases neighbouring symbols.
- **A binding row** (`sAssetSegmentMap`) maps a symbol's address to where its data really lives:

  ```c
  typedef struct { void* sym; unsigned char segment; unsigned int rom_base;
                   unsigned char compressed; unsigned int offset;
                   unsigned int image_size; unsigned int sym_size;
                   const char* o2r_key; } GdxAssetSegmentEntry;
  ```

- **A fixup row** (`sAssetFixups`) records a byte range inside a segment image that must be
  endian-corrected after loading, because the ROM bytes are big-endian and the host is not:

  ```c
  typedef struct { unsigned char segment; unsigned int rom_base; unsigned int offset;
                   unsigned int size; unsigned char kind; } GdxAssetFixupEntry;
  ```

  The consumer is `gdx_fixup_asset_segment_image()` in the same file. Its `kind` values are:

  | `kind` | Meaning |
  |---|---|
  | `1` | u32 byteswap across the range, in 4-byte steps. Display-list command words. |
  | `2` | u16 byteswap across the range, in 2-byte steps. Viewport data. |
  | `3` | **Vertex layout.** Steps 16 bytes at a time and byteswaps only the first 12 bytes as u16 pairs, leaving the last 4 bytes (the RGBA/normal bytes) untouched. |

  Kind 3 is not a plain byteswap, and that is the whole point of it — a `Vtx` is a mix of 16-bit fields
  and raw bytes, so a uniform swap would corrupt the colour.

  `gdx_register_asset_segment_command_ranges()` walks the same table but only for `kind == 1`, handing
  those ranges to the bridge as known N64 command ranges.

### CRITICAL MAINTENANCE HAZARD: `AssetBindings.c` is generator output *plus* hand-maintained content

**Never regenerate `port/gen/AssetBindings.c` in place.** `tools/gen_asset_bindings.py` refuses to
write to that path precisely because a blind regenerate silently deletes hand-maintained rows and
truncates hand-corrected array sizes back to one-element stubs — which still compiles, and corrupts
every asset it touches.

The refusal is by realpath, so an explicit `--out` pointing at the tracked file is caught too. The
tool names four classes of hand-edit it cannot reproduce: segment-image and symbol size corrections
measured at runtime; fixup range splits trimmed to true command boundaries; interior-indexed table
symbols with `extern`/NULL-o2r-key overrides; and manual edits to the loader helpers.

This is not theoretical. Measured on the current tree:

| Segment 8, `rom_base 0x0016C8A0` | Tracked file | Freshly generated |
|---|---|---|
| kind 1 (u32) rows | 50 | 50 |
| kind 2 (u16) rows | 13 | 13 |
| **kind 3 (vertex) rows** | **13** | **0** |
| **Total** | **76** | **63** |

A blind regenerate would drop exactly those 13 hand-added vertex fixup rows.

**The safe workflow** is to generate to a scratch path and diff:

```sh
python tools/gen_asset_bindings.py --profile us/rev0 --out /tmp/AssetBindings.fresh.c
diff /tmp/AssetBindings.fresh.c port/gen/AssetBindings.c
```

`--lint-only` runs the duplicate-offset lint and writes nothing, which is safe at any time.
`--force-overwrite` exists but obliges you to re-apply every hand-edit afterwards.

`port/gen/LinkStubs.c` is generated the same way by `tools/gen_link_stubs.py` and carries the same
caution: it holds hand-promoted real definitions (for example `osTvType = 1`) and a curated exclusion
list of symbols implemented for real elsewhere.

---

## 4. Segments and live carves

The N64 game addresses assets through segmented addresses (`0x0Sxxxxxx`), with a segment table mapping
segment number to a base address. The port keeps that model but backs each segment with host memory,
and several segments are **mode-owned**: the same segment number holds different content depending on
the game mode.

| Segment | Variants |
|---|---|
| 4 | `hud_gfx` / `create_machine_textures` |
| 5 | `podium_gfx` (GP-ending only, MIO0-compressed in ROM) |
| 7 | `machine_global_gfx` / `expansion_kit_textures_beta` |
| 9 | `machine_models` (cartridge, decoded) / `course_edit_textures` (disk-resident) |

Segments 4 and 7 are two **fixed** host buffers reused across every mode transition; only their content
rotates. That reuse was a deliberate performance fix: `Dma_LoadAssets` yields cooperatively every 32 KB,
and each yield round-trips through a full vsync-locked host frame, so reloading a few hundred KB cost a
measured ~131 ms mode transition even when the bytes were identical to what was already resident.

### The sticky-residency hazard

`gdx_load_seg4_if_needed()` and `gdx_load_seg7_if_needed()` in `port/decomp_port.c` compare the
requested variant against a `sGdxSegNResident` tracker and **skip the whole reload when it matches**:

```c
static void gdx_load_seg4_if_needed(GdxSeg4Content want, unsigned char* romStart, size_t size,
                                     const char* label) {
    if (sGdxSeg4Resident != want) {
        Dma_LoadAssets(...);
        gdx_fixup_asset_segment_image(0x04u, ..., (unsigned int) size);
        sGdxSeg4Resident = want;
        gdx_ck(label);
    } else {
        gdx_ck("[transition] seg4 reload skipped (already resident)");
    }
    Segment_SetAddress(4, gSegment1B8550VramStart);
}
```

The segment-5 podium activation, `gdx_activate_podium_segment5()`, does the same thing with
`sGdxSeg5Resident`, and there the skip is *required*: the fixups rewrite embedded `0x05xxxxxx` pointers
**in place**, so applying them twice would corrupt the command words. The flag is only cleared when the
buffer is (re)allocated.

**The hazard: the skipped path skips the byte-order fixups too, so anything written into a carve
survives into a later mode.** If some code path mutates a carve — a texture upload staged through it, a
diagnostic that scribbles, a decoded blob written over it — the next transition that requests the same
variant will not overwrite it and will not re-fix it. The corruption then appears in a mode that has no
obvious connection to the code that caused it. When a visual defect depends on *which mode you visited
first*, suspect this.

A related mechanism worth knowing: segment reloads are bracketed by `gdx_segment_epoch_begin()` /
`gdx_segment_epoch_end()`, a seqlock whose counter is odd during a reload. The graphics thread reads
`gSegments[]` without a lock, so it uses that counter to detect the window and skip an affected texture
for one frame rather than consuming torn state.

---

## 5. The recurring porting-defect classes

Each of these has been confirmed in this codebase. The point is the *shape*, so you recognise the next
instance.

### 5.1 Big-endian out-of-bounds reads

**Shape:** original code deliberately reads past the end of an array into adjacent `.data`, and the
value it lands on is only correct under big-endian byte order.

**Confirmed instance — the Expansion Kit name-entry keyboard**,
`decomp/src/overlays/expansion_kit/A6340.c`. The on-screen keyboard indexes a string literal as
`row * 10 + col`, with no bounds check. Retail's literal holds 40 characters plus a NUL — enough for
rows 0 through 3 only. **Row 4 is not in the literal at all.** Retail reached it by running off the end
into the adjacent `s32` (`= 0x202D`), whose big-endian bytes place `' '` at index 46 and `'-'` at
index 47 — exactly the row-4 cells the code expects. Verified at the byte level against the disk image.

The cursor can reach index 49, not just 48, via the START-press and auto-move-at-8-characters paths, and
the string terminator those read comes from zero `.data` padding rather than from the adjacent word.

The fix declares the array at its true used extent and spells row 4 out explicitly, which both
reproduces retail's effective bytes and removes the out-of-bounds access entirely rather than merely
correcting its result.

> The specific little-endian displacement figures quoted in that file's comment ("shifting the row two
> cells", "yields 0x04") are **not derivable from source** — host compilers guarantee nothing about
> inter-object layout or padding. Treat the mechanism as verified and those figures as empirical
> observations from one build.

**How to spot the next one:** an array index that can exceed the declared extent, where the code works
on hardware. Ask what symbol follows in `.data` and what its bytes spell when byte-swapped.

### 5.2 `SEGMENT_OFFSET()` applied to a host pointer

**Shape:** `SEGMENT_OFFSET(a)` is `((unsigned int)(a) & 0x00ffffff)` (`decomp/include/PR/mbi.h`). On
N64 the operand is a segmented or ROM address, so masking off the top byte correctly yields an offset
within a segment. Under PORT the same symbol is a **host array**, so masking its address yields
garbage.

**Confirmed instance — the Super-machine portrait DMA**, `Hud_ReplaceCharacterPortrait` in
`decomp/src/overlays/ovl_i3/hud.c`. Both the DMA destination and the source were masked:

```c
vramOffset = (Segment_GetAddress(4) + SEGMENT_OFFSET(aPortraitCaptainFalconTex)) + textureOffset;
romOffset  = (romOffset + SEGMENT_OFFSET(D_276FF0)) + textureOffset;
```

Under PORT, `aPortraitCaptainFalconTex` and `D_276FF0` are host arrays in `port/gen/AssetBindings.c`
(`D_276FF0` being a one-element placeholder). The fix substitutes compile-time segment-relative
literals, each independently cross-checked against the generated binding row and the linker script.

There are only **six** `SEGMENT_OFFSET` call sites in all of `decomp/src`, across four files. Two are
the defect above (`hud.c`); the other four take genuine segmented addresses and are correct as written:
`decomp/src/sys/segment.c`, `decomp/src/game/object.c` (two sites), and
`decomp/src/overlays/ead_demo/ead_demo_engine.c`. If you audit this macro, that is the whole inventory.

**Correction to earlier project notes:** `decomp/src/overlays/ovl_i2/save.c` has often been cited as a
second instance. It contains **no `SEGMENT_OFFSET` at all.** There is a real and closely related defect
there, but the mechanism differs — see 5.2b.

### 5.2b Host pointers stored in a ROM-offset table

**Shape:** a table holds *addresses of asset symbols* and the code adds them to a ROM base, expecting
ROM offsets. Under PORT those symbols are host stubs, so a host pointer gets added to a ROM base and
then truncated by the host DMA shim.

**Confirmed instance — the staff-ghost replay loader**, `Save_RomCopyGhostData` in
`decomp/src/overlays/ovl_i2/save.c`. `D_i2_80106DF0` is a table of addresses of the staff-ghost asset
symbols, which under PORT are zero-filled stubs in `port/gen/LinkStubs.c`. The retail path does
`Dma_RomCopyAsync(romOffset + offsets[1], ...)`. The truncation happens downstream in
`Dma_PortRomOffset` (`decomp/src/sys/dma.c`), which applies `& 0x1FFFFFFF` after a 32-bit narrowing —
not the `& 0x00FFFFFF` of `SEGMENT_OFFSET`.

The fix is structurally different from 5.2's: rather than substituting a literal offset, it replaces the
DMA entirely with an `.o2r` archive read plus a payload parse, keyed off an archive-key table.

**How to spot both:** any arithmetic that combines a decomp *symbol address* with a ROM or segment
base. Under PORT that address is a host allocation and carries no positional information whatsoever.

### 5.3 Duplicated code where only one copy got the port fix

**Shape:** two near-identical routines; the fix lands on the copy you found first, and the *other* one
is what actually runs.

**Confirmed instance — the Course Edit help-tooltip icon position.** Two near-identical functions draw
it:

| | Copy A | Copy B |
|---|---|---|
| Location | `decomp/src/overlays/course_edit/19FA50.c`, `func_xk2_800EE67C` | `decomp/src/overlays/course_edit/1A4210.c`, `func_xk2_800F3DAC` |
| Gate | `if (D_8076C960 != 0)` | `if (D_8076C958 == 0) return;` |
| Runs? | **No — dead code** | **Yes** |

Both are called unconditionally from the same draw function in
`decomp/src/overlays/course_edit/191080.c`. But copy A's icon pass is gated on `D_8076C960`, and every
single reference to that variable in the entire repository either initialises it to 0 or assigns 0 — it
is never nonzero. Copy B's gate is initialised to 90 and decremented, so copy B draws.

The retail constant is a hardcoded `left = 128`, correct only while every glyph was a fixed 16 px — which
two-byte Japanese text guaranteed. Proportional ASCII moves the hole the tooltip leaves for the icon, so
the position has to be derived instead. The helper's own comment names only `19FA50.c` as its caller,
which is the trace of the fix having been written against the copy that does not run.

The eventual fix publishes the computed left edge through a shared global from inside the tooltip
renderer, and both copies consume it — so the two implementations can no longer drift.

**Note:** these are two near-identical *functions*, not two near-identical files; the surrounding files
are otherwise unrelated. Both compile only in the Expansion Kit configuration.

**How to spot the next one:** before fixing a draw path, prove the function you are editing executes.
Check the gate variable's write sites across the whole tree, not just its declaration.

### 5.4 Under-declared asset extents

**Shape:** a trailing display-list symbol in a segment yaml, with no declared segment size, gets sized
to **zero** — which both under-declares the segment image and **suppresses its endian-fixup row**. The
data then stays big-endian and the display list is garbage.

**Mechanism, verified in `tools/gen_asset_bindings.py`.** A `type: GFX` symbol's size is inferred by
differencing against the *next* symbol's offset. The `next_offsets` map deliberately has no entry for
the last item, so for a trailing GFX entry the inference yields `None`, `declared` computes to 0, and
the fixup row is vetoed by a `declared > 0` guard:

```python
kind = fixup_kind(val)
if kind != FIXUP_NONE and declared > 0:
    asset_fixup_entries.append((int(segment_id), int(rom_base), offset, declared, kind))
```

**The convention that prevents it** is a trailing `# size = 0x...` comment in the yaml. The generator
scans the **raw text** for it with a regex before `yaml.safe_load` runs, because comments are invisible
to the YAML parser:

```python
size_matches = re.findall(r"(?m)^\s*#\s*size\s*=\s*(0x[0-9A-Fa-f]+|\d+)", yaml_text)
segment_declared_size = int(size_matches[-1], 0) if size_matches else 0
```

It takes the **last** match in the file, not the largest.

**Worked example.** `decomp/assets/yaml/us/rev0/course_track_gfx.yaml` ends with
`aSetupCourseEffectTextureDL` at offset `0x223F8`, followed by `# size = 0x22500`. With the comment the
symbol sizes to `0x108` and emits its fixup row. Without it, the symbol sizes to 0, emits **no** fixup
row, and the segment image is declared `0x108` bytes short.

**One nuance the convention's reputation gets wrong:** the `# size =` comment in `setup_gfx.yaml` is
*redundant*. Its trailing symbol is `type: VTX`, whose size comes from its own `count` field, so
inference already produces the right answer. **Only a trailing `type: GFX` entry needs the comment**,
because GFX is the only type sized by next-offset differencing. Just four of the roughly thirty US
yamls carry the comment today, so any other segment yaml whose last-by-offset entry is `type: GFX` has
the same latent under-declaration.

### 5.5 Host caches that assume immutability

**Shape:** the port adds a cache keyed on a host address and treats "this address is in a registered
range" as "these bytes never change". Registered ranges include per-frame RAM scratch, so the cache
serves a stale first copy indefinitely.

**Confirmed instance — the persistent raw-texture-copy path** in `port/n64_gfx_bridge.cpp`.
`MakePersistentRawTextureCopy` keys a heap copy on the resolved host source address. Its change check
is a three-way branch: native-RGBA16 ranges get a byte-order-aware compare, RDRAM and framebuffer
ranges get a DMA-generation fingerprint, and **everything else gets a plain `memcmp` — which was
skipped whenever the source lay inside any registered host range**:

```cpp
// ROM-backed textures are stable after the segment is loaded; skip memcmp.
const bool stableSource = RegisteredHostRemaining(source) > 0;
if (!stableSource) {
    changed = (std::memcmp(copy.bytes.get(), reinterpret_cast<const void*>(source), copyBytes) != 0);
}
```

The reasoning holds for ROM-backed data. But the `GfxPool` array is also a registered host range, and
it is **per-frame RAM scratch**, rewritten every frame. A TLUT staged through the pool therefore had its
first copy cached and returned unchanged forever.

The fix adds `IsGfxPoolHostRange()` as an explicit carve-out, keeping pool-sourced addresses on the
compare path. Note two properties of it: `IsGfxPoolHostRange` fails *open* — if the pool size query
returns 0 it returns `false`, i.e. back to the immutable path — and it derives the pool size from the one
translation unit that has the real `GfxPool` type rather than duplicating it.

**Two precisions on how this is often retold.** It was not *any* registered range that was treated as
immutable — the RDRAM, framebuffer and native-RGBA16 branches always had their own change detection; the
pools fell into the `else` because they are host BSS rather than inside the emulated RDRAM buffer. And
the staleness had one escape hatch: a later request for *more* bytes at the same address forces a
refresh. For the fixed-size 32-byte CI4 TLUTs at issue that never fires, so the practical effect was as
described.

**How to spot the next one:** any cache whose invalidation condition is "is this address registered?"
rather than "have these bytes changed?". Registration answers *where* memory came from, never *whether
it is stable*.

### 5.6 Stale comments asserting an invariant that has since been broken

**Shape:** a comment states an invariant that justified a shortcut. The invariant stopped being true
when a neighbouring piece was ported, and the comment now actively misleads.

**Confirmed instance — the staff-ghost stubs** in `port/gen/LinkStubs.c`. The comment on
`GDX_STAFF_GHOST_STUB` says zero-filled stubs are safe because `Save_LoadStaffGhostRecord` is
PORT-gated to return -1, "so the data is never consumed." That premise is false: the record loader
*has* been ported, reads the `.o2r` archive, and returns 0 — which is precisely what lets execution
reach the code that consumes the stub addresses (see 5.2b). Two sibling comments in `port/decomp_port.c`
and `port/CMakeLists.txt` repeat the same stale claim.

There is a wrinkle that makes this a good teaching case. The comment's *conclusion* — that the stub
bytes are never read — is arguably true again, for an entirely different reason: what was consumed were
the stubs' *addresses misused as ROM offsets*, never their zero-filled bytes. So a reader who spot-checks
the conclusion finds it defensible and moves on, while the stated *reason* is flatly wrong and points
the next investigation at the wrong file.

**How to spot the next one:** when you port a function that another module's shortcut depends on, grep
for its name across the whole tree — including comments — before you finish. A comment that names a
function is a comment that has a dependency on it.

---

## 6. Where to look when something renders wrong

Symptom to subsystem to the gate that observes it. **Every gate below is off by default**; see
`docs/DIAGNOSTICS.md` for how to enable them, what the log families mean, and which switches change
behaviour rather than merely observing it.

| Symptom | Look at | Gate |
|---|---|---|
| Untextured / wrong texture on a specific object | Asset resolution, binding rows, fixup rows | `GDX_DIAG_SETTIMG` |
| Garbage geometry, huge stray triangles, spikes | Display-list conversion, vertex fixups (kind 3) | `GDX_DIAG_VERBOSE` → `[bigtri]`, `[geodiag]` |
| Whole screen or one layer missing | Viewport / scissor, present path | `GDX_DIAG_VERBOSE` → `[gpustate]`; `GDX_PRESENT_PATH_TRACE` |
| Course materials wrong, striped text, bad palette | Segment-8 setup display lists, TLUT loads | `GDX_DIAG_SETUPDL` |
| Breaks only after visiting another mode first | **Sticky segment residency (section 4)** | `GDX_LOG` → `[segment]`, `[transition]` (always on) |
| Screen transition wrong or stuck | Transition capture, framebuffer readback | `GDX_DIAG_TRECT`, `GDX_DIAG_CAPTURE_PROBE`, `GDX_DIAG_HOLD` |
| HUD marker or rival icon missing | Draw-condition gates in the decomp | `GDX_DIAG_RIVAL` |
| Create Machine preview flat / untextured | `gCustomMachine` record, env-map LookAt basis | `GDX_DIAG_CUSTOMMACHINE`, `GDX_DIAG_LOOKAT` |
| Course Edit overlay text or icons wrong | Segment-9 ownership, **duplicated overlays (5.3)** | `GDX_DIAG_NODEINFO` |
| Stutter, hitches at mode change | Scheduler, DMA yield, segment reloads | `GDX_PERF`, plus `[transition]` timings |
| Texture correct once then frozen | **Host cache immutability (5.5)** | `GDX_DIAG_SETTIMG` |
| Sim divergence with interpolation on | Matrix interpolation determinism | `GDX_INTERP_DETERMINISM` |

Two habits that shorten most investigations here:

1. **Confirm the code you are editing actually runs** before you fix it (5.3).
2. **Reproduce from a cold start**, not after wandering through menus — sticky residency (section 4)
   makes mode order significant, so an intermittent bug is often a deterministic bug with a
   prerequisite.

---

## 7. Document status

Current as of the working tree at the time of writing, and verified against code rather than against
earlier notes. Where a claim could not be established from source it is marked unverified inline
(sections 5.1 and 2).

This file supersedes the earlier 52-line M0-era architecture stub, whose renderer decision,
build-location split and build flow are carried forward above. Its "submodule tracking policy" section
is intentionally **not** carried forward: all four submodules now point at project-owned forks, so that
section described a plan that has already been superseded.

`docs/` is tracked, so this document ships with the repository and the release. The internal working
notes it was distilled from live in `devdocs/`, which is gitignored and local-only.
