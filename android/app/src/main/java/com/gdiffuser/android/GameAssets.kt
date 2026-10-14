package com.gdiffuser.android

import android.content.Context
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.zip.ZipInputStream

/**
 * Everything the game needs on disk, and how it gets there.
 *
 * libultraship resolves every runtime path on Android through
 * `SDL_AndroidGetExternalStoragePath()`, i.e. `getExternalFilesDir(null)`, and the port resolves
 * its own the same way (port/android/gdx_android_paths.h). That is also somewhere the user can
 * reach with a file manager, which is what makes dropping mods in possible, so everything lives
 * there:
 *
 *     Android/data/com.gdiffuser.android/files/
 *       gdiffuser.o2r              engine assets (Fast3D shaders + branding), from the APK
 *       decomp-recipes/            what Torch reads to extract the ROM, from the APK
 *       shaders/, fonts/           post-processing presets and the GUI fonts, from the APK
 *       baserom.us.rev0.z64        the user's cartridge ROM, imported by the launcher
 *       baserom.translated.ek.ndd  the user's Expansion Kit disk image
 *       N64DDIPLROM.n64            the user's 64DD IPL ROM
 *       fzerox.o2r                 built on-device from the ROM
 *       n64ddipl.o2r               built on-device from the IPL
 *       fzerox-disk.o2r            built on-device from the disk image
 *       mods/                      texture and content packs (.o2r)
 *       saves/                     save files, ghosts and the disk journal
 *
 * G-Diffuser needs all THREE inputs, not just a ROM: the port is built Expansion-Kit-only, and
 * the game crashes or degrades without the 64DD disk and the drive's IPL. That is why the
 * launcher asks for three files where a cartridge-only port asks for one.
 *
 * The unpacking happens here rather than inside the engine because Torch needs the recipes on
 * disk before the game process starts, not after. The zip itself is assembled by
 * android/app/build.gradle.kts.
 *
 * Nothing here re-implements the port's acceptance rules. Which dumps are valid, what counts as
 * a finished setup, and when an archive has to be rebuilt are all decided by the same C++ the
 * game itself uses, reached through the JNI functions at the bottom of this file — so what the
 * launcher accepts and what the game accepts on the next launch cannot drift apart.
 */
object GameAssets {

    private const val TAG = "GameAssets"

    private const val GAMEDATA_ZIP = "gamedata.zip"
    private const val GAMEDATA_VERSION = "gamedata.version"
    private const val CONTROLLER_DB = "gamecontrollerdb.txt"
    private const val STAMP = ".gamedata.version"

    /** The three files the player has to supply, in the order the launcher asks for them. */
    enum class Input(val label: String, val what: String) {
        Rom("Cartridge ROM", "Your F-Zero X cartridge dump (US rev0, .z64)"),
        Disk("Expansion Kit disk", "The translated F-Zero X Expansion Kit disk image (.ndd)"),
        Ipl("64DD IPL ROM", "The 64DD drive firmware dump (N64DDIPLROM.n64)");

        /** Where this input is installed. Named by the port, never guessed at here. */
        fun canonicalName(): String = canonicalNames()[ordinal]
    }

    /** How a picked file was judged. [message] is shown to the user in every case. */
    sealed interface Verdict {
        data class Ok(val message: String) : Verdict
        data class Warn(val message: String) : Verdict
        data class Rejected(val message: String) : Verdict
    }

    fun gameDir(context: Context): File =
        (context.getExternalFilesDir(null) ?: context.filesDir).also { it.mkdirs() }

    fun modsDir(context: Context) = File(gameDir(context), "mods")

    /** ROM-hack archives are deliberately kept out of the ordinary texture-pack scan. */
    fun hackModsDir(context: Context) = File(modsDir(context), "~romhacks")

    fun savesDir(context: Context) = File(gameDir(context), "saves")

    fun fileFor(context: Context, input: Input) = File(gameDir(context), input.canonicalName())

    /** True when every input has been imported. Says nothing about whether it was accepted. */
    fun allInputsPresent(context: Context) = Input.entries.all { fileFor(context, it).length() > 0 }

