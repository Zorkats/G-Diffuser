/**
 * JNI surface for the Android app.
 *
 * Four unrelated things live here because they are the only places the Kotlin side reaches into
 * native code:
 *
 *   - the on-screen controller, fed into SDL as a virtual joystick,
 *   - whether the engine's menu is up, which decides when the overlay stands down,
 *   - the system document picker, which the engine's own file prompts are routed through, and
 *   - first-time setup: validating the three inputs G-Diffuser needs and building the archives.
 *
 * The setup half is deliberately independent of libultraship. It is called from the launcher
 * activity, in its own process, before SDL or Ship::Context exist, so it cannot discover paths
 * through the engine — the launcher passes them in. It reuses the port's own first-boot and
 * extraction code rather than reimplementing the rules in Kotlin, so what the launcher accepts
 * and what the game accepts on the next launch cannot drift apart.
 */
#ifdef __ANDROID__

#include <jni.h>
#include <android/log.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_gamecontroller.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

#include "gdx_android_bridge.h"
#include "gdx_firstboot.h"
#include "gdx_extract_launch.h"
#include "ship/Context.h"
#include "ship/window/Window.h"
#include "ship/window/gui/Gui.h"

namespace {

constexpr const char* kLogTag = "G-Diffuser";

std::string ToStdString(JNIEnv* env, jstring value) {
    if (value == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result = chars != nullptr ? chars : "";
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(value, chars);
    }
    return result;
}

// The on-screen controller is exposed to the game as an ordinary SDL game controller so it flows
// through libultraship's normal input mapping, which means it can be remapped in-game like any
// physical pad.
SDL_Joystick* gVirtualJoystick = nullptr;
int gVirtualJoystickId = -1;

// --------------------------- document picker ---------------------------
//
// The engine's file prompts (PickRomFile and friends in gdx_firstboot.cpp) are modal on the
// platform they were written for: they block until the user has chosen. Android's picker is an
// activity result that lands on the UI thread an arbitrary time later, so the call is made from
// the render thread, parked here, and woken when Kotlin hands the answer back.
//
// Blocking the render thread is safe and is the point: the UI thread is a different thread
// entirely, so it is free to run the picker while the game waits. What it must not do is wait
// forever — a picker that never returns (no document provider installed, the activity killed
// behind it) would otherwise hang the game with no way out, hence the deadline.
std::mutex gPickMutex;
std::condition_variable gPickReady;
bool gPickInFlight = false;
bool gPickResultReady = false;
std::string gPickResult;

// Calls a no-argument method on the SDL activity. Returns false if there is no activity yet, or
// the call threw.
bool CallActivityMethod(const char* name) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (env == nullptr || activity == nullptr) {
        if (activity != nullptr) {
            env->DeleteLocalRef(activity);
        }
        return false;
    }

    jclass activityClass = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(activityClass, name, "()V");
    bool ok = false;
    if (method != nullptr) {
        env->CallVoidMethod(activity, method);
        ok = env->ExceptionCheck() == JNI_FALSE;
        if (!ok) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return ok;
}

} // namespace

namespace gdx::android {

// Blocks the calling (render) thread until the user picks a document or cancels. Returns the
// path of a local copy the engine can open, or an empty string.
std::string PickDocumentBlocking() {
    {
        std::lock_guard<std::mutex> lock(gPickMutex);
        if (gPickInFlight) {
            // One at a time: a second picker would steal the first one's answer.
            __android_log_print(ANDROID_LOG_WARN, kLogTag, "A file pick is already in flight; ignoring");
            return {};
        }
        gPickInFlight = true;
        gPickResultReady = false;
        gPickResult.clear();
    }

    if (!CallActivityMethod("requestDocumentPick")) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not start the document picker");
        std::lock_guard<std::mutex> lock(gPickMutex);
        gPickInFlight = false;
        return {};
    }

    std::unique_lock<std::mutex> lock(gPickMutex);
    // Five minutes: long enough that browsing to a ROM on a slow cloud provider is not cut off,
    // short enough that a picker which never comes back does not strand the game for good.
    const bool answered = gPickReady.wait_for(lock, std::chrono::minutes(5),
                                              [] { return gPickResultReady; });
    std::string result = answered ? gPickResult : std::string();
    if (!answered) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "The document picker did not answer in time");
    }
    gPickInFlight = false;
    gPickResultReady = false;
    gPickResult.clear();
    return result;
}

