# 当前验证与证据（2026-10-24）

当前 Source 来源 fb1b7a3，测试在完整授权资产环境运行；main 只展示同一源码与摘录，没有独立进行运行验收。

| 检查 | 原环境结果 | 证据 |
| --- | --- | --- |
| Editor / Shipping 原生编译 | 成功；最后增量检查 up to date | [构建摘要](automation-20261024.json) |
| 完整 UE Automation | 99/99，86 clean、13 warned，0 failed/notRun | 同上，逐项名称/状态/告警数 |
| 正常联网与恢复 | 通过 | [原门禁摘录](network-20261024.json) |
| 严格弱网 | 未通过零意外拒绝门禁，重连未进入 | 同上，保留失败 |
| 160 AI 线程/模块 | 单轮 Editor Trace 诊断，有有效帧对齐 | [诊断摘录](160ai-20261024.json) |

公开 JSON 保存原报告 SHA-256，不包含原始日志、启动命令、机器身份或恢复票据。警告没有归零；缺失指标仍为未知/不适用，不填成0。旧 [43项摘要](automation-history-43.json) 仅作历史，不与当前计数相加。

可离线检查交付工具和测试注册关系：`Tools/Networking/BuildPvpDelivery.ps1 -SelfTest`，不启动 UE，不等于游戏测试通过。完整 `fpstrue.` Automation、蓝图集成与联网/渲染测试需先恢复资源，见 [依赖](../ASSET_REQUIREMENTS.md)。

历史 [性能实验](../Performance/EXPERIMENT_LOG.md) 与本次不同构建/负载，不把其百分比拼成当前整帧提升。
