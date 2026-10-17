package com.weiye.deskclip

/* Blocking GATT client for one board connection: request/response records
 * over the WRITE characteristic (fragments <= 244 B, one outstanding ATT
 * write at a time) and reassembled notifications. Call from a worker thread. */

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.ParcelUuid
import java.io.IOException
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

class Found(val device: BluetoothDevice, val name: String, val rssi: Int)

@SuppressLint("MissingPermission")
fun scanBoards(context: Context, seconds: Long = 5): List<Found> {
    val adapter = (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        ?: throw IOException("此手机不支持蓝牙")
    if (!adapter.isEnabled) throw IOException("请先打开蓝牙")
    val scanner = adapter.bluetoothLeScanner ?: throw IOException("请先打开蓝牙")
    val found = LinkedHashMap<String, Found>()
    val callback = object : ScanCallback() {
        override fun onScanResult(type: Int, result: ScanResult) {
            val name = result.scanRecord?.deviceName ?: result.device.name ?: return
            if (!name.startsWith("DeskClip-")) return
            synchronized(found) {
                val old = found[result.device.address]
                if (old == null || result.rssi > old.rssi) found[result.device.address] = Found(result.device, name, result.rssi)
            }
        }
    }
    val filter = ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE_UUID)).build()
    val settings = ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build()
    scanner.startScan(listOf(filter), settings, callback)
    try {
        Thread.sleep(seconds * 1000)
    } finally {
        scanner.stopScan(callback)
    }
    return synchronized(found) { found.values.sortedByDescending { it.rssi } }
}

@SuppressLint("MissingPermission")
class BleLink private constructor(private val context: Context, private val device: BluetoothDevice) : RecordLink {
    private val ops = LinkedBlockingQueue<Int>()          // GATT operation status; -1 = disconnected
    private val records = LinkedBlockingQueue<Any>()      // ByteArray or Exception
    private val stream = Stream()
    private var gatt: BluetoothGatt? = null
    private var write: BluetoothGattCharacteristic? = null
    private var fragment = 20
    @Volatile var connected = false
        private set

    companion object {
        const val OP_TIMEOUT_S = 15L
        const val RESPONSE_TIMEOUT_S = 30L

        fun open(context: Context, address: String): BleLink {
            val adapter = (context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
                ?: throw IOException("此手机不支持蓝牙")
            if (!adapter.isEnabled) throw IOException("蓝牙未打开")
            if (!BluetoothAdapter.checkBluetoothAddress(address)) throw IOException("开发板地址无效")
            return open(context, adapter.getRemoteDevice(address))
        }

        fun open(context: Context, device: BluetoothDevice): BleLink {
            val link = BleLink(context.applicationContext, device)
            try {
                link.connect()
            } catch (e: Exception) {
                link.close()
                throw e
            }
            return link
        }
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                connected = true
                ops.offer(0)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                connected = false
                ops.offer(-1)
                records.offer(IOException("蓝牙已断开"))
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS) fragment = minOf(MAX_FRAGMENT, mtu - 3)
            ops.offer(status)
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) { ops.offer(status) }
        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) { ops.offer(status) }
        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) { ops.offer(status) }

        @Deprecated("API < 33")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT < 33) received(c.value ?: return)
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            received(value)
        }
    }

    private fun received(value: ByteArray) {
        try {
            for (r in stream.feed(value)) records.offer(r)
        } catch (e: HubException) {
            records.offer(e)
        }
    }

    private fun await(what: String, seconds: Long = OP_TIMEOUT_S): Int {
        val status = ops.poll(seconds, TimeUnit.SECONDS) ?: throw IOException("$what 超时")
        if (status == -1) throw IOException("蓝牙已断开")
        return status
    }

    private fun connect() {
        gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
            ?: throw IOException("无法连接开发板")
        await("连接", 30)
        val g = gatt!!
        g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
        if (!g.requestMtu(247)) throw IOException("MTU 请求失败")
        await("MTU 协商")
        if (!g.discoverServices()) throw IOException("服务发现失败")
        if (await("服务发现") != BluetoothGatt.GATT_SUCCESS) throw IOException("服务发现失败")
        val service = g.getService(SERVICE_UUID) ?: throw IOException("不是 DeskClipboard 开发板")
        write = service.getCharacteristic(WRITE_UUID) ?: throw IOException("缺少写入特征")
        val notify = service.getCharacteristic(NOTIFY_UUID) ?: throw IOException("缺少通知特征")
        g.setCharacteristicNotification(notify, true)
        val cccd = notify.getDescriptor(CCCD_UUID) ?: throw IOException("缺少通知描述符")
        val enable = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        val started = if (Build.VERSION.SDK_INT >= 33) {
            g.writeDescriptor(cccd, enable) == BluetoothGatt.GATT_SUCCESS
        } else {
            @Suppress("DEPRECATION")
            cccd.value = enable
            @Suppress("DEPRECATION")
            g.writeDescriptor(cccd)
        }
        if (!started || await("订阅通知") != BluetoothGatt.GATT_SUCCESS) throw IOException("订阅通知失败")
    }

    /** Send one record, wait for the next record from the board. */
    override fun exchange(record: ByteArray): ByteArray {
        val g = gatt ?: throw IOException("蓝牙未连接")
        val c = write ?: throw IOException("蓝牙未连接")
        if (!connected) throw IOException("蓝牙未连接")
        records.clear()
        for (part in fragments(record, fragment)) {
            var attempts = 0
            while (true) {
                val type = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
                val ok = if (Build.VERSION.SDK_INT >= 33) {
                    g.writeCharacteristic(c, part, type) == BluetoothGatt.GATT_SUCCESS
                } else {
                    @Suppress("DEPRECATION")
                    c.writeType = type
                    @Suppress("DEPRECATION")
                    c.value = part
                    @Suppress("DEPRECATION")
                    g.writeCharacteristic(c)
                }
                if (ok) break
                // The stack is still busy with the previous write: retry shortly.
                if (++attempts > 200 || !connected) throw IOException("蓝牙发送失败")
                Thread.sleep(5)
            }
            if (await("发送") != BluetoothGatt.GATT_SUCCESS) throw IOException("蓝牙发送失败")
        }
        val response = records.poll(RESPONSE_TIMEOUT_S, TimeUnit.SECONDS) ?: throw IOException("开发板没有响应")
        if (response is Exception) throw response
        return response as ByteArray
    }

    override fun close() {
        connected = false
        try {
            gatt?.disconnect()
            gatt?.close()
        } catch (_: Exception) {
        }
        gatt = null
    }

    val address: String get() = device.address
}