bool OpenHackManager() {
    if (!CallActivityMethod("requestHackManager")) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not open the ROM-hack manager");
        return false;
    }
    return true;
}

} // namespace gdx::android

extern "C" {

// ─────────────────────────────── on-screen controller ───────────────────────────────

JNIEXPORT void JNICALL Java_com_gdiffuser_android_MainActivity_attachController(JNIEnv*, jobject) {
    if (gVirtualJoystick != nullptr) {
        return;
    }

    gVirtualJoystickId = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 18, 0);
    if (gVirtualJoystickId == -1) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not attach the on-screen controller: %s",
                            SDL_GetError());
        return;
    }

    gVirtualJoystick = SDL_JoystickOpen(gVirtualJoystickId);
    if (gVirtualJoystick == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "Could not open the on-screen controller: %s",
                            SDL_GetError());
        SDL_JoystickDetachVirtual(gVirtualJoystickId);
        gVirtualJoystickId = -1;
    }
}

JNIEXPORT void JNICALL Java_com_gdiffuser_android_MainActivity_detachController(JNIEnv*, jobject) {
    if (gVirtualJoystick == nullptr) {
        return;
    }

    SDL_JoystickClose(gVirtualJoystick);
    SDL_JoystickDetachVirtual(gVirtualJoystickId);
    gVirtualJoystick = nullptr;
    gVirtualJoystickId = -1;
}

JNIEXPORT void JNICALL Java_com_gdiffuser_android_MainActivity_setButton(JNIEnv*, jobject, jint button,
                                                                        jboolean value) {
    if (gVirtualJoystick == nullptr) {
        return;
    }

    // Negative ids address an axis instead, so Z and the shoulder triggers can be driven by a
    // plain on/off button in the overlay.
    if (button < 0) {
        SDL_JoystickSetVirtualAxis(gVirtualJoystick, -button, value ? SDL_MAX_SINT16 : -SDL_MAX_SINT16);
    } else {
        SDL_JoystickSetVirtualButton(gVirtualJoystick, button, value);
    }
}

JNIEXPORT void JNICALL Java_com_gdiffuser_android_MainActivity_setAxis(JNIEnv*, jobject, jint axis,
                                                                      jshort value) {
    if (gVirtualJoystick == nullptr) {
        return;
    }

    SDL_JoystickSetVirtualAxis(gVirtualJoystick, axis, value);
}

/**
 * Result of a document pick, called from the UI thread once the activity result has been copied
 * somewhere the engine can open by path. A null path means cancelled.
 */
JNIEXPORT void JNICALL Java_com_gdiffuser_android_MainActivity_nativeDocumentPicked(JNIEnv* env, jobject,
                                                                                   jstring jPath) {
    const std::string path = ToStdString(env, jPath);

    std::lock_guard<std::mutex> lock(gPickMutex);
    if (!gPickInFlight) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "Document pick result with nothing waiting for it");
        return;
    }
    gPickResult = path;
    gPickResultReady = true;
    gPickReady.notify_all();
}

/**
 * Whether libultraship's menu is currently up.
 *
 * The overlay needs this because the menu is not only opened by its own Menu button — a keyboard,
 * a gamepad, or the menu closing itself all change it behind the overlay's back. Reading the
 * engine's state instead of mirroring it in Kotlin is what keeps the two from drifting apart.
 *
 * Called from the UI thread while the game thread renders. It reads a bool the game thread may be
 * writing, which is worth accepting here: the value only drives which overlay buttons are shown,
 * and a torn read self-corrects on the next poll.
 */
JNIEXPORT jboolean JNICALL Java_com_gdiffuser_android_MainActivity_isMenuOpen(JNIEnv*, jobject) {
    // Locks a weak_ptr rather than creating anything, so it comes back null when the engine has
    // not started yet -- which is exactly what the first few polls hit.
    auto context = Ship::Context::GetInstance();
    if (context == nullptr) {
        return JNI_FALSE;
    }

    auto window = context->GetWindow();
    if (window == nullptr) {
        return JNI_FALSE;
    }

    auto gui = window->GetGui();
    if (gui == nullptr) {
        return JNI_FALSE;
    }

    return gui->GetMenuOrMenubarVisible() ? JNI_TRUE : JNI_FALSE;
}

