package com.weiye.deskclip

/* Authenticated requests over a BleLink (port of ble_transport.BLETransport
 * and hub_transport.upload), plus pairing by code. */

import java.io.IOException
import org.json.JSONObject

/** Anything that can carry one record each way; BleLink in the app, fakes in tests. */
interface RecordLink {
    fun exchange(record: ByteArray): ByteArray
    fun close()
}

class Credentials(val boardId: String, val deviceId: String, val token: String, val address: String, val name: String)

class HubConnection(private val link: RecordLink, creds: Credentials) {
    private val cipher: RecordCipher

    init {
        val handshake = Handshake(creds.boardId, creds.deviceId, creds.token)
        cipher = handshake.finish(link.exchange(handshake.request()))
    }

    private fun call(payload: ByteArray): Any {
        val response = cipher.decrypt(link.exchange(cipher.encrypt(payload)))
        return when (response.firstOrNull()) {
            'J'.code.toByte() -> checked(JSONObject(String(response, 1, response.size - 1, Charsets.UTF_8)))
            'B'.code.toByte() -> response.copyOfRange(1, response.size)
            else -> throw HubException("invalid_ble_response")
        }
    }

    fun request(json: JSONObject): JSONObject =
        call(byteArrayOf('J'.code.toByte()) + json.toString().toByteArray(Charsets.UTF_8)) as? JSONObject
            ?: throw HubException("invalid_ble_response")

    fun chunk(event: String, card: Boolean, offset: Int, data: ByteArray): JSONObject =
        call(byteArrayOf('U'.code.toByte()) + event.toByteArray(Charsets.US_ASCII) +
            byteArrayOf(if (card) 1 else 0) + u32(offset) + data) as? JSONObject
            ?: throw HubException("invalid_ble_response")

    fun download(job: String, offset: Int, size: Int): ByteArray =
        call(byteArrayOf('D'.code.toByte()) + job.toByteArray(Charsets.US_ASCII) + u32(offset) + u32(size)) as? ByteArray
            ?: throw HubException("invalid_ble_response")

    /** Resumable upload of one item; returns the board's record id. */
    fun upload(item: Upload, progress: () -> Unit = {}): Long {
        val reply = request(JSONObject().put("op", "begin").put("metadata", item.metadata))
        if (reply.optLong("record_id", 0) != 0L) return reply.getLong("record_id")
        try {
            for ((card, data) in listOf(false to item.content, true to item.card)) {
                var offset = reply.optInt(if (card) "card_offset" else "content_offset", 0)
                if (offset < 0 || offset > data.size) throw HubException("invalid_offset")
                while (offset < data.size) {
                    val end = minOf(offset + CHUNK, data.size)
                    chunk(item.eventId, card, offset, data.copyOfRange(offset, end))
                    offset = end
                    progress()
                }
            }
            return request(JSONObject().put("op", "commit").put("event_id", item.eventId)).getLong("record_id")
        } catch (e: HubException) {
            try {
                request(JSONObject().put("op", "abort").put("event_id", item.eventId))
            } catch (_: Exception) {
            }
            throw e
        }
    }

    fun close() = link.close()
}

fun checked(response: JSONObject): JSONObject {
    if (!response.optBoolean("ok", false)) throw HubException(response.optString("error", "invalid_response"))
    return response
}

/** Pair by code over an unauthenticated link; returns the credential parts. */
fun pairByCode(link: RecordLink, client: PairClient): Pair<String, String> {
    val confirm = client.confirm(link.exchange(client.start()))
    val token = client.finish(link.exchange(confirm))
    return (client.boardId ?: throw IOException("pairing failed")) to token
}
