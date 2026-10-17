package com.weiye.deskclip

/* Vectors come from the Python implementation (host/gen_android_vectors.py),
 * which is itself checked against RFC 9382 and the firmware's C code. */

import java.math.BigInteger
import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

class ProtocolTest {
    private val v = JSONObject(javaClass.classLoader!!.getResource("deskclip_vectors.json")!!.readText())

    private fun expectError(code: String, block: () -> Unit) {
        try {
            block()
            fail("expected $code")
        } catch (e: HubException) {
            assertEquals(code, e.code)
        }
    }

    @Test fun spake2Rfc9382Vectors() {
        val list = v.getJSONArray("spake2_rfc9382")
        for (i in 0 until list.length()) {
            val r = list.getJSONObject(i)
            val w = BigInteger(r.getString("w"), 16)
            val x = BigInteger(r.getString("x"), 16)
            val y = BigInteger(r.getString("y"), 16)
            val pa = Spake2.encode(Spake2.G.multiply(x).add(Spake2.M.multiply(w)))
            val pb = Spake2.encode(Spake2.G.multiply(y).add(Spake2.N.multiply(w)))
            assertEquals(r.getString("pA"), pa.hex())
            assertEquals(r.getString("pB"), pb.hex())
            val k = Spake2.decode(pb).subtract(Spake2.N.multiply(w)).multiply(x)
            assertEquals(r.getString("K"), Spake2.encode(k).hex())
            val tt = Spake2.transcript(r.getString("A").toByteArray(), r.getString("B").toByteArray(), pa, pb, Spake2.encode(k), w)
            val keys = Spake2.schedule(tt)
            assertEquals(r.getString("Ke"), keys.ke.hex())
            assertEquals(r.getString("KcA"), keys.kcA.hex())
            assertEquals(r.getString("KcB"), keys.kcB.hex())
            assertEquals(r.getString("cA"), keys.cA.hex())
            assertEquals(r.getString("cB"), keys.cB.hex())
        }
    }

    @Test fun pairingMatchesPythonAndFirmware() {
        val list = v.getJSONArray("pairing")
        for (i in 0 until list.length()) {
            val p = list.getJSONObject(i)
            assertEquals(p.getString("w"), Spake2.scalarBytes(Spake2.codeScalar(p.getString("code"))).hex())
            val client = PairClient(p.getString("code"), p.getString("device_id"), p.getString("name"), BigInteger(p.getString("x"), 16))
            assertEquals(p.getString("p1"), client.start().hex())
            assertEquals(p.getString("p3"), client.confirm(p.getString("p2").unhex()).hex())
            assertEquals(p.getString("board_id"), client.boardId)
            assertEquals(p.getString("token"), client.finish(p.getString("p4").unhex()))
        }
    }

    @Test fun pairingRejectsBadConfirmationAndErrors() {
        val p = v.getJSONArray("pairing").getJSONObject(0)
        val client = PairClient(p.getString("code"), p.getString("device_id"), p.getString("name"), BigInteger(p.getString("x"), 16))
        client.start()
        client.confirm(p.getString("p2").unhex())
        val forged = p.getString("p4").unhex().also { it[10] = (it[10].toInt() xor 1).toByte() }
        expectError("bad_code") { client.finish(forged) }
        expectError("bad_code") { client.finish(v.getString("pairing_error").unhex()) }
        val other = PairClient("999999", p.getString("device_id"), "x")
        other.start()
        expectError("invalid_pair_reply") { other.confirm("5002".unhex() + byteArrayOf(1, 'b'.code.toByte()) + ByteArray(65) { 4 }) }
        try {
            Spake2.codeScalar("12345a")
            fail()
        } catch (_: IllegalArgumentException) {
        }
    }

