package com.gdiffuser.android

import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.graphics.Typeface
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import kotlin.concurrent.thread

/**
 * Scoped-storage file manager for ROM-hack archives.
 *
 * Building and selecting a hack stays in the engine's ROM Hacks window; this activity replaces
 * its desktop "Open folder" action so prebuilt .o2r archives can be imported from Android's
 * document providers and existing ones can be exported or removed.
 */
class RomHacksActivity : ComponentActivity() {

    private lateinit var listContainer: LinearLayout
    private lateinit var emptyLabel: TextView
    private var dirty = false

    private val importArchives =
        registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
            if (uris.isNullOrEmpty()) return@registerForActivityResult
            runInBackground("Importing ROM hacks…") {
                uris.mapNotNull { HackStore.importArchive(this, it) }
            }
        }

    private var pendingExport: HackStore.Archive? = null
    private val exportArchive =
        registerForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
            val archive = pendingExport ?: return@registerForActivityResult
            pendingExport = null
            if (uri == null) return@registerForActivityResult
            runInBackground("Exporting…") { listOfNotNull(HackStore.export(this, archive, uri)) }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        refresh()
    }

    override fun onResume() {
        super.onResume()
        if (::listContainer.isInitialized) refresh()
    }

    private fun buildUi() {
        val pad = dp(16)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            setBackgroundColor(BACKGROUND)
        }

        root.addView(TextView(this).apply {
            text = getString(R.string.romhacks_title)
            setTextColor(Color.WHITE)
            textSize = 22f
            setTypeface(typeface, Typeface.BOLD)
        })

        root.addView(TextView(this).apply {
            text = "Import, export and delete prebuilt .o2r hack archives here. " +
                "To build one from a patched ROM or choose which hack runs, use the game's " +
                "Workshop > ROM Hacks screen. Only one hack can be selected, and it applies after restart."
            setTextColor(SUBTLE)
            textSize = 12f
            setPadding(0, dp(4), 0, dp(8))
        })

        emptyLabel = TextView(this).apply {
            text = "No ROM-hack archives yet.\n\nImport a prebuilt .o2r here, or build one " +
                "from a patched ROM in the game's ROM Hacks screen."
            setTextColor(SUBTLE)
            gravity = Gravity.CENTER
            setPadding(0, dp(24), 0, dp(24))
        }
        root.addView(emptyLabel)

        listContainer = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(
            ScrollView(this).apply { addView(listContainer) },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        )

        val actions = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            setPadding(0, dp(8), 0, 0)
        }
        actions.addView(button("Import archive") { importArchives.launch(arrayOf("*/*")) })
        actions.addView(button("Packs") { startActivity(Intent(this, ModsActivity::class.java)) })
        actions.addView(View(this), LinearLayout.LayoutParams(0, 1, 1f))
        actions.addView(button("Restart game") { restartGame() })
        actions.addView(button("Done") { finish() })
        root.addView(actions)

        setContentView(root)
        root.padForSystemBars()
    }

    private fun refresh() {
        val archives = HackStore.list(this)
        listContainer.removeAllViews()
        emptyLabel.visibility = if (archives.isEmpty()) View.VISIBLE else View.GONE
        archives.forEachIndexed { index, archive -> listContainer.addView(row(archive, index)) }
    }

    private fun row(archive: HackStore.Archive, index: Int): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(8), dp(6), dp(8), dp(6))
            setBackgroundColor(if (index % 2 == 0) ROW_EVEN else ROW_ODD)
        }

        val label = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        label.addView(TextView(this).apply {
            text = archive.name
            setTextColor(Color.WHITE)
            textSize = 15f
        })
        label.addView(TextView(this).apply {
            text = buildString {
                append(archive.sizeLabel)
                if (archive.selected) append(" · selected for restart")
            }
            setTextColor(if (archive.selected) SELECTED else SUBTLE)
            textSize = 11f
        })
        row.addView(label, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

        row.addView(button("Export") {
            pendingExport = archive
            exportArchive.launch(archive.name)
        })
        row.addView(button("Delete") { confirmDelete(archive) })
        return row
    }

    private fun confirmDelete(archive: HackStore.Archive) {
        val selectedWarning =
            if (archive.selected) "\n\nIt is currently selected; the next launch will fall back to the stock game."
            else ""
        AlertDialog.Builder(this)
            .setTitle("Delete ${archive.name}?")
            .setMessage("This removes it from the device. Exporting first keeps a copy.$selectedWarning")
            .setPositiveButton("Delete") { _, _ -> apply(HackStore.delete(archive)) }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun apply(error: String?) {
        if (error != null) {
            Toast.makeText(this, error, Toast.LENGTH_LONG).show()
        } else {
            dirty = true
        }
        refresh()
    }

    private fun runInBackground(message: String, work: () -> List<String>) {
        val progress = AlertDialog.Builder(this)
            .setMessage(message)
            .setCancelable(false)
            .show()

        thread(name = "romhacks-io") {
            val errors = runCatching(work).getOrElse { listOf(it.message ?: it.toString()) }
            runOnUiThread {
                progress.dismiss()
                if (errors.isEmpty()) dirty = true
                else Toast.makeText(this, errors.joinToString("\n\n"), Toast.LENGTH_LONG).show()
                refresh()
            }
        }
    }

    override fun finish() {
        if (dirty) {
            Toast.makeText(
                this,
                "ROM-hack file changes apply the next time the game starts.",
                Toast.LENGTH_LONG
            ).show()
        }
        super.finish()
    }

    private fun restartGame() {
        val intent = Intent(this, LauncherActivity::class.java)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
        startActivity(intent)
        finishAffinity()
        Runtime.getRuntime().exit(0)
    }

    private fun button(label: String, onClick: () -> Unit) = Button(this).apply {
        text = label
        textSize = 12f
        isAllCaps = false
        minWidth = 0
        minimumWidth = 0
        setPadding(dp(10), dp(4), dp(10), dp(4))
        setOnClickListener { onClick() }
    }

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()

    private companion object {
        val BACKGROUND = Color.rgb(14, 16, 28)
        val SUBTLE = Color.rgb(150, 160, 175)
        val SELECTED = Color.rgb(110, 220, 140)
        val ROW_EVEN = Color.argb(24, 255, 255, 255)
        val ROW_ODD = Color.TRANSPARENT
    }
}
