// G-Diffuser: controller profiles (4A). See gdx_input_profiles.h for the API.
//
// Each profile is one Race table: for every 3A output, the N64 input bits that trigger it. The
// official layouts (devdocs/feature-requests-2026-10-plan.md section 4) name physical inputs, so
// each row goes through the default LUS gamepad mapping before it becomes a mask.
//
// Default LUS gamepad mapping, physical input -> N64 bit
//   (libultraship/src/libultraship/controller/controldevice/controller/mapping/ControllerDefaultMappings.cpp)
//   A (south, SDL A)              -> A                  line 75
//   B (east, SDL B)               -> B                  line 76
//   LB (SDL leftshoulder)         -> L                  line 77
//   RB (SDL rightshoulder)        -> R                  line 80
//   LT and RT (axis, value 1)     -> Z, both triggers   line 101
//   Start                         -> START              line 81
//   D-pad up/down/left/right      -> D-Up/Down/Left/Right  lines 82-85
//   Right stick up/down/left/right -> C-Up/C-Down/C-Left/C-Right  lines 102-105
//   Left stick                    -> the N64 stick, not a button row
//   X, Y                          -> the spare bits GDX_INPUT_BTN_X / _Y (inputs to 3A only)
//   Back, Guide, stick clicks     -> nothing (not in the default map)
//
// Physical pads, from SDL's controller database (SDL 2.32.10, gamecontrollerdb.h):
//   Wii Classic (line 220) and Wii U Pro (line 221): ZL and ZR are triggers, so both send N64 Z.
//   GameCube (line 228): the Z button is rightshoulder, so it sends N64 R (SDL_hidapi_gamecube.c:273
//     agrees). Both analog L and R are triggers, so both send N64 Z. Not checked on hardware.
//   Nintendo Switch Pro, used for Nintendo Classics (line 231): ZL and ZR are triggers.
//
// Nintendo labels. SDL 2.32 reports a Nintendo pad by its printed label while
// SDL_GAMECONTROLLER_USE_BUTTON_LABELS is on. That is the default (SDL_hints.h, and
// SDL_hidapi_switch.c:1029), and G-Diffuser never sets it. So a printed "A" is SDL A and the
// profile names are the printed labels. Setting the hint to 0 swaps A and B on Nintendo pads.
//
// X and Y have no N64 button, so they reach actions only through these 3A masks. ZL and ZR both
// send N64 Z, so no profile can put drift left and drift right on the two triggers. Each profile's
// own limits are listed in its limitations string.

#ifndef _LANGUAGE_C
#define _LANGUAGE_C 1
#endif

#include "PR/os_pfs.h"
#include "controller.h"
#include "gdx_input_actions.h"
#include "gdx_input_profiles.h"

#include <stddef.h>

extern int CVarGetInteger(const char* name, int defaultValue); // libultraship consolevariablebridge.h
extern void CVarSetInteger(const char* name, int value);       // libultraship consolevariablebridge.h

// Index of the last applied profile, -1 when none. If two profiles ever share a table, it keeps
// showing the one the user picked, and it survives a restart.
#define GDX_INPUT_PROFILE_CVAR "gEnhancements.Input.Profile"

#define GDX_INPUT_PROFILE_D_PAD (BTN_UP | BTN_DOWN | BTN_LEFT | BTN_RIGHT)

typedef struct GdxInputProfile {
    const char* name;
    const char* description;
    const char* limitations;
    // Race masks in GdxInputOutput order (the UI row order).
    unsigned short race[GDX_INPUT_OUT_COUNT];
} GdxInputProfile;

