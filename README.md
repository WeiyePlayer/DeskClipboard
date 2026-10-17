# ESP32-S3 基础连接环境

已于 2026-10-17 在这台 Apple Silicon Mac 上完成安装和实板通信验证。

## 驱动与设备

- 当前 USB 接口：乐鑫原生 USB Serial/JTAG，VID:PID 为 `303A:1001`。
- macOS 已加载内置驱动 `com.apple.driver.usb.cdc.acm`（AppleUSBACMData），无需另外安装 CH340 或 CP210x 驱动。
- 本次串口：`/dev/cu.usbmodem1101`。更换 USB 插口后编号可能变化。
- 实际读取结果：ESP32-S3，revision v0.2，240MHz，内置 8MB PSRAM，16MB Flash。
- 验证只读取芯片和 Flash 标识，没有擦除或烧录固件；结束后已发送复位。

## 已安装工具

工具位于本目录的 `.venv` 中，使用电脑现有的 Homebrew Python 3.11。

- `esptool 5.6.0`：芯片识别、读取及烧录工具。
- `pyserial 3.5`：串口枚举及串口终端。

下面的命令均在 `/Users/weiye/DOS/esp-s3` 目录执行。

列出串口：

```sh
.venv/bin/python -m serial.tools.list_ports -v
```

读取芯片和 Flash 信息（会短暂重启开发板，不改写固件）：

```sh
.venv/bin/python -m esptool --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 115200 --no-stub --before usb-reset --after hard-reset flash-id
```

查看固件串口输出（是否有日志取决于当前固件的输出配置）：

```sh
.venv/bin/python -m serial.tools.miniterm /dev/cu.usbmodem1101 115200 --dtr 0 --rts 0
```

按 `Ctrl+]` 退出串口终端。读取芯片信息或烧录前，先退出占用同一串口的终端。

基础 USB 连接和通信工具已配置完成；无需安装 Arduino IDE 或 ESP-IDF 编译环境即可使用现有的厂家小智固件。小智安装进度与使用方法见 [XIAOZHI.md](XIAOZHI.md)。

## 官方说明

- [乐鑫 USB Serial/JTAG 连接说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/usb-serial-jtag-console.html)
- [esptool 安装说明](https://docs.espressif.com/projects/esptool/en/latest/esp32s3/installation.html)
