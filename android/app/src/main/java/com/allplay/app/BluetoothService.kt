package com.allplay.app

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothSocket
import android.util.Log
import java.io.InputStream
import java.io.OutputStream
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.LinkedBlockingQueue

/**
 * IBluetooth 的 Android 实现：配对设备枚举 + SPP/RFCOMM。
 * 蓝牙 socket 的 I/O 留在本类（Android 没公开 socket fd），native 经 JNI 下调
 * read/write/close，返回字节数组。
 */
class BluetoothService {
    private val adapter: BluetoothAdapter? = BluetoothAdapter.getDefaultAdapter()

    // handle → 连接
    private val sockets = ConcurrentHashMap<Int, BluetoothSocket>()
    private val inStreams = ConcurrentHashMap<Int, InputStream>()
    private val outStreams = ConcurrentHashMap<Int, OutputStream>()
    private val queues = ConcurrentHashMap<Int, LinkedBlockingQueue<ByteArray>>()
    private val nextHandle = java.util.concurrent.atomic.AtomicInteger(1)

    @SuppressLint("MissingPermission")
    fun pairedDevices(): Array<String> {
        val devs = adapter?.bondedDevices ?: return emptyArray()
        return devs.map { "${it.name ?: ""}|${it.address}" }.toTypedArray()
    }

    /// 客户端方向：连手机 SPP 服务，返回 handle（<0 失败）。
    @SuppressLint("MissingPermission")
    fun connectSpp(address: String, uuid: String): Int {
        val dev: BluetoothDevice = adapter?.getRemoteDevice(address) ?: return -1
        return try {
            val socket = dev.createRfcommSocketToServiceRecord(UUID.fromString(uuid))
            socket.connect()
            register(socket)
        } catch (e: Exception) {
            Log.w("AllPlay", "connectSpp 失败 ${e.javaClass.simpleName}: ${e.message}")
            -1
        }
    }

    /// 服务端方向：注册 SDP 并等手机连入，返回 handle（<0 失败）。
    @SuppressLint("MissingPermission")
    fun listenSpp(uuid: String, timeoutMs: Int): Int {
        return try {
            val server = adapter?.listenUsingRfcommWithServiceRecord("Wireless iAPv2", UUID.fromString(uuid)) ?: return -1
            val socket = server.accept(timeoutMs) ?: return -1
            register(socket)
        } catch (e: Exception) {
            -1
        }
    }

    /// native 读：返回 null=断开，空数组=超时，否则数据（≤ maxLen）。
    fun read(handle: Int, maxLen: Int, timeoutMs: Int): ByteArray? {
        val q = queues[handle] ?: return null
        val data = q.poll(timeoutMs.toLong(), java.util.concurrent.TimeUnit.MILLISECONDS) ?: return ByteArray(0)
        return if (data.size <= maxLen) data else data.copyOf(maxLen)
    }

    fun write(handle: Int, data: ByteArray): Boolean {
        val out = outStreams[handle] ?: return false
        return try {
            out.write(data); out.flush(); true
        } catch (e: Exception) {
            false
        }
    }

    fun close(handle: Int) {
        try { sockets.remove(handle)?.close() } catch (_: Exception) {}
        inStreams.remove(handle)
        outStreams.remove(handle)
        queues.remove(handle)
    }

    private fun register(socket: BluetoothSocket): Int {
        val handle = nextHandle.getAndIncrement()
        val input = socket.inputStream
        val output = socket.outputStream
        val queue = LinkedBlockingQueue<ByteArray>()
        sockets[handle] = socket
        inStreams[handle] = input
        outStreams[handle] = output
        queues[handle] = queue

        // 专用读线程：读到的字节放进队列，供 native 侧 read 轮询
        Thread {
            val buf = ByteArray(4096)
            try {
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    queue.put(buf.copyOf(n))
                }
            } catch (_: Exception) {
            } finally {
                queue.put(ByteArray(0))   // 哨兵：断开
            }
        }.start()
        return handle
    }
}
