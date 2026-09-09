// G-Diffuser — GDX_INPUT_SCRIPT: dev-only deterministic tick-level input playback.
//
// See gdx_input_script.h for the integration contract.
//
// ---------------------------------------------------------------------------------------------
// SCRIPT FORMAT (plain text, one command per line, '#' starts a full-line comment, keywords are
// case-insensitive, blank lines ignored):
//
//   WAIT <frames>
//       Neutral input (no buttons, stick centered) for <frames> polls.
//
//   PRESS <buttons> <frames>
//       Hold the given button set for <frames> polls. <buttons> is one or more of
//       A,B,Z,START,L,R,UP,DOWN,LEFT,RIGHT (d-pad),CUP,CDOWN,CLEFT,CRIGHT joined by '+'
//       (e.g. "A+START" or "L+R+Z").
//
//   STICK <x> <y> <frames>
//       Hold the analog stick at (<x>,<y>) (signed, -80..80, N64 convention: +Y is UP) for
//       <frames> polls. No buttons are pressed.
//
//   INPUT <buttons|-> <x> <y> <frames>
//       Combined buttons + stick for <frames> polls. <buttons> uses the same syntax as PRESS,
//       or '-' for no buttons.
//
//   WAITMODE <mode> [timeout_frames]
//       Neutral input until the live game mode (gGameMode, masked with GET_MODE's 0x1F — see
//       decomp/include/fzx_game.h) equals <mode>, or until <timeout_frames> polls have elapsed
//       (default 3600, i.e. ~60s at the poll cadence). <mode> accepts decimal or 0x-prefixed
//       hex. On timeout the command logs "[autotest] WAITMODE timeout (mode=<want>
//       current=<cur>)" and playback CONTINUES with the next command — a stuck wait never
//       aborts the whole script.
//
//   LOG <text...>
//       Emits "[autotest] <text>" via gdx_port_logf the moment playback reaches this line
//       (i.e. at the scripted poll, not at parse time). <text> is the rest of the line verbatim.
//
//   KEY <scancode>
//       Injects a real SDL key tap (keydown+keyup) for the given SDL scancode (decimal or
//       0x-prefixed hex; SDL_SCANCODE_1 is 30) at the next host-frame event pump. Unlike the pad
//       commands this drives the keyboard path (Course Edit tool keys, menu rebind capture), not
//       the N64 controller. Instant: does not consume a poll — put a WAIT between repeated taps
//       of the same key.
//
//   SHOT <label>
//       Requests a one-shot framebuffer dump named "autotest/<label>.bmp" (see
//       gdx_request_frame_dump, n64_gfx_bridge.cpp) of the next frame the game presents.
//
//   QUIT
//       Requests a clean game shutdown via the same path a window-close (X button / Alt-F4)
//       takes (see gdx_request_quit, main.cpp).
//
//   (implicit) END at EOF
//       Once every command has run, control returns to physical input — gdx_input_script_override
//       becomes a permanent no-op for the rest of the process.
//
// PARSE ERRORS: a malformed line disables playback entirely (fail safe to physical input) and
// logs "[autotest] script parse error at line <n>: <line text>". Nothing partially loaded runs.
// ---------------------------------------------------------------------------------------------

// MSVC's Annex K functions (sscanf_s/getenv_s/fopen_s/strtok_s) are not in glibc, and sscanf_s
// takes an extra buffer-size argument per %s that plain sscanf does not, so no macro can satisfy
// both compilers. The field widths below (%127s, %31s) are what bounds those writes, and they are
// portable. The G-Diffuser target does not define this globally. Must precede every include.
#define _CRT_SECURE_NO_WARNINGS 1

#include "gdx_input_script.h"

#include "port_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// MSVC has no strtok_r; its strtok_s takes the identical (str, delim, context) arguments, so this
// one is a straight rename rather than a reimplementation.
#if defined(_MSC_VER)
#define gdx_strtok_r strtok_s
#else
#define gdx_strtok_r strtok_r
#endif