    @Test fun handshakeAndCipher() {
        val h = v.getJSONObject("handshake")
        val (proof, session) = derive(h.getString("token"), h.getString("board_id"), h.getString("device_id"),
            h.getString("client_nonce").unhex(), h.getString("server_nonce").unhex())
        assertEquals(h.getString("proof"), proof.hex())
        assertEquals(h.getString("session"), session.hex())
        val hs = Handshake(h.getString("board_id"), h.getString("device_id"), h.getString("token"), h.getString("client_nonce").unhex())
        assertEquals(h.getString("request"), hs.request().hex())
        val cipher = hs.finish(h.getString("reply").unhex())
        val c = v.getJSONObject("cipher")
        val client = c.getJSONArray("client_records")
        for (i in 0 until client.length()) {
            val r = client.getJSONObject(i)
            assertEquals(r.getString("record"), cipher.encrypt(r.getString("plain").unhex()).hex())
        }
        val board = c.getJSONArray("board_records")
        val first = board.getJSONObject(0).getString("record").unhex()
        assertEquals(board.getJSONObject(0).getString("plain"), cipher.decrypt(first).hex())
        expectError("replayed_or_missing_record") { cipher.decrypt(first) }
        val second = board.getJSONObject(1).getString("record").unhex().also { it[12] = (it[12].toInt() xor 1).toByte() }
        expectError("device_authentication_failed") { cipher.decrypt(second) }
        assertEquals(board.getJSONObject(1).getString("plain"), cipher.decrypt(board.getJSONObject(1).getString("record").unhex()).hex())
        val bad = h.getString("reply").unhex().also { it[20] = 0 }
        expectError("device_authentication_failed") {
            Handshake(h.getString("board_id"), h.getString("device_id"), h.getString("token"), h.getString("client_nonce").unhex()).finish(bad)
        }
    }

    @Test fun framingAndFragments() {
        val s = v.getJSONObject("stream")
        val record = s.getString("record").unhex()
        val parts = fragments(record, s.getInt("fragment"))
        val expected = s.getJSONArray("fragments")
        assertEquals(expected.length(), parts.size)
        parts.forEachIndexed { i, part -> assertEquals(expected.getString(i), part.hex()) }
        val stream = Stream()
        val out = mutableListOf<ByteArray>()
        for (part in parts) out += stream.feed(part)
        assertEquals(1, out.size)
        assertArrayEquals(record, out[0])
        // Two records in one notification, split across a boundary.
        val two = u32(3) + "abc".toByteArray() + u32(2) + "de".toByteArray()
        assertEquals(listOf("abc"), stream.feed(two.copyOf(8)).map { String(it) })
        assertEquals(listOf("de"), stream.feed(two.copyOfRange(8, two.size)).map { String(it) })
        expectError("invalid_ble_frame") { Stream().feed(u32(0)) }
    }

    @Test fun chipsMatchPython() {
        val list = v.getJSONArray("chips")
        for (i in 0 until list.length()) {
            val c = list.getJSONObject(i)
            val expected = c.getJSONArray("chips").let { a -> (0 until a.length()).map { a.getString(it) } }
            assertEquals(c.getString("text"), expected, Chips.extract(c.getString("text")))
        }
    }

    @Test fun cardHeaderMatchesPython() {
        val c = v.getJSONObject("card_header")
        val regions = c.getJSONArray("regions").let { a ->
            (0 until a.length()).map { a.getJSONArray(it).let { r -> Region(r.getInt(0), r.getInt(1), r.getInt(2), r.getInt(3), r.getInt(4), r.getInt(5)) } }
        }
        assertEquals(c.getString("bytes"), cardHeader(c.getInt("width"), c.getInt("height"), regions).hex())
        assertArrayEquals(byteArrayOf(0, 0xf8.toByte()), toRgb565(intArrayOf(0xffff0000.toInt())))
    }

    @Test fun uploadMetadata() {
        val raster = Raster(CARD_WIDTH, 60, ByteArray(CARD_WIDTH * 60 * 2), listOf(Region(0, 0, 0, 0, 180, 60), Region(2, 0, 180, 0, 44, 60)))
        val item = makeUpload("0f6d1c2e-6a3b-4c5d-8e9f-a0b1c2d3e4f5", "A very long phone name", "验证码 123456", { _, _ -> raster })
        val m = item.metadata
        assertEquals("text", m.getString("kind"))
        assertEquals("A very long phon", m.getString("source_name"))
        assertEquals(item.content.size, m.getInt("bytes"))
        assertEquals(sha256(item.card).hex(), m.getString("card_sha256"))
        assertEquals(10 + 20 + CARD_WIDTH * 60 * 2, item.card.size)
        assertEquals(JSONArray(listOf("123456")).toString(), m.getJSONArray("chips").toString())
        expectError("empty_content") { makeUpload(m.getString("source_device"), "x", "", { _, _ -> raster }) }
        expectError("text_too_large_64KiB") { makeUpload(m.getString("source_device"), "x", "a".repeat(MAX_TEXT + 1), { _, _ -> raster }) }
    }
}
