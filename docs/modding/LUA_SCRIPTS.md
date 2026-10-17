# Lua scripts

> Run small scripts that react to races: lap times, positions, finishes.

A script pack is an `.o2r` file in `mods/` that holds one or more `.lua` scripts. The game runs
them while you race and calls them when something happens, such as a racer finishing a lap.

Scripts in this version are **read-only**. They can read race state and write to the log, but they
cannot change the game yet. For gameplay changes, see [Code mods](CODE_MODS.md).

## What you need

- G-Diffuser, already set up and running.
- A text editor.
- **Python 3**, only to zip the pack. Any tool that makes ZIP files works too.

No pack tools are needed. A script pack is a plain ZIP file renamed to `.o2r`.

## Step 1: Make the pack folder

Create a folder with this layout:

```text
lap-timer/
  workshop.json
  scripts/
    lap_timer.lua
```

`workshop.json` names your pack:

```json
{
  "name": "Lap Timer",
  "version": "1.0",
  "author": "your-name",
  "id": "your-name.lap-timer"
}
```

The `id` must be unique and must not contain commas. See
[Pack metadata](../MODDING_GUIDE.md#pack-metadata-workshopjson) for every field.

## Step 2: Write the script

Save this as `scripts/lap_timer.lua`:

```lua
local lapStart = {}

local function formatMs(ms)
    local m = math.floor(ms / 60000)
    local s = (ms % 60000) / 1000.0
    return string.format("%d:%06.3f", m, s)
end

function gdx.onRaceStart()
    for i = 0, gdx.racerCount() - 1 do
        lapStart[i] = 0
    end
    gdx.log("race started, " .. gdx.racerCount() .. " racers")
end

function gdx.onLap(racerId, lap)
    local r = gdx.racer(racerId)
    if r == nil then
        return
    end
    local lapTime = r.raceTime - (lapStart[racerId] or 0)
    lapStart[racerId] = r.raceTime
    gdx.log(string.format("racer %d finished lap %d in %s", racerId, lap - 1, formatMs(lapTime)))
end
```

A longer, commented version of this script is in the source code at
`docs/examples/lua/lap_timer.lua`.

## Step 3: Zip it and install it

Open a terminal **inside** the `lap-timer` folder and run:

```sh
python -m zipfile -c lap-timer.o2r workshop.json scripts
```

`workshop.json` and `scripts/` must be at the top of the ZIP, not inside a `lap-timer/` folder.
If you zip with another tool, select the two items themselves, not the folder that holds them.

Then:

1. Copy `lap-timer.o2r` into the `mods/` folder next to the game executable. **Workshop > Mods >
   Open mods folder** opens it for you.
2. On **Workshop > Mods**, tick **Enable Lua script packs**. It is off by default.
3. Click **Reload packs**.

The **Installed scripts** list on the same page shows each script and its status. Errors appear
there in red.

## Step 4: See the output

`gdx.log` writes to the game's log file as `[lua:<pack>/<script>] <message>`. Errors are logged
as `[lua] <pack>/<script>: <error>`. Turn the log on (see [Logs](../MODDING_GUIDE.md#logs)), race
a lap, and search the log for `[lua`.

To iterate: edit the script, zip it again, copy it into `mods/`, and click **Reload packs**.

## API reference (version 1)

Everything lives in the global `gdx` table.

### Functions you call

| Function | Returns |
| --- | --- |
| `gdx.log(msg)` | Nothing. Writes `msg` to the log. |
| `gdx.frame()` | The current frame counter. |
| `gdx.mode()` | The current game mode, as a number. |
| `gdx.racerCount()` | The number of racers on the grid. |
| `gdx.racer(i)` | A table describing racer `i`, or `nil` if `i` is out of range. |

`gdx.racer(i)` returns these fields:

| Field | Meaning |
| --- | --- |
| `id` | The racer's index. |
| `speed` | Current speed. |
| `energy`, `maxEnergy` | Current and maximum energy. |
| `lap` | The lap the racer is on. |
| `lapsCompleted` | Laps finished. |
| `position` | Race position. |
| `raceTime` | Race time in milliseconds. |
| `finished` | `true` once the racer crosses the finish line. |
| `crashed` | `true` while the racer is crashed. |

The table is a snapshot of that moment. Call `gdx.racer(i)` again each time you need fresh
values; do not keep the table across frames.

Racer `0` up to the number of human players minus one are the human players. The rest are CPU
racers.

### Functions the game calls

Define any of these in your script. Leave out the ones you do not need.

| Callback | When it runs |
| --- | --- |
| `gdx.onFrame()` | Every rendered frame. Keep it cheap. |
| `gdx.onRaceStart()` | Once, at GO. |
| `gdx.onLap(racerId, lap)` | When a racer crosses the line. `lap` is the new lap number, so the lap just finished is `lap - 1`. |
| `gdx.onFinish(racerId, position, raceTimeMs)` | When a racer finishes the race. |

## Limits

Scripts run in a sandbox, so a broken script cannot hurt the game or your files.

| Limit | Value |
| --- | --- |
| Instructions per callback | 1,000,000 |
| Memory per script | 8 MiB |
| Scripts loaded, across all packs | 32 |
| Libraries | base, `string`, `table`, `math`, `utf8` |
| Not available | `io`, `os`, `package`, `debug`, `coroutine`, `load`, `loadfile`, `dofile`, `require`, `collectgarbage` |

A script that raises an error or goes over a limit is turned off until the next **Reload packs**.
Other scripts keep running. Each script has its own state, so two packs can both ship a
`scripts/hud.lua` without clashing. Scripts from different packs run in pack order.

## If something goes wrong

**My script is not in the Installed scripts list.**
Check that the ZIP has `scripts/` at its top level, that the file ends in `.lua`, and that
**Enable Lua script packs** is ticked. Then click **Reload packs**.

**My script shows an error.**
The message names the line. Fix it, rebuild the pack, and click **Reload packs**.

**My script stopped working mid-race.**
It went over a limit, usually the instruction limit in `gdx.onFrame`. Move work out of
`onFrame`, or do it every few frames.

## Quick reference

```sh
# Layout:  workshop.json + scripts/<name>.lua
# Zip:     run inside the pack folder
python -m zipfile -c my-script.o2r workshop.json scripts

# Install: copy my-script.o2r into mods/
# Enable:  F1 > Workshop > Mods > Enable Lua script packs > Reload packs
# Output:  log lines prefixed [lua:<pack>/<script>]
```
