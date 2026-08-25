# G-Diffuser — project status

**As of 2026-07-31. Pre-1.0.**

Every claim here was verified against source when written, and carries a `file:line` where one
applies. This document replaces `devdocs/MASTER_IMPLEMENTATION_STATUS.md`, which had drifted far
enough from the code to be actively misleading — it described the Workshop as a placeholder and
texture packs as planned, months after both shipped, and contained no occurrence of `o2r`, `Torch`,
`mods/`, or `gdx_workshop`.

**Maintenance rule:** if you find a claim here that the code contradicts, fix this file in the same
change. A status doc that is wrong is worse than no status doc, because work gets planned against it.

---

## What G-Diffuser is

A native PC source port of F-Zero X, built from the `inspectredc/fzerox` matching decompilation and
running on a fork of `Kenix3/libultraship` (Fast3D). 64DD Expansion Kit supported. Windows and Linux,
including handheld (verified on a ROG Ally running CachyOS).

Users supply their own ROM and 64DD disk image. No copyrighted assets ship with the port, and none
ever will.

### Two build modes, one tree

`decomp/` compiles in two configurations. Every behavioural change there is `#ifdef PORT`-gated, and
the non-PORT build must stay byte-identical to the matching decompilation. This is a hard invariant,
not a preference — it is what keeps the port honest against the original.

---

## Shipped and verified

### Shell and presentation

| Capability | Evidence |
|---|---|
| First-boot wizard, ROM/disk validation | `port/gdx_firstboot.cpp`, `gdx_firstboot_gui.cpp` |
| Runtime O2R extraction from the user's ROM | `port/gdx_extract_launch.cpp`, `torch/src/gdx/` |
| ImGui settings shell — 5 headers, searchable | `port/gdx_menu.cpp:828` |
| DirectX 11 / OpenGL rendering parity | 16 changes in `libultraship/src/fast/backends/gfx_opengl.cpp`; owner-confirmed on Linux |
| Console log sink | `port/gdx_console_log.cpp` — libultraship had no spdlog sink at all |
| Input viewer overlay | `port/gdx_input_viewer.cpp` |
| Frame-time telemetry, FPS overlay | `port/gdx_perf.cpp`, `gdx_fps_overlay.cpp` |
| 28 developer gates with CVar + env parity | `port/gdx_dev_gates.c` |

### Content and data

| Capability | Evidence |
|---|---|
| Durable, journaled 64DD save sidecar (CRC-64, copy-on-write) | `port/disk_savefile.h`, `port/n64_leo.c:181`. The original `.ndd` is never modified. |
| EK translated-string binding | `port/gdx_ek_strings.c`, `port/gen/EkAssetBindings.c` |
| Texture-pack loading from `mods/`, hot reload | `port/gdx_workshop.cpp:557-598` |
| Asset dump — 11 classes, native, no Python required | `torch/src/gdx/dump_all.cpp:498-501` |
| `ARRAY`/`GARR` resource dumping | `torch/src/gdx/dump_arrays.cpp` — 62 entries, 38 PNGs |
| Ghost library — `.gdg` v1, import/export, 128 entries, browser | `port/gdx_ghost_io.h`, `port/gdx_ghost_window.cpp` |
| Workshop channels: texture, sequence, sample, soundfont, model packs + sandboxed Lua scripts (build- and tool-verified; owner runtime verification pending for the 1.1.0 wave) | `port/gdx_workshop.cpp`, `port/gdx_audio_*_packs.*`, `port/gdx_model_packs.*`, `port/gdx_lua.*`; MODDING_GUIDE §12–§16 |

### Platform

Linux builds clean and runs on handheld hardware. The bring-up closed four separate defects: a silent
0-byte `stb_image.h` download, MSVC-only functions in `port/gdx_input_script.c`, a stale CMake glob,
and missing post-link sync paths.

---

## Shipped but not working

These are compiled in and reachable from the menu. They do not do what their name says.

### Frame interpolation — excludes the camera

`port/n64_gfx_bridge.cpp:4883-4884` rejects every `G_MTX_PROJECTION` matrix load from the
interpolation reroute. But `race.c:250` loads camera.c's **combined projection × view** matrix with
exactly that flag, and `course.c` emits no `gSPMatrix` at all — the track has no matrix of its own.

Consequence: the camera and the entire world are frozen at 60 Hz while roughly ten machine bodies
tween at display rate. The feature costs full sub-frame render work and delivers a fraction of the
benefit.

