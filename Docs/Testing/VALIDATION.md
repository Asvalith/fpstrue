# 功能与配置验证

| 检查 | 结果 |
| --- | --- |
| Development Editor 编译 | 通过 |
| Development Game 编译 | 通过 |
| UE Automation `fpstrue.` | 41 项通过，0 失败、0 未运行 |
| 无警告 / 带警告 | 25 / 16；警告包含测试夹具与故障注入场景 |
| 性能配置脚本 | 56 项通过 |

自动化覆盖换弹及攻击播放身份、重复与迟到回调、死亡与离场清理、生命值事件重入、行为树契约、移动请求归属、Top-K、动画共享和采集资源释放。逐项名称及状态见[结果摘要](automation-summary.json)。

## 运行方式

配置检查：

```powershell
.\Tools\TestRenderCostConfig.ps1
```

功能测试在具备项目 Mesh、Montage、AnimBP 和蓝图的开发环境中运行，通过 Session Frontend → Automation 执行 `fpstrue.`；命令行入口见根目录 README。真实资产测试直接验证蓝图接线和动画播放。

NullRHI 验证逻辑与组件属性；实景性能测试使用 CSV Profiler 与 Unreal Insights，结果见[性能实验报告](../Performance/EXPERIMENT_LOG.md)。
