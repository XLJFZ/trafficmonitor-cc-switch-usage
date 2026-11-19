# CC Switch Usage 0.1.1

TrafficMonitor 任务栏插件，读取本机 CC Switch 的用量记录。选择与 TrafficMonitor 位数一致的压缩包，解压到 `TrafficMonitor.exe` 所在目录，然后重启 TrafficMonitor。

1. 从托盘菜单打开「显示任务栏窗口」。
2. 右键任务栏监控区，打开「显示设置」，勾选「CC Switch Token」和「CC Switch 费用」。
3. 双击其中任意一项，即可循环切换今日、近 7 天和近 30 天。两项使用同一周期，重新启动后从今日开始。

按住 Shift 并双击「CC Switch 费用」，即可在美元和人民币之间切换。也可通过任务栏右键菜单中的插件命令选择币种。重启后默认显示美元。

人民币金额由 CC Switch 记录的美元费用折算。内置参考汇率为 1 USD = 7.2 CNY，不会联网更新。若需自定义，在 DLL 所在的 `plugins` 目录创建 `CCSwitchUsage.ini`，填入以下内容。显示人民币时，下一次刷新会读取新汇率。

```ini
[Currency]
CnyPerUsd=7.2
```

统计按 Windows 本地日历计算，近 7 天和近 30 天均包含今天。Token 为数据库记录的输入与输出之和，缓存字段在悬停详情中单列。费用是 CC Switch 记录的美元金额。插件每隔约 15 秒读取一次数据库，范围包含各供应商的本地请求记录和历史归档。

数据库默认位于 `%USERPROFILE%\.cc-switch\cc-switch.db`，使用只读连接。显示 `--` 时，可检查数据库位置和 CC Switch 的用量记录。

源码可用 Visual Studio 的 C++ 开发环境与 Windows SDK 构建。在相应架构的 Developer Command Prompt 中运行 `build.cmd x64` 或 `build.cmd x86`。

插件接口头文件取自 bemaru/trafficmonitor-ai-usage-plugin v0.3.13，其中保留了 TrafficMonitor 原有的版权声明。许可见 `LICENSE`。