// decomp global (decomp/include/fzx_game.h: GAMEMODE_* enum + GET_MODE(m) = (m & 0x1F)). Declared
// locally as a plain int — same boundary idiom n64_gfx_bridge.cpp uses — so this TU has no decomp
// header dependency.
extern int gGameMode;

// main.cpp: closes the LUS window via the same path SDL_QUIT/WM_CLOSE take.
extern void gdx_request_quit(void);

// n64_gfx_bridge.cpp: arms a one-shot BMP dump of the next presented frame.
extern void gdx_request_frame_dump(const char* label);

// gdx_course_edit_mouse.cpp: pushes a real SDL keydown+keyup pair for the KEY command.
extern void gdx_course_edit_mouse_inject_key_tap(int scancode);

// N64 standard controller bitmask (decomp/include/controller.h BTN_*; identical layout to LUS's
// OSContPad.button). Redefined locally with the GDX_ prefix so this TU stays decomp-header-free;
// the values are fixed by the N64 SI protocol and never change.
#define GDX_BTN_A 0x8000
#define GDX_BTN_B 0x4000
#define GDX_BTN_Z 0x2000
#define GDX_BTN_START 0x1000
#define GDX_BTN_UP 0x0800
#define GDX_BTN_DOWN 0x0400
#define GDX_BTN_LEFT 0x0200
#define GDX_BTN_RIGHT 0x0100
#define GDX_BTN_L 0x0020
#define GDX_BTN_R 0x0010
#define GDX_BTN_CUP 0x0008
#define GDX_BTN_CDOWN 0x0004
#define GDX_BTN_CLEFT 0x0002
#define GDX_BTN_CRIGHT 0x0001

#define GDX_SCRIPT_TEXT_CAP 128
#define GDX_SCRIPT_DEFAULT_WAITMODE_TIMEOUT 3600

typedef enum GdxScriptOp {
    GDX_SCRIPT_OP_HOLD,     // WAIT / PRESS / STICK / INPUT all reduce to "hold this pad state N polls"
    GDX_SCRIPT_OP_WAITMODE,
    GDX_SCRIPT_OP_LOG,
    GDX_SCRIPT_OP_KEY,
    GDX_SCRIPT_OP_SHOT,
    GDX_SCRIPT_OP_QUIT
} GdxScriptOp;

typedef struct GdxScriptCmd {
    GdxScriptOp op;
    unsigned short buttons;              // HOLD
    signed char stickX, stickY;          // HOLD
    int frames;                          // HOLD: hold duration (polls). WAITMODE: timeout (polls).
    int mode;                            // WAITMODE: target GET_MODE value. KEY: SDL scancode.
    char text[GDX_SCRIPT_TEXT_CAP];      // LOG: message. SHOT: label.
} GdxScriptCmd;

// -------------------------------------------------------------------------------------------
// Parsing helpers
// -------------------------------------------------------------------------------------------

