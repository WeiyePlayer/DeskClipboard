# 自定义资源：小爱同学

2026-10-17 从用户指定的 `/Users/weiye/Downloads/assets.bin` 配置。

- 文件大小：291,659 字节。
- SHA-256：`42f8fbcec58f4f63fa18e6ab649a67e24bff48a3d94948f9b82be3a7c613cd7c`。
- 资源版本：1，与当前小智 2.0.2 支持的格式一致。
- 芯片与显示：ESP32-S3，240×320，RGB565。
- 唤醒模型：`wn9_xiaoaitongxue`，唤醒词为“小爱同学”。
- 配色：浅色模式黑字白底；深色模式白字、`#121212` 背景。
- 两个资源文件：`srmodels.bin` 与 `index.json`。
- 包内长度、文件表、`ZZ` 标识与校验和 `0x094d` 均已检查。
- 实机资源分区：偏移 `0x800000`，大小 `0x800000`（8MB），写入前已读取实机分区表核对。
- 写入命令仅覆盖资源区域；esptool 回报 `Hash of data verified`。
- 重启验证：固件仍为 2.0.2，资源内部校验通过，`MODEL_LOADER` 回报 `Successfully load srmodels`，显示主题已刷新。
- 在线与唤醒引擎验证：MQTT 连接成功并进入 `idle`，AFE 使用 `wn9_xiaoaitongxue`，打印“小爱同学”模型信息且音频检测任务启动成功。实际口头唤醒和扬声器效果仍需用户实测。

配置前的完整资源分区保存于 `../../backups/assets-20261017/assets-before.bin`。该快照已存在与新文件一致的资源头，但载荷尾部有 534 个字节不同，内部校验和不匹配，因此不作为正常回退版本。

可用的回退文件 `manufacturer-default-assets.bin` 来自厂家原始 `Portrait_2.0.2.bin` 中的资源分区，24 个资源，内部校验和已验证。需要恢复厂家默认资源时，先退出串口终端，再在项目根目录执行：

```sh
.venv/bin/python -m esptool --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 --before usb-reset --after hard-reset write-flash 0x800000 assets/20261017-xiaoaitongxue/manufacturer-default-assets.bin
```

此命令会恢复厂家资源与默认唤醒词；设备固件、Wi-Fi 配置和账号绑定所在分区不在其写入范围内。

格式核对依据：[小智 v2.0.2 的资源读取实现](https://github.com/78/xiaozhi-esp32/blob/v2.0.2/main/assets.cc)。