`MATRIX_INTERPOLATION_PLAN.md:296` set the P1 exit criterion as *"Camera + racers tween; world moves
correctly under camera."* The plan named the defect before the code was written.

**Escape hatch:** `GDX_INTERP_CAMERA` / `gDevTools.Behavior.InterpCamera` drops the exclusion. It is a
bucket-B gate, so it is compiled out of Release unless the build sets `-DGDX_FORCE_DEV_TOOLS=ON`.

**Update 2026-08-01 — tested, and it works.** With the gate on, the owner reports the image is
materially more fluid, and telemetry confirms the camera tweens. **The remaining problem is that it
is still a dev gate**: a stock Release build compiles it out, so shipping users cannot get the
working behaviour. It has to stop being a bucket-B gate before 1.0.

### Interpolation delivers only ~1.0–1.35× outside races

Measured 2026-08-01 from `run2.log`, correlating interpolated-matrix count against present rate:

| Interpolated matrices | Samples | Mean presents/s | Mean dropped |
|---|---|---|---|
| 0 | 6 | 121.8 | 0.67 |
| 1–19 | 41 | 127.3 | 0.32 |
| **≥ 20** | **12** | **67.9** | **0.92** |

A race lerps 4–15 matrices and holds a steady **143.9** presents/s. Machine select lerps 31–39 and
collapses to 61–82 with roughly triple the drop rate.

The screen is **not** expensive: with interpolation off it holds a steady 60. So the sub-frame loop
is failing to multiply frames there, delivering ~1.0–1.35× instead of the 2.4× a race gets.

**Mechanism unknown.** One hypothesis has already been tested and refuted: it does *not* track the
`gdx_dispatch`-path frames — inside that window the present rate is 134.9, *higher* than the 113.2
outside it.

### 2D menus cannot interpolate at all

There is no rect interpolation anywhere in the codebase. The entire mechanism is a `GfxPool`
double-buffer lerp on modelview matrices, and 2D menu elements are texture rectangles and fill
rects, which carry no matrix. `lerped=0` across the whole boot/menu window: those extra sub-frames
are duplicates, not tweened in-betweens.

This is absent machinery, not a broken feature. Adding it means pairing rect N across frames — the
same identity problem the matrices had, and a project rather than a patch.

### Local multiplayer — fixed 2026-08-01, awaiting a play test

**Previously:** `port/input_bridge.c` forced ports 1–3 to `PORT_DISCONNECTED` on every poll, so
split-screen VS and Death Race had no gamepad input at all.

The defect had **two independent halves**, and fixing only the first would have looked like the fix
failed:

1. **Decomp side** — the hardcoded `gControllersConnected = 1`, so players 2–4 resolved to the
   never-written `gControllers[4]` dummy (`racer.c:4952`).
2. **libultraship side** — `ConnectedPhysicalDeviceManager.cpp:110-112` inserts *every* newly-seen
   gamepad into the ignore list of ports 1–3, and `ControlDeck.cpp:36` seeds default mappings for
   port 0 only. Four plugged-in pads all drove player 1.

Both are addressed. `gSharedController` is now a real OR-aggregate across connected ports rather
than a mirror of port 0 — without that, pads 2–4 could not even press Start, because every menu
reads it through `Controller_SetGlobalInputs` (`common.c:184`).

Port routing is opt-out via `gEnhancements.Input.AutoAssignGamepadPorts` (default on) and is inert
below two pads, so single-pad behaviour is unchanged.

**Still open:** the keyboard remains port 1 only, so it cannot act as player 2 — which is also why
VS Battle cannot be entered with a single pad (`main_menu.c:323-330` refuses below two players).
The input-viewer overlay likewise still shows port 1 only.

### Modding onboarding — stale docs, working code

The pipeline itself is sound: the in-game **Asset Dump** writes `manifest.tsv` in exactly the format
`tools/gen_texture_pack.py` parses (`torch/src/gdx/dump_textures.cpp:301`, `:443` against
`gen_texture_pack.py:200-216`). Dump → edit → pack works.

What broke was the documentation around it. The old runtime "Dump textures while playing" checkbox was
retired (`gdx_menu.cpp:604` force-sets the CVar to 0) and the HTML contact sheet went with it —
`regenContactSheetIfDue` is now unreachable, called only from `gdx_workshop.cpp:387` (inside
`gdx_workshop_dump_count`, which has no callers) and `:506` (behind the retired CVar). Nothing in
`torch/` writes a contact sheet, so the offline path never had one.

