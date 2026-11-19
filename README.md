# CC Switch Usage 0.1.17

在 TrafficMonitor 任务栏显示本机 CC Switch 的 Token 用量和费用。

## 安装

退出 TrafficMonitor，将与主程序位数一致的安装包解压到 `TrafficMonitor.exe` 所在目录，再启动 TrafficMonitor。插件文件位于 `plugins/CCSwitchUsage.dll`。从托盘菜单打开任务栏窗口，在「显示设置」中勾选「CC Switch Token」和「CC Switch 费用」。

## 统计周期与供应商

双击任务栏里的插件项目，可在今日、近 24 小时、近 7 天、近 30 天和总计之间切换。按住 Shift 双击费用可切换币种。任务栏插件菜单也可选择周期和币种，修改会在下次启动时恢复。

在「插件管理 → CC Switch Usage → 选项」中，可以分别勾选 Token 和费用供应商。任一列全部留空时，该列统计全部供应商。今日、近 7 天和近 30 天按 Windows 本地日历计算，包含当前日期及其按日归档。近 24 小时只统计带时间戳的请求。Token 总数为输入与输出之和，悬停可查看缓存读写数据。

## 汇率与倍率

- 每家供应商的费用先乘以订阅倍率，再按适用汇率折算为人民币。显示美元时只应用倍率。
- 在「选项」中选中供应商，可查看、修改或清空该供应商的汇率和倍率。清空后使用对应默认值。默认费用汇率也留空时，使用 CC Switch 保存的汇率。默认订阅倍率留空时按 1 倍计算。
- 批量倍率覆盖右侧勾选供应商的倍率。右侧未勾选时，批量操作应用到当时列出的全部供应商。
- 在选项窗口或任务栏插件菜单打开「供应商费用明细」，可查看当前已生效设置下的原始 USD、倍率、汇率、汇率来源和折算 CNY。某家有费用但缺汇率时，人民币总额显示 `¥--`，可在明细里定位该供应商。

设置保存在 `%APPDATA%\TrafficMonitor\CCSwitchUsage.ini`。升级时会迁移可唯一匹配的旧供应商设置。如旧名称已变更或有多个同名项，可在选项中取消「未匹配」的旧筛选项，再勾选对应供应商。

插件约每 15 秒只读查询一次 `%USERPROFILE%\\.cc-switch\\cc-switch.db`。用量暂不可读时，任务栏会显示 `--`。请确认 CC Switch 已运行过并已生成该数据库文件，且使用的 TrafficMonitor 与插件位数一致。仓库中的 `CCSwitchUsage-v0.1.17-x64.zip` 和 `CCSwitchUsage-v0.1.17-x86.zip` 分别对应 64 位和 32 位 TrafficMonitor。

## 从源码构建

安装 Visual Studio C++ 桌面开发工具和 Windows SDK。在对应架构的 Developer Command Prompt 中运行 `build.cmd x64` 或 `build.cmd x86`。DLL 输出到相应架构的目录。接口版权及许可见 `NOTICE.md` 和 `LICENSE`。

