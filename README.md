# T-Display-S3 fnOS Monitor

独立的 LilyGO T-Display-S3 飞牛 NAS 状态监控项目。屏幕目标是 170×320 ST7789，默认使用安全的离线演示数据，不包含 Wi-Fi、NAS 地址或 token。

## 第一版目标

- 顶部：NAS 名称、在线/离线/演示状态
- 中部：CPU、内存、存储占用
- 底部：下载/上传吞吐、简化趋势纹理、运行天数
- 轮询周期：5 秒；网络失败必须显示 OFFLINE/错误原因，不伪装成实时数据

## 构建

```bash
pio run -e fnos_monitor
```

首次编译会下载 PlatformIO 依赖。项目固定 `espressif32@7.0.1`，使用 Arduino 框架以复用 T-Display-S3 的 ST7789 板卡支持。

## 接入真实 fnOS

复制配置模板并填写本地值：

```bash
cp include/local_config.example.h include/local_config.h
```

然后在 `local_config.h` 中填写 Wi-Fi，并将 `FNOS_HOST` 指向局域网内的 `fnos-exporter`（当前部署示例为 `192.168.9.23:9100`）。固件直接流式解析 `/metrics`，不依赖此前不可用的 `/api/system/info` JSON 接口。Docker 部署模板位于 `docker/fnos-exporter/compose.yaml`。

## UI 方向

使用 CUKTECH `system` + `network` 混合 profile：健康摘要优先，吞吐作为底部动态纹理；黑底、等宽数字、颜色与文案双编码状态。小字号和主数字必须在真实屏幕上分别验收，编译通过不等于字体清晰或布局通过。

## 硬件边界

本项目只针对 LilyGO T-Display-S3，不等同于 Seeed XIAO ESP32-S3。GPIO、Flash/PSRAM、LCD 排线和 USB-JTAG 以实际板卡型号及官方资料为准。编译或烧录成功不等于屏幕布局、字体清晰度及实时数据已经通过实机验收。
