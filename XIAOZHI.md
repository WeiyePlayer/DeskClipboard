# 小智 AI 配置

## 安装记录（2026-10-17）

- 实物：LCDWIKI ES3C28P / ES3N28P，2.8 英寸屏幕，ESP32-S3，16MB Flash、8MB PSRAM。
- 已安装：厂家快速使用资料包中的 `Portrait_2.0.2.bin`，小智 2.0.2 竖屏版。
- 烧录校验：esptool 回报 `Hash of data verified`。
- 启动验证：8MB PSRAM、ILI9341 显示驱动和 ES8311 音频编解码器均初始化成功，唤醒模型加载成功。
- 配网验证：用户完成手动配网后，串口确认已连接热点并取得 IP；小智官方服务证书校验通过，设备进入等待激活状态。
- 账号与设备绑定：用户已登录并完成绑定；串口确认 `Activation successful`、`MQTT: Connected to endpoint`，进入 `STATE: idle`。
- 控制台验证：设备在线，固件版本 2.0.2，MAC 显示的前后缀与本板一致。设备内部板型名称为 `quandong-s3-dev`。
- 当前智能体：未命名智能体；角色“湾湾小何”，模型“小智 Lite”。
- 尚待完成：实际语音唤醒、收音与扬声器回复验证。

## 自定义资源（2026-10-17）

已配置用户提供的 `/Users/weiye/Downloads/assets.bin`，包内资源版本为 1，适配 ESP32-S3、240×320、RGB565。

- 当前唤醒词改为 **“小爱同学”**，模型为 `wn9_xiaoaitongxue`。
- 浅色配色为黑字白底；深色配色为白字、`#121212` 背景。
- 已在 `0x800000` 资源分区写入并通过 Flash 哈希校验；重启后资源校验正常，串口确认成功加载 `srmodels` 并刷新显示主题。
- 运行验证：MQTT 已重新连接并进入待机；AFE 选择 `wn9_xiaoaitongxue`，检测任务已启动，模型报告唤醒词“小爱同学”。
- 文件、副本校验值、修改前资源快照与厂家默认资源的回退方法见 [资源记录](assets/20261017-xiaoaitongxue/README.md)。

## 账号激活

1. 登录 [小智官方控制台](https://xiaozhi.me/console/)。短信、图形验证码与用户协议由账号持有人操作。
2. 创建或选择一个智能体，在设备管理中添加设备，填写开发板当前显示的六位激活码。
3. 绑定完成后，对着麦克风说当前唤醒词“小爱同学”，再问一个简单问题，确认屏幕显示、收音与扬声器播放。

激活码可能变化，以设备当时的显示为准。本文件不保存账号密码、Wi-Fi 密码或激活码。

## 本地文件

- 固件与校验信息：`firmware/lcdwiki-es3c28p/`。
- 厂家下载来源和 SHA-256：[SOURCE.md](firmware/lcdwiki-es3c28p/SOURCE.md)。
- 出厂固件完整备份：`backups/factory-84c7bb78dc60-20261017.bin`（16MB，已排除 Git）。
- 基础连接与串口命令：[README.md](README.md)。

如需恢复出厂演示，先退出串口终端，在本项目目录执行下面的命令。它会用备份覆盖当前小智系统及配置：

```sh
.venv/bin/python -m esptool --chip esp32s3 --port /dev/cu.usbmodem1101 --baud 921600 --before usb-reset --after hard-reset write-flash 0x0 backups/factory-84c7bb78dc60-20261017.bin
```
