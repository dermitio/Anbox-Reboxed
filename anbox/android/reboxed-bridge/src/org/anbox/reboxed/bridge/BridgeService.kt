package org.anbox.reboxed.bridge

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ContentValues
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import android.net.LocalSocket
import android.net.LocalSocketAddress
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.IBinder
import android.provider.MediaStore
import android.provider.OpenableColumns
import android.provider.Settings
import android.util.Base64
import android.webkit.MimeTypeMap
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.FileOutputStream
import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import kotlin.concurrent.thread

class BridgeService : Service(), ClipboardManager.OnPrimaryClipChangedListener {
    companion object {
        const val ACTION_INSTALL_URIS = "org.anbox.reboxed.bridge.INSTALL_URIS"
        const val ACTION_EXPORT_URI = "org.anbox.reboxed.bridge.EXPORT_URI"
        private const val SOCKET_PATH = "/dev/anbox_sockets/reboxed_bridge_v1"
        private const val CHANNEL = "reboxed_bridge"
        private const val MAX_FILE = 2L * 1024 * 1024 * 1024
    }

    private data class Transfer(
        val id: String, val name: String, val expectedSize: Long,
        val expectedHash: String, val part: File, val output: BufferedOutputStream,
        val digest: MessageDigest, var received: Long = 0,
        val operation: String, val installId: String?, val index: Int, val count: Int
    )

    @Volatile private var running = false
    @Volatile private var output: DataOutputStream? = null
    @Volatile private var lastRemoteClipboard: String? = null
    private lateinit var clipboard: ClipboardManager
    private val transfers = ConcurrentHashMap<String, Transfer>()

