# Agent Note: 移除 Taskflow 执行 backend，收敛为 Sequential + oneTBB

Status: implemented

## Problem

`ExecutionBackend` seam 引入后曾同时维护 Sequential、oneTBB、Taskflow 三个实现。每个实现都要独立满足同一组契约不变量（保序、背压、计量、错误传播、资源回收），契约测试与构建矩阵随实现数线性增长。benchmark 对照完成后需要选出主力并行 backend 并收敛维护面。

## Decision

执行 backend 收敛为 `SequentialExecutionBackend` 与 `OneTbbExecutionBackend` 两个实现，`ExecutionBackend` seam 与其契约不变量保留不变。Taskflow 实现（`taskflow_execution_backend.cpp`）与 backend 对照脚本整体移除，对照实测数据保留在 `docs/performance/benchmark-reports/backends/summary.md`。

backend 选择规则不扩散：`Automatic` 由 runtime 内部路由（单线程或自定义 Reader/Writer 走 Sequential，原生 I/O 多线程走 oneTBB），CLI 与公共库 API 不暴露实验 backend 选择。

## Alternatives considered

- **保留 Taskflow 作第三 backend** — 最强论据：2026-07-13 对照快照里 TaskflowCpu/4 线程 1368 MiB/s 与 oneTBB 1389 MiB/s 基本持平，ReadWrite/4 线程一度占优（783 vs 524 MiB/s），多一个实现还能当正确性交叉验证。被否：持平的吞吐不抵三套调度器各自验证契约不变量的维护成本；对照结论已归档，需要时可回查。
- **只留 oneTBB、连 Sequential 一起删** — 最强论据：实现数最少。被否：Sequential 同时承担单线程回退与契约基线两种角色，`Automatic` + 单线程、`Automatic` + 自定义 Reader/Writer 都路由到它，无 TBB 环境也靠它兜底。

## Consequences

- **收益**：backend 契约只需在两个实现上验证；依赖树收窄（conanfile 无 taskflow 条目）；`Automatic` 路由表保持简单。
- **代价与已知上限**：失去第三实现的交叉验证位——oneTBB 若在 `parallel_pipeline` 上出现调度缺陷，没有现成替代实现可切换。重访信号：oneTBB 的维护状态/许可/平台支持恶化，或新 benchmark 显示 parallel_pipeline 在某负载上有结构性劣势——届时按 perf 归档数据重新评估 Taskflow 或其他调度器。

## Verification

`rg -i taskflow src/ include/ tests/` 无命中；对照数据仍在 `docs/performance/benchmark-reports/backends/summary.md`；backend 选择规则与契约不变量见 `docs/architecture.md` 的 backend 小节。
