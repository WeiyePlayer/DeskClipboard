package com.weiye.deskclip

/* Background loop (port of hub_client.HubClient.network for a phone):
 * keep one authenticated BLE session, heartbeat so the board lists the phone
 * as online, poll for items sent from the board and write them to the
 * clipboard inside the 2-second lease, and upload texts typed in the app.
 * No clipboard monitoring and no auto-paste on Android. */

import java.security.MessageDigest
import java.util.UUID
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import org.json.JSONObject

interface SessionEvents {
    fun state(text: String, connected: Boolean)
    fun sent(recordId: Long)
    fun failed(message: String)
    /** Write to the clipboard; return true once written. Must finish before deadlineMs (uptime). */
    fun deliver(text: String, deadlineMs: Long): Boolean
    fun needsPairing(message: String)
}

class Session(
    private val creds: Credentials,
    private val connect: () -> RecordLink,
    private val render: (String, List<String>) -> Raster,
    private val events: SessionEvents,
    private val clock: () -> Long = { android.os.SystemClock.uptimeMillis() },
) : Thread("deskclip-session") {
    private val outgoing = LinkedBlockingQueue<String>(8)
    @Volatile private var running = true
    private val received = LinkedHashSet<String>()
    /* The item being uploaded survives reconnects with the same event_id, so a
     * commit whose reply was lost is not stored twice. */
    private var active: Upload? = null

    fun send(text: String): Boolean = outgoing.offer(text)
    fun shutdown() {
        running = false
        interrupt()
    }

    override fun run() {
        var failures = 0
        while (running) {
            var hub: HubConnection? = null
            try {
                events.state("正在连接开发板…", false)
                hub = HubConnection(connect(), creds)
                val session = UUID.randomUUID().toString()
                heartbeat(hub, session)
                failures = 0
                events.state("已连接 ${creds.boardId.takeLast(6)}", true)
                var lastBeat = clock()
                var lastPoll = 0L
                while (running) {
                    if (active == null) {
                        val text = outgoing.poll(250, TimeUnit.MILLISECONDS)
                        if (text != null) prepare(text)
                    }
                    active?.let { upload(hub, it) }
                    if (clock() - lastBeat >= 3000) {
                        heartbeat(hub, session)
                        lastBeat = clock()
                    }
                    if (clock() - lastPoll >= 1000) {
                        receive(hub, session)
                        lastPoll = clock()
                    }
                }
            } catch (e: InterruptedException) {
                break
            } catch (e: HubException) {
                if (e.code in setOf("device_authentication_failed", "replayed_or_missing_record", "unauthorized")) {
                    events.needsPairing(errorMessage(e.code))
                    running = false
                } else {
                    events.failed(errorMessage(e.code))
                    failures++
                }
            } catch (e: Exception) {
                failures++
            } finally {
                hub?.close()
            }
            if (!running) break
            val delay = if (failures <= 1) 1000L else minOf(30000L, 1000L shl minOf(failures, 5))
            events.state("未连接，${delay / 1000} 秒后重试", false)
            try {
                sleep(delay)
            } catch (e: InterruptedException) {
                break
            }
        }
        events.state("已停止", false)
    }

    private fun heartbeat(hub: HubConnection, session: String) {
        hub.request(JSONObject().put("op", "heartbeat").put("session", session))
    }

    private fun prepare(text: String) {
        try {
            active = makeUpload(creds.deviceId, creds.name, text, render)
        } catch (e: HubException) {
            events.failed(errorMessage(e.code))
        }
    }

    private fun upload(hub: HubConnection, item: Upload) {
        try {
            events.sent(hub.upload(item))
            active = null
        } catch (e: HubException) {
            // Content/board-state errors are final for this item; link errors
            // (IOException) propagate and the item is retried after reconnecting.
            active = null
            events.failed(errorMessage(e.code))
        }
    }

    private fun receive(hub: HubConnection, session: String) {
        val job = hub.request(JSONObject().put("op", "poll")).optJSONObject("job") ?: return
        val id = job.getString("id")
        val size = job.getInt("bytes")
        if (job.getString("kind") != "text" || size <= 0 || size > MAX_TEXT) throw HubException("invalid_delivery")
        var data = ByteArray(0)
        var lastBeat = clock()
        while (data.size < size) {
            if (clock() - lastBeat >= 2000) {
                heartbeat(hub, session)
                lastBeat = clock()
            }
            val part = hub.download(id, data.size, minOf(CHUNK, size - data.size))
            if (part.isEmpty() || data.size + part.size > size) throw HubException("incomplete_delivery")
            data += part
        }
        if (!MessageDigest.isEqual(sha256(data).hex().toByteArray(), job.getString("sha256").toByteArray()))
            throw HubException("checksum_mismatch")
        val text = Charsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(data)).toString()
        val start = clock()
        val lease = hub.request(JSONObject().put("op", "ready").put("job_id", id))
        val deadline = start + minOf(lease.optLong("lease_ms", 2000), 2000)
        var success = false
        if (id !in received && clock() < deadline) {
            success = events.deliver(text, deadline)
            if (success) {
                received += id
                if (received.size > 128) received.remove(received.first())
            }
        }
        // An uncertain ACK is never retried by rewriting the clipboard.
        hub.request(JSONObject().put("op", "ack").put("job_id", id).put("success", success).put("paste", "off"))
    }
}
