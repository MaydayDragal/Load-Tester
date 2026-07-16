package com.loadtester.el15

import android.content.Intent
import android.os.Bundle
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * The test archive: every completed resistance test is saved on-device and
 * listed here, newest first. Tap a row to reopen its full results/report;
 * the trash icon deletes it (with confirmation).
 */
class HistoryActivity : BaseActivity() {

    private lateinit var repo: TestRepository
    private lateinit var adapter: RecordAdapter

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_history)
        findViewById<MaterialToolbar>(R.id.toolbar).setNavigationOnClickListener { finish() }
        repo = TestRepository(this)
        adapter = RecordAdapter()
        findViewById<RecyclerView>(R.id.recordList).apply {
            layoutManager = LinearLayoutManager(this@HistoryActivity)
            adapter = this@HistoryActivity.adapter
        }
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun refresh() {
        // list() reads and parses every archived record — keep it off the UI
        // thread so a large archive can't freeze History on entry.
        lifecycleScope.launch {
            val records = withContext(Dispatchers.IO) { repo.list() }
            adapter.submit(records)
            findViewById<View>(R.id.emptyText).visibility =
                if (records.isEmpty()) View.VISIBLE else View.GONE
        }
    }

    private fun confirmDelete(record: TestRecord) {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.history_delete)
            .setMessage(getString(R.string.history_delete_msg, formatOhm(record.resistanceOhm)))
            .setPositiveButton(R.string.history_delete) { _, _ ->
                repo.delete(record.id)
                refresh()
            }
            .setNegativeButton(R.string.cancel, null)
            .show()
    }

    private fun formatOhm(ohm: Float): String = when {
        ohm <= 0f -> "n/a"
        ohm < 1f -> "%.1f mΩ".format(ohm * 1000f)
        else -> "%.3f Ω".format(ohm)
    }

    private inner class RecordAdapter : RecyclerView.Adapter<RecordAdapter.VH>() {
        private val items = ArrayList<TestRecord>()

        fun submit(records: List<TestRecord>) {
            items.clear(); items.addAll(records)
            notifyDataSetChanged()
        }

        inner class VH(v: View) : RecyclerView.ViewHolder(v) {
            val resistance: android.widget.TextView = v.findViewById(R.id.recordResistance)
            val summary: android.widget.TextView = v.findViewById(R.id.recordSummary)
            val meta: android.widget.TextView = v.findViewById(R.id.recordMeta)
            val delete: View = v.findViewById(R.id.recordDelete)
        }

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): VH =
            VH(LayoutInflater.from(parent.context).inflate(R.layout.item_test_record, parent, false))

        override fun getItemCount(): Int = items.size

        override fun onBindViewHolder(holder: VH, position: Int) {
            val r = items[position]
            val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.getDefault())
                .format(Date(r.timestampMs))
            holder.resistance.text = formatOhm(r.resistanceOhm) +
                if (!r.reliable) "  ⚠" else ""
            holder.summary.text = "Voc %.2f V · peak %.2f A · %d pts · R² %.4f"
                .format(r.openCircuitVoltage, r.maxTestCurrent, r.samples.size, r.rSquared)
            holder.meta.text = "$stamp · ${r.deviceLabel}" +
                if (r.notes.isNotBlank()) " · ${r.notes.take(40)}" else ""
            holder.itemView.setOnClickListener {
                startActivity(Intent(this@HistoryActivity, ResultActivity::class.java).apply {
                    putExtra(ResultActivity.EXTRA_RECORD_ID, r.id)
                })
            }
            holder.delete.setOnClickListener { confirmDelete(r) }
        }
    }
}
