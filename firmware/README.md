# 主从固件 v0.3.7-dev r1

本版补齐主端人工RESET维护窗口。Windows编译和软件检查通过，未烧录/实机验收，硬件不在；现COM6为其他设备，不能按旧端口操作。

- [日常使用](docs/蓝牙同步设备-v0.3.7-使用手册.html)
- [固件烧录](docs/蓝牙同步设备-v0.3.7-固件烧录手册.html)
- [本版软件验证](docs/v0.3.7-维护窗口软件验证.md)
- [通用镜像和哈希](dist/v0.3.7-dev/)

在根目录创建Python环境并安装requirements-dev.txt；执行tools/build_release.py与tools/verify_release.py。原生用tools/test_core.py、tools/test_runtime.py，Node用根npm install后运行firmware/tests/web_test.cjs和remote-web/test.cjs。路径映射脚本清除编译输出中的开发机用户名，厂商库与许可证保留。

主端省电时手动RESET/上电和明确的保存/重启保留120秒（30～300可调），定时/自动故障重启不增加等待。锁不影响业务；窗口不自动解锁或开AP，解锁后长按BOOT开热点，结束维护恢复计划。GPIO9不能直接唤醒深睡。

普通更新只写0x10000 APP，主端原厂app0上限0x1A0000，保留分区/NVS/密码与缓存。新机维护密码88888888/admin。当前设备的真实维护资料只在私有仓库专门目录，公开导出不带入。
