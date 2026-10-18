# V4 蓝牙实现与验收

当前产品范围为 **纯文字 + BLE**，不记录或发送图片，不使用 Wi-Fi。旧数据库及备份中的图片保留。

## 设备和存储

ESP-IDF 5.4.2，ESP32-S3 N16R8，ILI9341 240×320，FT6336。保持 4MiB 应用分区，在 `0x410000` 增加 `0xA00000`（10MiB）FAT + wear leveling 分区，8MiB 为应用缓存上限。

`hub_store.c` 是唯一数据源。两个带世代号和 CRC 的索引轮换写入，临时文件校验 SHA-256、UTF-8、卡片尺寸及区域后才发布。每条 `.DAT` 保存文字正文，`.RGB` 保存卡片；启动检查摘要、删除未完成上传和孤儿文件。已初始化的损坏文件系统不自动格式化。

GUI 向队列提交操作，后台任务处理存储和传输。固定菜单资源内置。客户端剪贴板操作在主线程，蓝牙在后台循环。固件不启动 Wi-Fi、不监听 IP 端口，清除此前 Wi-Fi 原型保存的 SSID、密码和证书。

## 息屏

`screen.c`：背光 GPIO45（高电平点亮，厂家例程同）改为 LEDC PWM（5kHz、10 位）。无触摸达到「息屏时间」后背光降到约 20%，再过 5 秒关闭背光并发送 DISPOFF；LVGL、BLE 和后台任务照常运行（空闲时本来就不刷新）。闲置计时取自 LVGL（`lv_display_get_inactive_time`）。

变暗或熄屏时 `lv_layer_top()` 上有一层透明遮罩接住第一次触摸：按下即亮屏，抬手后才移除，因此唤醒的这一下（包括长按）不会触发卡片、⋮、按钮或发送。电脑提交新记录（`commit` 成功）后亮屏并重新计时；这一调用在 hub 锁之外执行，避免与 LVGL 锁交叉。

设置在「⋯ → 息屏时间」，点击依次切换 30秒 / 1分钟（默认）/ 2分钟 / 5分钟 / 从不，菜单保持打开。保存在 NVS 命名空间 `screen` 的 `timeout`（u8 序号），与配对数据 `deskclip/peers` 分开；只刷应用不影响它。模拟器 `firmware/sim` 覆盖变暗、熄屏、吞掉唤醒点击、新记录唤醒、选项循环和持久化。

## BLE 协议

服务 UUID 为 `89c11000-4e46-4a49-9418-dfc18a624070`，写入特征第一段为 `89c11001`，通知特征第一段为 `89c11002`，其余相同。支持最多四个连接。

消息为 little-endian `uint32 length` 加正文。ATT 片段不是消息边界。请求最多约 48KiB、响应最多 1MiB；二进制块为 16KiB。Mac 使用 CoreBluetooth 原生发送容量信号，其他后端每四个片段确认一次；板端通知窗口为四。末片段带响应确认顺序，首选 MTU 247，并协商长链路数据包，兼容较小 MTU。

USB 为每台电脑配置随机 256 位私有凭据。BLE 握手发送设备 UUID 和随机挑战，不明文发送凭据。开发板使用 HMAC-SHA256 对开发板编号、电脑编号、双方新随机数证明身份并派生会话密钥。内容使用 AES-256-GCM、分方向 nonce 和严格递增计数，拒绝篡改、反射和旧包重放。它不依赖将开发板配成系统蓝牙键盘。

握手：`H + device_uuid[36] + client_nonce[16]`；响应：`H + server_nonce[16] + proof[32]`。加密记录：`E + uint64 counter + ciphertext + tag[16]`。派生算法以 `ble_security.py` 和 `hub_ble.c` 为准。

| 解密后标记 | 内容 |
| --- | --- |
| J | JSON 控制请求或结果 |
| U | event UUID 36 字节、card 标志 1 字节、offset uint32、上传数据 |
| D | job ID 32 字节、offset uint32、size uint32，申请下载 |
| B | 下载响应原始数据 |

控制操作为 `status / heartbeat / list / begin / commit / abort / poll / ready / ack`。来源身份由凭据确定，不能通过 metadata 冒充。事件 UUID 和内容摘要处理重试及去重。

心跳约两秒，十五秒失联或 BLE 断开即取消该电脑任务。下载每个成功数据块续期两分钟的空闲超时；校验原始字节后申请独立的两秒写入租约，下载续期不会延长该写入期限。过期任务和新会话不重放；接收内容带自身标记，不再次上传。收到成功 ACK 才提示已写入。

