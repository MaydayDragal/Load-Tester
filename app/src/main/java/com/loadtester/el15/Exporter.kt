package com.loadtester.el15

import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.core.content.FileProvider
import java.io.File
import java.io.FileOutputStream
import java.io.OutputStream

/**
 * File delivery: share sheets (via FileProvider) and "save to phone"
 * (public Downloads). On API 29+ Downloads writes go through MediaStore and
 * need no permission; on API 24–28 the legacy public directory is used and the
 * caller must hold WRITE_EXTERNAL_STORAGE (see [needsLegacyWritePermission]).
 */
object Exporter {

    const val DOWNLOAD_SUBDIR = "EL15 Load Control"

    /** True when saving to Downloads requires the legacy runtime permission. */
    fun needsLegacyWritePermission(): Boolean = Build.VERSION.SDK_INT < Build.VERSION_CODES.Q

    /**
     * Write [bytes] as [fileName] into Downloads/[DOWNLOAD_SUBDIR].
     * Returns a human-readable location on success, null on failure.
     */
    fun saveToDownloads(context: Context, fileName: String, mime: String, bytes: ByteArray): String? =
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val values = ContentValues().apply {
                    put(MediaStore.Downloads.DISPLAY_NAME, fileName)
                    put(MediaStore.Downloads.MIME_TYPE, mime)
                    put(MediaStore.Downloads.RELATIVE_PATH,
                        Environment.DIRECTORY_DOWNLOADS + "/" + DOWNLOAD_SUBDIR)
                }
                val resolver = context.contentResolver
                val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                    ?: throw IllegalStateException("MediaStore insert failed")
                resolver.openOutputStream(uri)?.use { it.write(bytes) }
                    ?: throw IllegalStateException("openOutputStream failed")
                "Downloads/$DOWNLOAD_SUBDIR/$fileName"
            } else {
                @Suppress("DEPRECATION")
                val base = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
                val outDir = File(base, DOWNLOAD_SUBDIR).apply { mkdirs() }
                val f = uniqueFile(outDir, fileName)
                FileOutputStream(f).use { it.write(bytes) }
                "Downloads/$DOWNLOAD_SUBDIR/${f.name}"
            }
        } catch (e: Exception) {
            null
        }

    fun saveBitmapToDownloads(context: Context, fileName: String, bitmap: Bitmap): String? =
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                val values = ContentValues().apply {
                    put(MediaStore.Downloads.DISPLAY_NAME, fileName)
                    put(MediaStore.Downloads.MIME_TYPE, "image/png")
                    put(MediaStore.Downloads.RELATIVE_PATH,
                        Environment.DIRECTORY_DOWNLOADS + "/" + DOWNLOAD_SUBDIR)
                }
                val resolver = context.contentResolver
                val uri = resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                    ?: throw IllegalStateException("MediaStore insert failed")
                resolver.openOutputStream(uri)?.use { out: OutputStream ->
                    bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
                } ?: throw IllegalStateException("openOutputStream failed")
                "Downloads/$DOWNLOAD_SUBDIR/$fileName"
            } else {
                @Suppress("DEPRECATION")
                val base = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
                val outDir = File(base, DOWNLOAD_SUBDIR).apply { mkdirs() }
                val f = uniqueFile(outDir, fileName)
                FileOutputStream(f).use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
                "Downloads/$DOWNLOAD_SUBDIR/${f.name}"
            }
        } catch (e: Exception) {
            null
        }

    /** Share [bytes] as [fileName] via the system share sheet (cache + FileProvider). */
    fun share(context: Context, fileName: String, mime: String, bytes: ByteArray, subject: String) {
        val dir = File(context.cacheDir, "shared").apply { mkdirs() }
        val file = File(dir, fileName)
        FileOutputStream(file).use { it.write(bytes) }
        shareFile(context, file, mime, subject)
    }

    fun shareBitmap(context: Context, fileName: String, bitmap: Bitmap, subject: String) {
        val dir = File(context.cacheDir, "shared").apply { mkdirs() }
        val file = File(dir, fileName)
        FileOutputStream(file).use { bitmap.compress(Bitmap.CompressFormat.PNG, 100, it) }
        shareFile(context, file, "image/png", subject)
    }

    private fun shareFile(context: Context, file: File, mime: String, subject: String) {
        val uri: Uri = FileProvider.getUriForFile(context, "${context.packageName}.fileprovider", file)
        val send = Intent(Intent.ACTION_SEND).apply {
            type = mime
            putExtra(Intent.EXTRA_STREAM, uri)
            putExtra(Intent.EXTRA_SUBJECT, subject)
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        context.startActivity(Intent.createChooser(send, subject))
    }

    private fun uniqueFile(dir: File, fileName: String): File {
        var f = File(dir, fileName)
        if (!f.exists()) return f
        val dot = fileName.lastIndexOf('.')
        val stem = if (dot > 0) fileName.substring(0, dot) else fileName
        val ext = if (dot > 0) fileName.substring(dot) else ""
        var k = 1
        while (f.exists()) {
            f = File(dir, "$stem ($k)$ext")
            k++
        }
        return f
    }
}
