// G-Diffuser: input action layer (3A). Per-context N64 button remapping for the PORT.
//
// Two layers decide what a press does. The Input Editor (LUS ControlDeck) maps each physical
// control to one N64 bit. This layer then decides what each N64 bit does in three contexts:
//   RACE         on-track racing, not paused (and a Course Edit test drive)
//   COURSE_EDIT  the editor itself, not a test drive
//   MENU         everything else, including the pause menu
//
// Each context has a table. For every OUTPUT N64 bit, the table holds a mask of the INPUT N64
// bits that trigger it. An output is set when any of its input bits is held. The default table
// is identity (each output is fed only by its own bit), so nothing changes until a table is edited
// on the F1 Controls > Actions page.
//
// Dev overrides (gdx-autoinput.txt, GDX_INPUT_SCRIPT) and the Course Edit mouse buttons run after
// this layer, so they stay raw N64 bits and are never remapped.
//
// LATCHING. A held input keeps the context it was pressed in until it is released. The press edge
// latches the frame context per input bit and per port. A context switch while a button is held
// therefore produces no fake press and no fake release.
//
// PERSISTENCE. One integer CVar per (context, output) holds the input mask:
//   gEnhancements.Input.Actions.<Context>.<Output>   e.g. gEnhancements.Input.Actions.Race.Z
// The masks are cached. The CVars are re-read only after gdx_input_actions_mark_dirty() (the UI
// or a config reload), on the first begin_frame, and on the first get_mask.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GdxInputContext {
    GDX_INPUT_CONTEXT_MENU = 0,
    GDX_INPUT_CONTEXT_RACE,
    GDX_INPUT_CONTEXT_COURSE_EDIT,
    GDX_INPUT_CONTEXT_COUNT
} GdxInputContext;

// Output order is the UI row order. Output i's N64 bit is also input column i in the UI.
typedef enum GdxInputOutput {
    GDX_INPUT_OUT_A = 0,
    GDX_INPUT_OUT_B,
    GDX_INPUT_OUT_Z,
    GDX_INPUT_OUT_START,
    GDX_INPUT_OUT_DUP,
    GDX_INPUT_OUT_DDOWN,
    GDX_INPUT_OUT_DLEFT,
    GDX_INPUT_OUT_DRIGHT,
    GDX_INPUT_OUT_L,
    GDX_INPUT_OUT_R,
    GDX_INPUT_OUT_CUP,
    GDX_INPUT_OUT_CDOWN,
    GDX_INPUT_OUT_CLEFT,
    GDX_INPUT_OUT_CRIGHT,
    GDX_INPUT_OUT_COUNT
} GdxInputOutput;

// Gamepad X and Y (4A). The LUS deck maps them to the two button bits the N64 never used (BTN_X and
// BTN_Y in libultraship's controller.h), so they can trigger actions here. They are inputs only:
// gdx_input_actions_apply() outputs real N64 bits, so the game never sees them.
#define GDX_INPUT_BTN_X 0x0040
#define GDX_INPUT_BTN_Y 0x0080

// Input columns in UI order: input i < GDX_INPUT_OUT_COUNT is output i's own N64 bit, then X and Y.
#define GDX_INPUT_IN_COUNT (GDX_INPUT_OUT_COUNT + 2)

// Called once per gdx_controller_poll(), before any gdx_input_actions_apply() call. Refreshes the
// cached tables when they are dirty and sets the context that presses on this frame latch to.
void gdx_input_actions_begin_frame(int context);

// Maps one port's raw N64 button bits (the LUS pad read) to the action outputs for this frame.
// Input bits outside CONT_MASK, X and Y are dropped. The result holds CONT_MASK bits only, so an
// identity table returns the input minus X and Y.
unsigned short gdx_input_actions_apply(int port, unsigned short rawButtons);

// Input mask (N64 bits) that triggers `output` in `context`. Zero means nothing triggers it.
unsigned short gdx_input_actions_get_mask(int context, int output);

// Stores the input mask for (context, output) in its CVar and in the cache.
void gdx_input_actions_set_mask(int context, int output, unsigned short mask);

// Restores every output of one context to identity: each output fed only by its own N64 bit.
void gdx_input_actions_reset_context(int context);

// The N64 bit of an output. This is also the identity input for that output.
unsigned short gdx_input_actions_output_bit(int output);

// The input bit of UI column `input` (0..GDX_INPUT_IN_COUNT-1). Zero when out of range.
unsigned short gdx_input_actions_input_bit(int input);

// Call after CVars change outside gdx_input_actions_set_mask (config load, console edit).
void gdx_input_actions_mark_dirty(void);

#ifdef __cplusplus
}
#endif
