package com.gdiffuser.android

/**
 * Indices into the virtual SDL game controller created by the native bridge, named for the N64
 * button each one ends up driving.
 *
 * The indices themselves follow SDL's `SDL_GameControllerButton` / `SDL_GameControllerAxis`
 * ordering, because a virtual joystick attached as `SDL_JOYSTICK_TYPE_GAMECONTROLLER` is read
 * back through that layout. The N64 names come from libultraship's default gamepad mapping,
 * which G-Diffuser does not override:
 *
 *     N64 A      -> SDL A                        N64 Z  -> left trigger
 *     N64 B      -> SDL B                        N64 R  -> right trigger
 *     N64 L      -> SDL left shoulder            N64 C  -> right stick
 *     N64 Start  -> SDL Start                    N64 D  -> SDL d-pad
 *     N64 stick  -> SDL left stick
 *
 * A negative button id addresses the axis with that (positive) index instead, which is how Z and
 * R are driven from plain on/off buttons in the overlay.
 *
 * These are only the defaults: everything here arrives as an ordinary SDL pad, so it can be
 * remapped in the engine's input editor like any physical one.
 */
object ControllerButtons {
    /** N64 A — accelerate. */
    const val BUTTON_A = 0

    /** N64 B — brake. */
    const val BUTTON_B = 1

    /** N64 L — left side attack. */
    const val BUTTON_L = 9

    /** N64 Start — pause. */
    const val BUTTON_START = 6

    const val BUTTON_DPAD_UP = 11
    const val BUTTON_DPAD_DOWN = 12
    const val BUTTON_DPAD_LEFT = 13
    const val BUTTON_DPAD_RIGHT = 14

    /** N64 analog stick — steering, and air control off a ramp. */
    const val AXIS_STICK_X = 0
    const val AXIS_STICK_Y = 1

    /** N64 C buttons, which the default mapping reads off the right stick — camera views. */
    const val AXIS_C_X = 2
    const val AXIS_C_Y = 3

    /** N64 Z — boost, held on the left trigger axis. */
    const val AXIS_Z = 4

    /** N64 R — right side attack, held on the right trigger axis. */
    const val AXIS_R = 5

    /** The trigger axes addressed as on/off buttons — see the note above. */
    const val BUTTON_Z = -AXIS_Z
    const val BUTTON_R = -AXIS_R
}
