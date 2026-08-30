/* port/gdx_hackmods.h -- ROM-hack mod archives (W3, slice A: loading and isolation).
 *
 * A hack mod is an .o2r under `mods/~romhacks/` that replaces game content wholesale, as opposed
 * to the cosmetic overrides in `mods/`. Two properties separate it from an ordinary pack and
 * drive everything here:
 *
 *   Exactly one may be active. A hack redefines the game; two at once is not a state anyone
 *   wants, so instead of mounting several and repairing the conflict at boot, the selection is a
 *   SINGLE name in one CVar. A conflict is unrepresentable rather than detected.
 *
 *   Progress must not bleed into the stock save. Cup completion is keyed by cup, difficulty and
 *   character rather than by course identity (`decomp/include/fzx_save.h`, SaveDDCups), so
 *   clearing a hack's JACK cup would otherwise mark it cleared in the stock file. Each hack gets
 *   its own `saves/fzerox-hack-<name>.sav`; nothing is ever deleted, the stock save is simply
 *   left alone.
 *
 * The active hack is LATCHED at boot. Selecting a different one changes the CVar but does not
 * take effect until restart, because the mounted archive set and the save file are both fixed by
 * then. The window says so.
 *
 * Scope of this slice: discovery, selection, mounting and save isolation. Producing a hack .o2r
 * from a patched ROM is a separate slice; see W3 in
 * devdocs/1.2.0-import-and-limits-scope.md (2026-08-27).
 *
 * CVars:
 *   gEnhancements.Hacks.Active     (string, ""): basename of the enabled hack, "" for none.
 *   gEnhancements.Hacks.WindowOpen (int, 0):     ROM Hacks window visibility.
 */
#ifndef GDX_HACKMODS_H
#define GDX_HACKMODS_H

/* Reserved subfolder of mods/. The leading '~' keeps it sorting away from ordinary packs and out
 * of the texture-pack scan, which only looks at regular files directly inside mods/. */
#define GDX_HACKMODS_DIR "~romhacks"

#define GDX_HACKMOD_NAME_MAX 64
#define GDX_HACKMOD_SAVE_MAX 128
#define GDX_HACKMOD_SAVE_STOCK "fzerox.sav"
#define GDX_HACKMOD_SAVE_PREFIX "fzerox-hack-"

#ifdef __cplusplus
extern "C" {
#endif

/* Reduces an arbitrary archive basename to something safe to embed in a filename: keeps
 * [A-Za-z0-9._-], turns every other byte into '-', collapses runs, trims leading/trailing '-' and
 * '.', and caps the result at GDX_HACKMOD_NAME_MAX-1. Rejects names that reduce to nothing, and
 * rejects "." and ".." outright so no selection can ever climb out of saves/.
 * Returns 1 on success, 0 when nothing usable remains (out is then an empty string). */
int gdx_hackmod_sanitize_name(const char* in, char* out, unsigned long outCap);

/* Save-file basename for a hack: GDX_HACKMOD_SAVE_STOCK when hackName is NULL or empty, otherwise
 * "fzerox-hack-<sanitised>.sav". Returns 1 on success, 0 if the name is unusable or the buffer is
 * too small; out is set to the stock name on failure so a caller that ignores the result still
 * gets a valid, safe path rather than a broken one. */
int gdx_hackmod_save_basename(const char* hackName, char* out, unsigned long outCap);

/* Save basename for the hack latched at boot. Safe to call before the latch is set (returns the
 * stock name). This is what sram_buffer.c uses to pick its file. */
const char* gdx_hackmod_active_save_basename(void);

#ifdef __cplusplus
} /* extern "C" */

#include <string>
#include <vector>

typedef struct GdxHackModEntry {
    std::string basename; /* archive filename without the .o2r extension */
    std::string path;     /* absolute path to the archive */
    bool active;          /* matches the current selection */
} GdxHackModEntry;

/* Points the module at the resolved `<root>/mods/~romhacks` directory. main.cpp calls this once
 * during archive discovery, using the same root it picked for the base archives. */
void GdxHackModsSetDirectory(const std::string& dir);
const std::string& GdxHackModsDirectory();

/* Directory the executable lives in, which is where the packaged gdx-extract and decomp-recipes
 * ship. The hack builder needs it and it is NOT the data directory in an installed layout, so
 * main.cpp records it once from the resolved first-boot layout. Empty until then, which the UI
 * reads as "building is unavailable in this session". */
void GdxHackModsSetProgramDir(const std::string& dir);
const std::string& GdxHackModsProgramDir();

/* Every .o2r directly inside the hack directory, sorted by basename. Empty when the directory was
 * never set or does not exist. */
std::vector<GdxHackModEntry> GdxHackModsScan();

/* The selection CVar. Setting it does not affect the running session; see the latch note above. */
std::string GdxHackModsSelectedName();
void GdxHackModsSetSelected(const std::string& basename);

/* Resolves the selection to an archive path and LATCHES it for this boot, which also fixes the
 * save file. Returns "" when nothing is selected or the selected archive is missing. main.cpp
 * calls this exactly once, while building the archive list. */
std::string GdxHackModsLatchActivePath();

/* The name latched by GdxHackModsLatchActivePath, or "" when none. */
std::string GdxHackModsActiveName();

#endif /* __cplusplus */

#endif /* GDX_HACKMODS_H */
