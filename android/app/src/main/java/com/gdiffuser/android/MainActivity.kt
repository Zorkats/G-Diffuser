package com.gdiffuser.android

import android.annotation.SuppressLint
import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.util.Log
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.FrameLayout
import android.widget.ImageView
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import org.libsdl.app.SDLActivity
import java.io.File
import java.io.FileOutputStream
import kotlin.concurrent.thread
import kotlin.math.sqrt

/**
 * The game itself, plus the on-screen N64 pad drawn over the SDL surface.
 *
 * The overlay drives a virtual SDL joystick (see port/android/AndroidBridge.cpp) rather than
 * synthesising input events, so it goes through libultraship's normal controller mapping and can
 * be remapped in-game like any other pad.
 *
 * [LauncherActivity] guarantees the archives exist before this activity starts.
 */
class MainActivity : SDLActivity() {

    private lateinit var preferences: SharedPreferences
    private lateinit var modsButton: Button
    private lateinit var controlGroup: ViewGroup

    private var controlsEnabled = true
    private var menuOpen = false
    private var controlsHidden = false

    private val handler = Handler(Looper.getMainLooper())
    private val menuWatcher = object : Runnable {
        override fun run() {
            syncMenuState()
            handler.postDelayed(this, MENU_POLL_MS)
        }
    }

    // org/libsdl/app is kept byte-identical to the SDL release the CMake pin fetches, so the game
    // library is named here rather than patched in there. SDLActivity refuses to start if the
    // Java glue and libSDL2.so disagree on their version, so both have to move together.
    override fun getLibraries(): Array<String> = arrayOf("SDL2", "main")

    private external fun attachController()
    private external fun detachController()
    private external fun setButton(button: Int, value: Boolean)
    private external fun setAxis(axis: Int, value: Short)
    private external fun isMenuOpen(): Boolean
    private external fun nativeDocumentPicked(path: String?)