// Race rows are listed in GdxInputOutput order: A, B, Z, Start, D-up, D-down, D-left, D-right, L,
// R, C-up, C-down, C-left, C-right. Each comment names the official action that row serves.
static const GdxInputProfile sProfiles[] = {
    {
        "N64 (G-Diffuser default)",
        "The N64 layout. Each action stays on its own N64 button. Same as Reset to N64 default.",
        "None.",
        {
            BTN_A,      /* Accelerate */
            BTN_B,      /* Boost */
            BTN_Z,      /* Drift left / side attack */
            BTN_START,  /* Pause */
            BTN_UP,     /* D-pad, unused in races */
            BTN_DOWN,   /* D-pad, unused in races */
            BTN_LEFT,   /* D-pad, unused in races */
            BTN_RIGHT,  /* D-pad, unused in races */
            BTN_L,      /* Gap to leader toggle */
            BTN_R,      /* Drift right / side attack */
            BTN_CUP,    /* Rear view */
            BTN_CDOWN,  /* Air brake */
            BTN_CLEFT,  /* Lap counter toggle */
            BTN_CRIGHT, /* Camera */
        },
    },
    {
        "Wii Classic Controller",
        "Wii Classic layout: L drifts left, R drifts right, ZL or ZR brakes, the D-pad toggles the gap "
        "display, and the right stick does camera, rear view and the lap counter.",
        "None.",
        {
            BTN_A,                       /* Accelerate, A */
            BTN_B,                       /* Boost, B */
            BTN_L,                       /* Drift left, L (ZL and ZR send Z, so they do not drift) */
            BTN_START,                   /* Pause */
            BTN_UP,                      /* D-pad, unused in races */
            BTN_DOWN,                    /* D-pad, unused in races */
            BTN_LEFT,                    /* D-pad, unused in races */
            BTN_RIGHT,                   /* D-pad, unused in races */
            GDX_INPUT_PROFILE_D_PAD,     /* Gap to leader toggle, D-pad */
            BTN_R,                       /* Drift right, R */
            BTN_CUP,                     /* Rear view, R-stick up */
            BTN_CDOWN | BTN_Z,           /* Air brake, R-stick down or ZL/ZR (Z) */
            BTN_CLEFT | GDX_INPUT_BTN_Y, /* Lap counter toggle, R-stick left or Y */
            BTN_CRIGHT | GDX_INPUT_BTN_X, /* Camera, R-stick right or X */
        },
    },
    {
        "Wii GameCube",
        "Wii GameCube layout: the C-stick does camera, rear view and air brake, and the D-pad toggles "
        "the gap display. The Z button drifts right and the triggers drift left.",
        "Both analog triggers send N64 Z, so the R trigger drifts left like L and cannot drift "
        "right. The Z button sends N64 R, which is drift right here, so it does not also brake.",
        {
            BTN_A,                       /* Accelerate, A */
            BTN_B,                       /* Boost, B */
            BTN_Z,                       /* Drift left, L trigger (and R trigger, see limitations) */
            BTN_START,                   /* Pause */
            BTN_UP,                      /* D-pad, unused in races */
            BTN_DOWN,                    /* D-pad, unused in races */
            BTN_LEFT,                    /* D-pad, unused in races */
            BTN_RIGHT,                   /* D-pad, unused in races */
            GDX_INPUT_PROFILE_D_PAD,     /* Gap to leader toggle, D-pad */
            BTN_R,                       /* Drift right, Z button (sends N64 R) */
            BTN_CUP,                     /* Rear view, C-stick up */
            BTN_CDOWN,                   /* Air brake, C-stick down */
            BTN_CLEFT | GDX_INPUT_BTN_Y, /* Lap counter toggle, C-stick left or Y */
            BTN_CRIGHT | GDX_INPUT_BTN_X, /* Camera, C-stick right or X */
        },
    },
    {
        "Wii U VC",
        "Wii U Pro Controller layout. Under the default mapping it matches the N64 layout, plus Y for "
        "rear view and X for the camera.",
        "ZL and ZR both send N64 Z, so both drift left and neither drifts right. R stays on drift right, "
        "so the R bumper does not brake.",
        {
            BTN_A,      /* Accelerate, A */
            BTN_B,      /* Boost, B */
            BTN_Z,      /* Drift left, ZL (and ZR, see limitations) */
            BTN_START,  /* Pause */
            BTN_UP,     /* D-pad, unused in races */
            BTN_DOWN,   /* D-pad, unused in races */
            BTN_LEFT,   /* D-pad, unused in races */
            BTN_RIGHT,  /* D-pad, unused in races */
            BTN_L,      /* Gap to leader toggle, L */
            BTN_R,      /* Drift right, R bumper (ZR cannot be separated from ZL) */
            BTN_CUP | GDX_INPUT_BTN_Y,    /* Rear view, R-stick up or Y */
            BTN_CDOWN,                    /* Air brake, R-stick down */
            BTN_CLEFT,                    /* Lap counter toggle, R-stick left */
            BTN_CRIGHT | GDX_INPUT_BTN_X, /* Camera, R-stick right or X */
        },
    },
    {
        "Nintendo Classics",
        "Nintendo Classics layout: L also toggles the lap counter, R drifts right, ZL drifts left, the "
        "D-pad toggles the gap display, and the right stick does camera, rear view and air brake.",
        "ZR drifts left like ZL, because both send N64 Z. R stays on drift right, so the R bumper does "
        "not brake.",
        {
            BTN_A,                       /* Accelerate, A */
            BTN_B,                       /* Boost, B */
            BTN_Z,                       /* Drift left, ZL (and ZR, see limitations) */
            BTN_START,                   /* Pause */
            BTN_UP,                      /* D-pad, unused in races */
            BTN_DOWN,                    /* D-pad, unused in races */
            BTN_LEFT,                    /* D-pad, unused in races */
            BTN_RIGHT,                   /* D-pad, unused in races */
            GDX_INPUT_PROFILE_D_PAD,     /* Gap to leader toggle, D-pad */
            BTN_R,                       /* Drift right, R bumper (ZR cannot be separated from ZL) */
            BTN_CUP | GDX_INPUT_BTN_Y,    /* Rear view, R-stick up or Y */
            BTN_CDOWN,                    /* Air brake, R-stick down */
            BTN_CLEFT | BTN_L,            /* Lap counter toggle, R-stick left or L bumper */
            BTN_CRIGHT | GDX_INPUT_BTN_X, /* Camera, R-stick right or X */
        },
    },
};

