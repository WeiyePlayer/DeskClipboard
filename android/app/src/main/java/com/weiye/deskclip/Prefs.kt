package com.weiye.deskclip

/* Pairing result in app-private storage (allowBackup=false). The credential
 * is the per-device secret the board keeps in its 'peers' list; revoking the
 * phone on the board (⋯ → 配对管理) invalidates it. */

import android.content.Context

object Prefs {
    private const val FILE = "deskclip"

    fun load(context: Context): Credentials? {
        val p = context.getSharedPreferences(FILE, Context.MODE_PRIVATE)
        val board = p.getString("board_id", null) ?: return null
        val device = p.getString("device_id", null) ?: return null
        val token = p.getString("token", null) ?: return null
        val address = p.getString("address", null) ?: return null
        return Credentials(board, device, token, address, p.getString("name", "Android") ?: "Android")
    }

    fun save(context: Context, c: Credentials) {
        context.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit()
            .putString("board_id", c.boardId).putString("device_id", c.deviceId).putString("token", c.token)
            .putString("address", c.address).putString("name", c.name).apply()
    }

    fun clear(context: Context) {
        context.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit().clear().apply()
    }

    fun lastReceived(context: Context): String =
        context.getSharedPreferences(FILE, Context.MODE_PRIVATE).getString("last_received", "") ?: ""

    fun setLastReceived(context: Context, text: String) {
        context.getSharedPreferences(FILE, Context.MODE_PRIVATE).edit().putString("last_received", text.take(2000)).apply()
    }
}
