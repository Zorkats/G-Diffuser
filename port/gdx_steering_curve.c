// G-Diffuser: per-port steering response curve. See gdx_steering_curve.h for the API.

#include "gdx_steering_curve.h"

#include <math.h>
#include <stddef.h>

extern float CVarGetFloat(const char* name, float defaultValue); // libultraship consolevariablebridge.h

static const char* const sSteeringCurveCVars[GDX_STEERING_PORT_COUNT] = {
    "gEnhancements.Input.SteeringExponentPort1",
    "gEnhancements.Input.SteeringExponentPort2",
    "gEnhancements.Input.SteeringExponentPort3",
    "gEnhancements.Input.SteeringExponentPort4",
};

float gdx_steering_curve_apply(float steering, float exponent) {
    float curved;

    // The saved settings file can hold anything, and NaN fails every range comparison.
    if (exponent != exponent) {
        return steering;
    }
    if (exponent < GDX_STEERING_EXPONENT_MIN) {
        exponent = GDX_STEERING_EXPONENT_MIN;
    } else if (exponent > GDX_STEERING_EXPONENT_MAX) {
        exponent = GDX_STEERING_EXPONENT_MAX;
    }
    // Stock steering has to stay bit-exact, so skip powf and its rounding.
    if (exponent == 1.0f) {
        return steering;
    }

    curved = powf(fabsf(steering), exponent);
    return (steering < 0.0f) ? -curved : curved;
}

const char* gdx_steering_curve_cvar(int port) {
    if ((port < 0) || (port >= GDX_STEERING_PORT_COUNT)) {
        return NULL;
    }
    return sSteeringCurveCVars[port];
}

float gdx_steering_curve_for_port(int port, float steering) {
    const char* cvar = gdx_steering_curve_cvar(port);

    if (cvar == NULL) {
        return steering;
    }
    return gdx_steering_curve_apply(steering, CVarGetFloat(cvar, GDX_STEERING_EXPONENT_DEFAULT));
}
