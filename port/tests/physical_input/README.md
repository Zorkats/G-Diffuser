# Direct physical input regression harness

Run from the repository root with CMake and a C++20 compiler (VS/MSVC on Windows):

```sh
cmake -S port/tests/physical_input -B build/physical-input-tests
cmake --build build/physical-input-tests --config Debug
ctest --test-dir build/physical-input-tests -C Debug --output-on-failure
```

Release is also supported: replace `Debug` with `Release` in the last two commands.
Assertions remain active in both configurations. This standalone CMake project does
not configure the renderer or download dependencies; it needs no SDL development SDK.
No compiler installation path is hardcoded.

## What executes

- Production `gdx_physical_input.cpp` and `gdx_input_actions.c`.
- Actual Ship mapping classes, implementations and `ButtonMappingFactory.cpp`.
- The existing legacy action-layer regression executable.

Only SDL devices, the window, deck/controller services, wheel state, logging and
configuration services are fixtures. Generic and per-device blocker behavior is
modeled by the deck fixture; the production methods were inspected in
`libultraship/src/ship/controller/controldeck/ControlDeck.cpp`: each device-specific
check includes `AllGameInputBlocked()`, which checks registered blocker IDs. The
runtime harness does **not** exercise the renderer, OS event backend, real device
routing, the Fast3dWindow observer, or the production LUS lag queue. The optional
headless target below additionally exercises real ImGui. Lag tests run
production physical history against an independent full-pad LUS-history model,
plus literal edge sequences. A static bridge contract test checks that the one
legacy read still supplies sticks and that physical apply updates only buttons.

Coverage includes persistence isolation, first-event bank loading, all-port/context
release delivery, source deduplication and press-context latching, snapshots on
first Add/Replace/Remove, atomic replacement rejection and descriptor deduplication,
explicit empty vs inherit, SDL factory capture, keyboard capture,
mouse and axis descriptors, generic/per-device blockers, disabled/disconnected
sources, direct-only port availability, external invalidation, and cached hot paths.
Counters require zero bank-configuration reads and zero legacy mapping traversals
after warm-up, including keyboard events. Only the live simulated-lag setting is
read once per unblocked port poll. These are deterministic checks, not timing tests.

## Optional headless input editor tests

When cached ImGui sources exist at `build/x64/_deps/imgui-src` or
`build/_deps/imgui-src`, the same configure adds `gdx_input_editor_tests` and nine
independent CTest cases. No downloads, renderer, SDL SDK or game configure are used.
Override discovery with `-DGDX_TEST_IMGUI_DIR=<existing-imgui-source-directory>`;
missing/incomplete sources print a skip message and leave the original three tests
available. Disable explicitly with `-DGDX_TEST_HEADLESS_IMGUI=OFF`.

```sh
cmake -S port/tests/physical_input -B build/physical-input-ui-tests
cmake --build build/physical-input-ui-tests --config Debug
ctest --test-dir build/physical-input-ui-tests -C Debug -R gdx_input_editor --output-on-failure
```

This compiles production `port/gdx_input_editor.cpp/.h`, the physical runtime and
actual Ship mappings with cached ImGui core (validated with 1.91.9b). Only the LUS
base editor/services are stubbed. The base fixture models the inactive legacy
popup's navigation reset in `InputEditorWindow::UpdateElement`; it does not render
or validate legacy movement/device controls. It does not prove full-game linkage
or production base-class integration. The production global GUI hotkey/event route
is **not compiled**: these tests do not prove hotkey suppression or event routing.
They explicitly supply the runtime `consumed` flag as that route would.

A subclass exposes only protected draw/update. Real mouse events click widget
rectangles collected with ImGui's optional item hooks; tab/combo IDs cover labels
not emitted by 1.91's hooks. No private editor state or mocked widget results are
used. Keyboard capture is delivered through the real physical event API, as the
OS backend would; Escape is delivered through ImGui IO. Frames advance explicit
ImGui time, without sleeps or ini persistence. Each case runs in a separate process,
and checks remain active in Release.