`docs/MODDING_GUIDE.md` has been rewritten around Asset Dump. A contact sheet would still be a real
onboarding improvement — it is the fastest way to find a texture you saw on screen — but its absence
does not block pack authoring.

---

## Open defects

| Defect | Status | Leading mechanism |
|---|---|---|
| White buildings | Open, two prior root causes refuted | TMEM slot bookkeeping — `StoreLoadedTexture` (`libultraship/src/fast/interpreter.cpp:4422-4429`) erases the entire footprint of overlapping earlier loads, then re-materializes only its own smaller range |
| Course Edit pause corruption **+** Time Attack texture | Open, **merged — one root** | Registry miss returns NULL → `func_8007E410` skips the TLUT upload → CI text draws against the last-loaded palette. Named in-source at `decomp/src/game/object.c:653-658` |
| Ending fireworks | Open, **confirmed a real defect** | Owner finished on the podium and saw none, so the `gPlayer1OverallPosition < 4` gate at `ending.c:274` was satisfied and the flags were set. Fault is downstream: allocation, draw path, or the still-open widescreen cull |
| Race-entry crash (~1 in 13) | Open, evidence unattributable | Three of four crash records sit within `0x2200` — one site, one build, hit three times. An intermittent trigger on a deterministic site, not a broad race |
| KM/H stripes | Open, uncharacterised | No description exists in repo or memory. The asset itself is verified intact (`hud.c:735`) |
| Ceremony widescreen 4:3 squeeze | Open | Unchanged since the July-18 pass |
| Interpolation delivers ~1.0–1.35× outside races | Open, **quantified 2026-08-01** | See "Shipped but not working" above. Screen is cheap (steady 60 with interpolation off), so the sub-frame loop is failing to multiply. One hypothesis already refuted |
| `GDX_INTERP_CAMERA` is a dev gate | Open, **ship blocker** | Tested and working, but bucket-B, so a stock Release compiles it out and users cannot get the fixed behaviour |
| `G-Diffuser-JP` absent from the generated solution | Open | `rg -c "G-Diffuser-JP" build/x64/GDiffuser.sln` returns 0. It compiles the same `gdx_menu.cpp` and is therefore untested by construction |

### Untested — not yet defects

| Item | Note |
|---|---|
| Top-3 markers + rival marker | Never verified. A dedicated `GDX_DIAG_RIVAL` gate already exists for exactly this |
| Assorted graphical bugs with interpolation on | Owner reports these predate the camera fix. Partly explained by partial interpolation; not individually catalogued, so not individually actionable |

### Closed

**Runtime shader-compile stalls — fixed and confirmed 2026-08-01.** Frame stalls of 100–181 ms were
runtime HLSL compilation reached from inside the draw call, 9–15 ms per combiner variant, arriving
in bursts (eleven inside one tick for a 181 ms stall). A persistent compiled-shader cache
(`libultraship/src/fast/backends/gfx_shader_cache.{h,cpp}`, both backends) fixed it. A/B verified by
the owner: run 1 logged 40 compiles and 111 spikes; run 2 logged **zero** compiles and 25 spikes,
with the worst spike falling from 181 ms to 51.9 ms. The 100 ms+ population is gone.

**Seg-9 editor textures "resolving outside RDRAM" — not a defect.** `out=` is
`MakePersistentRawTextureCopy`'s heap copy by design (`port/n64_gfx_bridge.cpp:5141` → `:3914`).
All four logged byte prefixes were read back from `baserom.translated.ek.ndd` at their declared EK
offsets and match exactly.

---

## The diagnostic harness

**This is the project's most expensive defect, and it is not in the game.** Six of seven
investigations on 2026-07-31 terminated at a probe that had never run.

Two structural causes:

1. **`GDX_TRACE` double-gating.** `gdx_ck`, `gdx_cki` and `gdx_ckp` each open with
   `if (!gdx_trace_enabled()) return;` (`port/n64_sched.c:503-516`), and `GDX_TRACE` defaults to 0
   under `NDEBUG`. Every probe built on them — including all asset-delivery failure telemetry — is
   silent in a Release run unless a second, undocumented variable is set.