static int gdx_ci_streq(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

// Returns 1 on success, 0 if any name is unrecognized. `tok` is modified (strtok-style split).
static int gdx_parse_buttons(char* tok, unsigned short* outButtons) {
    unsigned short bits = 0;
    char* saveptr = NULL;
    char* piece = gdx_strtok_r(tok, "+", &saveptr);
    if (piece == NULL) {
        return 0;
    }
    while (piece != NULL) {
        if (gdx_ci_streq(piece, "A")) bits |= GDX_BTN_A;
        else if (gdx_ci_streq(piece, "B")) bits |= GDX_BTN_B;
        else if (gdx_ci_streq(piece, "Z")) bits |= GDX_BTN_Z;
        else if (gdx_ci_streq(piece, "START")) bits |= GDX_BTN_START;
        else if (gdx_ci_streq(piece, "L")) bits |= GDX_BTN_L;
        else if (gdx_ci_streq(piece, "R")) bits |= GDX_BTN_R;
        else if (gdx_ci_streq(piece, "UP")) bits |= GDX_BTN_UP;
        else if (gdx_ci_streq(piece, "DOWN")) bits |= GDX_BTN_DOWN;
        else if (gdx_ci_streq(piece, "LEFT")) bits |= GDX_BTN_LEFT;
        else if (gdx_ci_streq(piece, "RIGHT")) bits |= GDX_BTN_RIGHT;
        else if (gdx_ci_streq(piece, "CUP")) bits |= GDX_BTN_CUP;
        else if (gdx_ci_streq(piece, "CDOWN")) bits |= GDX_BTN_CDOWN;
        else if (gdx_ci_streq(piece, "CLEFT")) bits |= GDX_BTN_CLEFT;
        else if (gdx_ci_streq(piece, "CRIGHT")) bits |= GDX_BTN_CRIGHT;
        else return 0;
        piece = gdx_strtok_r(NULL, "+", &saveptr);
    }
    *outButtons = bits;
    return 1;
}

static void gdx_rstrip(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
}

// Returns a pointer into `s`, not a copy.
static char* gdx_lskip(char* s) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

// -------------------------------------------------------------------------------------------
// Script state (module-private)
// -------------------------------------------------------------------------------------------

static GdxScriptCmd* s_cmds = NULL;
static int s_cmdCount = 0;
static int s_cmdCap = 0;

static int s_state = -1; // -1 = unread, 0 = disabled (unset / open failed / parse error), 1 = active
static int s_ip = 0;
static int s_counter = -1;    // HOLD: polls remaining (counts down). WAITMODE: polls elapsed (counts up). -1 = not started.
static int s_loggedComplete = 0;

static int gdx_script_push(GdxScriptOp op, unsigned short buttons, signed char stickX, signed char stickY,
                           int frames, int mode, const char* text) {
    if (s_cmdCount == s_cmdCap) {
        int newCap = (s_cmdCap == 0) ? 64 : (s_cmdCap * 2);
        GdxScriptCmd* grown = (GdxScriptCmd*) realloc(s_cmds, (size_t) newCap * sizeof(GdxScriptCmd));
        if (grown == NULL) {
            return 0;
        }
        s_cmds = grown;
        s_cmdCap = newCap;
    }
    {
        GdxScriptCmd* cmd = &s_cmds[s_cmdCount++];
        cmd->op = op;
        cmd->buttons = buttons;
        cmd->stickX = stickX;
        cmd->stickY = stickY;
        cmd->frames = frames;
        cmd->mode = mode;
        cmd->text[0] = '\0';
        if (text != NULL) {
            size_t n = strlen(text);
            if (n >= sizeof(cmd->text)) {
                n = sizeof(cmd->text) - 1;
            }
            memcpy(cmd->text, text, n);
            cmd->text[n] = '\0';
        }
    }
    return 1;
}

// `line` is already trimmed, non-blank and non-comment. Returns 0 on a malformed line; the caller
// aborts the whole load on failure.
static int gdx_script_parse_line(char* line) {
    char kw[16];
    size_t kwLen;
    char* rest;

    // Manual keyword extraction rather than sscanf's "%n": MSVC's secure CRT restricts %n by
    // default, so this avoids relying on scanf-side %n support at all.
    kwLen = strcspn(line, " \t");
    if (kwLen == 0 || kwLen >= sizeof(kw)) {
        return 0;
    }
    memcpy(kw, line, kwLen);
    kw[kwLen] = '\0';
    rest = gdx_lskip(line + kwLen);

    if (gdx_ci_streq(kw, "WAIT")) {
        int frames;
        if (sscanf(rest, "%d", &frames) != 1) {
            return 0;
        }
        return gdx_script_push(GDX_SCRIPT_OP_HOLD, 0, 0, 0, frames, 0, NULL);
    }
    if (gdx_ci_streq(kw, "PRESS")) {
        char btnTok[128];
        int frames;
        unsigned short buttons;
        if (sscanf(rest, "%127s %d", btnTok, &frames) != 2) {
            return 0;
        }
        if (!gdx_parse_buttons(btnTok, &buttons)) {
            return 0;
        }
        return gdx_script_push(GDX_SCRIPT_OP_HOLD, buttons, 0, 0, frames, 0, NULL);
    }
    if (gdx_ci_streq(kw, "STICK")) {
        int x, y, frames;
        if (sscanf(rest, "%d %d %d", &x, &y, &frames) != 3) {
            return 0;
        }
        return gdx_script_push(GDX_SCRIPT_OP_HOLD, 0, (signed char) x, (signed char) y, frames, 0, NULL);
    }
    if (gdx_ci_streq(kw, "INPUT")) {
        char btnTok[128];
        int x, y, frames;
        unsigned short buttons = 0;
        if (sscanf(rest, "%127s %d %d %d", btnTok, &x, &y, &frames) != 4) {
            return 0;
        }
        if (strcmp(btnTok, "-") != 0 && !gdx_parse_buttons(btnTok, &buttons)) {
            return 0;
        }
        return gdx_script_push(GDX_SCRIPT_OP_HOLD, buttons, (signed char) x, (signed char) y, frames, 0, NULL);
    }
    if (gdx_ci_streq(kw, "WAITMODE")) {
        char modeTok[32];
        int timeout = GDX_SCRIPT_DEFAULT_WAITMODE_TIMEOUT;
        int n = sscanf(rest, "%31s %d", modeTok, &timeout);
        char* endptr = NULL;
        long mode;
        if (n < 1) {
            return 0;
        }
        mode = strtol(modeTok, &endptr, 0); // accepts decimal or 0x-prefixed hex
        if (endptr == modeTok) {
            return 0;
        }
        return gdx_script_push(GDX_SCRIPT_OP_WAITMODE, 0, 0, 0, timeout, (int) mode, NULL);
    }
    if (gdx_ci_streq(kw, "LOG")) {
        return gdx_script_push(GDX_SCRIPT_OP_LOG, 0, 0, 0, 0, 0, rest);
    }
    if (gdx_ci_streq(kw, "KEY")) {
        char scTok[32];
        char* endptr = NULL;
        long scancode;
        if (sscanf(rest, "%31s", scTok) != 1) {
            return 0;
        }
        scancode = strtol(scTok, &endptr, 0); // accepts decimal or 0x-prefixed hex
        if (endptr == scTok || scancode <= 0 || scancode > 0x7FFFFFFF) {
            return 0;
        }
        return gdx_script_push(GDX_SCRIPT_OP_KEY, 0, 0, 0, 0, (int) scancode, NULL);
    }
    if (gdx_ci_streq(kw, "SHOT")) {
        char label[128];
        if (sscanf(rest, "%127s", label) != 1) {
            return 0;
        }
        return gdx_script_push(GDX_SCRIPT_OP_SHOT, 0, 0, 0, 0, 0, label);
    }
    if (gdx_ci_streq(kw, "QUIT")) {
        return gdx_script_push(GDX_SCRIPT_OP_QUIT, 0, 0, 0, 0, 0, NULL);
    }
    return 0; // unrecognized keyword
}

// Called exactly once, from the first gdx_input_script_override().
static void gdx_script_load(void) {
    const char* path;
    FILE* f = NULL;
    int lineNo = 0;
    char line[512];

    s_state = 0; // only flipped to 1 on a fully clean parse

    path = getenv("GDX_INPUT_SCRIPT");
    if (path == NULL || path[0] == '\0') {
        return;
    }

    f = fopen(path, "r");
    if (f == NULL) {
        gdx_port_logf("[autotest] failed to open script: %s\n", path);
        return;
    }

    while (fgets(line, (int) sizeof(line), f) != NULL) {
        char* trimmed;
        lineNo++;
        gdx_rstrip(line);
        trimmed = gdx_lskip(line);
        if (*trimmed == '\0' || *trimmed == '#') {
            continue;
        }
        if (!gdx_script_parse_line(trimmed)) {
            gdx_port_logf("[autotest] script parse error at line %d: %s\n", lineNo, trimmed);
            fclose(f);
            free(s_cmds);
            s_cmds = NULL;
            s_cmdCount = 0;
            s_cmdCap = 0;
            s_state = 0; // fail safe: never run a partially loaded script
            return;
        }
    }
    fclose(f);

    s_state = 1;
    s_ip = 0;
    s_counter = -1;
    gdx_port_logf("[autotest] script loaded: %s (%d commands)\n", path, s_cmdCount);
}

// -------------------------------------------------------------------------------------------
// Playback
// -------------------------------------------------------------------------------------------

void gdx_input_script_override(GdxInputPad* pad) {
    if (s_state == -1) {
        gdx_script_load();
    }
    if (s_state != 1 || pad == NULL) {
        return; // disabled, or finished after EOF — control stays with physical input
    }

    // Exactly one poll is consumed per call. Instant commands (LOG/SHOT/QUIT, and a WAITMODE whose
    // condition already matches) do not consume one on their own, so the loop keeps advancing
    // `s_ip` within THIS call until something claims the poll or the script ends.
    for (;;) {
        GdxScriptCmd* cmd;

        if (s_ip >= s_cmdCount) {
            if (!s_loggedComplete) {
                s_loggedComplete = 1;
                gdx_port_logf("[autotest] script complete (%d commands)\n", s_cmdCount);
            }
            s_state = 2; // finished; every later call returns immediately above
            return;
        }

        cmd = &s_cmds[s_ip];
        switch (cmd->op) {
            case GDX_SCRIPT_OP_HOLD: {
                if (cmd->frames <= 0) {
                    s_ip++;
                    s_counter = -1;
                    continue; // zero-length hold: skip without consuming a poll
                }
                if (s_counter < 0) {
                    s_counter = cmd->frames;
                }
                pad->buttons = cmd->buttons;
                pad->stickX = cmd->stickX;
                pad->stickY = cmd->stickY;
                s_counter--;
                if (s_counter <= 0) {
                    s_ip++;
                    s_counter = -1;
                }
                return; // this poll consumed
            }
            case GDX_SCRIPT_OP_WAITMODE: {
                int curMode = gGameMode & 0x1F; // GET_MODE (decomp/include/fzx_game.h)
                pad->buttons = 0;
                pad->stickX = 0;
                pad->stickY = 0;
                if (curMode == cmd->mode) {
                    s_ip++;
                    s_counter = -1;
                    continue; // condition already true: instant, does not consume this poll
                }
                if (s_counter < 0) {
                    s_counter = 0;
                }
                s_counter++;
                {
                    int timeout = (cmd->frames > 0) ? cmd->frames : GDX_SCRIPT_DEFAULT_WAITMODE_TIMEOUT;
                    if (s_counter >= timeout) {
                        gdx_port_logf("[autotest] WAITMODE timeout (mode=%d current=%d)\n", cmd->mode, curMode);
                        s_ip++;
                        s_counter = -1;
                        continue; // give up on this wait, move on to the next command
                    }
                }
                return; // still waiting: this poll consumed with neutral input
            }
            case GDX_SCRIPT_OP_LOG: {
                gdx_port_logf("[autotest] %s\n", cmd->text);
                s_ip++;
                continue;
            }
            case GDX_SCRIPT_OP_KEY: {
                gdx_port_logf("[autotest] KEY scancode=%d\n", cmd->mode);
                gdx_course_edit_mouse_inject_key_tap(cmd->mode);
                s_ip++;
                continue;
            }
            case GDX_SCRIPT_OP_SHOT: {
                gdx_port_logf("[autotest] SHOT %s\n", cmd->text);
                gdx_request_frame_dump(cmd->text);
                s_ip++;
                continue;
            }
            case GDX_SCRIPT_OP_QUIT: {
                gdx_port_logf("[autotest] QUIT requested\n");
                gdx_request_quit();
                s_ip++;
                continue;
            }
            default: {
                s_ip++;
                continue;
            }
        }
    }
}
