# Agent Note: stat --json 用 nlohmann_json 序列化

Status: rejected — 手写 formatStatisticsJson 序列化同一指标集，不把 JSON 库接进 CLI 产物链路

## Problem

`stat` 只有 TSV 输出，下游流水线接入 QC 指标必须自行解析 TSV，需要机器可读输出（`issues/001-stat-json-output.md`）。

## Proposal

`stat --json` 用 nlohmann_json 序列化统计报告：库已在依赖树中（benchmark 结果存储用），引入零新依赖成本，且序列化正确性由成熟库保证。

## Alternatives considered

- **手写 JSON 序列化（采纳的方向）** — 指标集是固定小 schema，`formatStatisticsJson` 手写转义与拼接即可；与 TSV 共用 `buildStatisticsReport` 保证同一指标集两种序列化不漂移；CLI 产物链路不增加对 JSON 库的链接依赖。
- **不做 JSON 输出** — 曾真实权衡：TSV 已覆盖单测自检需求，缺口可以如实标注为"与 fastp 的差距"而非路线图承诺。被否：下游集成是高频诉求，机器可读输出值得做。

## Risks

否决的边界是「CLI 产物链路」而非「仓库禁用」：nlohmann_json 仍合法存在于 benchmark 结果存储（`tools/benchmark` 的 types.h 消费）。若 JSON schema 日后复杂化（嵌套结构、可选字段、schema 演进），手写维护成本可能反超引入库的成本——届时可对 CLI 输出路径单项重评。
