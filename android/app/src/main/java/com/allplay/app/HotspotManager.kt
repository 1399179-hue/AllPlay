package com.allplay.app

import android.annotation.SuppressLint
import android.content.Context
import android.net.wifi.WifiConfiguration
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.random.Random

/**
 * IHotspot 的 Android 实现。优先建**持久 SoftAP**（反射 WifiManager.setWifiApEnabled，退后台不回收），
 * 失败再退回 startLocalOnlyHotspot（前台可用、后台会被系统回收）。
 */
class HotspotManager(context: Context) {
    @Volatile var ssid: String = ""; private set
    @Volatile var passphrase: String = ""; private set
    @Volatile var ip: String = ""; private set
    @Volatile var persistent: Boolean = false; private set

    private val wifiManager: WifiManager =
        context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
    private val filesDir = context.filesDir
    // LocalOnlyHotspotReservation 是 API 26；用 Any? 避免 4.4 加载类时引用不存在的类型
    private var reservation: Any? = null

    /// 开热点（阻塞到就绪/失败）。优先持久 SoftAP，失败退 LocalOnlyHotspot。
    @SuppressLint("MissingPermission")
    fun start(timeoutMs: Long = 15000): Boolean {
        val (s, p) = stableCredentials()
        if (startPersistentAp(s, p)) {
            ssid = s; passphrase = p; persistent = true
            ip = detectHotspotIp() ?: "192.168.43.1"
            Log.i("AllPlay", "持久 SoftAP 已开：ssid=$s ip=$ip")
            return true
        }
        return startLocalOnly(timeoutMs)
    }

    /// 稳定 SSID/口令：files/ap_ssid、files/ap_passphrase 覆盖或首次生成后持久化（iPhone 可免重输自动回连）
    private fun stableCredentials(): Pair<String, String> {
        val s = readText("ap_ssid").ifBlank {
            "AllPlay-${Random.nextInt(1000, 10000)}".also { writeText("ap_ssid", it) }
        }
        val p = readText("ap_passphrase").ifBlank {
            val chars = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghjkmnpqrstuvwxyz23456789"
            (1..10).map { chars[Random.nextInt(chars.length)] }.joinToString("")
                .also { writeText("ap_passphrase", it) }
        }
        return s to p
    }

    /// 反射建真 SoftAP（后台不回收）。Android 13+ 用 startSoftAp(SoftApConfiguration)，旧版用 setWifiApEnabled。
    @SuppressLint("MissingPermission")
    private fun startPersistentAp(ssid: String, pass: String): Boolean {
        // Android 13+：startSoftAp(SoftApConfiguration)（隐藏 API，反射）
        if (Build.VERSION.SDK_INT >= 33) {
            try {
                val builderClass = Class.forName("android.net.wifi.SoftApConfiguration\$Builder")
                val builder = builderClass.getConstructor().newInstance()
                builderClass.getMethod("setSsid", String::class.java).invoke(builder, ssid)
                // setPassphrase(String, int)：securityType 2 = WPA2_PSK
                builderClass.getMethod("setPassphrase", String::class.java, Int::class.javaPrimitiveType)
                    .invoke(builder, pass, 2)
                val config = builderClass.getMethod("build").invoke(builder)
                val configClass = Class.forName("android.net.wifi.SoftApConfiguration")
                val m = wifiManager.javaClass.getMethod("startSoftAp", configClass)
                return m.invoke(wifiManager, config) as Boolean
            } catch (e: Exception) {
                Log.w("AllPlay", "startSoftAp 反射失败：${e.message}")
            }
        }
        // Android 10 及以下：setWifiApEnabled(WifiConfiguration, boolean)
        return try {
            val cfg = WifiConfiguration().apply {
                SSID = "\"$ssid\""
                preSharedKey = "\"$pass\""
                allowedKeyManagement.set(WifiConfiguration.KeyMgmt.WPA_PSK)
            }
            val m = wifiManager.javaClass.getMethod(
                "setWifiApEnabled", WifiConfiguration::class.java, Boolean::class.javaPrimitiveType,
            )
            m.invoke(wifiManager, cfg, true) as Boolean
        } catch (e: Exception) {
            val c = e.cause ?: e
            Log.w("AllPlay", "setWifiApEnabled 反射失败：${c.javaClass.name}: ${c.message}", c)
            false
        }
    }

