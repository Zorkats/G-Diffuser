// G-Diffuser: controller profiles (4A). Presets that write the Race table of the 3A action layer
// (gdx_input_actions.h). Each profile assumes the default LUS gamepad mapping, which decides which
// physical input produces which N64 bit. The profile only chooses the 3A masks on top of that.
// gdx_input_profiles.c has the derivation and each profile's limitations.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Number of profiles, in UI order. The first one is the N64 identity profile.
int gdx_input_profiles_count(void);

// Display name and one-line description of a profile. NULL when index is out of range.
const char* gdx_input_profiles_name(int index);
const char* gdx_input_profiles_description(int index);

// What the default mapping cannot express for this profile. "None." when nothing is lost.
const char* gdx_input_profiles_limitations(int index);

// Writes the profile's Race masks through gdx_input_actions_set_mask and records the profile index.
// Menu and Course Edit are not touched: no official row needs them. The caller saves the CVars.
void gdx_input_profiles_apply(int index);

// Index of the profile whose Race table equals the current table, or -1 ("Custom"). When two
// profiles have the same table, the one applied last wins, so the combo keeps the user's choice.
int gdx_input_profiles_match(void);

#ifdef __cplusplus
}
#endif
