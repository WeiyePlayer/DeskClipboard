package com.weiye.deskclip

/* HubConnection + Session against a fake board that speaks the real record
 * protocol (handshake, AES-GCM both ways, J/U/D records). */

import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class FakeBoard(private val token: String, private val board: String, private val text: String?) : RecordLink {
    var cipher: RecordCipher? = null
    val ops = mutableListOf<String>()
    var content = ByteArray(0)
    var card = ByteArray(0)
    var ack: JSONObject? = null
    var committed: JSONObject? = null
    private var jobSent = false
    val acked = CountDownLatch(1)
    val commit = CountDownLatch(1)
    private var metadata: JSONObject? = null

    @Synchronized override fun exchange(record: ByteArray): ByteArray {
        if (record[0] == 'H'.code.toByte()) {
            val device = String(record, 1, 36)
            val serverNonce = ByteArray(16) { (it * 7).toByte() }
            val (proof, key) = derive(token, board, device, record.copyOfRange(37, 53), serverNonce)
            cipher = RecordCipher(key, client = false)
            return byteArrayOf('H'.code.toByte()) + serverNonce + proof
        }
        val c = cipher!!
        val plain = c.decrypt(record)
        val reply: ByteArray = when (plain[0]) {
            'J'.code.toByte() -> json(JSONObject(String(plain, 1, plain.size - 1)))
            'U'.code.toByte() -> {
                val part = plain[37].toInt()
                val data = plain.copyOfRange(42, plain.size)
                if (part == 1) card += data else content += data
                json(null)
            }
            'D'.code.toByte() -> {
                val offset = readU32(plain, 33).toInt()
                val size = readU32(plain, 37).toInt()
                val bytes = text!!.toByteArray()
                byteArrayOf('B'.code.toByte()) + bytes.copyOfRange(offset, minOf(bytes.size, offset + size))
            }
            else -> error("unexpected record")
        }
        return c.encrypt(reply)
    }

    private fun json(request: JSONObject?): ByteArray {
        val out = JSONObject().put("ok", true)
        val op = request?.getString("op") ?: "chunk"
        ops += op
        when (op) {
            "poll" -> if (text != null && !jobSent) {
                jobSent = true
                val bytes = text.toByteArray()
                out.put("job", JSONObject().put("id", "a".repeat(32)).put("kind", "text").put("bytes", bytes.size).put("sha256", sha256(bytes).hex()))
            }
            "ready" -> out.put("lease_ms", 2000)
            "ack" -> { ack = request; acked.countDown() }
            "begin" -> { metadata = request!!.getJSONObject("metadata"); out.put("content_offset", 0).put("card_offset", 0) }
            "commit" -> { committed = metadata; out.put("record_id", 7); commit.countDown() }
        }
        return byteArrayOf('J'.code.toByte()) + out.toString().toByteArray()
    }

    override fun close() {}
}

class SessionTest {
    private val token = "11".repeat(32)
    private val creds = Credentials("deskclip-84c7bb78dc60", "0f6d1c2e-6a3b-4c5d-8e9f-a0b1c2d3e4f5", token, "AA:BB:CC:DD:EE:FF", "Pixel")
    private val raster = Raster(CARD_WIDTH, 60, ByteArray(CARD_WIDTH * 60 * 2) { it.toByte() },
        listOf(Region(0, 0, 0, 0, 180, 60), Region(2, 0, 180, 0, 44, 60)))

    private class Events : SessionEvents {
        val delivered = mutableListOf<String>()
        val sentIds = mutableListOf<Long>()
        override fun state(text: String, connected: Boolean) {}
        override fun sent(recordId: Long) { sentIds += recordId }
        override fun failed(message: String) {}
        override fun deliver(text: String, deadlineMs: Long): Boolean { delivered += text; return true }
        override fun needsPairing(message: String) {}
    }

    private fun clock() = System.nanoTime() / 1_000_000

    @Test fun receivesItemFromBoardIntoClipboard() {
        val text = "来自开发板的 ✓ 文字\n第二行".repeat(1000) // ~39 KB: several 16 KiB chunks, under 64 KiB
        val board = FakeBoard(token, creds.boardId, text)
        val events = Events()
        val session = Session(creds, { board }, { _, _ -> raster }, events, ::clock)
        session.start()
        assertTrue(board.acked.await(20, TimeUnit.SECONDS))
        session.shutdown()
        session.join(5000)
        assertEquals(listOf(text), events.delivered)
        assertEquals(true, board.ack!!.getBoolean("success"))
        assertEquals("off", board.ack!!.getString("paste"))
        assertEquals("heartbeat", board.ops.first())
    }

    @Test fun uploadsTypedText() {
        val board = FakeBoard(token, creds.boardId, null)
        val events = Events()
        val session = Session(creds, { board }, { _, _ -> raster }, events, ::clock)
        session.start()
        val text = "手机输入 https://example.com"
        assertTrue(session.send(text))
        assertTrue(board.commit.await(20, TimeUnit.SECONDS))
        session.shutdown()
        session.join(5000)
        assertEquals(text, String(board.content))
        val m = board.committed!!
        assertEquals(sha256(board.card).hex(), m.getString("card_sha256"))
        assertEquals(board.card.size, m.getInt("card_bytes"))
        assertEquals("https://example.com", m.getJSONArray("chips").getString(0))
        assertEquals(listOf(7L), events.sentIds)
    }
}