    override fun onCreate() {
        super.onCreate()
        val notifications = getSystemService(NotificationManager::class.java)
        notifications.createNotificationChannel(NotificationChannel(
            CHANNEL, "Reboxed Bridge", NotificationManager.IMPORTANCE_LOW))
        val launch = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val notification = android.app.Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.stat_sys_upload_done)
            .setContentTitle("Reboxed Bridge")
            .setContentText("Connecting host and Android")
            .setContentIntent(launch).setOngoing(true).build()
        startForeground(17, notification)
        clipboard = getSystemService(ClipboardManager::class.java)
        clipboard.addPrimaryClipChangedListener(this)
        running = true
        thread(name = "ReboxedBridgeConnector") { connectorLoop() }
    }

    override fun onDestroy() {
        running = false
        clipboard.removePrimaryClipChangedListener(this)
        output = null
        transfers.values.forEach { it.output.close(); it.part.delete() }
        transfers.clear()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_INSTALL_URIS) {
            val uris = if (Build.VERSION.SDK_INT >= 33)
                intent.getParcelableArrayListExtra("uris", Uri::class.java) ?: arrayListOf()
            else @Suppress("DEPRECATION")
                (intent.getParcelableArrayListExtra<Uri>("uris") ?: arrayListOf())
            if (uris.isNotEmpty()) thread(name = "ReboxedLocalInstaller") {
                installUris(uris)
            }
        } else if (intent?.action == ACTION_EXPORT_URI) {
            val uri = if (Build.VERSION.SDK_INT >= 33)
                intent.getParcelableExtra("uri", Uri::class.java)
            else @Suppress("DEPRECATION") intent.getParcelableExtra<Uri>("uri")
            uri?.let {
                thread(name = "ReboxedFileExport") { exportUri(it) }
            }
        }
        return START_STICKY
    }

    private fun connectorLoop() {
        var delay = 500L
        while (running) {
            val socket = LocalSocket()
            try {
                socket.connect(LocalSocketAddress(SOCKET_PATH, LocalSocketAddress.Namespace.FILESYSTEM))
                val input = DataInputStream(socket.inputStream)
                val stream = DataOutputStream(socket.outputStream)
                output = stream
                send(JSONObject().put("op", "hello").put("role", "guest")
                    .put("id", UUID.randomUUID().toString()))
                val hello = BridgeProtocol.read(input)
                require(hello.optBoolean("ok")) { hello.optString("error") }
                BridgeState.update(this, "connection", "Connected (protocol ${hello.optInt("protocol")})")
                sendAndroidInfo()
                delay = 500L
                while (running) handle(BridgeProtocol.read(input))
            } catch (error: Exception) {
                BridgeState.update(this, "connection", "Disconnected: ${error.message ?: "socket closed"}")
            } finally {
                output = null
                try { socket.close() } catch (_: Exception) {}
            }
            if (running) Thread.sleep(delay)
            delay = minOf(delay * 2, 10_000L)
        }
    }

    @Synchronized private fun send(value: JSONObject) {
        val stream = output ?: return
        BridgeProtocol.write(stream, value)
    }

    private fun hostEvent(value: JSONObject) {
        send(JSONObject().put("op", "host.event").put("event", value)
            .put("id", UUID.randomUUID().toString()))
    }

    override fun onPrimaryClipChanged() {
        if (!BridgeState.enabled(this, "clipboard_android_to_host")) return
        val text = clipboard.primaryClip?.getItemAt(0)?.coerceToText(this)?.toString() ?: return
        if (text.isEmpty() || text.toByteArray().size > 1024 * 1024) return
        val hash = hex(MessageDigest.getInstance("SHA-256").digest(text.toByteArray()))
        if (hash == lastRemoteClipboard) {
            lastRemoteClipboard = null
            return
        }
        send(JSONObject().put("op", "clipboard.push").put("text", text)
            .put("origin", "android").put("digest", hash)
            .put("id", UUID.randomUUID().toString()))
    }

    private fun handle(message: JSONObject) {
        when (message.optString("op")) {
            "clipboard.changed" -> receiveClipboard(message)
            "file.receive", "package.part" -> beginTransfer(message)
            "transfer.chunk" -> transferChunk(message)
            "transfer.end" -> endTransfer(message)
            "transfer.cancel" -> cancelTransfer(message.optString("transfer_id"))
            "intent.launch" -> launchIntent(message)
            "diagnostics.request" -> sendAndroidInfo()
            "native-bridge.results" -> receiveNativeBridgeResults(message)
            "" -> Unit // Response to an event emitted by this service.
            else -> hostEvent(JSONObject().put("op", "protocol.error")
                .put("state", "failed").put("detail", "Unsupported guest operation"))
        }
    }

    private fun receiveClipboard(message: JSONObject) {
        if (!BridgeState.enabled(this, "clipboard_host_to_android")) return
        val text = message.optString("text")
        if (text.isEmpty() || text.toByteArray().size > 1024 * 1024) return
        lastRemoteClipboard = message.optString("digest")
        clipboard.setPrimaryClip(ClipData.newPlainText("Reboxed Bridge", text))
    }

    private fun safeName(raw: String): String {
        require(raw.isNotEmpty() && raw.toByteArray().size <= 255 && raw != "." && raw != "..")
        require(!raw.contains('/') && !raw.contains('\\') && !raw.contains('\u0000'))
        val cleaned = raw.replace(Regex("[^A-Za-z0-9._() +@-]"), "_")
        require(cleaned.isNotEmpty() && cleaned != "." && cleaned != "..")
        return cleaned
    }

    private fun beginTransfer(message: JSONObject) {
        val id = message.getString("transfer_id")
        require(id.matches(Regex("[A-Za-z0-9-]{1,64}")))
        require(!transfers.containsKey(id))
        val size = message.getLong("size")
        require(size in 0..MAX_FILE)
        val hash = message.getString("sha256")
        require(hash.matches(Regex("[0-9a-f]{64}")))
        val root = File(cacheDir, "bridge-incoming").canonicalFile.apply { mkdirs() }
        val part = File(root, "$id.part").canonicalFile
        require(part.parentFile == root && !part.exists())
        val transfer = Transfer(id, safeName(message.getString("name")), size, hash,
            part, BufferedOutputStream(FileOutputStream(part)),
            MessageDigest.getInstance("SHA-256"), operation = message.getString("op"),
            installId = message.optString("install_id").takeIf { it.isNotEmpty() }?.also {
                require(it.matches(Regex("[A-Za-z0-9-]{1,64}")))
            },
            index = message.optInt("index", 0), count = message.optInt("count", 1))
        require(transfer.count in 1..128 && transfer.index in 0 until transfer.count)
        transfers[id] = transfer
        BridgeState.addHistory(this, JSONObject().put("kind", transfer.operation)
            .put("name", transfer.name).put("state", "receiving").put("size", size))
    }

    private fun transferChunk(message: JSONObject) {
        val transfer = transfers[message.getString("transfer_id")]
            ?: throw IllegalArgumentException("Unknown transfer")
        val data = Base64.decode(message.getString("data"), Base64.DEFAULT)
        require(data.isNotEmpty() && data.size <= 256 * 1024)
        require(transfer.received + data.size <= transfer.expectedSize)
        transfer.output.write(data)
        transfer.digest.update(data)
        transfer.received += data.size
        BridgeState.update(this, "transfer_state",
            "${transfer.name}: ${transfer.received}/${transfer.expectedSize}")
    }

    private fun endTransfer(message: JSONObject) {
        val transfer = transfers.remove(message.getString("transfer_id"))
            ?: throw IllegalArgumentException("Unknown transfer")
        transfer.output.flush()
        transfer.output.close()
        val actual = hex(transfer.digest.digest())
        if (transfer.received != transfer.expectedSize || actual != transfer.expectedHash) {
            transfer.part.delete()
            BridgeState.update(this, "transfer_state", "Failed: size or checksum mismatch")
            hostEvent(JSONObject().put("op", "transfer.result").put("state", "failed")
                .put("detail", "size or checksum mismatch"))
            return
        }
        if (transfer.operation == "package.part") finishPackagePart(transfer)
        else finishReceivedFile(transfer)
    }

    private fun finishReceivedFile(transfer: Transfer) {
        var destination: Uri? = null
        try {
            val extension = transfer.name.substringAfterLast('.', "").lowercase()
            val mimeType = MimeTypeMap.getSingleton().getMimeTypeFromExtension(extension)
                ?: "application/octet-stream"
            val values = ContentValues().apply {
                put(MediaStore.MediaColumns.DISPLAY_NAME, transfer.name)
                put(MediaStore.MediaColumns.MIME_TYPE, mimeType)
                put(MediaStore.MediaColumns.RELATIVE_PATH,
                    Environment.DIRECTORY_DOWNLOADS + "/")
                put(MediaStore.MediaColumns.IS_PENDING, 1)
            }
            val collection = MediaStore.Downloads.getContentUri(
                MediaStore.VOLUME_EXTERNAL_PRIMARY)
            val destinationUri = requireNotNull(contentResolver.insert(collection, values)) {
                "MediaStore rejected the download"
            }
            destination = destinationUri
            val copied = transfer.part.inputStream().use { input ->
                requireNotNull(contentResolver.openOutputStream(destinationUri, "w")) {
                    "MediaStore did not open the download"
                }.use { output -> input.copyTo(output, 256 * 1024) }
            }
            require(copied == transfer.expectedSize) {
                "MediaStore wrote $copied of ${transfer.expectedSize} bytes"
            }
            val published = ContentValues().apply {
                put(MediaStore.MediaColumns.IS_PENDING, 0)
            }
            require(contentResolver.update(destinationUri, published, null, null) == 1) {
                "MediaStore did not publish the download"
            }
            val publishedName = try {
                contentResolver.query(destinationUri,
                    arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                    if (cursor.moveToFirst()) cursor.getString(0) else null
                } ?: transfer.name
            } catch (_: Exception) { transfer.name }
            transfer.part.delete()
            BridgeState.update(this, "transfer_state", "Received $publishedName in Downloads")
            BridgeState.addHistory(this, JSONObject().put("kind", "file-received")
                .put("name", publishedName).put("state", "complete")
                .put("size", transfer.expectedSize))
            hostEvent(JSONObject().put("op", "transfer.result").put("state", "complete")
                .put("detail", publishedName))
            if (BridgeState.enabled(this, "auto_open_received", false)) {
                val view = Intent(Intent.ACTION_VIEW, destinationUri).addFlags(
                    Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_GRANT_READ_URI_PERMISSION)
                try { startActivity(view) } catch (_: Exception) {}
            }
        } catch (error: Exception) {
            destination?.let {
                try { contentResolver.delete(it, null, null) } catch (_: Exception) {}
            }
            transfer.part.delete()
            val detail = error.message ?: "unable to publish file"
            BridgeState.update(this, "transfer_state", "Failed to save ${transfer.name}: $detail")
            BridgeState.addHistory(this, JSONObject().put("kind", "file-received")
                .put("name", transfer.name).put("state", "failed")
                .put("size", transfer.expectedSize).put("detail", detail))
            hostEvent(JSONObject().put("op", "transfer.result").put("state", "failed")
                .put("detail", detail))
        }
    }

    private fun finishPackagePart(transfer: Transfer) {
        val installId = requireNotNull(transfer.installId)
        val root = File(cacheDir, "installations/$installId").canonicalFile.apply { mkdirs() }
        val destination = File(root, "%03d-%s".format(transfer.index, transfer.name)).canonicalFile
        require(destination.parentFile == root && transfer.part.renameTo(destination))
        val files = root.listFiles { item -> item.isFile } ?: emptyArray()
        if (files.size == transfer.count) installApks(files.sortedBy { it.name }, installId)
    }

    private fun cancelTransfer(id: String) {
        transfers.remove(id)?.let { it.output.close(); it.part.delete() }
        BridgeState.update(this, "transfer_state", "Cancelled $id")
    }

    private fun installUris(uris: List<Uri>) {
        BridgeState.update(this, "install_state", "Preparing")
        val id = UUID.randomUUID().toString()
        val root = File(cacheDir, "installations/$id").apply { mkdirs() }
        try {
            val files = uris.mapIndexed { index, uri ->
                val destination = File(root, "%03d.apk".format(index))
                contentResolver.openInputStream(uri).use { input ->
                    requireNotNull(input)
                    destination.outputStream().use { output -> input.copyTo(output) }
                }
                destination
            }
            installApks(files, id)
        } catch (error: Exception) {
            BridgeState.update(this, "install_state", "Failed: ${error.message}")
        }
    }

    private fun exportUri(uri: Uri) {
        if (output == null) {
            BridgeState.update(this, "transfer_state", "Failed: host bridge is disconnected")
            return
        }
        val id = UUID.randomUUID().toString()
        val name = contentResolver.query(uri,
            arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else "android-export"
        } ?: "android-export"
        val temporary = File(cacheDir, "bridge-export-$id")
        try {
            val digest = MessageDigest.getInstance("SHA-256")
            var size = 0L
            contentResolver.openInputStream(uri).use { input ->
                requireNotNull(input)
                temporary.outputStream().use { file ->
                    val buffer = ByteArray(256 * 1024)
                    while (true) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        size += count
                        require(size <= MAX_FILE) { "Export exceeds 2 GiB" }
                        digest.update(buffer, 0, count)
                        file.write(buffer, 0, count)
                    }
                }
            }
            send(JSONObject().put("op", "transfer.begin").put("transfer_id", id)
                .put("name", safeName(name)).put("size", size)
                .put("sha256", hex(digest.digest())).put("id", UUID.randomUUID().toString()))
            temporary.inputStream().use { input ->
                val buffer = ByteArray(256 * 1024)
                var sent = 0L
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    send(JSONObject().put("op", "transfer.chunk").put("transfer_id", id)
                        .put("data", Base64.encodeToString(buffer.copyOf(count), Base64.NO_WRAP))
                        .put("id", UUID.randomUUID().toString()))
                    sent += count
                    BridgeState.update(this, "transfer_state", "Exporting $name: $sent/$size")
                }
            }
            send(JSONObject().put("op", "transfer.end").put("transfer_id", id)
                .put("id", UUID.randomUUID().toString()))
            BridgeState.addHistory(this, JSONObject().put("kind", "file-export")
                .put("name", name).put("state", "submitted").put("size", size))
        } catch (error: Exception) {
            BridgeState.update(this, "transfer_state", "Failed: ${error.message}")
        } finally { temporary.delete() }
    }

    private fun installApks(apks: List<File>, installId: String) {
        BridgeState.update(this, "install_state", "Installing")
        var session: PackageInstaller.Session? = null
        try {
            require(apks.isNotEmpty())
            val packages = apks.map {
                packageManager.getPackageArchiveInfo(it.path, 0)?.packageName
                    ?: throw IllegalArgumentException("Package Installer rejected ${it.name}")
            }.toSet()
            require(packages.size == 1) { "Split package identities differ" }
            val parameters = PackageInstaller.SessionParams(
                PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
                setAppPackageName(packages.first())
                setSize(apks.sumOf { it.length() })
            }
            val sessionId = packageManager.packageInstaller.createSession(parameters)
            val activeSession = packageManager.packageInstaller.openSession(sessionId)
            session = activeSession
            apks.forEachIndexed { index, apk ->
                apk.inputStream().use { input ->
                    activeSession.openWrite("%03d-%s".format(index, apk.name), 0, apk.length()).use { output ->
                        input.copyTo(output)
                        activeSession.fsync(output)
                    }
                }
            }
            val callback = Intent(this, InstallResultReceiver::class.java)
                .putExtra("install_id", installId)
            val sender = PendingIntent.getBroadcast(this, sessionId, callback,
                PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE).intentSender
            activeSession.commit(sender)
            activeSession.close()
        } catch (error: Exception) {
            try { session?.abandon() } catch (_: Exception) {}
            try { session?.close() } catch (_: Exception) {}
            BridgeState.update(this, "install_state", "Failed: ${error.message ?: "unknown error"}")
            BridgeState.addHistory(this, JSONObject().put("kind", "package-install")
                .put("state", "failed").put("detail", error.toString()))
        }
    }

    private fun launchIntent(message: JSONObject) {
        val action = message.optString("action", Intent.ACTION_VIEW)
        val intent = Intent(action)
        message.optString("uri").takeIf { it.isNotEmpty() }?.let {
            val uri = Uri.parse(it)
            require(uri.scheme in listOf("content", "https", "http", "market", "package"))
            intent.data = uri
        }
        message.optString("type").takeIf { it.isNotEmpty() }?.let { intent.type = it }
        message.optString("package").takeIf { it.isNotEmpty() }?.let { intent.setPackage(it) }
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        startActivity(intent)
    }

    private fun getprop(name: String): String = try {
        Runtime.getRuntime().exec(arrayOf("/system/bin/getprop", name)).inputStream
            .bufferedReader().use { it.readText().trim() }
    } catch (_: Exception) { "unavailable" }

    private fun commandOutput(vararg command: String): String = try {
        val process = Runtime.getRuntime().exec(command)
        val text = process.inputStream.bufferedReader().use { it.readText() }
        process.waitFor()
        text.take(2048).ifEmpty { "exit=${process.exitValue()}" }
    } catch (error: Exception) { "unavailable: ${error.message}" }

    private fun receiveNativeBridgeResults(message: JSONObject) {
        val checks = message.optJSONObject("verification") ?: JSONObject()
        fun state(abi: String): String {
            val result = checks.optJSONObject(abi) ?: return "Not run"
            return if (result.optBoolean("passed")) "Passed (Android-process JNI call)"
                   else "Failed [${result.optString("state", "unknown")}]: " +
                       result.optString("output", "no output")
        }
        BridgeState.update(this, "arm32_test", state("armeabi-v7a"))
        BridgeState.update(this, "arm64_test", state("arm64-v8a"))
        BridgeState.update(this, "native_bridge_status", message.toString(2))
    }

    private fun sendAndroidInfo() {
        val info = JSONObject()
            .put("release", Build.VERSION.RELEASE)
            .put("sdk", Build.VERSION.SDK_INT)
            .put("supported_abis", JSONArray(Build.SUPPORTED_ABIS.toList()))
            .put("supported_32", JSONArray(Build.SUPPORTED_32_BIT_ABIS.toList()))
            .put("supported_64", JSONArray(Build.SUPPORTED_64_BIT_ABIS.toList()))
            .put("native_bridge", getprop("ro.dalvik.vm.native.bridge"))
            .put("native_bridge_exec", getprop("ro.enable.native.bridge.exec"))
            .put("isa_arm", getprop("ro.dalvik.vm.isa.arm"))
            .put("isa_arm64", getprop("ro.dalvik.vm.isa.arm64"))
            .put("cpu_abilist", getprop("ro.product.cpu.abilist"))
            .put("translator_32_present", File("/system/lib/libndk_translation.so").canRead())
            .put("translator_64_present", File("/system/lib64/libndk_translation.so").canRead())
            .put("linker32", commandOutput("/system/bin/linker", "--list", "/system/bin/sh"))
            .put("linker64", commandOutput("/system/bin/linker64", "--list", "/system/bin/sh"))
            .put("developer_options", Settings.Global.getInt(contentResolver,
                Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, 0))
        BridgeState.update(this, "android_info", info.toString(2))
        hostEvent(JSONObject().put("op", "android.info").put("state", "ready")
            .put("detail", info.toString()))
    }

    private fun hex(data: ByteArray): String = data.joinToString("") { "%02x".format(it) }
}
