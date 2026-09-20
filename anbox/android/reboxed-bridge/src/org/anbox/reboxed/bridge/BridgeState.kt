package org.anbox.reboxed.bridge

import android.content.Context
import android.content.Intent
import org.json.JSONArray
import org.json.JSONObject

object BridgeState {
    const val ACTION_CHANGED = "org.anbox.reboxed.bridge.STATE_CHANGED"
    private const val PREFS = "bridge-state"

    fun update(context: Context, key: String, value: String) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString(key, value).apply()
        context.sendBroadcast(Intent(ACTION_CHANGED).setPackage(context.packageName))
    }

    fun get(context: Context, key: String, fallback: String = "Unknown"): String =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getString(key, fallback) ?: fallback

    fun enabled(context: Context, key: String, fallback: Boolean = true): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(key, fallback)

    fun setEnabled(context: Context, key: String, value: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putBoolean(key, value).apply()
    }

    fun history(context: Context): JSONArray {
        val raw = get(context, "history", "[]")
        return try { JSONArray(raw) } catch (_: Exception) { JSONArray() }
    }

    fun addHistory(context: Context, event: JSONObject) {
        val old = history(context)
        val next = JSONArray()
        next.put(event.put("time", System.currentTimeMillis()))
        for (index in 0 until minOf(old.length(), 49)) next.put(old.get(index))
        update(context, "history", next.toString())
    }
}
