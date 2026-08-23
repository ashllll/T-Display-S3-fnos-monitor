# fnOS exporter 接入边界

数据源只使用 `fnos-exporter` 的 Prometheus `/metrics`。必须创建忽略提交的 `include/local_config.h` 才能构建；固件每 1 秒在独立采集任务中请求一次，请求耗时超过 1 秒时从本次完成后重新计时，不并发堆积请求，也不阻塞按键和页面动画。

当前固件读取并换算以下指标：

- `fnos_cpu_cpu_busy_all`、`fnos_cpu_cpu_temp`、`fnos_cpu_cpu_loadavg_avg1min`
- `fnos_gpu_busy`
- `fnos_memory_mem_used`、`fnos_memory_mem_total`
- `fnos_store_array_fssize`、`fnos_store_array_frsize`
- `fnos_disk_temp`、`fnos_disk_read`、`fnos_disk_smart_status_passed`
- `fnos_network_running`、`fnos_network_speed`、`fnos_network_receive`、`fnos_network_transmit`
- `fnos_uptime`、`fnos_host_name_info`

存储值按所有存储空间汇总；温度取最高值；磁盘读取和网络吞吐按接口汇总。Prometheus 原始网络值为字节每秒，屏幕换算为 Mb/s；存储和内存按二进制单位换算。

exporter 不提供 ICMP Ping、客户端数量、丢包率和 Docker 容器数量，主页面不再为这些字段保留永久的 `--` 占位。固件把一次完整 `/metrics` 请求与解析的耗时显示为 `API RT`，它只代表 exporter 响应时间，不等同于网络 Ping。系统页使用 exporter 已提供的 GPU、存储健康和磁盘温度。

已验证的部署使用 `fnos-exporter 0.5.0` 配合 `fnos 0.13.0`。上游镜像锁定的 `fnos 0.9.0` 在当前 fnOS 上会登录超时；如果重新创建或更新容器，需要再次确认 SDK 版本。开启两步验证的管理员账号还需要 exporter 上游增加挑战处理，因此当前部署使用已关闭两步验证的登录流程。

不要把 NAS 密码、两步验证码、Cookie、Wi-Fi 密码或公网地址放进源码、日志、截图或文档。
