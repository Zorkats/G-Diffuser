package com.gdiffuser.android

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream

/**
 * The game's saved data.
 *
 * G-Diffuser spreads this across two folders next to the archives, and both matter:
 *
 *   saves/   the cartridge SRAM image (fzerox.sav), the 64DD disk journal that holds every
 *            Expansion Kit save (<disk name>.gdd, plus its .bak), and the local achievement and
 *            playtime record (achievements.txt).
 *   ghosts/  the host-side ghost library — one .gdg per exported replay.
 *
 * The 64DD journal is the one to be careful with. On an Expansion Kit build it, not the SRAM
 * image, is where course edits, custom machines and most records live, and it is a copy-on-write
 * journal against the disk image the player imported: restoring one taken against a different
 * disk is what the port's own validation rejects at load time, falling back to the .bak and then
 * to the pristine image. So a backup of "the saves" that omits it is not a backup at all, which
 * is why [exportAll] takes both folders in one archive rather than offering them separately.
 *
 * Files are matched by extension rather than by name, so a file added upstream is picked up here
 * without changes.
 */
object SaveStore {

    private const val TAG = "SaveStore"
    private const val COPY_BUFFER = 1 shl 16

    const val BUNDLE_NAME = "gdiffuser-saves.zip"

    /** The two folders a backup covers, in the order they are listed. */
    private val FOLDERS = listOf("saves", "ghosts")

    data class Save(val file: File, val folder: String) {
        /** The path inside a backup archive, and the key a restore writes back to. */
        val relativePath: String get() = "$folder/${file.name}"

        val label: String
            get() = when {
                file.name.equals("fzerox.sav", true) -> "Cartridge save (SRAM)"
                file.name.equals("achievements.txt", true) -> "Achievements and playtime"
                file.name.endsWith(".gdd", true) -> "64DD disk save — ${file.name.removeSuffix(".gdd")}"
                file.name.endsWith(".gdd.bak", true) -> "64DD disk save, previous copy"
                file.name.endsWith(".gdg", true) -> "Ghost — ${file.name.removeSuffix(".gdg")}"
                else -> file.name
            }

        val sizeLabel: String
            get() {
                val bytes = file.length()
                return when {
                    bytes >= 1 shl 20 -> "%.1f MB".format(bytes / (1 shl 20).toDouble())
                    else -> "%.0f KB".format(bytes / 1024.0)
                }
            }
    }

    fun list(context: Context): List<Save> {
        val root = GameAssets.gameDir(context)
        return FOLDERS.flatMap { folder ->
            (File(root, folder).listFiles() ?: emptyArray())
                .filter { it.isFile }
                .sortedBy { it.name.lowercase() }
                .map { Save(it, folder) }
        }
    }

    fun delete(save: Save): String? =
        if (save.file.delete()) null else "Could not delete ${save.label}."

    fun export(context: Context, save: Save, destination: Uri): String? = runOrReport("export") {
        context.contentResolver.openOutputStream(destination).use { output ->
            requireNotNull(output) { "The document provider did not open the destination." }
            save.file.inputStream().use { input -> input.copyTo(output, COPY_BUFFER) }
        }
    }

    /** Everything from both folders in one archive, paths included so a restore can place them. */
    fun exportAll(context: Context, destination: Uri): String? = runOrReport("export") {
        val saves = list(context)
        check(saves.isNotEmpty()) { "There is nothing saved yet." }

        context.contentResolver.openOutputStream(destination).use { output ->
            requireNotNull(output) { "The document provider did not open the destination." }
            ZipOutputStream(output.buffered()).use { zip ->
                for (save in saves) {
                    zip.putNextEntry(ZipEntry(save.relativePath))
                    save.file.inputStream().use { input -> input.copyTo(zip, COPY_BUFFER) }
                    zip.closeEntry()
                }
            }
        }
    }

    /**
     * Restores a single file, or every file out of an archive written by [exportAll]. Existing
     * files of the same name are replaced.
     */
    fun import(context: Context, uri: Uri): String? {
        val displayName = queryDisplayName(context, uri) ?: return "Could not read the file's name."

        return when {
            displayName.endsWith(".zip", true) -> importBundle(context, uri)
            else -> {
                val folder = folderFor(displayName)
                    ?: return "$displayName is not a save. Restore a .sav, .gdd, .gdg or a .zip of them."
                runOrReport("import") {
                    val target = File(GameAssets.gameDir(context), folder).apply { mkdirs() }
                    context.contentResolver.openInputStream(uri).use { input ->
                        requireNotNull(input) { "The document provider did not open $displayName." }
                        FileOutputStream(File(target, displayName)).use { output ->
                            input.copyTo(output, COPY_BUFFER)
                        }
                    }
                }
            }
        }
    }

    /** Which folder a loose file belongs in, or null when it is not save data at all. */
    private fun folderFor(name: String): String? = when {
        name.endsWith(".gdg", true) -> "ghosts"
        name.endsWith(".sav", true) || name.endsWith(".gdd", true) ||
            name.endsWith(".gdd.bak", true) || name.equals("achievements.txt", true) -> "saves"
        else -> null
    }

    private fun importBundle(context: Context, uri: Uri): String? = runOrReport("import") {
        val root = GameAssets.gameDir(context)
        var restored = 0

        context.contentResolver.openInputStream(uri).use { input ->
            requireNotNull(input) { "The document provider did not open the archive." }
            ZipInputStream(input.buffered()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    // The leaf name only, placed by its own extension rather than by the path
                    // the archive claims: an archive from elsewhere could otherwise carry
                    // entries like ../../ pointing out of the game directory entirely.
                    val name = File(entry.name).name
                    val folder = if (entry.isDirectory) null else folderFor(name)
                    if (folder == null) {
                        zip.closeEntry()
                        continue
                    }

                    val target = File(root, folder).apply { mkdirs() }
                    FileOutputStream(File(target, name)).use { output -> zip.copyTo(output, COPY_BUFFER) }
                    zip.closeEntry()
                    restored++
                }
            }
        }

        check(restored > 0) { "That archive has no save files in it." }
    }

    private fun runOrReport(verb: String, work: () -> Unit): String? = try {
        work()
        null
    } catch (error: Exception) {
        Log.e(TAG, "Save $verb failed", error)
        "Could not $verb: ${error.message ?: error}"
    }

    private fun queryDisplayName(context: Context, uri: Uri): String? =
        context.contentResolver.query(uri, arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)
            ?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }
}