自动粘贴（默认开启，`--no-auto-paste` 或配置 `"auto_paste": false` 关闭）：主线程在写入租约内写剪贴板后立即检查权限并发送 Command-V / Ctrl-V，超过期限不写也不按键，粘贴后不恢复旧剪贴板（避免慢应用读到旧内容，也避免恢复动作被当作新复制上传）。ACK 增加 `paste` 字段：`pasted` / `off` / `accessibility` / `secure_input` / `blocked` / `late`，固件据此显示「已在对方电脑粘贴」「已写入剪贴板，请开启辅助功能」「已写入剪贴板，请手动粘贴」或原「已写入对方剪贴板」。旧固件忽略该字段。决策逻辑见 `autopaste.py`，测试 `test_autopaste.py` 只用假按键后端。

USB 使用协议 4，RPC 类型 `0x20 / 0x21`，正文开头为 uint32 关联号。公开本地操作只有状态、首次初始化、授权；管理和诊断操作需要电脑凭据。旧 V3 客户端被拒绝。

## 手机配对码（SPAKE2）

Android 没有 USB 配对，改用开发板显示的 6 位配对码。点「⋯ → 配对管理」（或 USB 调试命令 `pair_code`）打开 2 分钟窗口并生成均匀随机 6 位码；配对码一次有效，错误确认累计 3 次即作废，同一时刻只允许一个连接配对（30 秒会话）。

为什么用 PAKE：「ECDH + HMAC(配对码)」能防被动嗅探，但中间人可以拿到对自己已知密钥的 HMAC 后离线穷举 10^6 个配对码。SPAKE2 每次在线尝试只能验证一个猜测，嗅探者得不到任何关于配对码或凭据的信息；3 次上限下猜中概率为 3×10⁻⁶。

协议是 RFC 9382 的 SPAKE2-P256-SHA256-HKDF-HMAC（手机为 A 用 M，开发板为 B 用 N，RFC 给定的 M/N）：`w = SHA-512("deskclip-v4/pair-code\0" || 码) mod n`，身份 A = 设备 UUID，B = board_id，转录 TT、Ke/Ka、KcA/KcB、cA/cB 与 RFC 第 3.3/4 节一致。凭据 = `HKDF-SHA256(Ke, "deskclip-v4/credential")`，32 字节，与 USB 配对写入的每设备密钥同一形式；它从不在空中传输。之后的会话仍用原有 H 握手、AES-GCM 方向分离记录和序号防重放。

记录（握手前，沿用 <u32 长度> 分片）：`P01 设备ID pA 名称 平台` → `P02 board_id pB` → `P03 cA` → `P04 cB`（开发板已保存对端）或 `PFF 错误`。开发板先校验 pA 在曲线上，标量乘法用 mbedtls_ecp_mul（带盲化），cA 用常量时间比较；只有 cA 正确才写 NVS `deskclip/peers`（平台 `android`，与原有 Mac/Windows 条目并存，格式不变）。

实现：Python 参考 `host/clipboard_agent/pake.py`（对 RFC 附录 B 向量测试）；固件 `main/pair_crypto.c`（`firmware/sim/build_pair.sh` 用 ESP-IDF 的 mbedtls 在主机编译，`test_pair_crypto.py` 对 RFC 向量和 Python 参考逐字节核对）；Android `android/.../Spake2.kt`（JVM 单元测试使用 `host/gen_android_vectors.py` 生成的向量）。实板检查：`PYTHONPATH=host .venv/bin/python host/pair_code_check.py`（经 USB 取码，蓝牙配对临时测试设备、验证会话、单次有效、错码拒绝，最后撤销）。

## 验证

```sh
PYTHONPATH=host:host/tests .venv/bin/python -m unittest \
  test_ble_security test_hub_store test_hub_client test_render test_frames \
  test_watcher test_session test_store test_extract test_pngutil
bash firmware/sim/build.sh
```

存储测试编译实际固件 C 代码，在 ASan/UBSan 下验证损坏索引、断电点、校验、容量、收藏和幂等。BLE 测试验证身份、篡改、重放、方向隔离及分片。

实板脚本只允许空历史，在迁移前执行；结束删除临时电脑凭据和普通测试记录，保留两条收藏用于拔电核对，之后可通过卡片菜单删除：

```sh
PYTHONPATH=host .venv/bin/python -u host/hub_acceptance.py --minutes 30
```

`--minutes 4` 是覆盖全部检查的最短时长（连续阶段第 7 轮才做 BLE 重连检查，约 190 秒）。2026-10-17 纯文字固件以 4 分钟通过，报告在 `backups/v4-text-ble-acceptance.json`。脚本在后台运行时 SIGINT 可能被忽略；中断后需手动删除测试记录并撤销「USB test peer」。

脚本使用真实 Mac BLE 链路传送中文、换行、表情、64KiB 长文字，比较完整字节和摘要，验证 Mac 系统剪贴板、重启恢复、未提交上传、暂停、清空、收藏、图片拒绝、错误凭据、重放、篡改及连续运行。第二电脑通过 USB 测试凭据模拟，不代表 Windows 原生剪贴板或两台独立电脑同时连接 BLE 已通过。