    /**
     * Opens the system document picker on the engine's behalf.
     *
     * Called from the render thread through JNI (see AndroidBridge.cpp) whenever the engine asks
     * for a file — importing a ghost or a Course Edit track, pointing the setup screen at a dump.
     * libultraship's own ImGui browser is no use here: scoped storage leaves it able to see
     * little beyond the app's own directory, so a file in Downloads or on Drive is unreachable.
     *
     * The old startActivityForResult API rather than registerForActivityResult, because
     * SDLActivity extends plain Activity and the newer contract needs a ComponentActivity.
     */
    @Suppress("unused") // via JNI
    fun requestDocumentPick() {
        runOnUiThread {
            val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                // Not narrowed by MIME: providers report .z64/.ndd/.o2r inconsistently and an
                // over-tight filter greys out the very file being sought.
                type = "*/*"
            }
            runCatching { startActivityForResult(intent, REQUEST_PICK_DOCUMENT) }
                .onFailure {
                    // No document provider at all. Answer anyway — the engine's render thread is
                    // blocked waiting and would otherwise sit there until its own deadline.
                    nativeDocumentPicked(null)
                }
        }
    }

    /** Scoped-storage replacement for the native ROM Hacks window's desktop folder button. */
    @Suppress("unused") // via JNI
    fun requestHackManager() {
        runOnUiThread { startActivity(Intent(this, RomHacksActivity::class.java)) }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQUEST_PICK_DOCUMENT) return

        val uri = data?.data
        if (resultCode != RESULT_OK || uri == null) {
            nativeDocumentPicked(null) // cancelled
            return
        }

        // The engine opens files by path, but a picked document is a content:// stream that may
        // not have one at all, so it gets copied somewhere the engine can read. A ROM is tens of
        // megabytes; not on the UI thread.
        thread(name = "gdx-doc-pick") {
            val path = runCatching { copyToPickCache(uri) }
                .onFailure { Log.e(TAG, "Could not read the picked document", it) }
                .getOrNull()
            nativeDocumentPicked(path)
        }
    }

    /** Copies [uri] into the cache and returns its path. Previous picks are cleared first. */
    private fun copyToPickCache(uri: Uri): String {
        val dir = File(cacheDir, "picked").apply {
            deleteRecursively()
            mkdirs()
        }
        val target = File(dir, pickedDisplayName(uri))
        contentResolver.openInputStream(uri).use { input ->
            requireNotNull(input) { "The document provider did not open the file." }
            FileOutputStream(target).use { output -> input.copyTo(output, COPY_BUFFER_BYTES) }
        }
        return target.absolutePath
    }

    /** The provider's name for [uri], reduced to something safe as a filename. */
    private fun pickedDisplayName(uri: Uri): String {
        val name = contentResolver
            .query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }
        return (name ?: "picked.bin").replace(Regex("""[/\\]"""), "_")
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        preferences = getSharedPreferences(PREFS, MODE_PRIVATE)
        super.onCreate(savedInstanceState)
        goFullscreen()
        setupControllerOverlay()
        attachController()
    }

    /**
     * Edge to edge, with the status and navigation bars hidden.
     *
     * From API 35 the system no longer honours the old fullscreen window flags, so the bars have
     * to be dismissed through the insets controller instead — without this the navigation bar
     * sits on top of the game. Transient-by-swipe means a stray swipe shows them briefly rather
     * than resizing the window and forcing the engine to rebuild its framebuffers.
     */
    private fun goFullscreen() {
        WindowCompat.setDecorFitsSystemWindows(window, false)

        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        // Render into the camera cutout as well, so the game gets the whole panel instead of
        // being letterboxed beside it — which matters more here than usual, because the port
        // renders true 16:9 and a phone panel is wider still.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // The bars come back whenever focus is lost — to the mods manager, a notification shade
        // pull, anything. Put them away again on the way in.
        if (hasFocus) {
            goFullscreen()
        }
    }

    override fun onResume() {
        super.onResume()
        // Re-sync immediately as well as on the timer, so coming back from the mods manager never
        // leaves the overlay showing a stale menu state.
        syncMenuState()
        handler.removeCallbacks(menuWatcher)
        handler.postDelayed(menuWatcher, MENU_POLL_MS)
    }

    override fun onPause() {
        handler.removeCallbacks(menuWatcher)
        super.onPause()
    }

    override fun onDestroy() {
        handler.removeCallbacks(menuWatcher)
        detachController()
        super.onDestroy()

        // SDL finishes this activity when SDL_main returns — quitting from the engine's menu, say
        // — but leaves the process running, and SDL_main cannot be entered twice in one process.
        // The engine's globals survive it half torn down, so a second launch would reach a
        // half-initialised renderer and crash.
        //
        // Ending the process here instead makes every launch a fresh one. Backing out with Home
        // does not come through here, so that still resumes the running game rather than
        // restarting it.
        Runtime.getRuntime().exit(0)
    }

    /**
     * Mirrors the engine's menu state onto the overlay.
     *
     * The menu is not only driven by the on-screen Menu button: a physical keyboard, a gamepad,
     * or the menu dismissing itself all change it without the overlay hearing about it. Tracking
     * it with a local flag drifts out of sync and leaves the controls dead with the Mods button
     * stuck on screen, so the engine is asked instead.
     */
    private fun syncMenuState() {
        val open = runCatching { isMenuOpen() }.getOrDefault(false)
        if (open == menuOpen) return

        menuOpen = open
        controlsEnabled = !open
        // Managing mods and saves is a settings action, so it stays out of reach until the menu
        // is up.
        modsButton.visibility = if (open) View.VISIBLE else View.GONE
        applyControlVisibility()

        if (open) {
            releaseAllInputs()
        }
    }

    /**
     * Clears anything the overlay was holding down. Without this, opening the menu mid-race
     * leaves the accelerator stuck on in the virtual pad, because the touch listeners stop
     * reporting once controls are disabled.
     */
    private fun releaseAllInputs() {
        for (button in intArrayOf(
            ControllerButtons.BUTTON_A, ControllerButtons.BUTTON_B,
            ControllerButtons.BUTTON_L, ControllerButtons.BUTTON_START,
            ControllerButtons.BUTTON_DPAD_UP, ControllerButtons.BUTTON_DPAD_DOWN,
            ControllerButtons.BUTTON_DPAD_LEFT, ControllerButtons.BUTTON_DPAD_RIGHT,
            ControllerButtons.BUTTON_Z, ControllerButtons.BUTTON_R
        )) {
            setButton(button, false)
        }
        for (axis in intArrayOf(
            ControllerButtons.AXIS_STICK_X, ControllerButtons.AXIS_STICK_Y,
            ControllerButtons.AXIS_C_X, ControllerButtons.AXIS_C_Y
        )) {
            setAxis(axis, 0)
        }
    }

    private fun setupControllerOverlay() {
        val inflater = getSystemService(LAYOUT_INFLATER_SERVICE) as LayoutInflater
        val overlay = inflater.inflate(R.layout.touchcontrol_overlay, null).apply {
            layoutParams = FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT
            )
        }
        findViewById<ViewGroup>(android.R.id.content).addView(overlay)

        controlGroup = overlay.findViewById(R.id.button_group)

        overlay.bindButton(R.id.buttonA, ControllerButtons.BUTTON_A)
        overlay.bindButton(R.id.buttonB, ControllerButtons.BUTTON_B)
        overlay.bindButton(R.id.buttonL, ControllerButtons.BUTTON_L)
        overlay.bindButton(R.id.buttonStart, ControllerButtons.BUTTON_START)
        overlay.bindButton(R.id.buttonDpadUp, ControllerButtons.BUTTON_DPAD_UP)
        overlay.bindButton(R.id.buttonDpadDown, ControllerButtons.BUTTON_DPAD_DOWN)
        overlay.bindButton(R.id.buttonDpadLeft, ControllerButtons.BUTTON_DPAD_LEFT)
        overlay.bindButton(R.id.buttonDpadRight, ControllerButtons.BUTTON_DPAD_RIGHT)

        // Z (boost) and R (right side attack) are trigger axes on a real pad, so they are pushed
        // to full deflection rather than set as buttons.
        overlay.bindButton(R.id.buttonZ, ControllerButtons.BUTTON_Z)
        overlay.bindButton(R.id.buttonR, ControllerButtons.BUTTON_R)

        // The C buttons sit on the right stick, which is how libultraship maps the N64 pad by
        // default, so the cluster drives that axis.
        overlay.bindAxisButton(R.id.buttonCUp, ControllerButtons.AXIS_C_Y, positive = false)
        overlay.bindAxisButton(R.id.buttonCDown, ControllerButtons.AXIS_C_Y, positive = true)
        overlay.bindAxisButton(R.id.buttonCLeft, ControllerButtons.AXIS_C_X, positive = false)
        overlay.bindAxisButton(R.id.buttonCRight, ControllerButtons.AXIS_C_X, positive = true)

        modsButton = overlay.findViewById(R.id.buttonMods)
        modsButton.setOnClickListener { startActivity(Intent(this, ModsActivity::class.java)) }

        setupMenuButton(overlay.findViewById(R.id.buttonMenu))
        setupJoystick(overlay.findViewById(R.id.left_joystick), overlay.findViewById(R.id.left_joystick_knob))
        setupToggleButton(overlay.findViewById(R.id.buttonToggle))
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun View.bindButton(id: Int, button: Int) {
        val view = findViewById<Button>(id)
        view.setOnTouchListener { _, event ->
            if (!controlsEnabled) return@setOnTouchListener false
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    setButton(button, true)
                    view.isPressed = true
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    setButton(button, false)
                    view.isPressed = false
                    true
                }
                else -> false
            }
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun View.bindAxisButton(id: Int, axis: Int, positive: Boolean) {
        val view = findViewById<Button>(id)
        val pressed = if (positive) Short.MAX_VALUE else Short.MIN_VALUE
        view.setOnTouchListener { _, event ->
            if (!controlsEnabled) return@setOnTouchListener false
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    setAxis(axis, pressed)
                    view.isPressed = true
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    setAxis(axis, 0)
                    view.isPressed = false
                    true
                }
                else -> false
            }
        }
    }

    /**
     * Escape opens libultraship's menu. While it is up the overlay stops feeding the virtual pad
     * so taps land on the menu instead of the game.
     *
     * Escape rather than F1, though the engine accepts both: G-Diffuser registers a full-screen
     * menu through Gui::SetMenu(), and Gui's key handling routes Escape to that menu directly
     * while F1 goes through a menu-bar branch first. Escape is also the shorter path to a menu
     * that is already open, which is what the button has to close.
     */
    @SuppressLint("ClickableViewAccessibility")
    private fun setupMenuButton(button: Button) {
        button.setOnTouchListener { _, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    onNativeKeyDown(KeyEvent.KEYCODE_ESCAPE)
                    button.isPressed = true
                    // Don't assume this opened or closed the menu — syncMenuState picks up
                    // whatever the engine actually did.
                    true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    onNativeKeyUp(KeyEvent.KEYCODE_ESCAPE)
                    button.isPressed = false
                    true
                }
                else -> false
            }
        }
    }

    private fun setupToggleButton(button: Button) {
        controlsHidden = preferences.getBoolean(KEY_CONTROLS_HIDDEN, false)
        applyControlVisibility()

        button.setOnClickListener {
            controlsHidden = !controlsHidden
            applyControlVisibility()
            preferences.edit().putBoolean(KEY_CONTROLS_HIDDEN, controlsHidden).apply()
        }
    }

    /**
     * Hides the pad while the engine's menu is up.
     *
     * Not just cosmetic: the pad is drawn over the SDL surface the menu renders into, and a
     * Button swallows a touch whether or not its listener acts on it. Leaving it up means the
     * trigger buttons cover the menu's own controls and eat taps meant for them. An INVISIBLE
     * view receives no touches, so hiding the group hands the whole screen back to the menu.
     * Menu, Mods and the toggle sit outside the group so they stay reachable.
     */
    private fun applyControlVisibility() {
        controlGroup.visibility =
            if (menuOpen || controlsHidden) View.INVISIBLE else View.VISIBLE
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun setupJoystick(joystick: FrameLayout, knob: ImageView) {
        joystick.post {
            val centerX = joystick.width / 2f
            val centerY = joystick.height / 2f
            val maxRadius = joystick.width / 2f - knob.width / 2f

            fun recenter() {
                knob.x = centerX - knob.width / 2f
                knob.y = centerY - knob.height / 2f
            }

            joystick.setOnTouchListener { _, event ->
                if (!controlsEnabled) return@setOnTouchListener false
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
                        var deltaX = event.x - centerX
                        var deltaY = event.y - centerY
                        val distance = sqrt(deltaX * deltaX + deltaY * deltaY)
                        if (distance > maxRadius) {
                            val scale = maxRadius / distance
                            deltaX *= scale
                            deltaY *= scale
                        }

                        knob.x = centerX + deltaX - knob.width / 2f
                        knob.y = centerY + deltaY - knob.height / 2f

                        setAxis(
                            ControllerButtons.AXIS_STICK_X,
                            (deltaX / maxRadius * Short.MAX_VALUE).toInt().toShort()
                        )
                        setAxis(
                            ControllerButtons.AXIS_STICK_Y,
                            (deltaY / maxRadius * Short.MAX_VALUE).toInt().toShort()
                        )
                        true
                    }
                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        recenter()
                        setAxis(ControllerButtons.AXIS_STICK_X, 0)
                        setAxis(ControllerButtons.AXIS_STICK_Y, 0)
                        true
                    }
                    else -> false
                }
            }
        }
    }

    private companion object {
        const val TAG = "G-Diffuser"
        const val PREFS = "com.gdiffuser.android.prefs"
        const val KEY_CONTROLS_HIDDEN = "controlsHidden"

        /** Fast enough to feel immediate on a menu toggle, cheap enough to ignore. */
        const val MENU_POLL_MS = 150L

        const val REQUEST_PICK_DOCUMENT = 0x1470
        const val COPY_BUFFER_BYTES = 1 shl 17
    }
}
