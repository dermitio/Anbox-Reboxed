package org.anbox.reboxed.bridge

import android.app.Activity
import android.app.AlertDialog
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.OpenableColumns
import android.view.Gravity
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import java.util.zip.ZipInputStream

class MainActivity : Activity() {
    private val installRequest = 100
    private val exportRequest = 101
    private lateinit var content: LinearLayout
    private val changes = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) = render()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val scroll = ScrollView(this)
        content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(20), dp(24), dp(20), dp(32))
        }
        scroll.addView(content)
        setContentView(scroll)
    }

    override fun onResume() {
        super.onResume()
        registerReceiver(changes, IntentFilter(BridgeState.ACTION_CHANGED), RECEIVER_NOT_EXPORTED)
        render()
    }

    override fun onPause() {
        unregisterReceiver(changes)
        super.onPause()
    }

    private fun render() {
        content.removeAllViews()
        title("Reboxed Bridge")
        value("Host bridge", BridgeState.get(this, "connection", "Disconnected"))

        section("Guest Android and ABI information")
        value("Android", "${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
        value("Guest ABIs", Build.SUPPORTED_ABIS.joinToString())
        detail(BridgeState.get(this, "android_info", "Diagnostics have not been received"))

        section("Native Bridge")
        val diagnostics = parseInfo()
        value("Translator", diagnostics.optString("native_bridge", "Unavailable"))
        value("ARM32 ISA", diagnostics.optString("isa_arm", "Unavailable"))
        value("ARM64 ISA", diagnostics.optString("isa_arm64", "Unavailable"))
        value("ARM32 self-test", BridgeState.get(this, "arm32_test", "Not run"))
        value("ARM64 self-test", BridgeState.get(this, "arm64_test", "Not run"))

        section("Clipboard synchronization")
        toggle("Host → Android", "clipboard_host_to_android")
        toggle("Android → host", "clipboard_android_to_host")

        section("File transfer and drag-and-drop")
        value("Current transfer", BridgeState.get(this, "transfer_state", "Idle"))
        button("Export a file to host") {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE); type = "*/*"
            }, exportRequest)
        }
        toggle("Open received files automatically", "auto_open_received", false)
        val history = BridgeState.history(this)
        if (history.length() == 0) detail("No transfer history")
        else for (index in 0 until minOf(8, history.length())) {
            val item = history.optJSONObject(index) ?: continue
            detail("${item.optString("kind")}: ${item.optString("name", item.optString("detail"))} — ${item.optString("state")}")
        }

        section("APK and split-APK installation")
        value("Installer", BridgeState.get(this, "install_state", "Idle"))
        button("Select APK bundle") {
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                addCategory(Intent.CATEGORY_OPENABLE)
                type = "application/vnd.android.package-archive"
                putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
            }, installRequest)
        }

        section("Native Bridge and linker diagnostics")
        detail(BridgeState.get(this, "android_info", "Unavailable"))
    }

    private fun parseInfo(): JSONObject = try {
        JSONObject(BridgeState.get(this, "android_info", "{}"))
    } catch (_: Exception) { JSONObject() }

    private fun inspect(uri: Uri): ApkInfo {
        val name = contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME),
            null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else "selected.apk"
        } ?: "selected.apk"
        val temporary = java.io.File.createTempFile("reboxed-inspect-", ".apk", cacheDir)
        contentResolver.openInputStream(uri).use { input ->
            requireNotNull(input)
            temporary.outputStream().use { output -> input.copyTo(output) }
        }
        try {
            val info = packageManager.getPackageArchiveInfo(temporary.path, 0)
                ?: throw IllegalArgumentException("Android rejected $name")
            val abis = sortedSetOf<String>()
            ZipInputStream(temporary.inputStream()).use { archive ->
                while (true) {
                    val entry = archive.nextEntry ?: break
                    val match = Regex("^lib/([^/]+)/[^/]+\\.so$").find(entry.name)
                    if (match != null) abis.add(match.groupValues[1])
                }
            }
            @Suppress("DEPRECATION")
            val version = if (Build.VERSION.SDK_INT >= 28) info.longVersionCode.toString()
                          else info.versionCode.toString()
            return ApkInfo(uri, name, info.packageName, info.versionName ?: "unknown",
                version, info.splitNames.orEmpty().singleOrNull(), abis.toList())
        } finally { temporary.delete() }
    }

    private fun preflight(uris: List<Uri>): Pair<String, Boolean> {
        val apks = uris.map { inspect(it) }
        require(apks.isNotEmpty())
        require(apks.map { it.packageName to it.versionCode }.toSet().size == 1) {
            "Base and split APK package/version identities differ"
        }
        val bases = apks.filter { it.split == null }
        require(bases.size == 1) { "Select exactly one base APK" }
        val packageAbis = apks.flatMap { it.abis }.toSet()
        val guest = Build.SUPPORTED_ABIS.toSet()
        val native = guest.filter { it == "x86" || it == "x86_64" }
        val bridge = try {
            JSONObject(BridgeState.get(this, "native_bridge_status", "{}"))
        } catch (_: Exception) { JSONObject() }
        val checks = bridge.optJSONObject("verification") ?: JSONObject()
        val translated = listOf("arm64-v8a", "armeabi-v7a").filter { abi ->
            checks.optJSONObject(abi)?.optBoolean("passed") == true
        }
        val effective = native.toSet() + translated.toSet()
        val compatible = packageAbis.isEmpty() || packageAbis.any { it in effective }
        val requires = packageAbis.isNotEmpty() && packageAbis.none { it in native }
        val translatorInstalled = bridge.optBoolean("installed")
        val translatorEnabled = bridge.optBoolean("enabled")
        val requiredTranslated = packageAbis.filter { it == "arm64-v8a" || it == "armeabi-v7a" }
        val translatorVerified = !requires || requiredTranslated.any { it in translated }
        val reason = when {
            compatible -> "Compatible"
            requires && !translatorInstalled -> "Native Bridge is required but no translator is installed"
            requires && !translatorEnabled -> "Native Bridge is required but the translator is disabled"
            requires && !translatorVerified ->
                "Native Bridge is required but none of ${requiredTranslated.joinToString()} passed the Android JNI probe"
            else -> "APK ABIs ${packageAbis.joinToString()} do not match native ${native.joinToString()} or verified translated ${translated.joinToString()} ABIs"
        }
        val summary = """
            Package: ${bases[0].packageName}
            Version: ${bases[0].versionName} (${bases[0].versionCode})
            Base and splits: ${apks.joinToString { "${it.name} [${it.split ?: "base"}]" }}
            APK ABIs: ${packageAbis.ifEmpty { setOf("none") }.joinToString()}
            Native guest ABIs: ${native.ifEmpty { listOf("none") }.joinToString()}
            Verified translated ABIs: ${translated.ifEmpty { listOf("none") }.joinToString()}
            Native Bridge required: $requires
            Translator installed/enabled: $translatorInstalled/$translatorEnabled
            Required translator ABI verified: $translatorVerified
            Compatibility: $reason
        """.trimIndent()
        return summary to compatible
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK || data == null) return
        if (requestCode == exportRequest) {
            val uri = data.data ?: return
            startService(Intent(this, BridgeService::class.java)
                .setAction(BridgeService.ACTION_EXPORT_URI).putExtra("uri", uri))
            return
        }
        if (requestCode == installRequest) {
            val uris = arrayListOf<Uri>()
            data.data?.let { uris.add(it) }
            data.clipData?.let { clips ->
                for (index in 0 until clips.itemCount) uris.add(clips.getItemAt(index).uri)
            }
            try {
                val (summary, compatible) = preflight(uris)
                AlertDialog.Builder(this).setTitle("Package preflight")
                    .setMessage(summary)
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton(if (compatible) "Install" else "Incompatible") { _, _ ->
                        if (compatible) startService(Intent(this, BridgeService::class.java)
                            .setAction(BridgeService.ACTION_INSTALL_URIS)
                            .putParcelableArrayListExtra("uris", uris))
                    }.show()
            } catch (error: Exception) {
                Toast.makeText(this, error.message ?: "Package inspection failed", Toast.LENGTH_LONG).show()
            }
        }
    }

    private data class ApkInfo(val uri: Uri, val name: String, val packageName: String,
        val versionName: String, val versionCode: String, val split: String?,
        val abis: List<String>)

    private fun title(text: String) = content.addView(TextView(this).apply {
        this.text = text; textSize = 28f; setTextColor(Color.rgb(26, 32, 44))
        setPadding(0, 0, 0, dp(16))
    })

    private fun section(text: String) = content.addView(TextView(this).apply {
        this.text = text; textSize = 18f; setTextColor(Color.rgb(49, 92, 190))
        setPadding(0, dp(22), 0, dp(8))
    })

    private fun value(label: String, value: String) = content.addView(TextView(this).apply {
        text = "$label: $value"; textSize = 15f; setTextColor(Color.DKGRAY)
        setPadding(0, dp(3), 0, dp(3))
    })

    private fun detail(value: String) = content.addView(TextView(this).apply {
        text = value; textSize = 13f; setTextColor(Color.GRAY)
        setPadding(dp(8), dp(3), 0, dp(3))
    })

    private fun button(label: String, action: () -> Unit) = content.addView(Button(this).apply {
        text = label; gravity = Gravity.CENTER; setOnClickListener { action() }
        layoutParams = ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT)
    })

    private fun toggle(label: String, key: String, fallback: Boolean = true) =
        content.addView(CheckBox(this).apply {
            text = label; isChecked = BridgeState.enabled(this@MainActivity, key, fallback)
            setOnCheckedChangeListener { _, checked ->
                BridgeState.setEnabled(this@MainActivity, key, checked)
            }
        })

    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
}
