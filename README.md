# ESP32-C3 + ML307 BLE 主从同步 · DEMO

当前版本v0.3.7-dev r1（demo-v0.3.7-r1）。主端4G联网采集原始BLE广播，从端WiFi/MQTT接收并重播，支持SH1106 OLED、本地维护、定时工作及远程控制台。本版只有软件验证和编译，未烧录或实机验收；实际长测及续航本轮不安排。r1另检查维护标记在编译ELF的RTC no-init区域，避免软件重启丢失。

- [前期选型与最终硬件](hardware/README.md)
- [原理图、BOM及厂商开发资料](hardware/vendor/)
- [程序源码与工具](firmware/)
- [固件与SHA256](firmware/dist/v0.3.7-dev/)
- [日常使用手册](firmware/docs/蓝牙同步设备-v0.3.7-使用手册.html)
- [固件烧录手册](firmware/docs/蓝牙同步设备-v0.3.7-固件烧录手册.html)
- [部署远程控制台](docs/部署远程控制台.md) · [打开控制台](https://bedtimeenforcer.github.io/esp32-c3-ml307-ble-public/)

开启主端省电后先短按RESET，启动完成后保留120秒维护窗口，可调30～300秒。显示锁开启时先解锁，再长按BOOT三秒开热点。窗口不停止正常采集上传；定时唤醒和自动故障重启不增加等待。GPIO9 BOOT不能直接唤醒此C3的深睡。

通用首次维护热点和后台密码88888888、用户名admin；升级保留旧NVS。设备上网与MQTT使用自己的账号，示例全部为占位符。V5.1原厂双OTA主板APP仅更新0x10000、上限0x1A0000，不直接烧通用flash0。

此仓库从脱敏文件建立全新Git历史；不包含真实绑定、密码、平台API、原始Flash或私人购买截图。第三方资料/代码保留原版权与许可证，PDF公开副本仅清理元数据，技术正文未变。

构建：安装firmware/requirements-dev.txt，执行firmware/tools/build_release.py。Node检查先npm install，再运行node firmware/tests/web_test.cjs及node remote-web/test.cjs。原生检查使用CXX或PATH中的g++。测试/编译不代表实机通过。
