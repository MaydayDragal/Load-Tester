package com.loadtester.el15

import android.content.Context
import org.json.JSONObject
import java.io.File

/**
 * On-device archive of completed resistance tests. Each record is one JSON
 * file in filesDir/tests/ named by its id; listing reads the directory. All
 * operations are synchronous file I/O on small files (a record is a few KB) —
 * callers on the main thread are fine at this scale, but heavy users can wrap
 * calls in a coroutine.
 */
class TestRepository(context: Context) {

    private val dir = File(context.filesDir, "tests").apply { mkdirs() }

    private fun fileFor(id: String) = File(dir, "$id.json")

    /** Persist (create or overwrite) a record. Returns false on I/O failure. */
    fun save(record: TestRecord): Boolean = try {
        fileFor(record.id).writeText(record.toJson().toString())
        true
    } catch (e: Exception) {
        false
    }

    fun load(id: String): TestRecord? = try {
        val f = fileFor(id)
        if (!f.exists()) null else TestRecord.fromJson(JSONObject(f.readText()))
    } catch (e: Exception) {
        null
    }

    /** All saved records, newest first. Unreadable files are skipped. */
    fun list(): List<TestRecord> =
        (dir.listFiles { f -> f.isFile && f.name.endsWith(".json") } ?: emptyArray())
            .mapNotNull { f ->
                try {
                    TestRecord.fromJson(JSONObject(f.readText()))
                } catch (e: Exception) {
                    null
                }
            }
            .sortedByDescending { it.timestampMs }

    fun delete(id: String): Boolean = fileFor(id).delete()

    fun count(): Int = dir.listFiles { f -> f.name.endsWith(".json") }?.size ?: 0

    /** Update just the notes of an existing record. */
    fun updateNotes(id: String, notes: String): Boolean {
        val rec = load(id) ?: return false
        rec.notes = notes
        return save(rec)
    }
}