2. **Two parallel gate systems.** 28 CVar-backed gates in `port/gdx_dev_gates.c`, plus roughly two
   dozen `getenv`-only probes inside `libultraship` that have no CVar, no checkbox, mostly latch once
   per process, and are not compiled out of Release. They overlap on exactly one name,
   `GDX_DIAG_SETTIMG`, and the menu checkbox does not reach the libultraship half.

Both are documented in full in `docs/DIAGNOSTICS.md`. Read that before concluding a probe found
nothing.

Known outstanding: `[reg-miss]` at `decomp/src/game/object.c:660-667` is still on `gdx_ckp`/`gdx_cki`
— it was missed by the ~42-site conversion to `gdx_dbg_logf`.

---

## Not present, by design or by decision

| | |
|---|---|
| **Networking** | No socket, no HTTP client, no dependency that could become one. Verified across `port/`, `libultraship/src`, and both CMakeLists. |
| **Save states** | Built, found structurally inadequate for this substrate, and deleted on 2026-07-19. |
| **CI** | None. Every check is manual. This constrains what the project can safely take on — notably any public scripting API. |
| **Audio goldens** | `tools/audio_pcm_harness/golden/` contains only a README. Zero blessed references. |
| **Adaptive final-lap audio** | Does not exist. No CVar, no code. Previously claimed as implemented. |

---

## Determinism

**Partial: favourable architecture, no gameplay evidence.**

In favour: the decomp makes only 138 `sqrtf` + 4 `sinf` + 4 `cosf` calls; `sinf`/`cosf` resolve to the
decomp's own vendored SGI libultra sources rather than the platform libm; no fast-math, FMA or
`-march` flags anywhere; `-fno-strict-aliasing -fwrapv` on all build types; RNG pins ship PORT-gated.

Against: `-ffp-contract=off` is unpinned, so GCC contracts and MSVC does not — a live Windows-vs-Linux
parity risk today, and roughly an hour to close. No determinism test has ever been run.

**Ghost replay is not a determinism proxy.** F-Zero X ghosts are position-delta streams
(`decomp/src/game/racer.c:4905`, applied at `:5363-5370`); playback replays coordinates and never
re-simulates physics. Ghost *recording* is a sim product, so running the same input script twice with
pinned seeds and comparing `replayChecksum` + `raceTime` **is** a valid test. Every piece already
exists (`GDX_INPUT_SCRIPT`, `GDX_RAND_SEED1/2`, `racer.c:1099-1104`). Roughly one day. Never run.

---

## Where the documentation lives

| Path | Contains |
|---|---|
| `docs/` | Current, maintained, contributor- and user-facing. `ARCHITECTURE.md`, `DIAGNOSTICS.md`, `MODDING_GUIDE.md`, this file. |
| `devdocs/` | Working scope and design documents. Gitignored — **not backed up by git.** |
| `devdocs/investigation/<date>/` | Dated investigation reports. Point-in-time findings, never edited after the fact. |
| `devdocs/archive/` | Superseded documents, kept for provenance. Nothing here should be treated as current. |

**When a conclusion is withdrawn, demote its heading.** `WHITE_TEXTURES_BLACKSTRIPE_PERF_GP2.md`
retracted its own root cause in an appended addendum but left a `## ... ROOT CAUSE FOUND` heading
above it. A test session was spent re-verifying a conclusion the document had already abandoned.

---

## Open decisions

Not recommendations — forks that need an owner's answer.

1. **Direct-key asset shadowing.** Mods mount last and win, so a pack can already shadow any of the
   2,526 segment assets, models, or audio blobs. Nothing exposes it, and it is gated by no CVar —
   which conflicts with the standing "defaults-OFF ⇒ byte-identical to stock" invariant. Expose, or
   gate first?
2. **Ports 1–3.** Pre-1.0 parity defect, or explicit non-goal?
3. **Frame interpolation.** Fix the predicate for 1.0, or hide the toggle until it works?
4. **Lua scripting.** Resolved 2026-08-18 by owner directive: committed, shipped as the sandboxed
   read-only v1 (MODDING_GUIDE §15). The support-contract concern stands for v2+ (state write, HUD
   draw) — each remains its own decision.
5. **Leaderboards.** The ghost format makes replay verification impossible, not merely hard. Publish
   knowing entries are structurally forgeable, or not at all?
6. **Shared ghosts and PII.** A `.gdg` carries none today. An author handle would be the first
   personal data the project ever holds.
