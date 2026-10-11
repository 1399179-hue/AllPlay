package com.allplay.app

import android.content.Context
import android.net.wifi.WifiManager
import android.util.Log
import java.net.Inet4Address
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import javax.jmdns.JmDNS
import javax.jmdns.ServiceEvent
import javax.jmdns.ServiceInfo
import javax.jmdns.ServiceListener

/**
 * IMdnsAdvertiser 的 Android 实现。
 *
 * 广播与发现都用 JmDNS（而非 NsdManager）：热点模式下 NsdManager 会把 mDNS 绑到错误的
 * 网卡，iPhone 收不到 _airplay._tcp 就不会发起 CarPlay。JmDNS 显式绑定热点接口 IP，
 * 且 IPv4/IPv6 各建一个实例（参照协议 useInterfaceMdns 双栈）。
 */
class MdnsService(private val context: Context) {
    /** 热点接口 IPv4（AllPlaySessionService 在 nativeStart 前注入）。null/空 = 自动探测。 */
    var advertiseIp: String? = null

    private val jmdnsList = mutableListOf<JmDNS>()
    private var serviceInfo: ServiceInfo? = null
    private val multicastLock: WifiManager.MulticastLock =
        (context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager)
            .createMulticastLock("allplay-mdns").apply { setReferenceCounted(false) }

    fun register(name: String, port: Int, txt: Array<String>): Boolean {
        return try {
            multicastLock.acquire()
            val map = HashMap<String, String>()
            txt.forEach { kv ->
                val i = kv.indexOf('=')
                if (i > 0) map[kv.substring(0, i)] = kv.substring(i + 1)
            }
            val addrs = resolveAdvertiseAddresses()
            for (addr in addrs) {
                val dns = JmDNS.create(addr)
                jmdnsList.add(dns)
                // 只在 IPv4 上广播。iPhone 走 IPv4 连 RTSP；IPv6 的 JmDNS 只用于发现 _carplay-ctrl。
                if (addr is Inet4Address) {
                    dns.registerService(ServiceInfo.create("_airplay._tcp.local.", name, port, 0, 0, map))
                    // 同网模式：iPhone 可能靠 _carplay._tcp 发现 CarPlay 接收端（而非 _airplay._tcp）
                    dns.registerService(ServiceInfo.create("_carplay._tcp.local.", name, port, 0, 0, map))
                    Log.i("AllPlay", "JmDNS 广播 _airplay/_carplay._tcp iface=$addr")
                } else {
                    Log.i("AllPlay", "JmDNS 仅发现（不广播）iface=$addr")
                }
            }
            if (addrs.isEmpty()) {
                val dns = JmDNS.create()
                jmdnsList.add(dns)
                dns.registerService(ServiceInfo.create("_airplay._tcp.local.", name, port, 0, 0, map))
                Log.i("AllPlay", "JmDNS 广播 _airplay._tcp iface=(all)")
            }
            true
        } catch (e: Exception) {
            Log.w("AllPlay", "JmDNS 注册失败 ${e.javaClass.simpleName}: ${e.message}")
            false
        }
    }

    /// 发现 iPhone 的 _carplay-ctrl._tcp，解析出 host/port 后回调。
    fun discoverCarPlayCtrl(onResolved: (host: String, port: Int) -> Unit) {
        if (jmdnsList.isEmpty()) return
        val listener = object : ServiceListener {
            override fun serviceAdded(event: ServiceEvent) {
                Log.i("AllPlay", "JmDNS 发现 _carplay-ctrl serviceAdded name=${event.name}")
                event.dns.requestServiceInfo(event.type, event.name, true)
            }
            override fun serviceRemoved(event: ServiceEvent) {}
            override fun serviceResolved(event: ServiceEvent) {
                val info = event.info
                Log.i("AllPlay", "JmDNS _carplay-ctrl resolved name=${event.name} port=${info.port} addrs=${info.inetAddresses?.size}")
                val a = info.inetAddresses?.firstOrNull { (it is Inet4Address) == (event.dns.inetAddress is Inet4Address) }
                    ?: info.inet4Addresses?.firstOrNull()
                    ?: info.inetAddresses?.firstOrNull()
                a?.let { onResolved(it.hostAddress, info.port) }
            }
        }
        jmdnsList.forEach { dns ->
            dns.addServiceListener("_carplay-ctrl._tcp.local.", listener)
            Log.i("AllPlay", "JmDNS 发现 _carplay-ctrl 挂到 ${dns.inetAddress}")
        }
    }

    /// 探测 iPhone：GET /ctrl-int/1/connect（参照协议 CarPlayBonjour 请求格式）
    fun probeCarPlay(host: String, port: Int, deviceId: String, sourceVersion: String): Boolean {
        return try {
            val sock = Socket()
            sock.connect(InetSocketAddress(host, port), 3000)
            sock.soTimeout = 3000
            val deviceIdNoColon = deviceId.replace(":", "")
            val hostHeader = if (':' in host) "[$host]:$port" else "$host:$port"
            val req = "GET /ctrl-int/1/connect HTTP/1.1\r\n" +
                "Host: $hostHeader\r\n" +
                "User-Agent: AirPlay/$sourceVersion\r\n" +
                "AirPlay-Receiver-Device-ID: $deviceIdNoColon\r\n" +
                "Connection: close\r\n\r\n"
            sock.getOutputStream().write(req.toByteArray(Charsets.UTF_8))
            sock.getOutputStream().flush()
            sock.close()
            true
        } catch (e: Exception) {
            false
        }
    }

    fun unregister() {
        jmdnsList.forEach { dns ->
            try { dns.unregisterService(serviceInfo) } catch (_: Exception) {}
            try { dns.close() } catch (_: Exception) {}
        }
        jmdnsList.clear()
        serviceInfo = null
        if (multicastLock.isHeld) multicastLock.release()
    }

    /// 广播/发现应绑定的地址：显式配置优先；否则探测热点接口（wlan/ap/softap）的 IPv4+IPv6。
    private fun resolveAdvertiseAddresses(): List<InetAddress> {
        val ip = advertiseIp?.takeIf { it.isNotBlank() }
        if (ip != null) {
            try { return listOf(InetAddress.getByName(ip)) } catch (_: Exception) {}
        }
        val out = mutableListOf<InetAddress>()
        var fallback = mutableListOf<InetAddress>()
        return try {
            val ifs = NetworkInterface.getNetworkInterfaces()
            while (ifs.hasMoreElements()) {
                val ni = ifs.nextElement()
                if (!ni.isUp || ni.isLoopback) continue
                val apLike = ni.name.startsWith("wlan") || ni.name.startsWith("ap") ||
                    ni.name.startsWith("softap") || ni.name.startsWith("p2p")
                for (a in ni.inetAddresses) {
                    if (a.isLoopbackAddress) continue
                    if (apLike) out.add(a) else fallback.add(a)
                }
            }
            if (out.isEmpty()) out.addAll(fallback)
            // IPv4 在前（优先）
            out.sortBy { if (it is Inet4Address) 0 else 1 }
            out
        } catch (_: Exception) {
            out
        }
    }
}
