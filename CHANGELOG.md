# Changelog

本项目遵循 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/) 与[语义化版本](https://semver.org/lang/zh-CN/)。
版本号与根目录 `CMakeLists.txt` 中的 `PROJECT_VER` 保持一致；发布时同步更新两者并打 Git Tag。

## [Unreleased]

### Added

- 1 秒实时遥测：独立采集任务流式解析 `fnos-exporter` 的 `/metrics`，请求不阻塞按键与页面动画，耗时超过周期时从完成后重新计时。
- 背光 PWM 淡入：启动保持熄灭，首帧完成后点亮并限制为 50%。
- 主页面展示 exporter 实测响应时间 `API RT`，系统页展示 GPU 占用、存储健康与磁盘温度。
- `fitLabel`/`drawEdgePair` 宽度拟合标签，适配窄屏长字符串。

### Changed

- 移除离线演示模式；缺少 `include/local_config.h` 时编译明确失败，固件只连接真实 `fnos-exporter`。
- 轮询周期从 5 秒缩短到 1 秒，趋势历史窗口同步为 14 秒。
- 页面切换沿用弹簧动画，遥测更新不再阻塞动画。

### Fixed

- 修复背光使用未定义宏 `LCD_BL`/`LCD_POWER_ON` 导致的屏幕黑屏：背光改由 `TFT_BL`（GPIO38）经 PWM 控制，契约测试同步对齐并新增防回归断言。

## [0.3.0] - 2026-07-18

### Added

- 自动真机验收脚本：唯一串口识别、可选显式烧录、READY/内存/连续心跳验证和 JSON 输出。
- 标准固件交付包：bootloader、分区表、flash 参数、manifest、SHA-256 及 768 KiB 体积门槛。
- Python 单元测试、Dependabot 和不可变 SHA 固定的 GitHub Actions。

### Changed

- 升级到 PlatformIO Espressif32 7.0.1（官方集成 ESP-IDF 6.0.1），并固定开发与文档依赖。
- 三种板型使用 USB Serial/JTAG 作为主应用控制台，释放 D6/D7 给业务 UART。
- 在初始化 LED GPIO 前验证芯片与内存资源；GPIO 写入成功后才更新 LED 软件状态。

## [0.2.0] - 2026-07-18

### Added

- 启动时校验实际 Flash 容量，并在 S3 环境校验 ESP-IDF 已成功初始化外部 PSRAM。
- 输出 I²C、UART、SPI 默认引脚，以及可供串口验收脚本识别的 `XIAO_RUNTIME_READY` 标志。
- 心跳日志增加递增计数与运行时间，便于判断设备是否持续稳定运行。

### Fixed

- 为文档工作流指定 pip 依赖清单，并移除固件工作流中无依赖清单的 pip 缓存配置。

## [0.1.0] - 2026-07-18

### Added

- 初始模板：PlatformIO + ESP-IDF 三环境（`xiao_esp32c3` / `xiao_esp32s3` / `xiao_esp32c6`），分层 sdkconfig。
- `include/xiao_pins.h`：XIAO 丝印 D0–D10 到各板 GPIO 的符号映射及 I2C/UART/SPI 默认引脚。
- `components/xiao_board`：板级支持组件（用户 LED 心跳、板级信息 API），`main.c` 调用示例。
- MkDocs Material 中文文档站：硬件、引脚、固件、外围设备、调试、资料索引，以及 `docs/peripherals/_template.md` 外设页模板。
- GitHub Actions：固件三环境矩阵构建（含 PlatformIO 缓存）与 MkDocs Pages 部署。
- Apache-2.0 LICENSE。