    /// 兜底：LocalOnlyHotspot（前台可用，退后台会被系统回收）。API 26 才有，4.4 直接失败。
    @SuppressLint("MissingPermission")
    private fun startLocalOnly(timeoutMs: Long): Boolean {
        if (Build.VERSION.SDK_INT < 26) return false
        val latch = CountDownLatch(1)
        var ok = false
        val handler = Handler(Looper.getMainLooper())
        wifiManager.startLocalOnlyHotspot(object : WifiManager.LocalOnlyHotspotCallback() {
            override fun onStarted(res: WifiManager.LocalOnlyHotspotReservation) {
                reservation = res
                val cfg = res.wifiConfiguration
                ssid = cfg?.SSID?.removeSurrounding("\"") ?: ""
                passphrase = cfg?.preSharedKey ?: ""
                ip = detectHotspotIp() ?: "192.168.43.1"
                persistent = false
                ok = true
                Log.i("AllPlay", "LocalOnlyHotspot：ssid=$ssid 密码=$passphrase ip=$ip")
                latch.countDown()
            }
            override fun onStopped() { latch.countDown() }
            override fun onFailed(reason: Int) { latch.countDown() }
        }, handler)
        latch.await(timeoutMs, TimeUnit.MILLISECONDS)
        return ok
    }

    fun stop() {
        // reservation 是 API 26 的 LocalOnlyHotspotReservation；用反射 close() 避免 4.4 引用缺失类
        try {
            reservation?.let { r -> r.javaClass.getMethod("close").invoke(r) }
        } catch (_: Exception) {}
        reservation = null
        // 持久 SoftAP 不关：退后台/重建服务都保持热点（CarPlay 常驻）
    }

    /// 读用户手动开的系统热点的 SSID（Android 11+ getSoftApConfiguration 反射；旧版 getWifiApConfiguration）。
    /// 返回 null 表示没检测到手动热点。
    @SuppressLint("PrivateApi", "MissingPermission")
    fun readManualApSsid(): String? {
        return try {
            if (Build.VERSION.SDK_INT >= 30) {
                val m = WifiManager::class.java.getMethod("getSoftApConfiguration")
                val cfg = m.invoke(wifiManager) ?: return null
                cfg.javaClass.getMethod("getSsid").invoke(cfg) as? String
            } else {
                val m = WifiManager::class.java.getMethod("getWifiApConfiguration")
                val cfg = m.invoke(wifiManager) as? WifiConfiguration ?: return null
                cfg.SSID?.removeSurrounding("\"")
            }
        } catch (e: Exception) {
            val c = e.cause ?: e
            Log.w("AllPlay", "readManualApSsid 失败：${c.javaClass.name}: ${c.message}")
            null
        }
    }

    /// 手动热点接口 IP（ap*/softap*）；无则 null
    fun manualHotspotIp(): String? = detectHotspotIp()

    /// 热点接口（ap*/softap*/wlan1*）上的 IPv4；找不到退回 Android SoftAP 默认网关
    private fun detectHotspotIp(): String? {
        return try {
            val ifs = NetworkInterface.getNetworkInterfaces()
            while (ifs.hasMoreElements()) {
                val ni = ifs.nextElement()
                val n = ni.name.lowercase()
                if (n.startsWith("ap") || n.startsWith("softap") || n.startsWith("wlan1")) {
                    for (a in ni.inetAddresses) {
                        if (a is Inet4Address && !a.isLoopbackAddress) return a.hostAddress
                    }
                }
            }
            null
        } catch (e: Exception) { null }
    }

    private fun readText(name: String): String =
        try { java.io.File(filesDir, name).readText().trim() } catch (e: Exception) { "" }

    private fun writeText(name: String, value: String) {
        try { java.io.File(filesDir, name).writeText(value) } catch (e: Exception) {}
    }
}
