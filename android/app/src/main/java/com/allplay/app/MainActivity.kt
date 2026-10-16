package com.allplay.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.EditText
import android.widget.CheckBox
import android.widget.Button
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.ArrayAdapter

/**
 * 入口：申请权限 → 起前台服务 → 三态 UI（等待连接 / 连接中 / 投屏画面）。
 * Surface 交给服务（→ native → MediaCodec）。
 */
class MainActivity : Activity(), SurfaceHolder.Callback {
    private lateinit var serviceIntent: Intent
    private var surfaceReady = false

    private lateinit var surfaceView: SurfaceView
    private lateinit var launcherView: FrameLayout
    private lateinit var statusText: TextView
    private lateinit var btBadge: FrameLayout
    private lateinit var fpsText: TextView

    private val uiHandler = Handler(Looper.getMainLooper())
    private var lastRenderedFrames = 0
    private var lastState = -1
    private var dotStep = 0

    // 等待连接文字后的三个点动画
    private val dotsRunnable = object : Runnable {
        override fun run() {
            if (lastState == 0 && launcherView.visibility == View.VISIBLE) {
                val base = "等待连接"
                val dots = when (dotStep % 4) { 0 -> ""; 1 -> "."; 2 -> ".."; else -> "..." }
                statusText.text = base + dots
                dotStep++
            }
            uiHandler.postDelayed(this, 700)
        }
    }

    // 实时帧率（右上角角标，每秒刷新）
    private val fpsRunnable = object : Runnable {
        override fun run() {
            val cur = (application as? AllPlayApp)?.service?.videoSink?.renderedFrames ?: 0
            val fps = cur - lastRenderedFrames
            lastRenderedFrames = cur
            fpsText.text = "$fps fps"
            uiHandler.postDelayed(this, 1000)
        }
    }

    // 会话状态轮询：0 等待连接 / 1 连接中 / 2 已连接
    private val stateRunnable = object : Runnable {
        override fun run() {
            val s = (application as? AllPlayApp)?.service?.sessionState() ?: 0
            if (s != lastState) {
                lastState = s
                updateUiState(s)
            }
            uiHandler.postDelayed(this, 500)
        }
    }

    private fun updateUiState(state: Int) {
        when (state) {
            2 -> {   // 已连接 → 显示投屏画面，隐藏启动页
                launcherView.visibility = View.GONE
            }
            1 -> {   // 连接中
                launcherView.visibility = View.VISIBLE
                statusText.text = "连接中…"
            }
            else -> {  // 等待连接
                launcherView.visibility = View.VISIBLE
                statusText.text = "等待连接"
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // 全屏沉浸：刘海区域也铺满
        if (Build.VERSION.SDK_INT >= 28) {
            window.attributes.layoutInDisplayCutoutMode =
                WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES
        }

        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        surfaceView.setOnTouchListener { v, e -> onTouch(v, e) }

        // 启动/连接中页面：白色背景 + 品牌 + 蓝牙水纹 + 状态文字
        launcherView = FrameLayout(this)
        launcherView.setBackgroundColor(Color.WHITE)

        val content = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER
        }
        val title = TextView(this).apply {
            // 双色 logo：「All」深色 + 「Play」蓝色，避免单调
            val sp = android.text.SpannableString("AllPlay")
            sp.setSpan(android.text.style.ForegroundColorSpan(Color.parseColor("#1A1A1A")), 0, 3,
                android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            sp.setSpan(android.text.style.ForegroundColorSpan(Color.parseColor("#2F7CF6")), 3, 7,
                android.text.Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            text = sp
            textSize = 38f
            typeface = Typeface.create("sans-serif", Typeface.BOLD)
            if (Build.VERSION.SDK_INT >= 21) letterSpacing = 0.04f   // setLetterSpacing 是 API 21
            gravity = Gravity.CENTER
        }
        // 蓝牙水纹徽标：涟漪环 + 蓝色圆底 + 蓝牙图标
        btBadge = FrameLayout(this)
        val ripple = RippleView(this)
        btBadge.addView(ripple, FrameLayout.LayoutParams(dp(168), dp(168), Gravity.CENTER))
        val badgeBg = View(this).apply {
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(Color.parseColor("#2F7CF6"))
            }
        }
        btBadge.addView(badgeBg, FrameLayout.LayoutParams(dp(84), dp(84), Gravity.CENTER))
        val btIcon = ImageView(this).apply {
            setImageResource(R.drawable.bluetooth_icon)
        }
        btBadge.addView(btIcon, FrameLayout.LayoutParams(dp(52), dp(52), Gravity.CENTER))

        statusText = TextView(this).apply {
            text = "等待连接"
            setTextColor(Color.parseColor("#666666"))
            textSize = 19f
            gravity = Gravity.CENTER
        }

        content.addView(title, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT))
        content.addView(btBadge, LinearLayout.LayoutParams(
            dp(168), dp(168)).apply {
            topMargin = dp(44)
        })
        content.addView(statusText, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(32)
        })
        launcherView.addView(content, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.CENTER))

