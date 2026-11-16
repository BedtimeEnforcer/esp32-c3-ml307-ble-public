# ESP32-C3 + ML307 蓝牙主从同步

把一台蓝牙设备的广播，原样搬到另一个地方重播。

> BLE relay: an ESP32-C3 master captures the raw advertisement of a fixed-MAC BLE device and forwards it over 4G / MQTT; an ESP32-C3 slave rebroadcasts it at another location. No parsing, no re-encoding — the raw bytes are copied whole.

## 做这个是为了解决什么

公司有多处办公点，只有一处放着蓝牙打卡机。主端放在打卡机旁边，抓它的蓝牙广播；从端放在分公司或者项目地址，收到后原样重播。员工在从端那边打卡，效果和站在打卡机旁边一样。

不只打卡机能用这个办法。任何固定 MAC 的蓝牙广播都可以这样搬：iBeacon、BLE 信标、门禁、传感器网关。主端只负责抓原始广播、发出去，从端只负责收、重播，中间不解析内容，所以滚动码、加密字段这些也都跟着走，不用管它是什么协议。

## 怎么工作的

1. 主端（ESP32-C3 + ML307 4G）扫描指定 MAC 的设备，拿到 ADV 和 Scan Response 的原始字节。
2. 原始数据经 MQTT（TLS，QoS 1）发到云端，带 PUBACK 确认。
3. 从端（ESP32-C3，WiFi）订阅收到后，用同一个 MAC 和同样的广播参数重播。
4. 主端平时按时段工作：每天最多 5 个北京时间时段，每段开始时采一次，成功就睡；时段外深睡。从端接电常开，每 10 秒检查一次有没有新数据。
5. 远程控制台是静态网页（GitHub Pages），走 WSS 直连 MQTT，能看状态、改时段、发指令。

## 硬件

| 角色 | 板子 | 联网 | 供电 |
| --- | --- | --- | --- |
| 主端 | 小智物联 ESP32-C3 + ML307 V5.1 一体板 | 4G（ML307，UART） | 5V 电池 |
| 从端 | 合宙 ESP32-C3 简约版（USB 直连） | WiFi | USB 常供电 |
| 显示（可选） | 1.3 寸 SH1106 OLED，128×64，I2C 四针 | 主屏 SDA6/SCL7，从屏 SDA4/SCL5 | — |

选型过程和接线见 [hardware/README.md](hardware/README.md)，厂商手册和原理图在 [hardware/vendor/](hardware/vendor/)。

## 仓库里有什么

- [firmware/](firmware/)：主从固件源码
- [firmware/dist/v0.3.7-dev/](firmware/dist/v0.3.7-dev/)：编译好的固件和 SHA256
- [日常使用手册](firmware/docs/蓝牙同步设备-v0.3.7-使用手册.html)、[固件烧录手册](firmware/docs/蓝牙同步设备-v0.3.7-固件烧录手册.html)
- [remote-web/](remote-web/)：远程控制台静态网页，[在线打开](https://bedtimeenforcer.github.io/esp32-c3-ml307-ble-public/)，部署方法见 [docs/部署远程控制台.md](docs/部署远程控制台.md)
- [hardware/](hardware/)：选型、原理图、BOM、厂商手册

## 上手

1. 按 [hardware/README.md](hardware/README.md) 接好线。
2. 编译：装好 `firmware/requirements-dev.txt`，跑 `firmware/tools/build_release.py`；或者直接用 [firmware/dist/v0.3.7-dev/](firmware/dist/v0.3.7-dev/) 里的现成固件烧录，方法见烧录手册。
3. 配好 WiFi / 4G APN、MQTT 账号和要跟的设备 MAC，见使用手册。
4. 部署远程控制台见 [docs/部署远程控制台.md](docs/部署远程控制台.md)。

维护后台默认账号 `admin`，密码 `88888888`，第一次用完就改掉。MQTT 请给主端、从端、网页各建受限账号，不要共用一个全权限账号。

## 当前状态

- 版本：v0.3.7-dev r1。这一版只做了软件验证（791 项原生模拟检查，本地 111 项、远程 64 项 DOM 检查），没烧录、没上实机。
- 上一版 v0.3.6 在实机上跑通过：主端 4G / TLS / MQTT 上报和 PUBACK，从端 WiFi / 校时 / MQTT 双订阅和广播，都正常。
- 还没验的：独立射频抓包、屏幕按键、跨日深睡、多从端、断网掉电恢复。长测和续航本轮不做。

## 许可与来源

- 自己写的代码和文档随仓库公开；第三方库和厂商资料保留原版权和许可证，PDF 只清了元数据，正文没动。
- 仓库里没有任何真实绑定、密码、平台 API 或者原始 Flash，账号示例全是占位符，用自己的替换。
