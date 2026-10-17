package com.weiye.deskclip

/* Port of host/clipboard_agent/ble_security.py (session handshake, AES-GCM
 * records with direction-separated nonces and strict sequence numbers, and
 * <u32 length> framing over 244-byte ATT fragments). */

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.Mac
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

val SERVICE_UUID: UUID = UUID.fromString("89c11000-4e46-4a49-9418-dfc18a624070")
val WRITE_UUID: UUID = UUID.fromString("89c11001-4e46-4a49-9418-dfc18a624070")
val NOTIFY_UUID: UUID = UUID.fromString("89c11002-4e46-4a49-9418-dfc18a624070")
val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
const val MAX_FRAGMENT = 244
const val MAX_FRAME = 1024 * 1024

class HubException(val code: String) : Exception(code)

fun ByteArray.hex(): String = joinToString("") { "%02x".format(it.toInt() and 0xff) }
fun String.unhex(): ByteArray {
    require(length % 2 == 0)
    return ByteArray(length / 2) { substring(2 * it, 2 * it + 2).toInt(16).toByte() }
}

fun hmacSha256(key: ByteArray, data: ByteArray): ByteArray =
    Mac.getInstance("HmacSHA256").run { init(SecretKeySpec(key, "HmacSHA256")); doFinal(data) }

fun sha256(data: ByteArray): ByteArray = MessageDigest.getInstance("SHA-256").digest(data)

fun u32(value: Int): ByteArray = ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(value).array()
fun u64(value: Long): ByteArray = ByteBuffer.allocate(8).order(ByteOrder.LITTLE_ENDIAN).putLong(value).array()
fun readU32(data: ByteArray, at: Int): Long =
    ByteBuffer.wrap(data, at, 4).order(ByteOrder.LITTLE_ENDIAN).int.toLong() and 0xffffffffL
fun readU64(data: ByteArray, at: Int): Long = ByteBuffer.wrap(data, at, 8).order(ByteOrder.LITTLE_ENDIAN).long

/** proof, session key = HMAC(token, label \0 board device client server) */
fun derive(token: String, board: String, device: String, client: ByteArray, server: ByteArray): Pair<ByteArray, ByteArray> {
    val key = token.unhex()
    if (key.size != 32 || client.size != 16 || server.size != 16) throw HubException("invalid_credentials")
    val context = board.toByteArray(Charsets.US_ASCII) + device.toByteArray(Charsets.US_ASCII) + client + server
    val proof = hmacSha256(key, "deskclip-v4/server".toByteArray() + byteArrayOf(0) + context)
    val session = hmacSha256(key, "deskclip-v4/session".toByteArray() + byteArrayOf(0) + context)
    return proof to session
}

class RecordCipher(key: ByteArray, client: Boolean = true) {
    private val key = SecretKeySpec(key, "AES")
    private val sendDirection = (if (client) "C2B" else "B2C").toByteArray() + byteArrayOf(0)
    private val recvDirection = (if (client) "B2C" else "C2B").toByteArray() + byteArrayOf(0)
    var sent = 0L
        private set
    var received = 0L
        private set

    fun encrypt(data: ByteArray): ByteArray {
        sent += 1
        val header = byteArrayOf('E'.code.toByte()) + u64(sent)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key, GCMParameterSpec(128, sendDirection + header.copyOfRange(1, 9)))
        cipher.updateAAD(header)
        return header + cipher.doFinal(data)
    }

    fun decrypt(data: ByteArray): ByteArray {
        if (data.size < 26 || data[0] != 'E'.code.toByte()) throw HubException("invalid_encrypted_record")
        val sequence = readU64(data, 1)
        if (sequence != received + 1) throw HubException("replayed_or_missing_record")
        val plain = try {
            val cipher = Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(128, recvDirection + data.copyOfRange(1, 9)))
            cipher.updateAAD(data, 0, 9)
            cipher.doFinal(data, 9, data.size - 9)
        } catch (e: Exception) {
            throw HubException("device_authentication_failed")
        }
        received = sequence
        return plain
    }
}

class Handshake(private val board: String, private val device: String, private val token: String,
                val nonce: ByteArray = ByteArray(16).also { SecureRandom().nextBytes(it) }) {
    fun request(): ByteArray = byteArrayOf('H'.code.toByte()) + device.toByteArray(Charsets.US_ASCII) + nonce

    fun finish(reply: ByteArray): RecordCipher {
        if (reply.size != 49 || reply[0] != 'H'.code.toByte()) throw HubException("device_authentication_failed")
        val (proof, key) = derive(token, board, device, nonce, reply.copyOfRange(1, 17))
        if (!MessageDigest.isEqual(proof, reply.copyOfRange(17, 49))) throw HubException("device_authentication_failed")
        return RecordCipher(key)
    }
}

/** ATT writes/notifications are fragments of <u32 length>-prefixed records. */
class Stream {
    private var pending = ByteArray(0)

    fun feed(data: ByteArray): List<ByteArray> {
        pending += data
        val records = mutableListOf<ByteArray>()
        while (pending.size >= 4) {
            val size = readU32(pending, 0)
            if (size <= 0 || size > MAX_FRAME) {
                pending = ByteArray(0)
                throw HubException("invalid_ble_frame")
            }
            if (pending.size < size + 4) break
            records += pending.copyOfRange(4, 4 + size.toInt())
            pending = pending.copyOfRange(4 + size.toInt(), pending.size)
        }
        return records
    }
}

fun fragments(record: ByteArray, size: Int): List<ByteArray> {
    val framed = u32(record.size) + record
    return (framed.indices step size).map { framed.copyOfRange(it, minOf(it + size, framed.size)) }
}