    /** True when the game can boot straight away: all three archives built and hash-checked. */
    fun archivesReady(context: Context) = nativeArchivesReady(gameDir(context).absolutePath)

    /**
     * Unpacks the APK-bundled engine archive, recipes, shaders and fonts into [gameDir].
     *
     * Re-runs whenever the packaged zip changes, so an app update ships new recipes without the
     * user clearing data. Returns an error message, or null when the directory is ready.
     */
    fun stageBundledAssets(context: Context): String? {
        val target = gameDir(context)
        val stamp = File(target, STAMP)

        // These are user-owned directories, not bundled payload. Ensure them even on the warm
        // stamp path so an update which introduces ROM-hack support does not require clearing app
        // data before the engine can discover mods/~romhacks.
        val mods = modsDir(context).apply { mkdirs() }
        hackModsDir(context).mkdirs()

        return try {
            val expected = context.assets.open(GAMEDATA_VERSION).use { it.readBytes().decodeToString() }
            if (stamp.takeIf { it.isFile }?.readText() == expected) {
                return null
            }

            context.assets.open(GAMEDATA_ZIP).use { input ->
                ZipInputStream(input.buffered()).use { zip ->
                    while (true) {
                        val entry = zip.nextEntry ?: break
                        val destination = File(target, entry.name).canonicalFile

                        // An entry named ../../something would otherwise write outside the game
                        // directory.
                        if (!destination.path.startsWith(target.canonicalPath + File.separator)) {
                            Log.w(TAG, "Skipping suspicious zip entry ${entry.name}")
                            zip.closeEntry()
                            continue
                        }

                        if (entry.isDirectory) {
                            destination.mkdirs()
                        } else {
                            destination.parentFile?.mkdirs()
                            FileOutputStream(destination).use { output -> zip.copyTo(output, COPY_BUFFER) }
                        }
                        zip.closeEntry()
                    }
                }
            }

            // Users are meant to edit this one, so only seed it once.
            val controllerDb = File(target, CONTROLLER_DB)
            if (!controllerDb.isFile) {
                copyAsset(context, CONTROLLER_DB, controllerDb)
            }

            // Only a hint for the player, and nothing reads it back, so it must not fail an
            // otherwise complete staging.
            val placeholder = File(mods, "place_mods_here.txt")
            if (!placeholder.isFile && mods.listFiles()?.none { it.isFile } != false) {
                try {
                    placeholder.writeText(MODS_README)
                } catch (error: IOException) {
                    Log.w(TAG, "Could not write the mods placeholder", error)
                }
            }

            stamp.writeText(expected)
            null
        } catch (error: IOException) {
            Log.e(TAG, "Could not unpack the bundled assets", error)
            "Could not unpack the bundled game files: ${error.message}"
        }
    }

    /**
     * Copies a picked document into place under its canonical name and judges it.
     *
     * The copy lands on a `.part` file first and is only renamed once the port has accepted it,
     * so a rejected file never replaces a working one — which matters most for the ROM, where
     * the previous import is what the already-built archives were made from.
     */
    fun importInput(context: Context, input: Input, source: File): Verdict {
        val dir = gameDir(context)
        val target = File(dir, input.canonicalName())
        val partial = File(dir, "${input.canonicalName()}.part")
        partial.delete()

        try {
            source.inputStream().use { stream ->
                FileOutputStream(partial).use { output ->
                    stream.copyTo(output, COPY_BUFFER)
                    output.fd.sync()
                }
            }
        } catch (error: IOException) {
            partial.delete()
            Log.e(TAG, "Could not copy the picked ${input.label}", error)
            return Verdict.Rejected("Could not read that file: ${error.message ?: error}")
        }

        val verdict = judge(input, partial, dir)
        if (verdict is Verdict.Rejected) {
            partial.delete()
            return verdict
        }

        target.delete()
        if (!partial.renameTo(target)) {
            partial.delete()
            return Verdict.Rejected("Could not store the ${input.label} in the app's files directory.")
        }
        return verdict
    }

    /** Re-judges an input already sitting in the game directory, e.g. after an app update. */
    fun judgeInstalled(context: Context, input: Input): Verdict? {
        val file = fileFor(context, input)
        if (!file.isFile) return null
        return judge(input, file, gameDir(context))
    }