// ────────────────────────────────── first-time setup ──────────────────────────────────
//
// Called from the launcher activity's own process. Everything below runs the port's real
// acceptance rules (gdx_firstboot.h), never a Kotlin-side approximation of them.

/**
 * Judges one already-copied input file.
 *
 * `kind` is 0 for the cartridge ROM, 1 for the Expansion Kit disk image, 2 for the 64DD IPL ROM.
 * Returns a single line the launcher shows verbatim, tagged with what the caller has to do:
 *
 *   "OK|<label>"    accepted, and recognized as a known-good dump
 *   "WARN|<text>"   accepted, but unrecognized — the warning has to stay visible
 *   "ERR|<reason>"  refused; the reason names what is wrong with the file
 *
 * The structural check runs first so a truncated or wrong-console file is reported as such
 * rather than as an unrecognized hash.
 */
JNIEXPORT jstring JNICALL Java_com_gdiffuser_android_GameAssets_nativeValidateInput(
    JNIEnv* env, jobject, jint kind, jstring jPath, jstring jDataDir) {
    const std::string path = ToStdString(env, jPath);
    const std::string dataDir = ToStdString(env, jDataDir);

    std::string why;
    bool structureOk = false;
    const char* canonical = nullptr;
    switch (kind) {
        case 0:
            structureOk = gdx::ValidateRomFile(path, why);
            canonical = gdx::SetupRomFileName();
            break;
        case 1:
            structureOk = gdx::ValidateDiskFile(path, why);
            canonical = gdx::SetupDiskFileName();
            break;
        default:
            structureOk = gdx::ValidateIplFile(path, why);
            canonical = gdx::SetupIplFileName();
            break;
    }
    if (!structureOk) {
        return env->NewStringUTF(("ERR|" + why).c_str());
    }

    // dataDir doubles as exeDir on Android (see port/android/gdx_android_paths.h): the recipe
    // tree the expected-ROM hash is read from was unpacked there.
    const gdx::GdxInputRecognition recognition = gdx::GdxRecognizeInput(canonical, path, dataDir);
    switch (recognition.verdict) {
        case gdx::GdxInputVerdict::VerifiedKnown: {
            const std::string label =
                recognition.okHeaderOverride.empty() ? recognition.message : recognition.okHeaderOverride;
            return env->NewStringUTF(("OK|" + label).c_str());
        }
        case gdx::GdxInputVerdict::AcceptedUnknownWarn:
            return env->NewStringUTF(("WARN|" + recognition.message).c_str());
        case gdx::GdxInputVerdict::Rejected:
        default:
            return env->NewStringUTF(("ERR|" + recognition.message).c_str());
    }
}

/**
 * Records that setup is done, so the next launch takes the game's warm path instead of showing
 * its own setup screen.
 *
 * Also makes the managed copy of the disk image, which is what lets the user delete the .ndd
 * they imported from. Returns null on success, or a message.
 */
JNIEXPORT jstring JNICALL Java_com_gdiffuser_android_GameAssets_nativeCompleteSetup(
    JNIEnv* env, jobject, jstring jDataDir, jstring jRom, jstring jDisk, jstring jIpl) {
    const std::string dataDir = ToStdString(env, jDataDir);
    const std::string rom = ToStdString(env, jRom);
    const std::string disk = ToStdString(env, jDisk);
    const std::string ipl = ToStdString(env, jIpl);

    std::string managed;
    if (!gdx::EnsureManagedDiskCopy(dataDir, disk, managed)) {
        // Not fatal: the game falls back to reading the imported image directly. It only means
        // the original stays load-bearing, so say so rather than failing the whole setup.
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "Could not make the managed copy of the disk image at %s", managed.c_str());
    }

    if (!gdx::WriteSetupComplete(dataDir, rom, disk, ipl)) {
        return env->NewStringUTF("Could not record that setup finished. The setup screen will run again.");
    }
    return nullptr;
}

/**
 * Starts the archive build on a background thread and returns immediately.
 *
 * This is the port's own extractor, driven exactly as the desktop setup screen drives it, so
 * the launcher gets the same staged progress and the same complete-or-absent guarantee: a build
 * that fails leaves no half-written archive behind and the game still boots from the raw ROM.
 */
