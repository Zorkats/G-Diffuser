# Code mods

> Change how the game plays by editing its source code, then build and share your own version.

Packs change what the game **looks and sounds like**. To change what the game **does**, such as
physics, rules, or menus, you edit the C and C++ source and build the game yourself.

This guide walks through one real change from start to finish: a **Pit refill rate** setting that
makes pit lanes refill energy faster or slower. It covers finding the code, changing it, adding a
menu setting, and building a release on GitHub that other people can download.

You do not need to be an expert in C. You do need to be comfortable running commands in a
terminal.

## How the code is organized

G-Diffuser is made of several projects, each in its own folder:

| Folder | What it holds | Edit it for |
| --- | --- | --- |
| `decomp/` | The F-Zero X game code, rebuilt as C from the original game. | Gameplay, physics, rules, menus. |
| `port/` | The PC side: settings menu, input, mods, saves. | New settings, PC features. |
| `libultraship/` | The renderer and window system. | Rarely. |
| `torch/` | The tool that reads assets from the ROM. | Rarely. |

`decomp/` is a **git submodule**: a separate repository linked inside this one. That matters when
you commit and share (step 8).

### The one rule: `#ifdef PORT`

The game code in `decomp/` can still build the original N64 game. Any change you make there goes
inside an `#ifdef PORT` block, with the original code in the `#else` branch:

```c
#ifdef PORT
    /* your PC-only version */
#else
    /* the original code, unchanged */
#endif
```

That keeps the original game intact and makes your change easy to find and review.

## Step 1: Fork and clone

You need your own copies of two repositories on GitHub, so you can push your changes:

