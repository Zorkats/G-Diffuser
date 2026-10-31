// G-Diffuser: per-port steering response curve. Each controller port has its own exponent, read by
// the race physics in racer.c Racer_UpdateFromControls for human racers only. 1.0 is stock.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_STEERING_EXPONENT_MIN 0.25f
#define GDX_STEERING_EXPONENT_MAX 4.0f
#define GDX_STEERING_EXPONENT_DEFAULT 1.0f

// Number of ports with their own setting.
#define GDX_STEERING_PORT_COUNT 4

// Maps steering in -1..1 through |steering|^exponent, keeping the sign. Center and full lock don't
// move. Above 1.0 is gentler near center, below 1.0 is sharper. An exponent outside the range is
// clamped, and NaN counts as 1.0. Exactly 1.0 returns the input untouched.
float gdx_steering_curve_apply(float steering, float exponent);

// CVar that holds a port's exponent. NULL when port is out of range.
const char* gdx_steering_curve_cvar(int port);

// Applies the port's configured curve. A port out of range, such as PORT_DISCONNECTED, returns the
// input untouched.
float gdx_steering_curve_for_port(int port, float steering);

#ifdef __cplusplus
}
#endif
