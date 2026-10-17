"""Physical-USB trust bootstrap for the Bluetooth-only companion."""
from __future__ import annotations

import html
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import secrets
import sys
import threading
import urllib.parse
import uuid
import webbrowser

from clipboard_agent.device_link import candidate_ports
from clipboard_agent.hub_client import load_config, save_config
from clipboard_agent.hub_protocol import HubError
from clipboard_agent.hub_transport import USBTransport


def pair(link, args):
    status = link.request({"op": "status"})
    if status["version"] != 4:
        raise HubError("firmware_upgrade_required")
    if status.get("transport") != "ble":
        raise HubError("bluetooth_firmware_required")
    if not status["storage_ready"]:
        if status["initialized"]:
            raise HubError("storage_corrupt_restore_backup")
        link.request({"op": "initialize"}, timeout=60)
    old = load_config(args.config) if args.config.exists() else {}
    if old and old["board_id"] != status["board_id"]:
        raise HubError("different_board_use_separate_config")
    device_id = old.get("device_id", str(uuid.uuid4()))
    response = link.request({"op": "pair", "device_id": device_id, "name": args.name[:16],
                             "platform": "windows" if sys.platform == "win32" else "macos",
                             "token": old.get("token", "")})
    config = {"version": 4, "board_id": response["board_id"], "device_id": device_id,
              "name": args.name[:16], "token": response["token"], "transport": "ble"}
    save_config(args.config, config)
    return config


def setup(args):
    ports = candidate_ports()
    if not args.port and len(ports) != 1:
        raise HubError("connect_one_board_or_specify_port")
    link = USBTransport(args.port or ports[0])
    try:
        if args.setup_ui:
            return setup_ui(link, args)
        result = pair(link, args)
        print("USB 信任配对完成；日常通过蓝牙连接：DeskClip-" + result["board_id"][-6:])
        print("启动：" + ("powershell -ExecutionPolicy Bypass -File .\\scripts\\run-windows.ps1" if sys.platform.startswith("win") else "./scripts/run-mac.command"))
        return 0
    except HubError as e:
        if e.code == "pairing_closed":
            print("请先在开发板屏幕点「⋯ → 配对管理」，两分钟内重新配对。", file=sys.stderr)
        raise
    finally:
        link.close()


def setup_ui(link, args):
    nonce = secrets.token_urlsafe(32)
    route = "/setup/" + nonce
    done = threading.Event()
    result = {"ok": False}
    request_lock = threading.Lock()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *unused):
            pass

        def page(self, text, status=200):
            body = ("<!doctype html><html lang='zh-CN'><meta charset='utf-8'>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>DeskClipboard 配置</title><style>body{font:16px system-ui;background:#e4e4e9;"
                    "margin:40px auto;max-width:480px;padding:20px}main{background:white;padding:28px;border-radius:20px}"
                    "input,button{box-sizing:border-box;width:100%;font:inherit;padding:12px;margin:8px 0 20px;"
                    "border:1px solid #ccc;border-radius:10px}button{background:#202020;color:white}p{line-height:1.6;color:#555}</style>"
                    "<main><h2>桌面剪贴板</h2>" + text + "</main></html>").encode()
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path != route:
                return self.page("<p>配置链接无效。</p>", 404)
            self.page("<p>通过 USB 建立这台电脑与开发板的信任关系。日常内容走加密蓝牙，无需 Wi-Fi。</p>"
                      f"<form method='post' action='{route}'><input type='hidden' name='nonce' value='{nonce}'>"
                      "<button>配对这台电脑</button></form><p>另一台电脑配对前，请先在屏幕打开「⋯ → 配对管理」。</p>")

        def do_POST(self):
            origin = "http://127.0.0.1:" + str(self.server.server_port)
            if self.path != route or self.headers.get("Origin") != origin:
                return self.page("<p>请求来源无效。</p>", 403)
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = 0
            if not 0 < length <= 4096:
                return self.page("<p>请求大小无效。</p>", 400)
            data = urllib.parse.parse_qs(self.rfile.read(length).decode())
            if not secrets.compare_digest(data.get("nonce", [""])[0], nonce):
                return self.page("<p>配置链接已失效。</p>", 403)
            if done.is_set() or not request_lock.acquire(blocking=False):
                return self.page("<p>配置正在处理。</p>", 409)
            try:
                cfg = pair(link, args)
                name = html.escape("DeskClip-" + cfg["board_id"][-6:])
                self.page(f"<p>配对完成。蓝牙设备：{name}</p><p>现在可以关闭本页并启动客户端。</p>")
                result["ok"] = True
                done.set()
                threading.Thread(target=self.server.shutdown, daemon=True).start()
            except Exception as exc:
                code = getattr(exc, "code", type(exc).__name__)
                if code == "pairing_closed":
                    code = "请先在屏幕点 ⋯ → 配对管理，然后重试。"
                self.page("<p>配置未完成：" + html.escape(code) + "</p><p>返回上一页可以重试。</p>", 400)
            finally:
                request_lock.release()

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    url = f"http://127.0.0.1:{server.server_port}{route}"
    print("请在本地浏览器填写配置：" + url, flush=True)
    webbrowser.open(url)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0 if result["ok"] else 1
