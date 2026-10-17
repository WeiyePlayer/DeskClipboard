# 小智 AI 厂家适配固件

- 板卡：LCDWIKI ES3C28P / ES3N28P，2.8 英寸 ILI9341V 显示屏，ESP32-S3 N16R8。
- 版本：Portrait 2.0.2，竖屏版；应用描述符为 `xiaozhi 2.0.2`，基于 ESP-IDF v5.4.2。
- [厂商产品页](https://www.lcdwiki.com/zh/2.8inch_ESP32-S3_Display)
- [厂商快速使用资料包](https://www.lcdwiki.com/res/ES3C28P/2.8inch_IPS_ESP32-S3_ES3C28P_ES3N28P_Quick_Start.zip)
- 包内文件：`bin/xiaozhi/Portrait_2.0.2.bin`。
- 资料包服务端 Last-Modified：2026-09-15。
- 文件大小：9,981,139 字节。
- SHA-256：`32fecaa7a8cd82d66cc8ce4a8ee411a7adef0e56b1ec1d2479c894a3759cd9ff`。
- 下载时通过 ZIP CRC 校验，并验证 ESP32-S3 bootloader 镜像校验和与 validation hash。
- 合并固件从 `0x0` 写入，DIO、80MHz、16MB Flash。

此版本是厂商为本板提供的适配版。2026-10-17 检查到的小智上游最新版为 v2.5.2，其预编译 LCDWIKI 变体对应 ES3C35P（3.5 英寸），该变体与本板的显示和音频引脚不同。

出厂固件完整备份：`../../backups/factory-84c7bb78dc60-20261017.bin`，16,777,216 字节；SHA-256 为 `72d62ee164c6383c5ae191de39cc149ad366d41d4e25626524c8690f628668e6`。备份文件可能包含原设备配置，已设为仅当前用户可读写，并排除 Git。
