package com.weiye.deskclip

/* Port of host/clipboard_agent/extract.py (chips) and hub_protocol.make_upload
 * (metadata + card container). The card pixels come from CardRenderer. */

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID
import org.json.JSONArray
import org.json.JSONObject

const val MAX_TEXT = 64 * 1024
const val CHUNK = 16 * 1024
const val CARD_WIDTH = 224
const val MENU_STRIP = 44
const val REGION_BODY = 0
const val REGION_CHIP = 1
const val REGION_MENU = 2

object Chips {
    private val url = Regex("https?://[^\\s<>\"']+")
    private val email = Regex("(?<![A-Za-z0-9._%+-])[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}")
    private val phone = Regex("(?<!\\d)(1[3-9]\\d{9})(?!\\d)")
    private val code = Regex("验证码[:：\\s]*(\\d{4,8})")
    private const val MAX_CHIPS = 3

    fun extract(text: String): List<String> {
        val found = mutableListOf<Pair<Int, String>>()
        val spans = mutableListOf<IntRange>()
        fun take(regex: Regex, group: Int) {
            var accepted = 0
            for (match in regex.findAll(text)) {
                val g = match.groups[group] ?: continue
                val start = g.range.first
                val end = g.range.last + 1
                if (spans.any { !(end <= it.first || start >= it.last + 1) }) continue
                spans += start until end
                found += start to g.value
                accepted++
                if (accepted == MAX_CHIPS) break
            }
        }
        take(url, 0)
        take(email, 0)
        take(phone, 1)
        take(code, 1)
        return found.sortedBy { it.first }.take(MAX_CHIPS).map { it.second }
    }
}

data class Region(val kind: Int, val chip: Int, val x: Int, val y: Int, val w: Int, val h: Int)

class Raster(val width: Int, val height: Int, val rgb565: ByteArray, val regions: List<Region>)

/** Little-endian RGB565 from ARGB pixels (alpha ignored; cards are opaque). */
fun toRgb565(argb: IntArray): ByteArray {
    val out = ByteArray(argb.size * 2)
    for (i in argb.indices) {
        val p = argb[i]
        val r = (p shr 16) and 0xff
        val g = (p shr 8) and 0xff
        val b = p and 0xff
        val v = ((r and 0xf8) shl 8) or ((g and 0xfc) shl 3) or (b shr 3)
        out[2 * i] = (v and 0xff).toByte()
        out[2 * i + 1] = (v shr 8).toByte()
    }
    return out
}

fun cardHeader(width: Int, height: Int, regions: List<Region>): ByteArray {
    val b = ByteBuffer.allocate(10 + regions.size * 10).order(ByteOrder.LITTLE_ENDIAN)
    b.putInt(0).put(0).putShort(width.toShort()).putShort(height.toShort()).put(regions.size.toByte())
    for (r in regions) {
        b.put(r.kind.toByte()).put(r.chip.toByte()).putShort(r.x.toShort()).putShort(r.y.toShort())
            .putShort(r.w.toShort()).putShort(r.h.toShort())
    }
    return b.array()
}

class Upload(val metadata: JSONObject, val content: ByteArray, val card: ByteArray) {
    val eventId: String get() = metadata.getString("event_id")
}

fun makeUpload(deviceId: String, name: String, text: String, render: (String, List<String>) -> Raster,
               eventId: String = UUID.randomUUID().toString()): Upload {
    val content = text.toByteArray(Charsets.UTF_8)
    if (content.size > MAX_TEXT) throw HubException("text_too_large_64KiB")
    if (content.isEmpty()) throw HubException("empty_content")
    val chips = Chips.extract(text).filter { it.toByteArray(Charsets.UTF_8).size <= 2048 }
    val raster = render(text, chips)
    val card = cardHeader(raster.width, raster.height, raster.regions) + raster.rgb565
    val metadata = JSONObject()
        .put("event_id", eventId).put("source_device", deviceId).put("source_name", name.take(16))
        .put("kind", "text").put("bytes", content.size).put("sha256", sha256(content).hex())
        .put("card_bytes", card.size).put("card_sha256", sha256(card).hex())
        .put("width", 0).put("height", 0).put("chips", JSONArray(chips)).put("favorite", false)
    return Upload(metadata, content, card)
}

fun errorMessage(code: String): String = when (code) {
    "text_too_large_64KiB" -> "文字超过 64KiB，未发送"
    "empty_content" -> "请输入要同步的文字"
    "storage_full" -> "开发板空间或条数已满，请清理记录"
    "paused" -> "开发板已暂停记录"
    "busy" -> "开发板正忙，请稍后重试"
    "pairing_closed" -> "配对码已失效，请在开发板重新打开「⋯ → 配对管理」"
    "bad_code" -> "配对码错误"
    "peer_limit" -> "开发板已有 4 台设备，请先在开发板移除一台"
    "device_authentication_failed", "unauthorized" -> "凭据已失效，请重新配对"
    "invalid_pair_reply" -> "开发板响应无效，请确认开发板固件已升级"
    else -> code
}
