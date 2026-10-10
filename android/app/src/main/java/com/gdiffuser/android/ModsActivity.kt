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
 * Pack manager: import, export and delete what is in the mods folder.
 *
 * Only the files. Which packs are enabled and in what order they mount belongs to the game's own
 * Workshop menu, which owns the conflict rules that go with it; this screen shows the state it
 * recorded but does not edit it.
 *
 * Packs are mounted once when the game starts, so anything changed here takes effect on the next
 * launch — hence the restart button rather than pretending it applied live.
 */
class ModsActivity : ComponentActivity() {

    private lateinit var listContainer: LinearLayout
    private lateinit var emptyLabel: TextView
    private lateinit var texturePackNotice: TextView
    private var dirty = false

    private val importArchives =
        registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
            if (uris.isNullOrEmpty()) return@registerForActivityResult
            runInBackground("Importing…") {
                uris.mapNotNull { ModStore.importArchive(this, it) }
            }
        }

    /** Set just before the export picker opens, since the contract carries only a filename. */
    private var pendingExport: ModStore.Mod? = null

    private val exportMod =
        registerForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")) { uri ->
            val mod = pendingExport ?: return@registerForActivityResult
            pendingExport = null
            if (uri == null) return@registerForActivityResult
            runInBackground("Exporting…") { listOfNotNull(ModStore.export(this, mod, uri)) }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
    }

    /** Also the first fill. Coming back from the game, the switch may have been flipped in the Workshop menu. */
    override fun onResume() {
        super.onResume()
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
            text = getString(R.string.mods_title)
            setTextColor(Color.WHITE)
            textSize = 22f
            setTypeface(typeface, Typeface.BOLD)
        })

        root.addView(TextView(this).apply {
            text = "Texture packs and other mods are .o2r files. Import, export and delete them here.\n" +
                "Turn packs on or off, and set their load order, in the game's own Workshop menu — " +
                "it owns both, and applies them on restart."
            setTextColor(SUBTLE)
            textSize = 12f
            setPadding(0, dp(4), 0, dp(8))
        })

        // The master switch is off by default and gates every pack, so without this an import
        // looks like it worked and then changes nothing in game.
        texturePackNotice = TextView(this).apply {
            text = "Texture packs are switched OFF, so no pack will load.\n" +
                "Turn them on in game: Menu → Workshop → Mods → Enable texture packs, then restart."
            setTextColor(WARNING)
            setBackgroundColor(WARNING_BACKGROUND)
            textSize = 13f
            setPadding(dp(10), dp(8), dp(10), dp(8))
        }
        root.addView(texturePackNotice)

        emptyLabel = TextView(this).apply {
            text = "No packs yet.\n\nImport an .o2r texture pack — or copy one straight into\n" +
                "${GameAssets.modsDir(this@ModsActivity)}"
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
        actions.addView(button("Import pack") { importArchives.launch(arrayOf("*/*")) })
        actions.addView(button("ROM hacks") { startActivity(Intent(this, RomHacksActivity::class.java)) })
        actions.addView(button("Saves") { startActivity(Intent(this, SavesActivity::class.java)) })
        actions.addView(View(this), LinearLayout.LayoutParams(0, 1, 1f))
        actions.addView(button("Restart game") { restartGame() })
        actions.addView(button("Done") { finish() })
        root.addView(actions)

        setContentView(root)
        root.padForSystemBars()
    }

    private fun refresh() {
        val mods = ModStore.list(this)
        listContainer.removeAllViews()
        emptyLabel.visibility = if (mods.isEmpty()) View.VISIBLE else View.GONE
        texturePackNotice.visibility = if (ModStore.texturePacksEnabled(this)) View.GONE else View.VISIBLE

        mods.forEachIndexed { index, mod -> listContainer.addView(row(mod, index)) }
    }

    private fun row(mod: ModStore.Mod, index: Int): View {
        val row = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(8), dp(6), dp(8), dp(6))
            setBackgroundColor(if (index % 2 == 0) ROW_EVEN else ROW_ODD)
        }

        val label = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        label.addView(TextView(this).apply {
            text = mod.name
            setTextColor(if (mod.enabled) Color.WHITE else SUBTLE)
            textSize = 15f
        })
        label.addView(TextView(this).apply {
            // The position is the mount order, which is what decides who wins where two packs
            // touch the same resource — worth showing, because it is not alphabetical.
            text = buildString {
                append("#${index + 1} · ")
                append(mod.sizeLabel)
                if (!mod.enabled) append(" · disabled")
            }
            setTextColor(SUBTLE)
            textSize = 11f
        })
        row.addView(label, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))

        row.addView(button("Export") {
            pendingExport = mod
            exportMod.launch(mod.name)
        })
        row.addView(button("Delete") { confirmDelete(mod) })

        return row
    }

    private fun confirmDelete(mod: ModStore.Mod) {
        AlertDialog.Builder(this)
            .setTitle("Delete ${mod.name}?")
            .setMessage("This removes it from the device. Exporting first keeps a copy.")
            .setPositiveButton("Delete") { _, _ -> apply(ModStore.delete(mod)) }
            .setNegativeButton("Cancel", null)
            .show()
    }

    /** Applies the result of a store operation: report the error, or refresh. */
    private fun apply(error: String?) {
        if (error != null) {
            Toast.makeText(this, error, Toast.LENGTH_LONG).show()
        } else {
            dirty = true
        }
        refresh()
    }

    /**
     * Imports and exports move real data around, so they run off the main thread behind a
     * blocking progress dialog.
     */
    private fun runInBackground(message: String, work: () -> List<String>) {
        val progress = AlertDialog.Builder(this)
            .setMessage(message)
            .setCancelable(false)
            .show()

        thread(name = "mods-io") {
            val errors = runCatching(work).getOrElse { listOf(it.message ?: it.toString()) }

            runOnUiThread {
                progress.dismiss()
                if (errors.isEmpty()) {
                    dirty = true
                } else {
                    Toast.makeText(this, errors.joinToString("\n\n"), Toast.LENGTH_LONG).show()
                }
                refresh()
            }
        }
    }

    override fun finish() {
        if (dirty) {
            Toast.makeText(this, "Pack changes apply the next time the game starts.", Toast.LENGTH_LONG).show()
        }
        super.finish()
    }

    /**
     * Packs are mounted once during engine start-up, so the process has to go. Relaunching
     * through the launcher picks the new set up on the way back in — and, because the launcher
     * re-checks the archives, also catches a pack import that arrived alongside a cleared
     * game directory.
     */
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
        val ROW_EVEN = Color.argb(24, 255, 255, 255)
        val ROW_ODD = Color.TRANSPARENT
        val WARNING = Color.rgb(255, 205, 110)
        val WARNING_BACKGROUND = Color.argb(48, 255, 170, 0)
    }
}