JNIEXPORT void JNICALL Java_com_gdiffuser_android_GameAssets_nativeExtractStart(JNIEnv* env, jobject,
                                                                               jstring jDataDir,
                                                                               jstring jRom) {
    const std::string dataDir = ToStdString(env, jDataDir);
    const std::string rom = ToStdString(env, jRom);
    // dataDir is also exeDir here; suppressNativeDialog is about a Win32 window that does not
    // exist on this platform, and is passed true so nothing tries to open one.
    gdx::GdxExtractStartAsync(dataDir.c_str(), rom.c_str(), dataDir.c_str(), true);
}

/**
 * One line of extractor status for the launcher's progress view:
 *
 *   "<phase>|<subStage>|<entriesSeen>|<entriesExpected>|<outcome>|<text>"
 *
 * phase is 0 idle / 1 running / 2 done; outcome is only meaningful once done (0 up to date,
 * 1 extracted, 2 failed). text is the newest stage line while running and the reason once a run
 * has failed, so the launcher can show progress and a failure through the same field.
 */
JNIEXPORT jstring JNICALL Java_com_gdiffuser_android_GameAssets_nativeExtractPoll(JNIEnv* env, jobject) {
    const gdx::ExtractProgress progress = gdx::GdxExtractPollStatus();

    int phase = 0;
    switch (progress.phase) {
        case gdx::ExtractPhase::Idle:    phase = 0; break;
        case gdx::ExtractPhase::Running: phase = 1; break;
        case gdx::ExtractPhase::Done:    phase = 2; break;
    }

    int outcome = 2;
    switch (progress.outcome) {
        case gdx::ExtractOutcome::UpToDate:  outcome = 0; break;
        case gdx::ExtractOutcome::Extracted: outcome = 1; break;
        case gdx::ExtractOutcome::FailedRawFallback:
        default:                             outcome = 2; break;
    }

    // The cart stage counts against the golden entry count; the IPL stage against its own small
    // fixed total. Anything else has no denominator and the launcher shows an indeterminate bar.
    int expected = 0;
    if (progress.subStage == 0) {
        expected = gdx::GdxExtractExpectedCartEntryCount();
    } else if (progress.subStage == 2) {
        expected = gdx::GdxExtractExpectedIplEntryCount();
    }

    const std::string text = (phase == 2 && outcome == 2 && !progress.lastError.empty())
                                 ? progress.lastError
                                 : progress.stage;

    char header[96];
    std::snprintf(header, sizeof(header), "%d|%d|%d|%d|%d|", phase, progress.subStage,
                  progress.entriesSeen, expected, outcome);
    return env->NewStringUTF((std::string(header) + text).c_str());
}

/** Joins the finished worker and resets it, so a retry can start a new one. */
JNIEXPORT void JNICALL Java_com_gdiffuser_android_GameAssets_nativeExtractReset(JNIEnv*, jobject) {
    gdx::GdxExtractResetAsync();
}

/**
 * True when `dataDir` already holds a valid archive for each of the three inputs, judged by the
 * same hash-checked chain the game's own warm-boot path uses. That is what lets the launcher skip
 * straight to the game — and, just as importantly, what makes it NOT skip when an archive is
 * present but corrupt.
 */
JNIEXPORT jboolean JNICALL Java_com_gdiffuser_android_GameAssets_nativeArchivesReady(JNIEnv* env, jobject,
                                                                                    jstring jDataDir) {
    const std::string dataDir = ToStdString(env, jDataDir);
    const bool ready = gdx::GdxFirstbootArchiveSatisfies(gdx::GdxFirstbootArchiveKind::Game, dataDir) &&
                       gdx::GdxFirstbootArchiveSatisfies(gdx::GdxFirstbootArchiveKind::Ipl, dataDir) &&
                       gdx::GdxFirstbootArchiveSatisfies(gdx::GdxFirstbootArchiveKind::Disk, dataDir);
    return ready ? JNI_TRUE : JNI_FALSE;
}

/** The canonical filenames the launcher has to copy the user's files to, newline separated. */
JNIEXPORT jstring JNICALL Java_com_gdiffuser_android_GameAssets_nativeCanonicalNames(JNIEnv* env, jobject) {
    const std::string names = std::string(gdx::SetupRomFileName()) + "\n" + gdx::SetupDiskFileName() +
                              "\n" + gdx::SetupIplFileName();
    return env->NewStringUTF(names.c_str());
}

} // extern "C"

#endif // __ANDROID__
