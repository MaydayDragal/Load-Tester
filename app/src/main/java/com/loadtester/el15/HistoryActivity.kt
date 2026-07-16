package com.loadtester.el15

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.core.widget.doAfterTextChanged
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.chip.ChipGroup
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.textfield.TextInputEditText
import java.io.ByteArrayOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Unified test archive: resistance sweeps and bench sessions in one list,
 * newest first, with free-text search (notes, tags, device), kind filtering,
 * tagging, side-by-side comparison of two records, archive-wide trends, and
 * a one-tap ZIP export of everything.
 */
class HistoryActivity : BaseActivity() {

    /** One list row wrapping either record kind behind a common face. */
    private data class Row(
        val id: String,
        val timestampMs: Long,
        val isSession: Boolean,
        val kind: String,
        val headline: String,
        val summary: String,
        val tag: String,
        val notes: String,
        val device: String,
        val test: TestRecord?,
        val session: SessionRecord?,
    )

    private lateinit var repo: TestRepository
    private lateinit var sessionRepo: SessionRepository
    private lateinit var adapter: RecordAdapter

    private var allRows: List<Row> = emptyList()
    private var query = ""
    private var kindFilter = KIND_ALL
    private val selectedIds = LinkedHashSet<String>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_history)
        findViewById<MaterialToolbar>(R.id.toolbar).setNavigationOnClickListener { finish() }
        repo = TestRepository(this)
        sessionRepo = SessionRepository(this)
        adapter = RecordAdapter()
        findViewById<RecyclerView>(R.id.recordList).apply {
            layoutManager = LinearLayoutManager(this@HistoryActivity)
            adapter = this@HistoryActivity.adapter
        }

        findViewById<TextInputEditText>(R.id.searchInput).doAfterTextChanged {
            query = it?.toString()?.trim().orEmpty()
            applyFilter()
        }
        findViewById<ChipGroup>(R.id.kindGroup).setOnCheckedStateChangeListener { _, checked ->
            kindFilter = when (checked.firstOrNull()) {
                R.id.kindSweeps -> KIND_SWEEPS
                R.id.kindSessions -> KIND_SESSIONS
                else -> KIND_ALL
            }
            applyFilter()
        }
        findViewById<MaterialButton>(R.id.compareButton).setOnClickListener { showCompare() }
        findViewById<MaterialButton>(R.id.trendsButton).setOnClickListener {
            startActivity(Intent(this, TrendsActivity::class.java))
        }
        findViewById<MaterialButton>(R.id.exportAllButton).setOnClickListener { exportAll() }
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun refresh() {
        // Reading + parsing every archived record stays off the UI thread.
        lifecycleScope.launch {
            val rows = withContext(Dispatchers.IO) {
                val tests = repo.list().map { r ->
                    Row(
                        id = r.id, timestampMs = r.timestampMs, isSession = false,
                        kind = "RESISTANCE TEST",
                        headline = formatOhm(r.resistanceOhm) + if (!r.reliable) "  ⚠" else "",
                        summary = "Voc %.2f V · peak %.2f A · %d pts · R² %.4f"
                            .format(r.openCircuitVoltage, r.maxTestCurrent, r.samples.size, r.rSquared),
                        tag = r.tag, notes = r.notes, device = r.deviceLabel,
                        test = r, session = null,
                    )
                }
                val sessions = sessionRepo.list().map { s ->
                    Row(
                        id = s.id, timestampMs = s.timestampMs, isSession = true,
                        kind = s.typeName().uppercase(Locale.US),
                        headline = s.headline(),
                        summary = s.params.entries.take(3)
                            .joinToString(" · ") { "${it.key} ${fmtNum(it.value)}" } +
                            " · ${s.points.size} pts",
                        tag = s.tag, notes = s.notes, device = s.deviceLabel,
                        test = null, session = s,
                    )
                }
                (tests + sessions).sortedByDescending { it.timestampMs }
            }
            allRows = rows
            selectedIds.retainAll(rows.map { it.id }.toSet())
            applyFilter()
        }
    }

    private fun applyFilter() {
        val q = query.lowercase(Locale.getDefault())
        val filtered = allRows.filter { row ->
            val kindOk = when (kindFilter) {
                KIND_SWEEPS -> !row.isSession
                KIND_SESSIONS -> row.isSession
                else -> true
            }
            val queryOk = q.isEmpty() || listOf(row.tag, row.notes, row.device, row.kind, row.headline)
                .any { it.lowercase(Locale.getDefault()).contains(q) }
            kindOk && queryOk
        }
        adapter.submit(filtered)
        findViewById<View>(R.id.emptyText).visibility =
            if (filtered.isEmpty()) View.VISIBLE else View.GONE
        updateCompareButton()
    }

    private fun updateCompareButton() {
        findViewById<MaterialButton>(R.id.compareButton).isEnabled = selectedIds.size == 2
    }

    // ---- Row actions ---------------------------------------------------------
    private fun openRow(row: Row) {
        if (row.isSession) SessionResultActivity.start(this, row.id)
        else startActivity(Intent(this, ResultActivity::class.java)
            .putExtra(ResultActivity.EXTRA_RECORD_ID, row.id))
    }

    private fun longPressMenu(row: Row) {
        val selectLabel = if (row.id in selectedIds) "Deselect" else "Select for compare"
        MaterialAlertDialogBuilder(this)
            .setTitle(row.headline)
            .setItems(arrayOf(selectLabel, getString(R.string.history_edit_tag),
                getString(R.string.history_delete))) { _, which ->
                when (which) {
                    0 -> toggleSelect(row)
                    1 -> editTag(row)
                    2 -> confirmDelete(row)
                }
            }
            .show()
    }

    private fun toggleSelect(row: Row) {
        if (row.id in selectedIds) selectedIds.remove(row.id)
        else {
            if (selectedIds.size >= 2) selectedIds.remove(selectedIds.first())
            selectedIds.add(row.id)
        }
        adapter.notifyDataSetChanged()
        updateCompareButton()
    }

    private fun editTag(row: Row) {
        val edit = TextInputEditText(this).apply {
            setText(row.tag)
            hint = getString(R.string.record_tag_hint)
        }
        val pad = (resources.displayMetrics.density * 20).toInt()
        val holder = android.widget.FrameLayout(this).apply {
            setPadding(pad, pad / 2, pad, 0)
            addView(edit)
        }
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.history_edit_tag)
            .setView(holder)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                val tag = edit.text?.toString()?.trim().orEmpty()
                if (row.isSession) row.session?.let { it.tag = tag; sessionRepo.save(it) }
                else row.test?.let { it.tag = tag; repo.save(it) }
                refresh()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun confirmDelete(row: Row) {
        val (title, msg) =
            if (row.isSession) R.string.history_delete_session to
                getString(R.string.history_delete_session_msg, row.kind.lowercase(Locale.US))
            else R.string.history_delete to getString(R.string.history_delete_msg, row.headline)
        MaterialAlertDialogBuilder(this)
            .setTitle(title)
            .setMessage(msg)
            .setPositiveButton(R.string.history_delete) { _, _ ->
                if (row.isSession) sessionRepo.delete(row.id) else repo.delete(row.id)
                selectedIds.remove(row.id)
                refresh()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    // ---- Compare ---------------------------------------------------------------
    private fun showCompare() {
        val rows = selectedIds.mapNotNull { id -> allRows.firstOrNull { it.id == id } }
        if (rows.size != 2) return
        val (a, b) = rows.sortedBy { it.timestampMs }
        val sb = StringBuilder()
        val fmt = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault())
        fun line(label: String, va: String, vb: String) =
            sb.append("%-12s %14s %14s\n".format(label.take(12), va.take(14), vb.take(14)))
        fun numLine(label: String, na: Float?, nb: Float?, fmtStr: String) {
            if (na == null && nb == null) return
            val d = if (na != null && nb != null)
                "  Δ " + fmtStr.format(nb - na) else ""
            line(label, na?.let { fmtStr.format(it) } ?: "—", (nb?.let { fmtStr.format(it) } ?: "—"))
            if (d.isNotEmpty()) sb.append("%-12s %30s\n".format("", d))
        }

        line("", "A (older)", "B (newer)")
        sb.append("-".repeat(42)).append("\n")
        line("Date", fmt.format(Date(a.timestampMs)), fmt.format(Date(b.timestampMs)))
        line("Kind", a.kind.take(14), b.kind.take(14))
        line("Tag", a.tag.ifEmpty { "—" }, b.tag.ifEmpty { "—" })
        sb.append("\n")

        if (!a.isSession && !b.isSession) {
            val ta = a.test!!; val tb = b.test!!
            numLine("R (mΩ)", ta.resistanceOhm * 1000f, tb.resistanceOhm * 1000f, "%.1f")
            numLine("Voc (V)", ta.openCircuitVoltage, tb.openCircuitVoltage, "%.3f")
            numLine("R²", ta.rSquared, tb.rSquared, "%.4f")
            numLine("Peak A", ta.maxTestCurrent, tb.maxTestCurrent, "%.2f")
            numLine("Peak W", ta.peakPower, tb.peakPower, "%.1f")
            numLine("Max °C", ta.maxTemp, tb.maxTemp, "%.1f")
        } else if (a.isSession && b.isSession) {
            val sa = a.session!!; val sb2 = b.session!!
            val keys = LinkedHashSet<String>().apply { addAll(sa.metrics.keys); addAll(sb2.metrics.keys) }
            for (k in keys) numLine(k.take(12), sa.metrics[k], sb2.metrics[k], "%.2f")
        } else {
            sb.append("Different record kinds — showing headline only.\n")
            line("Result", a.headline, b.headline)
        }

        val tv = TextView(this).apply {
            typeface = android.graphics.Typeface.MONOSPACE
            textSize = 12f
            val pad = (resources.displayMetrics.density * 20).toInt()
            setPadding(pad, pad / 2, pad, 0)
            text = sb.toString().trimEnd()
            setTextIsSelectable(true)
        }
        val scroller = android.widget.HorizontalScrollView(this).apply { addView(tv) }
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.compare_title)
            .setView(scroller)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    // ---- Bulk export -------------------------------------------------------------
    private fun exportAll() {
        lifecycleScope.launch {
            val bytes = withContext(Dispatchers.IO) {
                try {
                    val out = ByteArrayOutputStream()
                    ZipOutputStream(out).use { zip ->
                        for (f in repo.files()) {
                            zip.putNextEntry(ZipEntry("tests/${f.name}"))
                            zip.write(f.readBytes())
                            zip.closeEntry()
                        }
                        for (f in sessionRepo.files()) {
                            zip.putNextEntry(ZipEntry("sessions/${f.name}"))
                            zip.write(f.readBytes())
                            zip.closeEntry()
                        }
                        // CSV flavours alongside the raw JSON, ready for spreadsheets.
                        for (r in repo.list()) {
                            zip.putNextEntry(ZipEntry("csv/el15-test-${r.id}.csv"))
                            zip.write(buildTestCsv(r).toByteArray())
                            zip.closeEntry()
                        }
                        for (s in sessionRepo.list()) {
                            zip.putNextEntry(ZipEntry("csv/el15-session-${s.id}.csv"))
                            zip.write(s.toCsv().toByteArray())
                            zip.closeEntry()
                        }
                    }
                    out.toByteArray()
                } catch (e: Exception) {
                    null
                }
            }
            if (bytes == null || bytes.isEmpty()) { toast("Nothing to export"); return@launch }
            val stamp = SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(Date())
            try {
                Exporter.share(this@HistoryActivity, "el15-archive-$stamp.zip", "application/zip",
                    bytes, "EL15 test archive")
            } catch (e: Exception) { toast("Export failed: ${e.message}") }
        }
    }

    private fun buildTestCsv(r: TestRecord): String {
        val sb = StringBuilder()
        sb.append("# EL15 circuit resistance test ${r.id}\n")
        sb.append("# resistance_ohm,%.6f\n".format(Locale.US, r.resistanceOhm))
        sb.append("# open_circuit_v,%.4f\n".format(Locale.US, r.openCircuitVoltage))
        sb.append("# r_squared,%.6f\n".format(Locale.US, r.rSquared))
        sb.append("step,current_A,voltage_V,power_W,temp_C,fan\n")
        r.samples.forEachIndexed { idx, s ->
            sb.append("%d,%.4f,%.4f,%.4f,%.2f,%d\n".format(
                Locale.US, idx + 1, s.current, s.voltage, s.power, s.temperature, s.fanSpeed))
        }
        return sb.toString()
    }

    private fun fmtNum(v: Float): String =
        if (v == v.toLong().toFloat()) v.toLong().toString()
        else String.format(Locale.US, "%.2f", v)

    private fun formatOhm(ohm: Float): String = when {
        ohm <= 0f -> "n/a"
        ohm < 1f -> "%.1f mΩ".format(ohm * 1000f)
        else -> "%.3f Ω".format(ohm)
    }

    private fun toast(m: String) =
        android.widget.Toast.makeText(this, m, android.widget.Toast.LENGTH_LONG).show()

    // ---- Adapter -----------------------------------------------------------------
    private inner class RecordAdapter : RecyclerView.Adapter<RecordAdapter.VH>() {
        private val items = ArrayList<Row>()

        fun submit(rows: List<Row>) {
            items.clear(); items.addAll(rows)
            notifyDataSetChanged()
        }

        inner class VH(v: View) : RecyclerView.ViewHolder(v) {
            val kind: TextView = v.findViewById(R.id.recordKind)
            val headline: TextView = v.findViewById(R.id.recordResistance)
            val summary: TextView = v.findViewById(R.id.recordSummary)
            val meta: TextView = v.findViewById(R.id.recordMeta)
            val delete: View = v.findViewById(R.id.recordDelete)
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH =
            VH(LayoutInflater.from(parent.context).inflate(R.layout.item_test_record, parent, false))

        override fun getItemCount(): Int = items.size

        override fun onBindViewHolder(holder: VH, position: Int) {
            val r = items[position]
            val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault())
                .format(Date(r.timestampMs))
            val selected = r.id in selectedIds
            holder.kind.text = (if (selected) "✓ SELECTED · " else "") + r.kind
            holder.headline.text = r.headline
            holder.summary.text = r.summary
            holder.meta.text = buildString {
                append(stamp).append(" · ").append(r.device)
                if (r.tag.isNotBlank()) append(" · #").append(r.tag)
                if (r.notes.isNotBlank()) append(" · ").append(r.notes.take(40))
            }
            holder.itemView.alpha = if (selectedIds.isEmpty() || selected) 1f else 0.55f
            holder.itemView.setOnClickListener { openRow(r) }
            holder.itemView.setOnLongClickListener { longPressMenu(r); true }
            holder.delete.setOnClickListener { confirmDelete(r) }
        }
    }

    companion object {
        private const val KIND_ALL = 0
        private const val KIND_SWEEPS = 1
        private const val KIND_SESSIONS = 2
    }
}