        // 左上角设置图标
        val settingsBtn = ImageView(this).apply {
            setImageResource(R.drawable.settings_icon)
            setPadding(dp(2), dp(2), dp(2), dp(2))
            setOnClickListener { showSettingsDialog() }
        }
        launcherView.addView(settingsBtn, FrameLayout.LayoutParams(
            dp(60), dp(60), Gravity.TOP or Gravity.START))

        // FPS 角标：右上角实时帧率（仅显示）
        fpsText = TextView(this).apply {
            setTextColor(Color.GREEN)
            textSize = 20f
            gravity = Gravity.CENTER
            setPadding(dp(16), dp(8), dp(16), dp(8))
            setBackgroundColor(Color.argb(150, 0, 0, 0))
            text = "-- fps"
        }

        val root = FrameLayout(this)
        root.addView(surfaceView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        root.addView(launcherView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        root.addView(fpsText, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.WRAP_CONTENT, FrameLayout.LayoutParams.WRAP_CONTENT,
            Gravity.TOP or Gravity.END))
        setContentView(root)
        hideSystemBars()
        // 根据设置决定是否显示 FPS 角标
        fpsText.visibility = if (java.io.File(filesDir, "fps_show").exists()) View.VISIBLE else View.GONE

        serviceIntent = Intent(this, AllPlaySessionService::class.java)
        maybeRequestPermissions()

        uiHandler.post(fpsRunnable)
        uiHandler.post(stateRunnable)
        uiHandler.post(dotsRunnable)
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    /// 触摸 → 归一化 0..1 → 最多 2 触点 → [x,y,down] × 2 → 发到事件通道（参照协议 sendTouch）
    private fun onTouch(v: View, e: MotionEvent): Boolean {
        val service = (application as? AllPlayApp)?.service ?: return false
        if (v.width == 0 || v.height == 0) return true
        val pts = FloatArray(6)   // 2 触点 × 3
        val count = minOf(e.pointerCount, 2)
        for (i in 0 until count) {
            pts[i * 3] = e.getX(i) / v.width.toFloat()
            pts[i * 3 + 1] = e.getY(i) / v.height.toFloat()
            val up = e.actionMasked == MotionEvent.ACTION_UP ||
                (e.actionMasked == MotionEvent.ACTION_POINTER_UP && e.actionIndex == i)
            pts[i * 3 + 2] = if (up) 0f else 1f
        }
        service.sendTouch(pts)
        return true
    }

    private fun maybeRequestPermissions() {
        // Android 4.4（<23）：危险权限安装即授予，无运行时权限模型，直接起服务
        if (Build.VERSION.SDK_INT < 23) {
            startService()
            return
        }
        val perms = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) {
            perms += listOf(Manifest.permission.BLUETOOTH_CONNECT, Manifest.permission.BLUETOOTH_SCAN)
        }
        if (Build.VERSION.SDK_INT >= 33) perms += Manifest.permission.NEARBY_WIFI_DEVICES
        if (Build.VERSION.SDK_INT <= 32) perms += Manifest.permission.ACCESS_FINE_LOCATION
        val missing = perms.filter {
            checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            requestPermissions(missing.toTypedArray(), 1)
        } else {
            startService()
            requestBatteryExemption()
        }
    }

    override fun onRequestPermissionsResult(code: Int, p: Array<out String>, r: IntArray) {
        startService()
        requestBatteryExemption()
    }

    /// 申请电池优化豁免（退后台不被 Doze 杀进程/断热点；需用户在系统弹窗里点允许）
    private fun requestBatteryExemption() {
        if (Build.VERSION.SDK_INT < 23) return   // Android 4.4 无电池优化概念
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        if (pm.isIgnoringBatteryOptimizations(packageName)) return
        try {
            startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
        } catch (e: Exception) {}
    }

    private fun startService() {
        if (Build.VERSION.SDK_INT >= 26) startForegroundService(serviceIntent)
        else startService(serviceIntent)   // Android 4.4 无 startForegroundService
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        surfaceReady = true
        attachToService(holder.surface)
    }

    private fun attachToService(surface: android.view.Surface) {
        val app = application as? AllPlayApp ?: return
        app.surface = surface
        app.service?.attachSurface(surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, f: Int, w: Int, h: Int) {}
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady = false
        // 退后台 Surface 销毁：通知服务释放解码器对死 Surface 的引用，避免崩溃导致 CarPlay 断连
        (application as? AllPlayApp)?.service?.releaseSurface()
    }

