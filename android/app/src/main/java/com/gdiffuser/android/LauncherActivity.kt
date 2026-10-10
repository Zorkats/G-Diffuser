package com.gdiffuser.android

import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import java.io.File
import java.io.FileOutputStream
import kotlin.concurrent.thread

/**
 * First-run flow: unpack the bundled game files, take the player's three dumps, and build the
 * archives from them before handing off to the game.
 *
 * Three, not one. G-Diffuser is built Expansion-Kit-only — the 64DD disk and the drive's IPL ROM
 * are as load-bearing as the cartridge ROM, and the game does not run correctly without them.
 * Each row below is one of those inputs, and each stays red until the port's own validators have
 * accepted the file; nothing here decides what is acceptable on its own.
 *
 * No game data ships in the APK. The dumps stay on the device and the extraction runs here.
 */
class LauncherActivity : ComponentActivity() {

    private lateinit var status: TextView
    private lateinit var progress: ProgressBar
    private lateinit var playButton: Button
    private val rows = mutableMapOf<GameAssets.Input, InputRow>()

    /** Which input the picker currently in flight is for. */
    private var pickingFor: GameAssets.Input? = null

    private val handler = Handler(Looper.getMainLooper())

    private val pickDocument =
        registerForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
            val input = pickingFor
            pickingFor = null
            if (uri != null && input != null) importInput(input, uri)
        }

    /** One requirement: its name, what it is, and how the last file offered for it was judged. */
    private inner class InputRow(val input: GameAssets.Input, container: ViewGroup) {
        private val heading: TextView
        private val detail: TextView
        private val button: Button

        init {
            val density = resources.displayMetrics.density
            val row = LinearLayout(this@LauncherActivity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                setPadding(0, (8 * density).toInt(), 0, (8 * density).toInt())
            }
            // Not named `text`: a local of that name shadows the TextView.text property inside
            // the apply blocks below, and the assignments silently retarget onto it.
            val labels = LinearLayout(this@LauncherActivity).apply {
                orientation = LinearLayout.VERTICAL
            }
            heading = TextView(this@LauncherActivity).apply {
                setTextColor(Color.WHITE)
                textSize = 15f
                text = input.label
            }
            detail = TextView(this@LauncherActivity).apply {
                setTextColor(Color.rgb(170, 180, 195))
                textSize = 12f
                text = input.what
            }
            button = Button(this@LauncherActivity).apply {
                text = getString(R.string.launcher_choose)
                setOnClickListener { startPick(input) }
            }

            labels.addView(heading)
            labels.addView(detail)
            row.addView(labels, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            row.addView(button)
            container.addView(row, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        }

        var accepted = false
            private set

        fun show(verdict: GameAssets.Verdict?) {
            when (verdict) {
                null -> {
                    accepted = false
                    heading.setTextColor(Color.WHITE)
                    heading.text = input.label
                    detail.setTextColor(Color.rgb(170, 180, 195))
                    detail.text = input.what
                }
                is GameAssets.Verdict.Ok -> {
                    accepted = true
                    heading.setTextColor(ACCEPTED)
                    heading.text = "${input.label} — ready"
                    detail.setTextColor(ACCEPTED)
                    detail.text = verdict.message
                }
                is GameAssets.Verdict.Warn -> {
                    // Accepted, but the warning has to stay on screen: an unrecognised dump can
                    // extract into an archive that then misbehaves in ways nothing later
                    // attributes back to the file.
                    accepted = true
                    heading.setTextColor(WARNING)
                    heading.text = "${input.label} — accepted with a warning"
                    detail.setTextColor(WARNING)
                    detail.text = verdict.message
                }
                is GameAssets.Verdict.Rejected -> {
                    accepted = false
                    heading.setTextColor(REJECTED)
                    heading.text = "${input.label} — not accepted"
                    detail.setTextColor(REJECTED)
                    detail.text = verdict.message
                }
            }
        }

        fun setEnabled(enabled: Boolean) {
            button.isEnabled = enabled
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        prepare()
    }

    override fun onDestroy() {
        handler.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    /**
     * Unpacks the bundled game files, then decides what still has to happen: nothing (start the
     * game), build the archives from dumps already imported, or ask for the missing ones.
     *
     * The unpacking moves the recipes and the engine archive out of the APK, so it cannot run on
     * the main thread.
     */
    private fun prepare() {
        setBusy(true, getString(R.string.launcher_preparing))

        thread(name = "gdx-prepare") {
            val stagingError = GameAssets.stageBundledAssets(this)
            // Re-judged rather than remembered: an app update can ship recipes that expect a
            // different ROM, and a file the user deleted through a file manager has to stop
            // counting as imported.
            val judged = GameAssets.Input.entries.associateWith { GameAssets.judgeInstalled(this, it) }
            val ready = stagingError == null && GameAssets.archivesReady(this)

            runOnUiThread {
                if (stagingError != null) {
                    // Nothing else will work without the bundled files, so leave the buttons
                    // disabled rather than invite a retry that cannot succeed.
                    setBusy(false, stagingError)
                    rows.values.forEach { it.setEnabled(false) }
                    return@runOnUiThread
                }

                judged.forEach { (input, verdict) -> rows.getValue(input).show(verdict) }

                if (ready) {
                    startGame()
                } else {
                    setBusy(false, getString(R.string.launcher_intro))
                    refreshPlayButton()
                }
            }
        }
    }

    private fun buildUi() {
        val density = resources.displayMetrics.density
        val padding = (24 * density).toInt()

        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, padding, padding, padding)
            setBackgroundColor(Color.rgb(14, 16, 28))
        }

        val title = TextView(this).apply {
            setTextColor(Color.WHITE)
            textSize = 22f
            text = getString(R.string.app_name)
        }

        status = TextView(this).apply {
            setTextColor(Color.rgb(200, 208, 220))
            textSize = 13f
        }

        val rowHolder = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

        playButton = Button(this).apply {
            text = getString(R.string.launcher_build)
            isEnabled = false
            setOnClickListener { beginExtraction() }
        }
        progress = ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal).apply {
            visibility = View.GONE
            max = 100
        }

        // The failure and warning messages can run long, and the launcher is fixed to landscape,
        // so let the whole page scroll rather than push the button off the bottom.
        val scroller = ScrollView(this)
        val content = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        content.addView(title)
        content.addView(status)
        content.addView(rowHolder)
        scroller.addView(content)

        root.addView(scroller, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        root.addView(progress)
        root.addView(playButton)
        setContentView(root)
        root.padForSystemBars()

        GameAssets.Input.entries.forEach { rows[it] = InputRow(it, rowHolder) }
    }

    private fun startPick(input: GameAssets.Input) {
        pickingFor = input
        // Not narrowed by MIME: providers report .z64/.ndd/.n64 inconsistently and an
        // over-tight filter greys out the very file being sought.
        runCatching { pickDocument.launch(arrayOf("*/*")) }.onFailure {
            pickingFor = null
            setBusy(false, "No file picker is available on this device.")
        }
    }

    private fun importInput(input: GameAssets.Input, uri: Uri) {
        setBusy(true, getString(R.string.launcher_verifying, input.label))

        thread(name = "gdx-import") {
            // A picked document is a content:// stream with no path the port can open, and the
            // dumps run to tens of megabytes, so it is staged in the cache first.
            val scratch = File(cacheDir, "picked").apply { deleteRecursively(); mkdirs() }
            val staged = File(scratch, displayName(uri))
            val verdict = runCatching {
                contentResolver.openInputStream(uri).use { stream ->
                    requireNotNull(stream) { "The document provider did not open the file." }
                    FileOutputStream(staged).use { output -> stream.copyTo(output, COPY_BUFFER) }
                }
                GameAssets.importInput(this, input, staged)
            }.getOrElse { GameAssets.Verdict.Rejected(it.message ?: it.toString()) }
            scratch.deleteRecursively()

            runOnUiThread {
                rows.getValue(input).show(verdict)
                setBusy(false, getString(R.string.launcher_intro))
                refreshPlayButton()
            }
        }
    }

    private fun displayName(uri: Uri): String {
        val name = contentResolver
            .query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)
            ?.use { cursor -> if (cursor.moveToFirst()) cursor.getString(0) else null }
        return (name ?: "picked.bin").replace(Regex("""[/\\]"""), "_")
    }

    private fun refreshPlayButton() {
        playButton.isEnabled = rows.values.all { it.accepted }
    }

    /**
     * Builds the three archives, then records that setup finished.
     *
     * The extractor runs on its own thread inside the port and is polled here, so the progress
     * bar tracks the real work rather than a spinner: the cartridge stage counts entries against
     * the archive's known total, and the 64DD stages against theirs.
     */
    private fun beginExtraction() {
        setBusy(true, getString(R.string.launcher_extracting))
        rows.values.forEach { it.setEnabled(false) }
        GameAssets.resetExtraction()
        GameAssets.startExtraction(this)
        handler.post(pollExtraction)
    }

    private val pollExtraction = object : Runnable {
        override fun run() {
            val state = GameAssets.extractionStatus()

            if (!state.finished) {
                val percent = state.percent
                if (percent != null) {
                    progress.isIndeterminate = false
                    progress.progress = percent
                } else {
                    progress.isIndeterminate = true
                }
                status.text = buildString {
                    append(getString(R.string.launcher_extracting))
                    append("\n\n")
                    append(state.stageLabel)
                    if (percent != null) append(" — $percent%")
                    if (state.text.isNotEmpty()) {
                        append("\n")
                        append(state.text)
                    }
                }
                handler.postDelayed(this, POLL_MS)
                return
            }

            val succeeded = state.succeeded
            val failure = state.text
            GameAssets.resetExtraction()

            if (!succeeded) {
                setBusy(false, "Could not build the game archives.\n\n$failure")
                rows.values.forEach { it.setEnabled(true) }
                refreshPlayButton()
                return
            }

            // Recorded only once the archives exist, so an interrupted build leaves setup
            // unfinished rather than marking it done over a missing archive.
            val setupError = GameAssets.completeSetup(this@LauncherActivity)
            if (setupError != null) {
                setBusy(false, setupError)
                rows.values.forEach { it.setEnabled(true) }
                refreshPlayButton()
                return
            }
            startGame()
        }
    }

    /**
     * Both long-running steps keep the screen awake: the extraction can outlast the display
     * timeout, and being killed part-way through wastes the whole wait.
     */
    private fun setBusy(busy: Boolean, message: String) {
        status.text = message
        playButton.isEnabled = !busy && rows.values.all { it.accepted }
        rows.values.forEach { it.setEnabled(!busy) }
        progress.visibility = if (busy) View.VISIBLE else View.GONE
        if (busy) {
            progress.isIndeterminate = true
            window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    private fun startGame() {
        startActivity(Intent(this, MainActivity::class.java))
        finish()

        // This activity has a process to itself and the game runs in the app's main one, so
        // ending this one costs the game nothing and hands back everything the launcher touched.
        // Extraction already runs in a child process that frees its own peak, but this process
        // has still mapped in the whole game library to reach a handful of JNI calls.
        // startActivity() has reached the activity manager by this point, so the game starts
        // either way.
        Runtime.getRuntime().exit(0)
    }

    private companion object {
        const val COPY_BUFFER = 1 shl 17
        const val POLL_MS = 250L

        val ACCEPTED = Color.rgb(126, 217, 140)
        val WARNING = Color.rgb(240, 196, 96)
        val REJECTED = Color.rgb(240, 122, 122)
    }
}
