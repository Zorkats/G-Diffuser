package com.gdiffuser.android

import android.app.AlertDialog
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
 * Save manager: back up, restore and delete saved data.
 *
 * "Saved data" is more than one file here. The cartridge SRAM image, the 64DD disk journal that
 * holds every Expansion Kit save, the achievement record and the ghost library are separate
 * files in two folders, and a backup that takes only some of them is not a backup — which is why
 * Back up everything writes one archive covering the lot, and why that is the button offered
 * first.
 */
class SavesActivity : ComponentActivity() {

    private lateinit var listContainer: LinearLayout
    private lateinit var emptyLabel: TextView

    private val importSave =
        registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
            if (uris.isNullOrEmpty()) return@registerForActivityResult
            runInBackground("Restoring…") { uris.mapNotNull { SaveStore.import(this, it) } }
        }

    /** Set just before the export picker opens, since the contract carries only a filename. */
    private var pendingExport: SaveStore.Save? = null

    private val exportSave =
        registerForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
            val save = pendingExport
            pendingExport = null
            if (uri == null) return@registerForActivityResult
            runInBackground("Backing up…") {
                listOfNotNull(
                    if (save != null) SaveStore.export(this, save, uri) else SaveStore.exportAll(this, uri)
                )
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        refresh()
    }

    private fun buildUi() {
        val pad = dp(16)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            setBackgroundColor(BACKGROUND)
        }

        root.addView(TextView(this).apply {
            text = getString(R.string.saves_title)
            setTextColor(Color.WHITE)
            textSize = 22f
            setTypeface(typeface, Typeface.BOLD)
        })

        root.addView(TextView(this).apply {
            text = "Most of an Expansion Kit game lives in the 64DD disk save, not the cartridge " +
                "save, and ghosts are separate again. Back up everything takes all of it in one " +
                "archive; restoring that archive puts each file back where it belongs.\n\n" +
                "Restore what the game is currently using and it will be read on the next launch."
            setTextColor(SUBTLE)
            textSize = 12f
            setPadding(0, dp(4), 0, dp(8))
        })

        emptyLabel = TextView(this).apply {
            text = "Nothing saved yet.\n\nPlay a race, or restore a backup."
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
        actions.addView(button("Back up everything") {
            pendingExport = null
            exportSave.launch(SaveStore.BUNDLE_NAME)
        })
        actions.addView(button("Restore") { importSave.launch(arrayOf("*/*")) })
        actions.addView(View(this), LinearLayout.LayoutParams(0, 1, 1f))
        actions.addView(button("Done") { finish() })
        root.addView(actions)

        setContentView(root)
        root.padForSystemBars()
    }

    private fun refresh() {
        val saves = SaveStore.list(this)
        listContainer.removeAllViews()
        emptyLabel.visibility = if (saves.isEmpty()) View.VISIBLE else View.GONE

        saves.forEachIndexed { index, save -> listContainer.addView(row(save, index)) }
    }

    private fun row(save: SaveStore.Save, index: Int): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(8), dp(6), dp(8), dp(6))
            setBackgroundColor(if (index % 2 == 0) ROW_EVEN else ROW_ODD)
        }

        val label = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        label.addView(TextView(this).apply {
            text = save.label
            setTextColor(Color.WHITE)
            textSize = 15f
        })
        label.addView(TextView(this).apply {
            text = "${save.relativePath} · ${save.sizeLabel}"
            setTextColor(SUBTLE)
            textSize = 11f
        })
        row.addView(label, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

        row.addView(button("Back up") {
            pendingExport = save
            exportSave.launch(save.file.name)
        })
        row.addView(button("Delete") { confirmDelete(save) })

        return row
    }

    private fun confirmDelete(save: SaveStore.Save) {
        AlertDialog.Builder(this)
            .setTitle("Delete ${save.label}?")
            .setMessage("This cannot be undone. Backing up first keeps a copy.")
            .setPositiveButton("Delete") { _, _ ->
                val error = SaveStore.delete(save)
                if (error != null) Toast.makeText(this, error, Toast.LENGTH_LONG).show()
                refresh()
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    /**
     * Backups and restores move real data around, so they run off the main thread behind a
     * blocking progress dialog.
     */
    private fun runInBackground(message: String, work: () -> List<String>) {
        val progress = AlertDialog.Builder(this)
            .setMessage(message)
            .setCancelable(false)
            .show()

        thread(name = "saves-io") {
            val errors = runCatching(work).getOrElse { listOf(it.message ?: it.toString()) }

            runOnUiThread {
                progress.dismiss()
                if (errors.isNotEmpty()) {
                    Toast.makeText(this, errors.joinToString("\n\n"), Toast.LENGTH_LONG).show()
                }
                refresh()
            }
        }
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
        val ROW_EVEN = Color.argb(24, 255, 255, 255)
        val ROW_ODD = Color.TRANSPARENT
    }
}