Cases cover Add opening the modal, debounce and saved/reloaded key descriptors,
Cancel/Escape without any CVar writes, replacement/removal, player/context/other-row
isolation, delayed blocker release, hidden/collapsed missing-draw cancellation,
continuous draw/update heartbeat, and destruction during capture. Every capture
must claim `Gui::IsInputCaptureActive()` independently of navigation blocking;
save/cancel/hidden/collapsed/destruction must release ownership immediately, before
the deck's delayed unblock. The base stub's navigation reset deliberately does not
reset capture ownership. Hidden/collapsed checks also verify keyboard capture has
stopped and reopening does not revive the modal. Destruction requires immediate
deck, keyboard, navigation and capture-ownership cleanup.

Cancel starts with an existing Escape binding. Both button and armed Escape cancel
must preserve all CVars without new writes/saves. A consumed Escape must remain
inactive after the blocker expires, cache invalidation and an unconsumed repeat;
only release followed by a fresh unconsumed press may activate gameplay. This is a
direct runtime consumption check, not a production global GUI hotkey test.

`controller_capture` holds fake SDL Back through the debounce deadline, requires
release/neutral input before arming, then presses Back again. It verifies no early
writes, exactly one save, the real SDL mapping descriptor after reload, and capture
ownership/blocker cleanup. The production SDL mapping factory is used.

Production files are not modified by this harness. Regressions remain ordinary
failing tests, not expected-failure tests. The added ownership assertions require
production BeginCapture to set capture active and EndCapture/destruction to clear
it; the fixture never infers ownership from navigation blocking.

## Cache/reload integration contract

`GdxPhysicalInput::Invalidate()` must run on the input/main thread after external
physical CVar edits, legacy controller mapping edits, or a configuration reload:

```cpp
CVarLoad();                       // Existing LUS bridge -> ConsoleVariable::Load().
GdxPhysicalInput::Invalidate();    // Includes gdx_input_actions_mark_dirty().
```

LUS currently provides no reload notification, and there is no port-side reload
caller to attach to. This change deliberately does not edit that bridge or the
menu/InputEditor. Any caller using `GetConsoleVariables()->Load()` directly needs
the same follow-up. `gdx_input_actions_mark_dirty()` alone does not invalidate the
physical cache. Call `Invalidate()` as the single shared refresh entry instead.

Invalidation reads/writes no CVars and preserves source latches. The next access
loads changed banks; the next keyboard event loads every bank before forwarding
its event. No periodic fingerprint polling or time heuristic is used. Runtime UI
Add/Replace/Remove/Clear/UseLegacy calls synchronously update only the edited row. Ordinary
legacy action-mask setters are detected via their already-cached mask values;
legacy physical mapping changes require `Invalidate()`.

## Simulated input lag parity

The physical runtime maintains per-port history with the same ordering as
`LUS::Controller::ReadToOSContPad()`: resolve mappings with their device-specific
checks, push a sample, select `min(size - 1, size_t(lag))`, then trim to six samples.
Selection occurs before trimming, so a full queue temporarily has seven entries;
large values and negative integers follow LUS's oldest-available-sample behavior.
The setting is `CVAR_SIMULATED_INPUT_LAG` (default `gSimulatedInputLag`, integer 0).

History advances on every unblocked poll, even when no output is configured, so
first-time overrides do not start a cold queue. Configured and recomposed inherited
banks use historical resolved bits with **current** replacement masks. UI binding
changes, explicit invalidation and live lag-setting changes preserve history;
pending presses/releases drain normally. A new deck starts fresh history.

Generic `BlockGameInput` blockers are different from device-specific checks:
LUS's deck skips the controller read entirely. The read-only `GameInputBlocked()`
getter lets the physical layer likewise freeze history and suppress historical
physical output, while still sampling releases and updating device availability.
Device-specific blocking instead contributes zero to the new sample, allowing
older queued presses/releases to drain just as they do in LUS.

The existing single legacy pad read and physical apply occur once in the same
poll. No extra `WriteToPad()` is performed. Completely inherited button outputs
and all sticks retain their already-lagged legacy values; physical apply changes
only the replaced button bits. Tests cover lag 0/1/multi, startup clamping,
selection before trim, independent ports, inherited-context recomposition, changes
to bindings/configuration/lag while samples are pending, both blocker modes,
and button-only stick passthrough.