    private fun judge(input: Input, file: File, dir: File): Verdict {
        val answer = nativeValidateInput(input.ordinal, file.absolutePath, dir.absolutePath)
        val tag = answer.substringBefore('|')
        val message = answer.substringAfter('|')
        return when (tag) {
            "OK" -> Verdict.Ok(message)
            "WARN" -> Verdict.Warn(message)
            else -> Verdict.Rejected(message)
        }
    }

    /** Records that setup finished, so the game boots straight into itself next time. */
    fun completeSetup(context: Context): String? {
        val dir = gameDir(context)
        return nativeCompleteSetup(
            dir.absolutePath,
            fileFor(context, Input.Rom).absolutePath,
            fileFor(context, Input.Disk).absolutePath,
            fileFor(context, Input.Ipl).absolutePath
        )
    }

    /** Progress of a running archive build. [text] is a stage line, or the reason it failed. */
    data class ExtractStatus(
        val running: Boolean,
        val finished: Boolean,
        val succeeded: Boolean,
        val stage: Int,
        val done: Int,
        val total: Int,
        val text: String
    ) {
        /** Percent complete, or null while there is no denominator to measure against. */
        val percent: Int?
            get() = if (total > 0) (done.coerceAtMost(total) * 100 / total) else null

        val stageLabel: String
            get() = when (stage) {
                0 -> "Extracting the game archive"
                1 -> "Verifying the game archive"
                else -> "Extracting the 64DD archives"
            }
    }

    fun startExtraction(context: Context) {
        val dir = gameDir(context)
        nativeExtractStart(dir.absolutePath, fileFor(context, Input.Rom).absolutePath)
    }

    fun extractionStatus(): ExtractStatus {
        val parts = nativeExtractPoll().split('|', limit = 6)
        val phase = parts.getOrNull(0)?.toIntOrNull() ?: 0
        val outcome = parts.getOrNull(4)?.toIntOrNull() ?: 2
        return ExtractStatus(
            running = phase == 1,
            finished = phase == 2,
            // UpToDate (0) and Extracted (1) are both success: a warm relaunch of a failed
            // attempt that already produced a valid archive should not report a failure.
            succeeded = phase == 2 && outcome != 2,
            stage = parts.getOrNull(1)?.toIntOrNull() ?: 0,
            done = parts.getOrNull(2)?.toIntOrNull() ?: 0,
            total = parts.getOrNull(3)?.toIntOrNull() ?: 0,
            text = parts.getOrNull(5).orEmpty()
        )
    }

    fun resetExtraction() = nativeExtractReset()

    private const val COPY_BUFFER = 1 shl 17

    private val MODS_README = """
        Drop .o2r texture or content packs in this folder.

        They are mounted in the order the in-game Workshop menu lists them, last one wins, and
        the menu is also where they are enabled and disabled. Manage the files themselves from
        the Mods button in the in-game menu.
    """.trimIndent()

    private fun copyAsset(context: Context, assetPath: String, target: File) {
        target.parentFile?.mkdirs()
        context.assets.open(assetPath).use { input ->
            FileOutputStream(target).use { output ->
                input.copyTo(output)
                output.fd.sync()
            }
        }
    }

    /** Canonical filenames, in [Input] order. Cached: they are compiled-in constants. */
    private fun canonicalNames(): List<String> = canonicalNamesCache

    private val canonicalNamesCache: List<String> by lazy { nativeCanonicalNames().split('\n') }

    private external fun nativeValidateInput(kind: Int, path: String, dataDir: String): String
    private external fun nativeCompleteSetup(dataDir: String, rom: String, disk: String, ipl: String): String?
    private external fun nativeExtractStart(dataDir: String, rom: String)
    private external fun nativeExtractPoll(): String
    private external fun nativeExtractReset()
    private external fun nativeArchivesReady(dataDir: String): Boolean
    private external fun nativeCanonicalNames(): String

    init {
        // libmain.so carries both the game and the Torch extractor.
        System.loadLibrary("SDL2")
        System.loadLibrary("main")
    }
}
