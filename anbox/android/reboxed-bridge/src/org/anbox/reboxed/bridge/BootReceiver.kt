package org.anbox.reboxed.bridge

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build

class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val service = Intent(context, BridgeService::class.java)
        if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(service)
        else context.startService(service)
    }
}
