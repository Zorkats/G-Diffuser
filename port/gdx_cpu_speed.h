/* port/gdx_cpu_speed.h -- CPU speed (%) enhancement.
 *
 * Scales the CPU racers' commanded speed (Cpu_GenerateInputs, ovl_i3/cpu.c) and their drive force
 * (racer.c), so they accelerate harder and reach a higher top speed. Human racers are untouched.
 * Meant for ROM hacks whose authors made the CPUs faster in code, which G-Diffuser never runs.
 *
 * CVars:
 *   gEnhancements.Gameplay.CpuSpeed          (int, 100): percent, 100 to 200, every mode.
 *   gEnhancements.Hacks.CpuSpeed.<hackname>  (int, 0):   percent for one ROM hack (the sanitised
 *                                                         basename latched at boot); 0 = use the
 *                                                         global value.
 */
#ifndef GDX_CPU_SPEED_H
#define GDX_CPU_SPEED_H

#define GDX_CPU_SPEED_MIN 50
#define GDX_CPU_SPEED_DEFAULT 100
#define GDX_CPU_SPEED_MAX 200

#ifdef __cplusplus
extern "C" {
#endif

/* Percent in effect for this race: the active hack's own value when set, else the global one. */
int gdx_cpu_speed_percent(void);
/* gdx_cpu_speed_percent() / 100. 1.0f is stock. */
float gdx_cpu_speed_scale(void);
/* Name of the active hack's CVar, or NULL when no hack is active. */
const char* gdx_cpu_speed_hack_cvar(void);
/* 1 while a ROM hack is active (latched at boot), else 0. */
int gdx_cpu_speed_hack_active(void);
/* [cpu-pace] log lines: once per race (the reference race time before scaling, ms) and a periodic
   sample of one CPU racer. */
void gdx_cpu_speed_log_race(int course, int tuningCourse, int difficulty, int targetMs);
void gdx_cpu_speed_log_sample(int id, int lap, int position, float speed, float playerSpeed, float preCap, int cap,
                              float target, float boost, float accel, int finishMs, int timeMs);
/* Once per race: the first entries of the CPU per-segment speed cap table (D_i3_8013DBE8). */
void gdx_cpu_speed_log_caps(const short* caps, int count);

#ifdef __cplusplus
}
#endif

#endif
