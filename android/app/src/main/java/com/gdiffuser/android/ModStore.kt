package com.gdiffuser.android

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

/**
 * The mods folder, as the engine sees it.
 *
 * This manages mod *files* — importing, exporting, deleting. It deliberately does not turn mods
 * on or off, or reorder them: which are enabled, and in what order, is not a property of the
 * folder at all. The engine keeps both in cvars — `gEnhancements.Workshop.PackOrder` and
 * `gEnhancements.Workshop.DisabledPacks`, comma-separated (see port/gdx_workshop.cpp) — and its
 * own Workshop menu owns them, along with the conflict rules that go with them. This class only
 * reads those cvars out of gdiffuser.cfg.json so it can show which mods are on and in what order.
 *
 * Nothing here renames a file, because the filename is one of the two keys those cvars match on
 * — renaming a mod silently detaches it from its own enabled state.
 *
 * A mod is an `.o2r` archive. Unlike some other libultraship ports, G-Diffuser mounts only .o2r
 * files from this folder (main.cpp's archive scan filters on the extension), so a .zip or a
 * loose folder dropped here is ignored rather than loaded.
 */
object ModStore {

    private const val TAG = "ModStore"

    /** Matches the separator gdx_workshop.cpp's splitCommaList expects. */
    private const val SEPARATOR = ","
    private const val CONFIG_NAME = "gdiffuser.cfg.json"

    data class Mod(
        val file: File,
        /** The filename, which is one of the keys the engine's cvars refer to. */
        val name: String,
        val enabled: Boolean
    ) {
        val sizeLabel: String
            get() {
                val bytes = file.length()
                return when {
                    bytes >= 1 shl 20 -> "%.1f MB".format(bytes / (1 shl 20).toDouble())
                    else -> "%.0f KB".format(bytes / 1024.0)
                }
            }
    }

    /**
     * Mods in mount order: the last entry wins where two packs touch the same resource.
     *
     * Order comes from `PackOrder` and enabled state from `DisabledPacks`, both read out of
     * gdiffuser.cfg.json — the same lists the in-game Workshop menu edits. Anything on disk the
     * cvars have not seen yet is appended, enabled, which is how the engine treats it too: the
     * disable list is opt-out, so an unknown pack loads.
     */
    fun list(context: Context): List<Mod> {
        val dir = GameAssets.modsDir(context)
        dir.mkdirs()

        val onDisk = (dir.listFiles() ?: emptyArray())
            .filter { it.isFile && it.name.endsWith(".o2r", ignoreCase = true) }
            .associateBy { it.name }

        val disabled = readCVarList(context, "DisabledPacks").map { it.lowercase() }.toSet()
        val ordered = LinkedHashMap<String, Mod>()

        for (name in readCVarList(context, "PackOrder")) {
            val file = onDisk[name] ?: onDisk.entries.firstOrNull { it.key.equals(name, true) }?.value ?: continue
            ordered[file.name] = Mod(file, file.name, enabled = !isDisabled(file.name, disabled))
        }
        for ((name, file) in onDisk.entries.sortedBy { it.key.lowercase() }) {
            if (name in ordered) continue
            ordered[name] = Mod(file, name, enabled = !isDisabled(name, disabled))
        }
        return ordered.values.toList()
    }

    /**
     * The Workshop "Enable texture packs" master switch (`gEnhancements.Workshop.TexturePacks`).
     * Off by default, and with it off main.cpp's archive scan mounts no pack at all, so an
     * imported pack silently does nothing until it is turned on. Read here only to say so; it is
     * left to the in-game menu to change, because the running engine owns the config file and
     * would write its own value back over an edit made from here.
     */
    fun texturePacksEnabled(context: Context): Boolean =
        readWorkshop(context)?.optInt("TexturePacks", 0)?.let { it != 0 } ?: false

    /**
     * The disable list is dual-key: a pack matches by its manifest id or by its filename, either
     * with or without the extension. Only the filename is knowable from here, so both spellings
     * of it are checked and an id-keyed entry is left to the in-game menu to reflect.
     */
    private fun isDisabled(name: String, disabled: Set<String>): Boolean =
        name.lowercase() in disabled || name.substringBeforeLast('.').lowercase() in disabled

    /** Reads a comma-separated Workshop cvar out of the engine's config file. */
    private fun readCVarList(context: Context, key: String): List<String> =
        readWorkshop(context)
            ?.optString(key)
            .orEmpty()
            .split(SEPARATOR)
            .map { it.trim() }
            .filter { it.isNotEmpty() }

    /** The `gEnhancements.Workshop` cvar object out of the engine's config file, if there is one. */
    private fun readWorkshop(context: Context): JSONObject? {
        val config = File(GameAssets.gameDir(context), CONFIG_NAME)
        if (!config.isFile) return null
        return try {
            JSONObject(config.readText())
                .optJSONObject("CVars")
                ?.optJSONObject("gEnhancements")
                ?.optJSONObject("Workshop")
        } catch (error: Exception) {
            Log.w(TAG, "Could not read $CONFIG_NAME", error)
            null
        }
    }

    fun delete(mod: Mod): String? =
        if (mod.file.delete()) null else "Could not delete ${mod.name}."

    /** Copies an `.o2r` picked through the document picker into the mods folder. */
    fun importArchive(context: Context, uri: Uri): String? {
        val displayName = queryDisplayName(context, uri) ?: return "Could not read the file's name."
        if (!displayName.endsWith(".o2r", ignoreCase = true)) {
            return "$displayName is not a mod. G-Diffuser loads .o2r packs."
        }

        val target = nextFreeFile(context, displayName)
        return try {
            context.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "The document provider did not open $displayName." }
                FileOutputStream(target).use { output -> input.copyTo(output, COPY_BUFFER) }
            }
            null
        } catch (error: Exception) {
            target.delete()
            Log.e(TAG, "Import of $displayName failed", error)
            "Could not import $displayName: ${error.message ?: error}"
        }
    }

    /** Writes a mod back out to a location the user picked. */
    fun export(context: Context, mod: Mod, destination: Uri): String? = try {
        context.contentResolver.openOutputStream(destination).use { output ->
            requireNotNull(output) { "The document provider did not open the destination." }
            mod.file.inputStream().use { input -> input.copyTo(output, COPY_BUFFER) }
        }
        null
    } catch (error: Exception) {
        Log.e(TAG, "Export of ${mod.name} failed", error)
        "Could not export ${mod.name}: ${error.message ?: error}"
    }

    private const val COPY_BUFFER = 1 shl 16

    private fun nextFreeFile(context: Context, name: String): File {
        val dir = GameAssets.modsDir(context)
        dir.mkdirs()

        // Imported under its own name: the filename is a key the Workshop cvars match on, so
        // decorating it would divorce the file from any order or disable state already recorded
        // for that name. A collision is the only reason to deviate.
        var candidate = File(dir, name)
        var attempt = 1
        while (candidate.exists()) {
            val stem = name.substringBeforeLast('.', name)
            candidate = File(dir, "${stem}_$attempt.o2r")
            attempt++
        }
        return candidate
    }

    private fun queryDisplayName(context: Context, uri: Uri): String? =
        context.contentResolver.query(uri, arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)
            ?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }
}
