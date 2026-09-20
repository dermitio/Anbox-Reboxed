package org.anbox.reboxed.bridge

import android.app.Application
import android.content.Intent
import android.os.Build

class BridgeApplication : Application() {
    override fun onCreate() {
        super.onCreate()
        val intent = Intent(this, BridgeService::class.java)
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(intent) else startService(intent)
    }
}
