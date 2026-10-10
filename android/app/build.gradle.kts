import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.security.MessageDigest
import java.util.Properties
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

plugins {
    // Kotlin comes with AGP 9 rather than as its own plugin; see the note in the root
    // build.gradle.kts before adding org.jetbrains.kotlin.android back.
    id("com.android.application")
}

// Named distinctly from PackGameData's own `repositoryRoot` property. Sharing the name would
// mean reaching past the task's shadowing copy with a `this@Build_gradle` label, and the
// Kotlin DSL's implicit script label is not stable across Gradle versions.
val repoRoot: File = rootProject.projectDir.parentFile

// One source of truth for the version: the CMake cache variable the About page reads.
val projectVersion: String =
    Regex("""GDX_DISPLAY_VERSION\s+"([0-9.]+)"""")
        .find(File(repoRoot, "port/CMakeLists.txt").readText())
        ?.groupValues?.get(1)
        ?: "0.0.0"

// Release signing: android/key.properties locally, the environment on CI. The file holds
// passwords, so it is gitignored and never read into the build output.
val keystoreProperties = Properties().apply {
    val file = rootProject.file("key.properties")
    if (file.isFile) file.inputStream().use { load(it) }
}

// With neither configured — a fork's CI, or a fresh clone — the release build falls back to the
// debug key so it still produces an installable APK rather than failing on a keystore nobody has.
val hasReleaseKeystore =
    keystoreProperties.getProperty("storeFile") != null || System.getenv("KEYSTORE_FILE") != null

/**
 * Bundles everything the game reads off the filesystem into one zip inside the APK.
 *
 * On a desktop build these files are staged next to the executable by post-build steps in
 * port/CMakeLists.txt. Android has no such directory — the APK is read-only and the game's own
 * paths all resolve to the app's external files directory — so the same set is assembled here
 * and unpacked there on first run (GameAssets.kt).
 *
 * What goes in:
 *   gdiffuser.o2r      the engine archive (Fast3D shaders + the port's own branding). Generated
 *                      here rather than taken from a desktop build tree, because it is built by
 *                      a Python script from files in this checkout and needs no compiler.
 *   decomp-recipes/    what Torch reads to turn the player's ROM into the game archive.
 *   shaders/           the post-processing pipeline resolves file-based presets from disk.
 *   fonts/             added to ImGui's atlas before the renderer uploads it, so they cannot
 *                      live inside an archive the renderer has not mounted yet.
 *   the licences       the project's own and every bundled dependency's.
 *
 * The zip is written sorted and at a fixed timestamp so its digest only changes when its
 * contents do — which is what stops an app update re-unpacking a byte-identical payload.
 */
abstract class PackGameData : DefaultTask() {

    // Every input is named individually rather than taking the repository root as one directory.
    // The root contains this build's own output directory, so declaring it whole makes Gradle
    // see the task consuming other tasks' outputs and refuse to order them.

    /** Torch's recipe tree for the only ROM revision the port extracts, us/rev0. */
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val recipes: DirectoryProperty

    /** The Fast3D shaders gen_f3d_o2r.py packs into the engine archive. */
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val engineShaders: DirectoryProperty

    /** The post-processing presets the pipeline resolves from disk at runtime. */
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val postShaders: DirectoryProperty

    /** The GUI fonts, added to ImGui's atlas before the renderer uploads it. */
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val fonts: DirectoryProperty

    /** The Expansion Kit recipe tree the dump tables are derived from. */
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val expansionKitYaml: DirectoryProperty

    /** Licence notices for every bundled and statically linked dependency. */
    @get:InputDirectory
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val licenses: DirectoryProperty

    /**
     * The individual files: the two generator scripts, the recipe config Torch keys off, the
     * audio tables the dump classes are built from, the branding the engine archive carries,
     * the controller database, and the two licence texts.
     *
     * A collection rather than one InputFile each, because they share a single cache-key
     * contribution and nothing here needs to tell them apart.
     */
    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val sourceFiles: ConfigurableFileCollection

    /**
     * The optional recorded shader-cache seed. gen_f3d_o2r.py packs it when it exists and says
     * nothing when it does not, so this is a collection: an InputFile would fail the build on a
     * checkout that has never recorded one.
     */
    @get:InputFiles
    @get:PathSensitive(PathSensitivity.RELATIVE)
    abstract val shaderCacheSeed: ConfigurableFileCollection

    /** Where the generator scripts run from; they resolve their own inputs against it. */
    @get:Internal
    abstract val repositoryRoot: DirectoryProperty

    @get:OutputDirectory
    abstract val outputDir: DirectoryProperty

    @get:Internal
    abstract val workDir: DirectoryProperty

    @TaskAction
    fun pack() {
        val root = repositoryRoot.get().asFile
        val work = workDir.get().asFile
        work.deleteRecursively()
        work.mkdirs()

        // ---- generated pieces -------------------------------------------------------------
        val engineArchive = File(work, "gdiffuser.o2r")
        runPython(root, listOf("tools/gen_f3d_o2r.py", engineArchive.absolutePath))

        val dumpTables = File(work, "decomp-recipes").apply { mkdirs() }
        runPython(
            root,
            listOf(
                "tools/gen_dump_tables_data.py",
                "--audio-tables", File(root, "decomp/src/audio/disk/audio_tables.c").absolutePath,
                "--out-dir", dumpTables.absolutePath,
                "--ek-yaml-dir", expansionKitYaml.get().asFile.absolutePath
            )
        )

        // ---- the entry list ---------------------------------------------------------------
        val entries = buildList {
            add("gdiffuser.o2r" to engineArchive)
            add("LICENSE" to File(root, "LICENSE"))
            add("THIRD_PARTY_NOTICES.md" to File(root, "THIRD_PARTY_NOTICES.md"))

            // Only us/rev0 ships. Other revisions' config keys are never dereferenced, because
            // recipe selection is by matched ROM SHA-1.
            add("decomp-recipes/config.yml" to File(root, "decomp/config.yml"))
            add("decomp-recipes/LICENSE" to File(root, "decomp/LICENSE"))
            add("decomp-recipes/ek_slice_manifest.txt" to File(root, "port/gen/ek_slice_manifest.txt"))
            addTree(recipes.get().asFile, "decomp-recipes/assets/yaml/us/rev0")
            addTree(dumpTables, "decomp-recipes")

            addTree(postShaders.get().asFile, "shaders")
            addTree(licenses.get().asFile, "LICENSES")

            // The GUI fonts, and the licence text that has to travel with them.
            addTree(fonts.get().asFile, "fonts")
        }.sortedBy { it.first }

        for ((name, file) in entries) {
            if (!file.isFile) throw GradleException("Missing game data file: $file (for zip entry $name)")
        }

        val target = outputDir.get().asFile
        target.mkdirs()
        val zipFile = File(target, "gamedata.zip")
        ZipOutputStream(zipFile.outputStream().buffered()).use { zip ->
            entries.forEach { (name, file) ->
                zip.putNextEntry(ZipEntry(name).apply { time = FIXED_ENTRY_TIME })
                file.inputStream().use { it.copyTo(zip) }
                zip.closeEntry()
            }
        }

        val digest = MessageDigest.getInstance("MD5").digest(zipFile.readBytes())
        File(target, "gamedata.version").writeText(digest.joinToString("") { "%02x".format(it) })

        // Seeded once on the device rather than unpacked with the rest, because the player is
        // meant to edit it, so it ships as its own asset.
        File(root, "port/assets/gamecontrollerdb.txt")
            .copyTo(File(target, "gamecontrollerdb.txt"), overwrite = true)
    }

    private fun MutableList<Pair<String, File>>.addTree(dir: File, prefix: String) {
        if (!dir.isDirectory) throw GradleException("Missing game data directory: $dir")
        dir.walkTopDown().filter(File::isFile).forEach {
            add("$prefix/${it.relativeTo(dir).invariantSeparatorsPath}" to it)
        }
    }

    private fun runPython(workingDir: File, args: List<String>) {
        // The same interpreter CMake's find_package(Python3) would pick, resolved the same way
        // on every host: a plain "python3" first, then the name Windows installs it under.
        val interpreter = listOf("python3", "python").firstOrNull { candidate ->
            runCatching {
                ProcessBuilder(candidate, "--version").redirectErrorStream(true).start().waitFor() == 0
            }.getOrDefault(false)
        } ?: throw GradleException(
            "Python 3 is required to build the APK: it generates gdiffuser.o2r and the dump " +
                "tables. Install it and put it on PATH."
        )

        val process = ProcessBuilder(listOf(interpreter) + args)
            .directory(workingDir)
            .redirectErrorStream(true)
            .start()
        val output = process.inputStream.bufferedReader().readText()
        if (process.waitFor() != 0) {
            throw GradleException("${args.first()} failed:\n$output")
        }
    }

    private companion object {
        // 1980-02-01T00:00:00Z, the same instant Gradle's own archive tasks use for this.
        // Anything earlier risks falling before the 1980 floor a zip entry's DOS timestamp can
        // represent.
        const val FIXED_ENTRY_TIME = 318211200000L
    }
}

// Wired through the variant API rather than added as a source-set srcDir. The srcDir form looks
// equivalent and is not: the asset merge takes the path but not the dependency on the task that
// fills it, so a clean checkout silently packages an APK whose assets are missing everything
// above — and the missing-file check never runs to say so.
androidComponents {
    onVariants { variant ->
        val packGameData =
            tasks.register<PackGameData>("pack${variant.name.replaceFirstChar(Char::titlecase)}GameData") {
                description = "Bundles the engine archive, Torch recipes, shaders and fonts into the APK."
                val repo = repoRoot
                repositoryRoot.set(repo)
                recipes.set(File(repo, "decomp/assets/yaml/us/rev0"))
                engineShaders.set(File(repo, "libultraship/src/fast/shaders"))
                postShaders.set(File(repo, "shaders"))
                fonts.set(File(repo, "port/assets/fonts"))
                expansionKitYaml.set(File(repo, "fzerox-expansion-kit/assets/yaml/jp"))
                licenses.set(File(repo, "LICENSES"))
                sourceFiles.from(
                    File(repo, "tools/gen_f3d_o2r.py"),
                    File(repo, "tools/gen_dump_tables_data.py"),
                    File(repo, "tools/gen_dump_all_extra.py"),
                    File(repo, "decomp/config.yml"),
                    File(repo, "decomp/LICENSE"),
                    File(repo, "decomp/src/audio/disk/audio_tables.c"),
                    File(repo, "port/gen/ek_slice_manifest.txt"),
                    File(repo, "port/assets/gamecontrollerdb.txt"),
                    File(repo, "assets/branding/gdiffuser-logo-menu.png"),
                    File(repo, "LICENSE"),
                    File(repo, "THIRD_PARTY_NOTICES.md")
                )
                shaderCacheSeed.from(fileTree(File(repo, "port/shadercache")) { include("**/*.gdxshc") })
                workDir.set(layout.buildDirectory.dir("gamedata/${variant.name}"))
            }
        variant.sources.assets?.addGeneratedSourceDirectory(packGameData, PackGameData::outputDir)
    }
}

android {
    namespace = "com.gdiffuser.android"
    compileSdk = 36
    ndkVersion = "30.0.15729638"

    defaultConfig {
        applicationId = "com.gdiffuser.android"
        minSdk = 24
        targetSdk = 36
        versionCode = 1
        versionName = projectVersion

        ndk {
            // arm64 only, and not merely for size. The port maps the N64's 32-bit address space
            // onto host pointers and recovers host addresses from bit patterns — it treats the
            // 64-bit pointer as the wide side of that mapping throughout, and compiles the
            // truncation warnings that would catch a violation as errors. A 32-bit ABI is a
            // different question from "does it build", and one nothing here has answered.
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                arguments += listOf(
                    "-DANDROID_STL=c++_shared",
                    "-DUSE_OPENGLES=ON",
                    "-DSDL_SHARED=ON",
                    "-DSDL_STATIC=OFF",
                    // Forced for every variant, debug included. The port tunes its own optimizer
                    // flags per configuration and pairs them with -fno-strict-aliasing/-fwrapv
                    // for the decompiled C, which is load-bearing rather than a preference; the
                    // Android plugin would otherwise build the release variant as
                    // RelWithDebInfo, a flag set the project never tunes.
                    "-DCMAKE_BUILD_TYPE=Release"
                )
                targets += "G-Diffuser"
            }
        }
    }

    signingConfigs {
        if (hasReleaseKeystore) {
            create("release") {
                val configuredStore = keystoreProperties.getProperty("storeFile")
                if (configuredStore != null) {
                    // A relative path in key.properties reads against that file's own directory,
                    // which is what someone editing it would expect.
                    storeFile = rootProject.file(configuredStore)
                    storePassword = keystoreProperties.getProperty("storePassword")
                    keyAlias = keystoreProperties.getProperty("keyAlias")
                    keyPassword = keystoreProperties.getProperty("keyPassword")
                } else {
                    storeFile = file(System.getenv("KEYSTORE_FILE"))
                    storePassword = System.getenv("KEYSTORE_PASSWORD")
                    keyAlias = System.getenv("KEY_ALIAS")
                    keyPassword = System.getenv("KEY_PASSWORD")
                }
            }
        }
    }

    buildTypes {
        debug {
            // No isJniDebuggable: the native side is built Release regardless (see the
            // CMAKE_BUILD_TYPE argument above), so there would be nothing useful to attach to.
            applicationIdSuffix = ".debug"
        }
        release {
            // The APK is almost entirely native code and assets, so shrinking the thin Kotlin
            // layer buys nothing and only risks stripping the classes the JNI bridge looks up
            // by name.
            isMinifyEnabled = false
            signingConfig = signingConfigs.getByName(if (hasReleaseKeystore) "release" else "debug")
            ndk {
                debugSymbolLevel = "SYMBOL_TABLE"
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = File(repoRoot, "CMakeLists.txt")
            // libultraship needs CMake >= 3.26; the NDK-bundled 3.22.1 is too old.
            version = "3.30.3+"
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    lint {
        abortOnError = false
    }
}

// AGP 9's built-in Kotlin extension. Kept in step with compileOptions above: a jvmTarget that
// disagrees with the Java target fails the build the moment Kotlin and Java code in the same
// module reference each other, which the launcher and the SDL glue do.
kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")
    implementation("androidx.activity:activity-ktx:1.9.3")
    // The on-screen controller layout (res/layout/touchcontrol_overlay.xml).
    implementation("androidx.constraintlayout:constraintlayout:2.1.4")
}
