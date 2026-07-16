package com.loadtester.el15

import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.core.content.FileProvider
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.io.OutputStream

/**
 * File delivery: share sheets (via FileProvider) and "save to phone"
 * (public Downloads). On API 29+ Downloads writes go through MediaStore with
 * IS_PENDING semantics (no permission, no half-written files visible); on
 * API 24–28 the legacy public directory is used — the caller must hold
 * WRITE_EXTERNAL_STORAGE (see [needsLegacyWritePermission]) and files are
 * media-scanned so they appear over MTP and in pickers immediately.
 */
object Exporter {

    const val DOWNLOAD_SUBDIR = "EL15 Load Control"

    /** True when saving to Downloads requires the legacy runtime permission. */
    fun needsLegacyWritePermission(): Boolean = Build.VERSION.SDK_INT < Build.VERSION_CODES.Q

    /**
     * Write [fileName] into Downloads/[DOWNLOAD_SUBDIR] using [writeBody].
     * Returns the human-readable location (with the *actual* stored name,
     * which MediaStore may have uniquified) or null on failure.
     */
    private fun saveToDownloadsImpl(
        context: Context,
        fileName: String,
        mime: String,
        writeBody: (OutputStream) -> Unit,
    ): String? {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val resolver = context.contentResolver
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, fileName)
                put(MediaStore.Downloads.MIME_TYPE, mime)
                put(MediaStore.Downloads.RELATIVE_PATH,
                    Environment.DIRECTORY_DOWNLOADS + "/" + DOWNLOAD_SUBDIR)
                put(MediaStore.Downloads.IS_PENDING, 1)
            }
            val uri = try {
                resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
            } catch (e: Exception) {
                null
            } ?: return null
            try {
                resolver.openOutputStream(uri)?.use(writeBody)
                    ?: throw IOException("openOutputStream failed")
                resolver.update(uri, ContentValues().apply {
                    put(MediaStore.Downloads.IS_PENDING, 0)
                }, null, null)
                // MediaStore may have uniquified the name on collision.
                val actual = resolver.query(
                    uri, arrayOf(MediaStore.Downloads.DISPLAY_NAME), null, null, null
                )?.use { c -> if (c.moveToFirst()) c.getString(0) else null } ?: fileName
                "Downloads/$DOWNLOAD_SUBDIR/$actual"
            } catch (e: Exception) {
                // Don't leave an orphaned pending row / ghost file behind.
                try { resolver.delete(uri, null, null) } catch (ignored: Exception) {}
                null
            }
        } else {
            try {
                @Suppress("DEPRECATION")
                val base = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
                val outDir = File(base, DOWNLOAD_SUBDIR).apply { mkdirs() }
                val f = uniqueFile(outDir, fileName)
                FileOutputStream(f).use(writeBody)
                // Without a scan the file is invisible over MTP / in pickers
                // until the next full rescan.
                MediaScannerConnection.scanFile(context, arrayOf(f.absolutePath), arrayOf(mime), null)
                "Downloads/$DOWNLOAD_SUBDIR/${f.name}"
            } catch (e: Exception) {
                null
            }
        }
    }

    fun saveToDownloads(context: Context, fileName: String, mime: String, bytes: ByteArray): String? =
        saveToDownloadsImpl(context, fileName, mime) { it.write(bytes) }

    fun saveBitmapToDownloads(context: Context, fileName: String, bitmap: Bitmap): String? =
        saveToDownloadsImpl(context, fileName, "image/png") { out ->
            // compress returns false on encode failure instead of throwing.
            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)) {
                throw IOException("PNG encode failed")
            }
        }

    /**
     * Paginate a tall report bitmap into an A4 PDF (multi-page, fit-to-width).
     * Rendering to PDF here keeps report generation in one place — the same
     * bitmap drives print, PNG and PDF outputs.
     */
    fun pdfFromBitmap(bitmap: Bitmap): ByteArray {
        val pageW = 595
        val pageH = 842
        val margin = 26f
        val availW = pageW - margin * 2
        val availH = pageH - margin * 2
        val scale = availW / bitmap.width
        val sliceHpx = (availH / scale).toInt().coerceAtLeast(1)
        val doc = android.graphics.pdf.PdfDocument()
        val paint = android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG)
        try {
            var top = 0
            var pageNum = 1
            while (top < bitmap.height) {
                val h = minOf(sliceHpx, bitmap.height - top)
                val page = doc.startPage(
                    android.graphics.pdf.PdfDocument.PageInfo.Builder(pageW, pageH, pageNum).create())
                val src = android.graphics.Rect(0, top, bitmap.width, top + h)
                val dst = android.graphics.RectF(margin, margin, margin + availW, margin + h * scale)
                page.canvas.drawBitmap(bitmap, src, dst, paint)
                doc.finishPage(page)
                top += h
                pageNum++
            }
            val out = java.io.ByteArrayOutputStream()
            doc.writeTo(out)
            return out.toByteArray()
        } finally {
            doc.close()
        }
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
        FileOutputStream(file).use {
            if (!bitmap.compress(Bitmap.CompressFormat.PNG, 100, it)) {
                throw IOException("PNG encode failed")
            }
        }
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
