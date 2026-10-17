package com.weiye.deskclip

/* Pairing by code: SPAKE2-P256-SHA256-HKDF-HMAC (RFC 9382), phone = side A.
 * Port of host/clipboard_agent/pake.py; message formats are documented there. */

import java.math.BigInteger
import java.security.MessageDigest
import java.security.SecureRandom
import org.bouncycastle.crypto.ec.CustomNamedCurves
import org.bouncycastle.math.ec.ECPoint

object Spake2 {
    private val params = CustomNamedCurves.getByName("secp256r1")
    val G: ECPoint = params.g
    val order: BigInteger = params.n
    val M: ECPoint = decode("02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f".unhex())
    val N: ECPoint = decode("03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49".unhex())

    fun decode(data: ByteArray): ECPoint {
        val point = try {
            params.curve.decodePoint(data).normalize()
        } catch (e: IllegalArgumentException) {
            throw HubException("invalid_pair_reply")
        }
        if (point.isInfinity || !point.isValid) throw HubException("invalid_pair_reply")
        return point
    }

    fun encode(point: ECPoint): ByteArray = point.normalize().getEncoded(false)

    fun scalarBytes(k: BigInteger): ByteArray {
        val raw = k.toByteArray().dropWhile { it == 0.toByte() }.toByteArray()
        return ByteArray(32 - raw.size) + raw
    }

    fun randomScalar(random: SecureRandom = SecureRandom()): BigInteger {
        while (true) {
            val k = BigInteger(256, random)
            if (k.signum() > 0 && k < order) return k
        }
    }

    fun hkdf(ikm: ByteArray, info: ByteArray, length: Int): ByteArray {
        val prk = hmacSha256(ByteArray(32), ikm)
        var out = ByteArray(0)
        var block = ByteArray(0)
        var counter = 1
        while (out.size < length) {
            block = hmacSha256(prk, block + info + byteArrayOf(counter.toByte()))
            out += block
            counter++
        }
        return out.copyOf(length)
    }

    private fun lp(data: ByteArray) = u64(data.size.toLong()) + data

    fun transcript(a: ByteArray, b: ByteArray, pa: ByteArray, pb: ByteArray, k: ByteArray, w: BigInteger) =
        lp(a) + lp(b) + lp(pa) + lp(pb) + lp(k) + lp(scalarBytes(w))

    class Keys(val ke: ByteArray, val kcA: ByteArray, val kcB: ByteArray, val cA: ByteArray, val cB: ByteArray)

    fun schedule(tt: ByteArray): Keys {
        val digest = sha256(tt)
        val kc = hkdf(digest.copyOfRange(16, 32), "ConfirmationKeys".toByteArray(), 32)
        val kcA = kc.copyOfRange(0, 16)
        val kcB = kc.copyOfRange(16, 32)
        return Keys(digest.copyOfRange(0, 16), kcA, kcB, hmacSha256(kcA, tt), hmacSha256(kcB, tt))
    }

    fun codeScalar(code: String): BigInteger {
        require(code.length == 6 && code.all { it in '0'..'9' }) { "pairing code must be 6 digits" }
        val digest = MessageDigest.getInstance("SHA-512")
            .digest("deskclip-v4/pair-code".toByteArray() + byteArrayOf(0) + code.toByteArray(Charsets.US_ASCII))
        return BigInteger(1, digest).mod(order)
    }

    fun credential(ke: ByteArray): String = hkdf(ke, "deskclip-v4/credential".toByteArray(), 32).hex()
}

fun pairError(reply: ByteArray) {
    if (reply.size >= 2 && reply[0] == 'P'.code.toByte() && reply[1] == 0xff.toByte()) {
        val code = String(reply, 2, reply.size - 2, Charsets.US_ASCII)
        throw HubException(code.ifEmpty { "pairing_failed" })
    }
}

/** Side A. start() -> P01; confirm(P02) -> P03; finish(P04) -> credential. */
class PairClient(code: String, val deviceId: String, private val name: String,
                 private val x: BigInteger = Spake2.randomScalar()) {
    private val w = Spake2.codeScalar(code)
    val pa: ByteArray = Spake2.encode(Spake2.G.multiply(x).add(Spake2.M.multiply(w)))
    var boardId: String? = null
        private set
    private var expectedCb: ByteArray? = null
    private var token: String? = null

    init {
        require(deviceId.length == 36)
    }

    fun start(): ByteArray {
        var nameBytes = name.toByteArray(Charsets.UTF_8)
        if (nameBytes.size > 64) {
            // Cut at a character boundary.
            var end = 64
            while (end > 0 && (nameBytes[end].toInt() and 0xc0) == 0x80) end--
            nameBytes = nameBytes.copyOf(end)
        }
        val platform = "android".toByteArray(Charsets.US_ASCII)
        return byteArrayOf('P'.code.toByte(), 1) + deviceId.toByteArray(Charsets.US_ASCII) + pa +
            byteArrayOf(nameBytes.size.toByte()) + nameBytes + byteArrayOf(platform.size.toByte()) + platform
    }

    fun confirm(reply: ByteArray): ByteArray {
        pairError(reply)
        if (reply.size < 3 || reply[0] != 'P'.code.toByte() || reply[1] != 2.toByte()) throw HubException("invalid_pair_reply")
        val size = reply[2].toInt() and 0xff
        if (reply.size != 3 + size + 65) throw HubException("invalid_pair_reply")
        val board = reply.copyOfRange(3, 3 + size)
        val pb = reply.copyOfRange(3 + size, reply.size)
        val k = Spake2.decode(pb).subtract(Spake2.N.multiply(w)).multiply(x).normalize()
        if (k.isInfinity) throw HubException("invalid_pair_reply")
        val tt = Spake2.transcript(deviceId.toByteArray(Charsets.US_ASCII), board, pa, pb, Spake2.encode(k), w)
        val keys = Spake2.schedule(tt)
        boardId = String(board, Charsets.US_ASCII)
        expectedCb = keys.cB
        token = Spake2.credential(keys.ke)
        return byteArrayOf('P'.code.toByte(), 3) + keys.cA
    }

    fun finish(reply: ByteArray): String {
        pairError(reply)
        val expected = expectedCb ?: throw HubException("invalid_pair_reply")
        if (reply.size != 34 || reply[0] != 'P'.code.toByte() || reply[1] != 4.toByte() ||
            !MessageDigest.isEqual(reply.copyOfRange(2, 34), expected)) throw HubException("bad_code")
        return token!!
    }
}