实板物理验收（2026-10-17，纯文字固件）：拔插 USB 断电重启后（复位原因 power-on）两条收藏 #13（59 字节）与 #15（64KiB）仍在，经临时 USB 对端下载与验收报告、索引 SHA-256 逐字节一致；触摸、滚动、卡片 ⋮ 菜单和右上角设置按钮实测正常。

Windows 11 需另行实际验收：中文、换行、表情，Mac→Windows / Windows→Mac，过期任务不写入、收到后不再上传。物理触摸、滚动和真正拔电也需要实板检查，模拟点击和软件重启不能替代。

## 编译与回退

```sh
source /Users/weiye/esp/esp-idf/export.sh
idf.py -C firmware/clipboard build
```

升级前整片备份在 `backups/v3-before-hub/`：`full-flash.bin`（16MiB）及 SHA-256、旧应用/分区表/引导程序、Mac 数据库与原图。新固件无需擦除整片 Flash。烧录前按住 BOOT 拔插 USB 后松开，串口号可能变化。

仅需回退时执行以下命令，将整片 Flash 恢复为备份状态：

```sh
.venv/bin/python -m esptool --chip esp32s3 --port /dev/cu.usbmodem1101 \
  --before no-reset --after watchdog-reset write-flash \
  0x0 backups/v3-before-hub/full-flash.bin
PYTHONPATH=host .venv/bin/python -m clipboard_agent.agent
```

V4 仅导入文字，不改写原 Mac 数据库或图片。恢复 V3 固件后才能运行旧 agent，不要让两个版本同时监听剪贴板。

## 备份与恢复开发板状态（换刷其他固件前）

配对在 NVS（`0x9000`，`0x6000`，命名空间 `deskclip` 的 `peers`/`initialized`），历史在 `hubdata`（`0x410000`，`0xA00000`，FAT + 磨损均衡）。开发板编号和 BLE 公共地址来自 eFuse MAC，重刷不变；没有系统级蓝牙绑定需要恢复。两台电脑的 `client-v4.json` 不受影响。

```sh
# 按住 BOOT 拔插 USB 进入下载模式后
scripts/backup-board.sh            # 分区表、bootloader、nvs、phy_init、应用、hubdata → backups/board-<时间>/（含 SHA256SUMS、manifest.json）
# ……刷其他固件……再次进入下载模式
scripts/restore-board.sh backups/board-<时间> --app --dry-run   # 先核对
scripts/restore-board.sh backups/board-<时间> --app             # 校验 SHA-256 与 MAC 后写回并重启
```

只读写列出的区域，不整片擦除；备份含配对凭据，`backups/` 不入库。

## 界面卡死保护（2026-10-18）

现象：点一下开发板后界面不再响应，同时电脑和手机的蓝牙请求、USB 状态请求全部超时，开发板不会自动重启。

原因：触摸读取（esp_lcd I2C，超时为 -1 即永久等待）在 LVGL 任务里持有 LVGL 锁执行；触摸芯片或 I2C 总线一旦卡住，LVGL 任务永远阻塞。hub 后台任务持有存储锁去等 LVGL 锁，于是所有 BLE / USB 请求都卡在存储锁上。任务看门狗只检测空闲任务，不会发现这种阻塞。

修复：
- `board.c`：触摸改为自己的轮询读取，每次 I2C 最多等 50 ms；连续 5 次失败时复位 I2C 总线，界面继续运行。
- `guard.c`：LVGL 每 500 ms 打一次心跳；高优先级监视任务发现 15 秒没有心跳就记录卡住的阶段（NVS `guard`）并重启开发板（历史与配对都在闪存中，不受影响）。
- USB `status` 新增 `last_stall_stage`（0 无；1 其他；2 触摸；3 渲染；4 等待屏幕刷新）和 `last_stall_uptime_s`，用于确认以后是否还发生过。

### 更新（2026-10-18 20:20）：真正的原因是屏幕刷新，不是触摸

382b822 的卡死保护记录到 `last_stall_stage: 4`（等待屏幕刷新完成），`reset_reason: 3`（看门狗主动重启）。原因：LVGL 绘制缓冲在 PSRAM，SPI 不能直接 DMA，驱动每次刷新都要临时申请一块内部 DMA 内存（最多 24000 字节）做中转；内部内存紧张时申请失败，`esp_lcd_panel_draw_bitmap` 返回错误，但 esp_lvgl_port 忽略返回值，LVGL 于是永远等待刷新完成。点卡片会弹出全屏半透明层，需要大面积刷新，最容易触发。

修复（board.c）：绘制缓冲改到内部 DMA 内存（20 行 × 2，开机一次性分配，失败才退回 PSRAM），SPI 不再需要临时申请；自己的刷新回调检查绘制错误并立即释放 LVGL；等待刷新最多 500 ms 且让出 CPU（不再空转）；出错后自动整屏重绘。USB `status` 新增 `free_internal`、`min_free_internal`、`largest_dma`、`flush_errors`、`flush_timeouts`。