1. Fork [Zorkats/G-Diffuser](https://github.com/Zorkats/G-Diffuser).
2. Fork [Zorkats/fzerox](https://github.com/Zorkats/fzerox). This is the `decomp/` submodule.

Clone your G-Diffuser fork with all its submodules:

```sh
git clone --recursive https://github.com/<your-user>/G-Diffuser.git
cd G-Diffuser
```

## Step 2: Build it once, unchanged

Follow [Building in the README](../../README.md#building) for your system. Do this before you
change anything, so you know your tools work.

On Windows, from a Developer Command Prompt:

```sh
cmake -S . -B build/x64 -DUSE_AUTO_VCPKG=ON
cmake --build build/x64 --config Release --target G-Diffuser
```

The game lands in `build/x64/port/Release/`. Run it once and set it up with your ROM, as in the
README's [Quick Start](../../README.md#quick-start).

The first build is slow. Later builds only recompile what you changed.

## Step 3: Learn the pattern from an existing setting

Most code mods are "a setting that changes a piece of game code". The game already has one you can
copy: **Lava strips never kill**, under **Enhancements > Gameplay**. It lives in three places.
Search the code for `LavaNoKill` to find them:

| Place | File | What it does |
| --- | --- | --- |
| 1. Register | `port/gdx_menu.cpp` | `CVarRegisterInteger("gEnhancements.Gameplay.LavaNoKill", 0);` creates the setting with its default value. |
| 2. Show | `port/gdx_menu_registry.cpp` | `AddWidget(...)` puts a checkbox for it on the menu page. |
| 3. Use | `decomp/src/game/racer.c` | `CVarGetInteger("gEnhancements.Gameplay.LavaNoKill", 0)` reads it inside `#ifdef PORT`. |

A setting is called a **CVar**. Its name, here `gEnhancements.Gameplay.LavaNoKill`, is how all three
places refer to it. The game saves CVars for you, so a player's choice survives a restart.

## Step 4: Find the code you want to change

The pit refill happens in `decomp/src/game/racer.c`. Search that file for `COURSE_EFFECT_PIT`:

```c
    if ((racer->stateFlags & (RACER_STATE_CRASHED | RACER_STATE_AIRBORNE | RACER_STATE_SPINNING_OUT |
                              COURSE_EFFECT_MASK)) == COURSE_EFFECT_PIT) {
        racer->energy += racer->energyIncrease;
        if (racer->maxEnergy < racer->energy) {
            racer->energy = racer->maxEnergy;
        }
```

Read it as: "if the machine is on a pit and is not crashed, airborne, or spinning, add
`energyIncrease` to its energy, without going over the maximum." That one line is what you will
change:

```c
        racer->energy += racer->energyIncrease;
```

> **Finding code in general.** Search the source for words you can see in the game, or for names
> of related things. `decomp/src/game/` holds most of the race logic. The variable names come from
> the decompilation project, so many are descriptive, and some are still placeholders such as
> `unk_8C`.

## Step 5: Try a quick change

Before building a menu for it, check that you found the right line. Replace it with:

```c
#ifdef PORT
        racer->energy += racer->energyIncrease * 4.0f;
#else
        racer->energy += racer->energyIncrease;
#endif
```

Rebuild with the same build command as step 2, start a race, take some damage, and drive through
the pit. It should refill four times as fast.

If nothing changed, the build probably failed. Read the build output for the first `error` line.

## Step 6: Turn it into a setting

Now replace the hard-coded `4.0f` with a slider players can set. Make the same three edits as
`LavaNoKill`.

**1. Register it.** In `port/gdx_menu.cpp`, find the `LavaNoKill` line and add yours below it:

```cpp
    // Percent of the stock pit refill speed, read in racer.c. 100 = stock.
    CVarRegisterInteger("gEnhancements.Gameplay.PitRefillRate", 100);
```

**2. Show it.** In `port/gdx_menu_registry.cpp`, find the `LavaNoKill` widget and add this after
it. It is a slider, built the same way as the **AI aggression (%)** slider in the same file:

```cpp
    AddWidget("Enhancements", "Gameplay", SECTION_COLUMN_1,
              WidgetInfo{ .name = "Pit refill rate (%)", .cVar = "gEnhancements.Gameplay.PitRefillRate",
                          .type = GdxUI::WIDGET_CVAR_SLIDER_INT }
                  .Options(UIWidgets::IntSliderOptions()
                               .Min(25)
                               .Max(400)
                               .DefaultValue(100)
                               .ShowButtons(false)
                               .LabelPosition(UIWidgets::LabelPositions::Near)
                               .Tooltip("How fast pit lanes refill energy.\n100 = stock."))
                  .ModifiedMarker()
                  .SearchTerms("pit refill energy recharge rate"));
```

**3. Use it.** In `decomp/src/game/racer.c`, replace your quick test with:

```c
#ifdef PORT
        racer->energy += racer->energyIncrease *
                         (CVarGetInteger("gEnhancements.Gameplay.PitRefillRate", 100) / 100.0f);
#else
        racer->energy += racer->energyIncrease;
#endif
```

`racer.c` already declares `CVarGetInteger` near its top, so there is nothing else to add. The
`100` in the call is the value used if the setting does not exist, and it matches the default you
registered.

## Step 7: Rebuild and test

Rebuild, start the game, and press **F1**. Open **Enhancements > Gameplay**: your **Pit refill
rate (%)** slider is there. Set it to `400`, race, and drive through the pit. Then set it to `100`
and check that the pit behaves like the original game.

Testing the default value matters. A setting at its default should play exactly like stock.

## Step 8: Commit your change

Your change touches two repositories: the game code in `decomp/`, and the menu code in the main
repository. Commit the submodule first.

**The `decomp/` submodule:**

```sh
cd decomp
git checkout -b pit-refill-rate
git add src/game/racer.c
git commit -m "feat(racer): scale pit refill by the PitRefillRate setting"
git remote add myfork https://github.com/<your-user>/fzerox.git
git push myfork pit-refill-rate
cd ..
```

**The main repository.** Point `decomp/` at your fork, so anyone who clones yours (including
GitHub's build machines) can fetch your submodule commit:

```sh
git submodule set-url decomp https://github.com/<your-user>/fzerox.git
git checkout -b pit-refill-rate
git add .gitmodules decomp port/gdx_menu.cpp port/gdx_menu_registry.cpp
git commit -m "feat(gameplay): add a Pit refill rate setting"
git push origin pit-refill-rate
```

`git add decomp` records which `decomp/` commit your version uses. Forgetting it is the most
common mistake: your fork would build with the original game code.

## Step 9: Build a release on GitHub

Your fork already contains the workflows that build the official releases. GitHub turns them off
on new forks, so turn them on once:

1. Open your fork on GitHub and click the **Actions** tab.
2. Click the button that enables workflows.

Then build:

1. In **Actions**, pick **Release (Windows)** or **Release (Linux)**.
2. Click **Run workflow**, choose your `pit-refill-rate` branch, enter a version such as
   `1.1.0-pitrate`, and click **Run workflow**.
3. When the run finishes, open it. The packaged game is under **Artifacts** at the bottom of the
   page.

That download is the same kind of package as an official release, built from your code. Share
it, or attach it to a release on your fork.

## Step 10: Share it upstream (optional)

If you think everyone would want your change, open a pull request to
[Zorkats/G-Diffuser](https://github.com/Zorkats/G-Diffuser) and one to
[Zorkats/fzerox](https://github.com/Zorkats/fzerox) for the `decomp/` part. Keep your setting's
default at stock behavior, as in this guide.

## Tips

- **Start small.** Change one number, build, test. Then build the setting around it.
- **Keep stock as the default.** Gameplay settings in **Enhancements** default to the original
  game's behavior, and players opt in.
- **Change the PC side when you can.** If your idea fits in `port/`, you do not need the `decomp/`
  submodule steps at all.
- **No rebuild needed?** For read-only race overlays and logging, a [Lua script](LUA_SCRIPTS.md)
  may be enough.

## Quick reference

```text
Game logic:        decomp/src/   (changes go inside #ifdef PORT ... #else original #endif)
Register a CVar:   port/gdx_menu.cpp           CVarRegisterInteger("gEnhancements.<Page>.<Name>", <default>);
Show it:           port/gdx_menu_registry.cpp  AddWidget("Enhancements", "<Page>", ...)
Read it in C:      CVarGetInteger("gEnhancements.<Page>.<Name>", <default>)
Build (Windows):   cmake --build build/x64 --config Release --target G-Diffuser
Release build:     your fork > Actions > Release (Windows | Linux) > Run workflow > Artifacts
```
