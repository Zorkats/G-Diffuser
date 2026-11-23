/* port/gdx_cpu_speed.cpp -- see gdx_cpu_speed.h. */
#include "gdx_cpu_speed.h"

#include "gdx_hackmods.h"
#include "port_log.h"

#include <libultraship/bridge/consolevariablebridge.h>

#include <string>

namespace {
int ClampPercent(int pct) {
    return pct < GDX_CPU_SPEED_MIN ? GDX_CPU_SPEED_MIN : pct > GDX_CPU_SPEED_MAX ? GDX_CPU_SPEED_MAX : pct;
}
} // namespace

extern "C" const char* gdx_cpu_speed_hack_cvar(void) {
    // The active hack is latched at boot and never changes for the rest of the run.
    static const std::string sName = [] {
        const std::string active = GdxHackModsActiveName();
        char safe[GDX_HACKMOD_NAME_MAX];
        if (active.empty() || !gdx_hackmod_sanitize_name(active.c_str(), safe, sizeof(safe))) {
            return std::string();
        }
        return std::string("gEnhancements.Hacks.CpuSpeed.") + safe;
    }();
    return sName.empty() ? nullptr : sName.c_str();
}

extern "C" int gdx_cpu_speed_hack_active(void) {
    return gdx_cpu_speed_hack_cvar() != nullptr ? 1 : 0;
}

extern "C" int gdx_cpu_speed_percent(void) {
    const char* hackCvar = gdx_cpu_speed_hack_cvar();
    const int hackPct = hackCvar != nullptr ? CVarGetInteger(hackCvar, 0) : 0;
    if (hackPct > 0) {
        return ClampPercent(hackPct);
    }
    return ClampPercent(CVarGetInteger("gEnhancements.Gameplay.CpuSpeed", GDX_CPU_SPEED_DEFAULT));
}

extern "C" float gdx_cpu_speed_scale(void) {
    return gdx_cpu_speed_percent() / 100.0f;
}

extern "C" void gdx_cpu_speed_log_race(int course, int tuningCourse, int difficulty, int targetMs) {
    gdx_port_logf("[cpu-pace] course=%d tuning=%d difficulty=%d reference=%d ms speed=%d%%\n", course, tuningCourse,
                  difficulty, targetMs, gdx_cpu_speed_percent());
}

extern "C" void gdx_cpu_speed_log_sample(int id, int lap, int position, float speed, float playerSpeed,
                                         float preCap, int cap, float target, float boost, float accel, int finishMs,
                                         int timeMs) {
    gdx_port_logf("[cpu-pace] cpu%d lap=%d pos=%d speed=%.1f player=%.1f want=%.1f cap=%d target=%.1f "
                  "boost=%.2f accel=%.3f finish=%d time=%d\n",
                  id, lap, position, speed, playerSpeed, preCap, cap, target, boost, accel, finishMs, timeMs);
}

extern "C" void gdx_cpu_speed_log_caps(const short* caps, int count) {
    if (caps == nullptr) {
        return;
    }
    std::string line = "[cpu-pace] caps:";
    for (int i = 0; i < count; i += 2) {
        line += " " + std::to_string(caps[i]);
    }
    gdx_port_logf("%s\n", line.c_str());
}
