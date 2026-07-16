package com.loadtester.el15

import android.content.Context
import android.util.AtomicFile
import org.json.JSONObject
import java.io.File

/**
 * On-device archive of completed resistance tests. Each record is one JSON
 * file in filesDir/tests/ named by its id; listing reads the directory.
 *
 * Writes go through [AtomicFile] (temp file + fsync + rename), so a process
 * kill or power loss mid-write — e.g. during the notes rewrite in onPause —
 * can never truncate an archived test; the last committed version survives.
 *
 * Individual load/save calls are cheap (a record is a few KB), but [list]
 * parses every record and should be dispatched off the main thread.
 */
class TestRepository(context: Context) {

    private val dir = File(context.filesDir, "tests").apply { mkdirs() }

    private fun fileFor(id: String) = File(dir, "$id.json")

    /** Persist (create or overwrite) a record atomically. Returns false on I/O failure. */
    fun save(record: TestRecord): Boolean {
        val af = AtomicFile(fileFor(record.id))
        val out = try {
            af.startWrite()
        } catch (e: Exception) {
            return false
        }
        return try {
            out.write(record.toJson().toString().toByteArray(Charsets.UTF_8))
            af.finishWrite(out)
            true
        } catch (e: Exception) {
            af.failWrite(out)
            false
        }
    }

    fun load(id: String): TestRecord? = try {
        val f = fileFor(id)
        if (!f.exists()) null
        else TestRecord.fromJson(JSONObject(AtomicFile(f).readFully().toString(Charsets.UTF_8)))
    } catch (e: Exception) {
        null
    }

    /** All saved records, newest first. Unreadable files are skipped. */
    fun list(): List<TestRecord> =
        (dir.listFiles { f -> f.isFile && f.name.endsWith(".json") } ?: emptyArray())
            .mapNotNull { f ->
                try {
                    TestRecord.fromJson(JSONObject(AtomicFile(f).readFully().toString(Charsets.UTF_8)))
                } catch (e: Exception) {
                    null
                }
            }
            .sortedByDescending { it.timestampMs }

    fun delete(id: String): Boolean {
        AtomicFile(fileFor(id)).delete()
        return !fileFor(id).exists()
    }

    fun count(): Int = dir.listFiles { f -> f.name.endsWith(".json") }?.size ?: 0

    /** Update just the notes of an existing record. */
    fun updateNotes(id: String, notes: String): Boolean {
        val rec = load(id) ?: return false
        rec.notes = notes
        return save(rec)
    }
}
