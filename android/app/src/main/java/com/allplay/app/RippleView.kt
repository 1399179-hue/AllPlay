package com.allplay.app

import android.animation.ValueAnimator
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.view.View
import android.view.animation.LinearInterpolator

/**
 * 蓝牙徽标 + 水纹扩散动画：中心蓝色实心圆，外围三圈涟漪循环扩散渐隐。
 */
class RippleView(context: Context) : View(context) {
    private val centerPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        color = Color.parseColor("#2F7CF6")
        style = Paint.Style.FILL
    }
    private val ringPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = dp(4).toFloat()
        color = Color.parseColor("#2F7CF6")
    }
    private var progress = 0f
    private val animator = ValueAnimator.ofFloat(0f, 1f).apply {
        duration = 2800
        repeatCount = ValueAnimator.INFINITE
        interpolator = LinearInterpolator()
        addUpdateListener { progress = it.animatedValue as Float; invalidate() }
    }

    override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        animator.start()
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        animator.cancel()
    }

    override fun onDraw(canvas: Canvas) {
        val cx = width / 2f
        val cy = height / 2f
        val badgeR = minOf(width, height) * 0.25f   // 与蓝圆底(84dp)半径对齐，保证水纹从徽标边缘居中扩散
        val maxR = minOf(width, height) / 2f
        for (i in 0 until 2) {
            val p = (progress + i / 2f) % 1f
            ringPaint.alpha = ((1f - p) * 150).toInt()
            canvas.drawCircle(cx, cy, badgeR + p * (maxR - badgeR), ringPaint)
        }
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()
}
