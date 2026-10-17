package com.weiye.deskclip

/* Draw the board's text card like host/clipboard_agent/render.py
 * render_text_card: 224 px wide, two lines of 15 px text, ⋮ dots, up to three
 * chips. Sizes are board pixels (no density scaling). */

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextUtils

object CardRenderer {
    private const val PAD = 10
    private const val TEXT_SIZE = 15f
    private const val CHIP_SIZE = 13f
    private const val LINE = 20
    private const val MAX_LINES = 2
    private val PAGE_BG = Color.rgb(228, 228, 233)

    fun render(text: String, chips: List<String>): Raster {
        val textHeight = MAX_LINES * LINE
        val chipBlock = if (chips.isEmpty()) 0 else 6 + chips.size * 28
        val height = PAD + textHeight + chipBlock + PAD
        val bitmap = Bitmap.createBitmap(CARD_WIDTH, height, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        canvas.drawColor(PAGE_BG)
        val fill = Paint(Paint.ANTI_ALIAS_FLAG)
        fill.color = Color.WHITE
        canvas.drawRoundRect(RectF(0f, 0f, CARD_WIDTH.toFloat(), height.toFloat()), 14f, 14f, fill)

        val paint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = TEXT_SIZE; color = Color.rgb(20, 20, 20) }
        val body = text.take(512)
        val layout = StaticLayout.Builder.obtain(body, 0, body.length, paint, CARD_WIDTH - 36)
            .setAlignment(Layout.Alignment.ALIGN_NORMAL)
            .setMaxLines(MAX_LINES)
            .setEllipsize(TextUtils.TruncateAt.END)
            .setIncludePad(false)
            .setLineSpacing(LINE - paint.fontSpacing, 1f)
            .build()
        canvas.save()
        canvas.clipRect(PAD, PAD, CARD_WIDTH - 26, PAD + textHeight)
        canvas.translate(PAD.toFloat(), PAD.toFloat())
        layout.draw(canvas)
        canvas.restore()

        fill.color = Color.rgb(64, 64, 64)
        for (dy in intArrayOf(-6, 0, 6)) canvas.drawCircle((CARD_WIDTH - 18).toFloat(), (18 + dy).toFloat(), 1.5f, fill)

        val regions = mutableListOf(
            Region(REGION_BODY, 0, 0, 0, CARD_WIDTH - MENU_STRIP, height),
            Region(REGION_MENU, 0, CARD_WIDTH - MENU_STRIP, 0, MENU_STRIP, height),
        )
        val chipPaint = TextPaint(Paint.ANTI_ALIAS_FLAG).apply { textSize = CHIP_SIZE; color = Color.rgb(60, 60, 60) }
        var y = PAD + textHeight + 6
        chips.forEachIndexed { index, chip ->
            val label = if (chip.length <= 28) chip else chip.take(26) + "…"
            val chipW = minOf(CARD_WIDTH - MENU_STRIP - PAD, 12 + chipPaint.measureText(label).toInt() + 8)
            fill.color = Color.rgb(233, 233, 238)
            canvas.drawRoundRect(RectF(PAD.toFloat(), y.toFloat(), (PAD + chipW).toFloat(), (y + 24).toFloat()), 8f, 8f, fill)
            val shown = TextUtils.ellipsize(label, chipPaint, (chipW - 16).toFloat(), TextUtils.TruncateAt.END).toString()
            val baseline = y + 3 - chipPaint.fontMetrics.ascent
            canvas.drawText(shown, (PAD + 8).toFloat(), baseline, chipPaint)
            regions += Region(REGION_CHIP, index, PAD, y, chipW, 24)
            y += 28
        }
        val pixels = IntArray(CARD_WIDTH * height)
        bitmap.getPixels(pixels, 0, CARD_WIDTH, 0, 0, CARD_WIDTH, height)
        bitmap.recycle()
        return Raster(CARD_WIDTH, height, toRgb565(pixels), regions)
    }
}
