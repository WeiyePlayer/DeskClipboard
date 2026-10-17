package com.weiye.deskclip

/* Two screens, built in code: pairing (6-digit code from the board + a name
 * for this phone) and main (text box + 同步, connection state, last item
 * received from the board). */

import android.Manifest
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.text.InputFilter
import android.text.InputType
import android.view.Gravity
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import java.util.UUID

class MainActivity : Activity() {
    private val refresh: () -> Unit = { render() }
    private var statusView: TextView? = null
    private var messageView: TextView? = null
    private var receivedView: TextView? = null
    private var pairing = false

    private fun permissions(): Array<String> {
        val list = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) {
            list += Manifest.permission.BLUETOOTH_SCAN
            list += Manifest.permission.BLUETOOTH_CONNECT
        } else {
            list += Manifest.permission.ACCESS_FINE_LOCATION
        }
        if (Build.VERSION.SDK_INT >= 33) list += Manifest.permission.POST_NOTIFICATIONS
        return list.toTypedArray()
    }

    private fun missing() = permissions().filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (missing().isNotEmpty()) requestPermissions(missing().toTypedArray(), 1)
        show()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, results: IntArray) {
        show()
    }

    override fun onResume() {
        super.onResume()
        SyncState.listeners += refresh
        render()
    }

    override fun onPause() {
        SyncState.listeners -= refresh
        super.onPause()
    }

    private fun bluetoothReady(): Boolean {
        if (missing().any { it != Manifest.permission.POST_NOTIFICATIONS }) {
            toastMessage("需要蓝牙权限：请在系统设置里允许「附近的设备」")
            startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, android.net.Uri.parse("package:$packageName")))
            return false
        }
        val adapter = getSystemService(BluetoothManager::class.java).adapter
        if (adapter == null || !adapter.isEnabled) {
            try {
                startActivity(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            } catch (_: SecurityException) {
            }
            toastMessage("请先打开蓝牙")
            return false
        }
        return true
    }

    private fun show() {
        val creds = Prefs.load(this)
        if (creds == null) showPairing() else {
            showMain(creds)
            if (missing().none { it != Manifest.permission.POST_NOTIFICATIONS }) SyncService.start(this)
        }
    }

    private fun column(): LinearLayout = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL
        val pad = (16 * resources.displayMetrics.density).toInt()
        setPadding(pad, pad, pad, pad)
    }

    private fun text(parent: LinearLayout, value: String, size: Float = 15f): TextView =
        TextView(this).apply { text = value; textSize = size; setPadding(0, 8, 0, 8); parent.addView(this) }

    private fun showPairing() {
        val root = column()
        text(root, "配对 DeskClipboard", 22f)
        text(root, "在开发板上点「⋯ → 配对管理」，屏幕会显示 6 位配对码（2 分钟内有效）。手机靠近开发板，输入配对码：")
        val code = EditText(this).apply {
            hint = "6 位配对码"
            inputType = InputType.TYPE_CLASS_NUMBER
            filters = arrayOf(InputFilter.LengthFilter(6))
            textSize = 24f
            gravity = Gravity.CENTER
        }
        root.addView(code)
        text(root, "这台手机在开发板上显示的名称：")
        val name = EditText(this).apply { setText(Build.MODEL ?: "Android"); setSingleLine() }
        root.addView(name)
        val button = Button(this).apply { text = "配对" }
        root.addView(button)
        messageView = text(root, "")
        button.setOnClickListener {
            val digits = code.text.toString()
            if (digits.length != 6) {
                toastMessage("请输入 6 位配对码")
                return@setOnClickListener
            }
            if (pairing || !bluetoothReady()) return@setOnClickListener
            pairing = true
            button.isEnabled = false
            messageView?.text = "正在查找开发板…"
            val label = name.text.toString().trim().ifEmpty { "Android" }
            Thread { pair(digits, label) { button.isEnabled = true } }.start()
        }
        setContentView(ScrollView(this).apply { addView(root) })
    }

    private fun pair(code: String, name: String, done: () -> Unit) {
        val result = try {
            val boards = scanBoards(this)
            if (boards.isEmpty()) throw HubException("未找到开发板：请确认开发板已通电并靠近手机")
            ui { messageView?.text = "正在与 ${boards[0].name} 配对…" }
            val device = UUID.randomUUID().toString()
            val link = BleLink.open(this, boards[0].device)
            try {
                val (board, token) = pairByCode(link, PairClient(code, device, name))
                val creds = Credentials(board, device, token, link.address, name)
                // The board has stored us: keep the credential, then prove it
                // with a normal authenticated session.
                Prefs.save(this, creds)
                HubConnection(link, creds).request(org.json.JSONObject().put("op", "status"))
                null
            } finally {
                link.close()
            }
        } catch (e: HubException) {
            errorMessage(e.code)
        } catch (e: Exception) {
            e.message ?: e.javaClass.simpleName
        }
        ui {
            pairing = false
            done()
            if (result == null) {
                toastMessage("配对成功")
                show()
            } else {
                messageView?.text = "配对失败：$result"
            }
        }
    }

    private fun showMain(creds: Credentials) {
        val root = column()
        text(root, "DeskClipboard", 22f)
        statusView = text(root, "")
        val input = EditText(this).apply {
            hint = "输入要同步到开发板的文字"
            minLines = 4
            gravity = Gravity.TOP
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE
        }
        root.addView(input)
        val sync = Button(this).apply { text = "同步" }
        root.addView(sync)
        messageView = text(root, "")
        text(root, "最近收到（已复制到剪贴板）：", 13f)
        receivedView = text(root, "")
        val forget = Button(this).apply { text = "取消配对" }
        root.addView(forget)
        text(root, "开发板：${creds.boardId}  本机：${creds.name}", 12f)
        sync.setOnClickListener {
            val value = input.text.toString()
            if (value.isEmpty()) {
                toastMessage("请输入要同步的文字")
                return@setOnClickListener
            }
            if (value.toByteArray().size > MAX_TEXT) {
                toastMessage(errorMessage("text_too_large_64KiB"))
                return@setOnClickListener
            }
            if (!bluetoothReady()) return@setOnClickListener
            SyncService.start(this, Intent(SyncService.ACTION_SEND).putExtra(SyncService.EXTRA_TEXT, value))
            input.setText("")
        }
        forget.setOnClickListener {
            SyncService.start(this, Intent(SyncService.ACTION_STOP))
            Prefs.clear(this)
            SyncState.status = "未连接"
            SyncState.message = "已在手机上删除配对；请在开发板「⋯ → 配对管理」里移除这台手机"
            show()
        }
        setContentView(ScrollView(this).apply { addView(root) })
        render()
    }

    private fun render() {
        statusView?.text = "状态：" + SyncState.status
        if (Prefs.load(this) != null) messageView?.text = SyncState.message
        receivedView?.text = Prefs.lastReceived(this).ifEmpty { "（暂无）" }
    }

    private fun ui(block: () -> Unit) = runOnUiThread(block)

    private fun toastMessage(text: String) = android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_SHORT).show()
}
