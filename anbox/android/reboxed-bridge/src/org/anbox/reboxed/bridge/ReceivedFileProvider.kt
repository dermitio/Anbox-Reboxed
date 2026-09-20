package org.anbox.reboxed.bridge

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.OpenableColumns
import java.io.File
import java.io.FileNotFoundException

class ReceivedFileProvider : ContentProvider() {
    private fun file(uri: Uri): File {
        require(uri.pathSegments.size == 1) { "Malformed content URI" }
        val name = uri.pathSegments[0]
        require(name.isNotEmpty() && name != "." && name != ".." &&
                !name.contains('/') && !name.contains('\\') && !name.contains('\u0000')) {
            "Unsafe content URI"
        }
        val root = File(requireNotNull(context).filesDir, "received").canonicalFile
        val target = File(root, name).canonicalFile
        require(target.parentFile == root && target.isFile) { "File is outside received storage" }
        return target
    }

    override fun onCreate() = true
    override fun getType(uri: Uri): String =
        requireNotNull(context).contentResolver.getType(Uri.parse("file://${file(uri).name}"))
            ?: "application/octet-stream"

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        if (mode != "r") throw FileNotFoundException("Received files are read-only")
        return ParcelFileDescriptor.open(file(uri), ParcelFileDescriptor.MODE_READ_ONLY)
    }

    override fun query(uri: Uri, projection: Array<out String>?, selection: String?,
                       selectionArgs: Array<out String>?, sortOrder: String?): Cursor {
        val target = file(uri)
        return MatrixCursor(arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE)).apply {
            addRow(arrayOf(target.name, target.length()))
        }
    }

    override fun insert(uri: Uri, values: ContentValues?): Uri? =
        throw UnsupportedOperationException("read-only")
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0
    override fun update(uri: Uri, values: ContentValues?, selection: String?,
                        selectionArgs: Array<out String>?) = 0
}
