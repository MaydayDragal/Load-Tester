package com.loadtester.el15

import android.content.Context
import android.util.AtomicFile
import org.json.JSONObject
import java.io.File

/** Archive of long-running bench sessions (atomic JSON files, like tests). */
class SessionRepository(context: Context) {

    private val dir = File(context.filesDir, "sessions").apply { mkdirs() }

    private fun fileFor(id: String) = File(dir, "$id.json")

    fun save(record: SessionRecord): Boolean {
        val af = AtomicFile(fileFor(record.id))
        val out = try { af.startWrite() } catch (e: Exception) { return false }
        return try {
            out.write(record.toJson().toString().toByteArray(Charsets.UTF_8))
            af.finishWrite(out); true
        } catch (e: Exception) {
            af.failWrite(out); false
        }
    }

    fun load(id: String): SessionRecord? = try {
        val f = fileFor(id)
        if (!f.exists()) null
        else SessionRecord.fromJson(JSONObject(AtomicFile(f).readFully().toString(Charsets.UTF_8)))
    } catch (e: Exception) { null }

    fun list(): List<SessionRecord> =
        (dir.listFiles { f -> f.isFile && f.name.endsWith(".json") } ?: emptyArray())
            .mapNotNull { f ->
                try { SessionRecord.fromJson(JSONObject(AtomicFile(f).readFully().toString(Charsets.UTF_8))) }
                catch (e: Exception) { null }
            }
            .sortedByDescending { it.timestampMs }

    fun delete(id: String): Boolean {
        AtomicFile(fileFor(id)).delete()
        return !fileFor(id).exists()
    }

    fun files(): List<File> =
        (dir.listFiles { f -> f.isFile && f.name.endsWith(".json") } ?: emptyArray()).toList()

    /** Delete oldest sessions beyond [keep]; 0 keeps everything. */
    fun applyRetention(keep: Int) {
        if (keep <= 0) return
        val all = list()
        if (all.size > keep) all.drop(keep).forEach { delete(it.id) }
    }
}