    /// 旋转（configChanges 已声明 orientation|screenSize）：方向变了让服务换分辨率重启会话
    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        (application as? AllPlayApp)?.service?.restartForOrientation()
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // 沉浸模式失焦后会退出，重新聚焦时再隐藏，保证回前台仍全屏
        if (hasFocus) hideSystemBars()
    }

    /// 手动切换流畅模式弹窗（点右上角 FPS 角标触发）
    private fun showSmoothModeDialog() {
        val svc = (application as? AllPlayApp)?.service ?: return
        val active = svc.smoothModeActive
        val builder = AlertDialog.Builder(this).setCancelable(false)
        if (active) {
            builder.setTitle("退出流畅模式")
                .setMessage("当前已是流畅模式（1280x720 + 30fps）。\n是否恢复正常分辨率？")
                .setPositiveButton("恢复正常") { _, _ -> svc.disableSmoothMode() }
                .setNegativeButton("保持流畅模式", null)
        } else {
            builder.setTitle("进入流畅模式")
                .setMessage("固定 1280x720 + 最高 30fps，降低解码压力、减少掉帧。")
                .setPositiveButton("进入流畅模式") { _, _ -> svc.enableSmoothMode() }
                .setNegativeButton("取消", null)
        }
        builder.show()
    }

    /// 设置弹窗（左上角齿轮触发）：下拉选模式 + 填 SSID/密码 + 流畅模式开关，保存后重启生效
    private fun showSettingsDialog() {
        val files = filesDir
        val lanFile = java.io.File(files, "lan")
        val ssidFile = java.io.File(files, "ssid")
        val passFile = java.io.File(files, "passphrase")
        val smoothFile = java.io.File(files, "smooth")
        val portraitFile = java.io.File(files, "portrait")
        val fpsFile = java.io.File(files, "fps_show")

        val layout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(20), dp(24), dp(8))
        }
        val modeLabel = TextView(this).apply {
            text = "无线连接模式"
            setTextColor(Color.parseColor("#333333"))
            textSize = 15f
        }
        val modeSpinner = Spinner(this)
        val modes = arrayOf("热点模式（手机连本机热点）", "LAN 模式（连路由器 WiFi）")
        val adapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, modes)
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        modeSpinner.adapter = adapter
        modeSpinner.setSelection(if (lanFile.exists()) 1 else 0)

        val orientLabel = TextView(this).apply {
            text = "屏幕方向"
            setTextColor(Color.parseColor("#333333"))
            textSize = 15f
        }
        val orientSpinner = Spinner(this)
        val orientModes = arrayOf("横屏（默认）", "竖屏")
        val orientAdapter = ArrayAdapter(this, android.R.layout.simple_spinner_item, orientModes)
        orientAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        orientSpinner.adapter = orientAdapter
        orientSpinner.setSelection(if (portraitFile.exists()) 1 else 0)

        val ssidEdit = EditText(this).apply {
            hint = "SSID / 热点名"
            setText(if (ssidFile.exists()) ssidFile.readText().trim() else "")
        }
        val passEdit = EditText(this).apply {
            hint = "密码"
            setText(if (passFile.exists()) passFile.readText().trim() else "")
        }
        val smoothCheck = CheckBox(this).apply {
            text = "流畅模式"
            setTextColor(Color.parseColor("#333333"))
            isChecked = smoothFile.exists()
        }
        val fpsCheck = CheckBox(this).apply {
            text = "显示 FPS"
            setTextColor(Color.parseColor("#333333"))
            isChecked = fpsFile.exists()
        }

        layout.addView(modeLabel)
        layout.addView(modeSpinner)
        layout.addView(ssidEdit, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(16)
        })
        layout.addView(passEdit, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(10)
        })
        layout.addView(orientLabel, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(16)
        })
        layout.addView(orientSpinner)
        layout.addView(smoothCheck, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(16)
        })
        layout.addView(fpsCheck, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(8)
        })

        val scroll = ScrollView(this).apply { addView(layout) }

        AlertDialog.Builder(this)
            .setTitle("设置")
            .setView(scroll)
            .setPositiveButton("保存") { _, _ ->
                try {
                    // 0 = 热点模式（删 lan），1 = LAN 模式（建 lan）
                    if (modeSpinner.selectedItemPosition == 1) lanFile.createNewFile() else lanFile.delete()
                    // 0 = 横屏，1 = 竖屏
                    if (orientSpinner.selectedItemPosition == 1) portraitFile.createNewFile() else portraitFile.delete()
                    ssidFile.writeText(ssidEdit.text.toString().trim())
                    passFile.writeText(passEdit.text.toString())
                    if (smoothCheck.isChecked) smoothFile.createNewFile() else smoothFile.delete()
                    if (fpsCheck.isChecked) fpsFile.createNewFile() else fpsFile.delete()
                } catch (e: Exception) {}
                fpsText.visibility = if (fpsCheck.isChecked) View.VISIBLE else View.GONE
                (application as? AllPlayApp)?.service?.restartNative()
            }
            .setNegativeButton("取消", null)
            .show()
    }

    private fun hideSystemBars() {
        if (Build.VERSION.SDK_INT >= 30) {
            // 部分机型（如 vivo）setContentView 前 insetsController 可能为 null 抛 NPE，try/catch 兜底
            try {
                val c = window.insetsController
                c?.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                c?.systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            } catch (e: Exception) {
                @Suppress("DEPRECATION")
                window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION)
            }
        } else {
            @Suppress("DEPRECATION")
            window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                or View.SYSTEM_UI_FLAG_FULLSCREEN
                or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION)
        }
    }

    override fun onDestroy() {
        uiHandler.removeCallbacks(fpsRunnable)
        uiHandler.removeCallbacks(stateRunnable)
        uiHandler.removeCallbacks(dotsRunnable)
        super.onDestroy()
    }
}
