# CC Switch Usage for TrafficMonitor

在 Windows 任务栏显示 CC Switch 记录的 Token 和费用。两项可分别筛选供应商。

## 安装

1. 从 GitHub 的 Releases 下载与 TrafficMonitor 位数一致的 ZIP。发布包面向 Windows 10/11。
2. 退出 TrafficMonitor，将 ZIP 解压到 `TrafficMonitor.exe` 所在目录。插件文件应位于 `plugins/CCSwitchUsage.dll`。
3. 重新启动 TrafficMonitor。在任务栏监控区右键打开「显示设置」，勾选「CC Switch Token」和「CC Switch 费用」。

## 使用

- 双击任务栏里的 Token 或费用，切换今日、最近 24 小时、近 7 天和近 30 天。今日从本地零点开始，最近 24 小时按当前时刻滚动。近 7 天和近 30 天都包含今天。
- 按住 Shift 双击费用，可切换美元和人民币。
- 在「插件管理 → CC Switch Usage → 选项」中，可设置周期、币种和人民币实付单价。填入 `0.12` 表示每记录 1 美元用量，按实付 0.12 元显示。留空时用 CC Switch 保存的汇率。
- Token 和费用分别多选供应商。每列均可全选和反选，留空时统计全部供应商。点击「确定」后，统计周期、币种、实付单价和两列供应商会在下次启动时恢复。任务栏快捷操作也会保存周期和币种。

Token 是数据库记录的输入与输出之和。悬停可查看缓存字段和费用。日历周期包含历史归档，滚动 24 小时使用带有具体时间的请求记录。

插件约每 15 秒读取一次 `%USERPROFILE%\.cc-switch` 下的本地用量数据库和汇率设置。数据库以只读方式打开，插件不联网。如果没有可用汇率，可以切换为美元。

## 从源码构建

安装 Visual Studio 的 C++ 桌面开发工具和 Windows SDK。打开对应架构的 Developer Command Prompt，运行 `build.cmd x64` 或 `build.cmd x86`。DLL 输出到对应架构的目录。

## 来源和许可

TrafficMonitor 插件接口沿用 [bemaru/trafficmonitor-ai-usage-plugin v0.3.13](https://github.com/bemaru/trafficmonitor-ai-usage-plugin/tree/v0.3.13) 收录的接口头文件。保留原有版权和许可声明，见 [LICENSE](LICENSE) 和 [NOTICE.md](NOTICE.md)。
