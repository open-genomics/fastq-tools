# 性能数据与优化记录

本目录沉淀 FastQTools 的性能测试数据与优化历程。分两层：

| 子目录 | 性质 | 内容 |
|--------|------|------|
| [`benchmark-reports/`](./benchmark-reports/) | 原始快照 | 不可变数据：表格、JSON、profiler 输出。按场景+日期归档。 |
| [`optimizations/`](./optimizations/) | 优化叙事 | 每条记录一次优化：背景、瓶颈分析、手段、before/after 对比、结论。 |

## 阅读顺序

1. [`environment.md`](./environment.md) — 测试环境基线，所有快照的口径。
2. [`benchmark-reports/README.md`](./benchmark-reports/README.md) — 报告归档规范与 summary.md 模板。
3. [`benchmark-reports/v4-baseline/2026-07-17/summary.md`](./benchmark-reports/v4-baseline/2026-07-17/summary.md) — v4 生产路径基线、原始 JSON 和 median/CV 摘要。
4. [`optimizations/2026-07-13-backlog.md`](./optimizations/2026-07-13-backlog.md) — 已按 v4 证据校正的性能优化路线图。
5. [`benchmark-reports/`](./benchmark-reports/) — 历次原始数据。
6. [`optimizations/`](./optimizations/) — 数据背后的故事（已完成的优化叙事）。
7. [`../benchmark.md`](../benchmark.md) — 面向用户的代表性结果摘要。

## 写作规范

- **快照与叙事分离**：`benchmark-reports/` 只放数据，不写分析；分析写进 `optimizations/` 并链接快照。
- **报告格式**：`summary.md` 按 [`benchmark-reports/README.md`](./benchmark-reports/README.md) 中的模板撰写。
- **同口径对比**：所有快照必须引用 `environment.md` 中的基线；偏离基线需在快照头注明。
- **文件命名**：`YYYY-MM-DD-<kebab-case-slug>.md`。
- **快照归档路径**：`benchmark-reports/<场景>/<YYYY-MM-DD>/`，内含 `summary.md` 与原始输出文件。
- **优化条目必填**：基线快照链接、优化后快照链接、同口径对比表。
- **中文撰写**。

## 索引

### 优化记录

> 按日期倒序。新增后在此追加一行。

| 日期 | 标题 | 场景 | 增益 |
|------|------|------|------|
| 2026-07-17 | [Writer 热点复核](./optimizations/2026-07-17-writer-profile.md) | Writer profile | —（real_time 被放大，未达优化门槛） |
| 2026-07-17 | [统计热点复核](./optimizations/2026-07-17-statistics-profile.md) | 统计 profile | —（CV 45.64% 过高，未达优化门槛） |
| 2026-07-13 | [性能优化路线图](./optimizations/2026-07-13-backlog.md) | 全局 backlog（已按 v4 证据门校正） | —（路线图） |

### 快照归档

| 场景 | 路径 | 说明 |
|------|------|------|
| 历史 backend 对照 | [`benchmark-reports/backends/summary.md`](./benchmark-reports/backends/summary.md) | oneTBB vs Taskflow（v3 归档） |
| Callgrind 热点分析 | [`benchmark-reports/callgrind/2026-07-13/summary.md`](./benchmark-reports/callgrind/2026-07-13/summary.md) | stat + filter 函数级热点 |
| Cachegrind 缓存分析 | [`benchmark-reports/cachegrind/2026-07-13/summary.md`](./benchmark-reports/cachegrind/2026-07-13/summary.md) | L1/L2/LL 命中率模拟 |
| Memcheck 内存正确性 | [`benchmark-reports/memcheck/2026-07-13/summary.md`](./benchmark-reports/memcheck/2026-07-13/summary.md) | 内存泄露 + 越界检测 |
| Massif 堆内存分析 | [`benchmark-reports/massif/2026-07-13/summary.md`](./benchmark-reports/massif/2026-07-13/summary.md) | 堆内存峰值 + 增长曲线 |
| Helgrind 线程正确性 | [`benchmark-reports/helgrind/2026-07-13/summary.md`](./benchmark-reports/helgrind/2026-07-13/summary.md) | 多线程数据竞争检测 |
| v4 生产基线 | [`benchmark-reports/v4-baseline/2026-07-17/summary.md`](./benchmark-reports/v4-baseline/2026-07-17/summary.md) | 1M×150 bp，Reader/Writer/filter/stat，5 次重复 |
| FASTQ IO 历史快照 | [`benchmark-reports/fastq-io/2026-07-13/summary.md`](./benchmark-reports/fastq-io/2026-07-13/summary.md) | 旧版 Reader/Writer 吞吐，仅作历史记录 |

## 如何跑基准

见 [`../benchmark.md`](../benchmark.md) 的"基准测试工具"章节。简述：

```bash
# 构建基准目标（需 -DBUILD_BENCHMARKS=ON 配置的构建目录）
cmake --build build/clang-release --target benchmarks

# 运行全部生产基准并保存 raw JSON + median/CV
cmake --build build/clang-release --target run_benchmarks
```

新快照入库前，确认：
1. 测试机与 `environment.md` 一致；不一致则在快照头注明差异。
2. 数据规模、构建类型、编译器版本完整记录。
3. 原始 JSON/CSV 一并归档，便于后续复核。