#define GDX_INPUT_PROFILE_COUNT ((int)(sizeof(sProfiles) / sizeof(sProfiles[0])))

static int gdx_input_profiles_valid(int index) {
    return index >= 0 && index < GDX_INPUT_PROFILE_COUNT;
}

static int gdx_input_profiles_race_equal(int index) {
    int output;

    for (output = 0; output < GDX_INPUT_OUT_COUNT; output++) {
        if (gdx_input_actions_get_mask(GDX_INPUT_CONTEXT_RACE, output) != sProfiles[index].race[output]) {
            return 0;
        }
    }
    return 1;
}

int gdx_input_profiles_count(void) {
    return GDX_INPUT_PROFILE_COUNT;
}

const char* gdx_input_profiles_name(int index) {
    return gdx_input_profiles_valid(index) ? sProfiles[index].name : NULL;
}

const char* gdx_input_profiles_description(int index) {
    return gdx_input_profiles_valid(index) ? sProfiles[index].description : NULL;
}

const char* gdx_input_profiles_limitations(int index) {
    return gdx_input_profiles_valid(index) ? sProfiles[index].limitations : NULL;
}

void gdx_input_profiles_apply(int index) {
    int output;

    if (!gdx_input_profiles_valid(index)) {
        return;
    }
    for (output = 0; output < GDX_INPUT_OUT_COUNT; output++) {
        gdx_input_actions_set_mask(GDX_INPUT_CONTEXT_RACE, output, sProfiles[index].race[output]);
    }
    CVarSetInteger(GDX_INPUT_PROFILE_CVAR, index);
}

int gdx_input_profiles_match(void) {
    const int stored = CVarGetInteger(GDX_INPUT_PROFILE_CVAR, -1);
    int index;

    if (gdx_input_profiles_valid(stored) && gdx_input_profiles_race_equal(stored)) {
        return stored;
    }
    for (index = 0; index < GDX_INPUT_PROFILE_COUNT; index++) {
        if (gdx_input_profiles_race_equal(index)) {
            return index;
        }
    }
    return -1;
}
