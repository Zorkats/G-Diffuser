-- lap_timer.lua — minimal G-Diffuser Lua script pack example.
--
-- Ships inside a mods/*.o2r pack at "scripts/lap_timer.lua" and logs every racer's lap times to
-- gdiffuser-run.log (and stderr). Requires the "Enable Lua script packs" checkbox
-- (gEnhancements.Workshop.Scripts) in Workshop -> Content.
--
-- The script API v1 is read-only: gdx.log/frame/mode/racerCount/racer plus the four callbacks
-- below. A script that errors or blows its instruction budget is auto-disabled until the next
-- "Reload packs".

-- Track each racer's lap start time so onLap can report the LAP time rather than the running
-- race total. racerId indexes gRacers (0..29; 0..gNumPlayers-1 are the human players).
local lapStart = {}

local function formatMs(ms)
    local m = math.floor(ms / 60000)
    local s = (ms % 60000) / 1000.0
    return string.format("%d:%06.3f", m, s)
end

function gdx.onRaceStart()
    -- gdx.racer(i) returns a per-call snapshot table; never keep it across frames.
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
    gdx.log(string.format("racer %d finished lap %d in %s (total %s, pos %d)",
                          racerId, lap - 1, formatMs(lapTime), formatMs(r.raceTime), r.position))
end

function gdx.onFinish(racerId, position, raceTimeMs)
    gdx.log(string.format("racer %d finished P%d in %s", racerId, position, formatMs(raceTimeMs)))
end
