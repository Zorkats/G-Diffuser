/* port/tests/gdx_steering_curve_tests.c -- unit tests for the per-port steering response curve.
 * Standalone: links gdx_steering_curve.c and stubs the one CVar read it makes.
 */

#include "../gdx_steering_curve.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int sFailures;
static float sPortExponents[GDX_STEERING_PORT_COUNT];

float CVarGetFloat(const char* name, float defaultValue) {
    int port;
    for (port = 0; port < GDX_STEERING_PORT_COUNT; port++) {
        if (strcmp(name, gdx_steering_curve_cvar(port)) == 0) {
            return (sPortExponents[port] != 0.0f) ? sPortExponents[port] : defaultValue;
        }
    }
    return defaultValue;
}

static void check(int condition, const char* label) {
    if (condition) {
        printf("PASS: %s\n", label);
    } else {
        fprintf(stderr, "FAIL: %s\n", label);
        sFailures++;
    }
}

static int near_value(float a, float b) {
    return fabsf(a - b) < 0.0001f;
}

/* Bit-exact, not just close: the default has to leave stock steering alone. */
static int same_bits(float a, float b) {
    return memcmp(&a, &b, sizeof(float)) == 0;
}

static void test_default_is_stock(void) {
    int stick;
    int exact = 1;
    for (stick = -80; stick <= 80; stick++) {
        float steering = stick / 63.0f;
        if (!same_bits(gdx_steering_curve_apply(steering, GDX_STEERING_EXPONENT_DEFAULT), steering)) {
            exact = 0;
        }
    }
    check(exact, "exponent 1.0 returns every stick value unchanged");
}

static void test_curve_shape(void) {
    check(near_value(gdx_steering_curve_apply(0.5f, 2.0f), 0.25f), "half stick at 2.0 gives 25%");
    check(near_value(gdx_steering_curve_apply(0.5f, 0.5f), 0.70711f), "half stick at 0.5 gives about 71%");
    check(near_value(gdx_steering_curve_apply(-0.5f, 2.0f), -0.25f), "left steering keeps its sign");
    check(gdx_steering_curve_apply(0.0f, 2.0f) == 0.0f, "center stays at center");
    check(gdx_steering_curve_apply(1.0f, 4.0f) == 1.0f, "full right stays at full lock");
    check(gdx_steering_curve_apply(-1.0f, 0.25f) == -1.0f, "full left stays at full lock");
}

static void test_exponent_limits(void) {
    float nan_value = (float) NAN;
    check(near_value(gdx_steering_curve_apply(0.5f, 100.0f), gdx_steering_curve_apply(0.5f, GDX_STEERING_EXPONENT_MAX)),
          "too high an exponent clamps to the maximum");
    check(near_value(gdx_steering_curve_apply(0.5f, -3.0f), gdx_steering_curve_apply(0.5f, GDX_STEERING_EXPONENT_MIN)),
          "too low an exponent clamps to the minimum");
    check(gdx_steering_curve_apply(0.5f, nan_value) == 0.5f, "NaN exponent leaves steering stock");
}

static void test_ports_are_independent(void) {
    memset(sPortExponents, 0, sizeof(sPortExponents));
    sPortExponents[1] = 2.0f;
    check(gdx_steering_curve_for_port(0, 0.5f) == 0.5f, "an unset port stays stock");
    check(near_value(gdx_steering_curve_for_port(1, 0.5f), 0.25f), "port 2 uses its own exponent");
    check(gdx_steering_curve_for_port(2, 0.5f) == 0.5f, "port 2's exponent doesn't reach port 3");
}

static void test_out_of_range_ports(void) {
    memset(sPortExponents, 0, sizeof(sPortExponents));
    sPortExponents[0] = 2.0f;
    check(gdx_steering_curve_cvar(-1) == NULL, "negative port has no CVar");
    check(gdx_steering_curve_cvar(GDX_STEERING_PORT_COUNT) == NULL, "PORT_DISCONNECTED has no CVar");
    check(gdx_steering_curve_for_port(GDX_STEERING_PORT_COUNT, 0.5f) == 0.5f, "PORT_DISCONNECTED stays stock");
}

int main(void) {
    test_default_is_stock();
    test_curve_shape();
    test_exponent_limits();
    test_ports_are_independent();
    test_out_of_range_ports();

    if (sFailures != 0) {
        fprintf(stderr, "%d steering curve test(s) failed\n", sFailures);
        return 1;
    }
    printf("All steering curve tests passed\n");
    return 0;
}
