# fnOS exporter 接入边界

真实数据源使用 `fnos-exporter` 的 Prometheus `/metrics`，默认仍以离线演示模式构建；只有创建忽略提交的 `include/local_config.h` 后才连接局域网。

当前固件读取并换算以下指标：

- `fnos_cpu_cpu_busy_all`、`fnos_cpu_cpu_temp`、`fnos_cpu_cpu_loadavg_avg1min`
- `fnos_memory_mem_used`、`fnos_memory_mem_total`
- `fnos_store_array_fssize`、`fnos_store_array_frsize`
- `fnos_disk_temp`、`fnos_disk_read`、`fnos_disk_smart_status_passed`
- `fnos_network_running`、`fnos_network_speed`、`fnos_network_receive`、`fnos_network_transmit`
- `fnos_uptime`、`fnos_host_name_info`

存储值按所有存储空间汇总；温度取最高值；磁盘读取和网络吞吐按接口汇总。Prometheus 原始网络值为字节每秒，屏幕换算为 Mb/s；存储和内存按二进制单位换算。

exporter 不提供 Ping、客户端数量、丢包率和 Docker 容器数量，这些项目必须显示 `--` 或 `UNKNOWN`，不能从其他指标推算。

已验证的部署使用 `fnos-exporter 0.5.0` 配合 `fnos 0.13.0`。上游镜像锁定的 `fnos 0.9.0` 在当前 fnOS 上会登录超时；如果重新创建或更新容器，需要再次确认 SDK 版本。开启两步验证的管理员账号还需要 exporter 上游增加挑战处理，因此当前部署使用已关闭两步验证的登录流程。

不要把 NAS 密码、两步验证码、Cookie、Wi-Fi 密码或公网地址放进源码、日志、截图或文档。
