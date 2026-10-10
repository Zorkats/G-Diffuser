package com.gdiffuser.android

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream

/**
 * File operations for the engine's reserved mods/~romhacks directory.
 *
 * Selection remains owned by the native ROM Hacks window because it latches the archive and its
 * isolated save name together at boot. This store only makes scoped-storage file operations
 * possible on Android, where the desktop "Open folder" workflow cannot reach Downloads or Drive.
 */
object HackStore {

    private const val TAG = "HackStore"
    private const val CONFIG_NAME = "gdiffuser.cfg.json"

    data class Archive(
        val file: File,
        val name: String,
        val selected: Boolean
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

    fun list(context: Context): List<Archive> {
        val dir = GameAssets.hackModsDir(context).apply { mkdirs() }
        val selected = selectedName(context)
        return (dir.listFiles() ?: emptyArray())
            .filter { it.isFile && it.name.endsWith(".o2r", ignoreCase = true) }
            .sortedBy { it.name.lowercase() }
            .map { file ->
                Archive(file, file.name, file.nameWithoutExtension.equals(selected, ignoreCase = true))
            }
    }

    fun delete(archive: Archive): String? =
        if (archive.file.delete()) null else "Could not delete ${archive.name}."

    /** Copies a prebuilt hack archive from a document provider into mods/~romhacks. */
    fun importArchive(context: Context, uri: Uri): String? {
        val providerName = queryDisplayName(context, uri) ?: return "Could not read the file's name."
        if (!providerName.endsWith(".o2r", ignoreCase = true)) {
            return "$providerName is not a ROM-hack archive. Choose an .o2r file."
        }

        // A document display name should already be one component, but do not let an unusual
        // provider turn it into a path below (or outside) the reserved directory.
        val safeName = providerName.replace(Regex("""[/\\]"""), "_")
        val target = nextFreeFile(context, safeName)
        return try {
            context.contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input) { "The document provider did not open $providerName." }
                FileOutputStream(target).use { output ->
                    input.copyTo(output, COPY_BUFFER)
                    output.fd.sync()
                }
            }
            null
        } catch (error: Exception) {
            target.delete()
            Log.e(TAG, "Import of $providerName failed", error)
            "Could not import $providerName: ${error.message ?: error}"
        }
    }

    fun export(context: Context, archive: Archive, destination: Uri): String? = try {
        context.contentResolver.openOutputStream(destination).use { output ->
            requireNotNull(output) { "The document provider did not open the destination." }
            archive.file.inputStream().use { input -> input.copyTo(output, COPY_BUFFER) }
        }
        null
    } catch (error: Exception) {
        Log.e(TAG, "Export of ${archive.name} failed", error)
        "Could not export ${archive.name}: ${error.message ?: error}"
    }

    private fun selectedName(context: Context): String {
        val config = File(GameAssets.gameDir(context), CONFIG_NAME)
        if (!config.isFile) return ""
        return try {
            JSONObject(config.readText())
                .optJSONObject("CVars")
                ?.optJSONObject("gEnhancements")
                ?.optJSONObject("Hacks")
                ?.optString("Active")
                .orEmpty()
        } catch (error: Exception) {
            Log.w(TAG, "Could not read the selected ROM hack from $CONFIG_NAME", error)
            ""
        }
    }

    private fun nextFreeFile(context: Context, name: String): File {
        val dir = GameAssets.hackModsDir(context).apply { mkdirs() }
        var candidate = File(dir, name)
        var attempt = 1
        while (candidate.exists()) {
            candidate = File(dir, "${name.substringBeforeLast('.', name)}_$attempt.o2r")
            attempt++
        }
        return candidate
    }

    private fun queryDisplayName(context: Context, uri: Uri): String? =
        context.contentResolver.query(
            uri,
            arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME),
            null,
            null,
            null
        )?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }

    private const val COPY_BUFFER = 1 shl 16
}
