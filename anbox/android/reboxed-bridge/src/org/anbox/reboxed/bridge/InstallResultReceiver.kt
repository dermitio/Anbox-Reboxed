package org.anbox.reboxed.bridge

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInstaller
import org.json.JSONObject

class InstallResultReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS,
                                        PackageInstaller.STATUS_FAILURE)
        val detail = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE)
            ?: "Package Installer returned status $status"
        val state = when (status) {
            PackageInstaller.STATUS_SUCCESS -> "Installed"
            PackageInstaller.STATUS_PENDING_USER_ACTION -> {
                val confirmation = if (android.os.Build.VERSION.SDK_INT >= 33)
                    intent.getParcelableExtra(Intent.EXTRA_INTENT, Intent::class.java)
                else @Suppress("DEPRECATION")
                    intent.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                if (confirmation != null) {
                    confirmation.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    try {
                        context.startActivity(confirmation)
                        "Awaiting confirmation"
                    } catch (error: Exception) {
                        "Failed: cannot open Package Installer confirmation (${error.message})"
                    }
                } else "Failed: confirmation intent missing"
            }
            else -> "Failed: $detail"
        }
        // Every terminal callback replaces Installing, including all error paths.
        BridgeState.update(context, "install_state", state)
        BridgeState.addHistory(context, JSONObject()
            .put("kind", "package-install").put("state", state).put("detail", detail))
    }
}
