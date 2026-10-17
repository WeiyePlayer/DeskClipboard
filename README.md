# DeskClipboard：ESP32-S3 独立蓝牙文字剪贴板

使用现有 LCDWIKI ES3C28P 触摸屏开发板，在 Mac 与 Windows 间保存和选择发送文字。USB 负责供电和首次授权，日常内容只通过加密 BLE 蓝牙传输，不需要 Wi-Fi、互联网或云服务。

开发板保存正文、卡片和收藏，来源电脑关闭后仍可浏览、发送，重新上电后恢复历史。点卡片正文或提取出的号码，选择在线电脑后，该电脑写入系统剪贴板并立即自动粘贴到当前窗口（Mac 发送 Command-V，Windows 发送 Ctrl-V）。

## 容量与界面

- 只记录文字，最多 64KiB（UTF-8 字节），支持中文、换行、表情。图片和文件不记录。
- 最多 40 条，收藏计入总数。正文、卡片、索引和临时文件共最多 8MiB；达到限制时淘汰最旧非收藏，收藏占满时拒绝新增。
- 「⋯」包括暂停记录、清空历史、设备状态、配对管理、存储用量。清空历史保留收藏，删除收藏请在卡片菜单操作。
- 灰色背景、圆角卡片、居中分段顶栏。固定菜单资源随固件内置。

## 电脑端运行

需要 Python 3.11 或更新版本，客户端保持运行才能监听复制和接收内容。

macOS，在项目目录运行：

```sh
./scripts/run-mac.command --pair  # 首次：USB 连接这台电脑并授权
./scripts/run-mac.command         # 日常：蓝牙连接
./scripts/run-mac.command --status
```

已有环境也可直接运行 `PYTHONPATH=host .venv/bin/python -m clipboard_agent`。

### 自动粘贴

默认开启。收到内容后先写入剪贴板，再向当前前台窗口发送粘贴快捷键；粘贴后剪贴板保留收到的内容（不恢复之前的剪贴板），可再次手动粘贴，且不会被重新上传或打乱开发板顺序。关闭方法：启动时加 `--no-auto-paste`，或在 `client-v4.json` 写入 `"auto_paste": false`，此时只更新剪贴板。

- macOS 需要辅助功能权限：首次启动会弹出系统提示。到「系统设置 → 隐私与安全性 → 辅助功能」打开运行客户端的应用（终端），然后重启客户端。未授权或当前处于安全输入（密码框）时，内容仍留在剪贴板，开发板提示「已写入剪贴板，请开启辅助功能」或「请手动粘贴」，客户端日志说明原因。
- Windows 使用 SendInput。锁屏/UAC 安全桌面或目标窗口以管理员身份运行时系统会拦截按键，内容仍在剪贴板，请手动 Ctrl-V。
- 发送期限不变：超过两秒写入租约的任务既不写入也不粘贴。

Windows 11，在解压后的项目目录运行 PowerShell：

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\run-windows.ps1 -Pair
powershell -ExecutionPolicy Bypass -File .\scripts\run-windows.ps1
```

脚本在第一次运行时创建独立 Python 环境并安装依赖，Windows 不安装 PyObjC。打开系统蓝牙，并允许运行客户端的程序访问蓝牙。

第一台电脑可直接 USB 授权。另一台电脑需先在屏幕点「⋯ → 配对管理」，然后两分钟内把 USB 接到该电脑并运行 `--pair`。完成后 USB 可接普通供电；不需要在系统蓝牙设置里把它配成键盘。最多保存 4 台电脑凭据，离线电脑不可选。

Mac 配置位于 `~/Library/Application Support/DeskClipboard/client-v4.json`，Windows 位于 `%LOCALAPPDATA%\DeskClipboard\client-v4.json`。配置含私有设备凭据，不要互相复制或公开。广播名称为 `DeskClip-` 加开发板编号末六位；真正身份还会使用 USB 凭据校验。

## 迁移与恢复

```sh
./scripts/run-mac.command --migrate
```

迁移只读取旧 Mac 数据库，建立一致数据库快照于 `migration-v4-backup/`，仅导入文字，优先收藏再选择最近内容。图片及超出容量的条目继续保存在原数据库。

协议、验收、固件编译和回退见 [HUB_V4.md](HUB_V4.md)，实施状态见 [PLAN.md](PLAN.md)。旧版 USB 面板资料保存在 [docs/USB_V3_PLAN.md](docs/USB_V3_PLAN.md)。
