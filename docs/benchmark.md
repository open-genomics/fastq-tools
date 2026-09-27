# 性能基准

## 测试环境

- CPU：AMD Ryzen 7 5800H（WSL2，8 核 16 线程）
- 数据：固定 seed=42，1M reads × 150 bp（316,888,890 bytes）
- 构建：Clang 21 Release，Google Benchmark 1.9.5，5 次重复
- 详细快照：[v4-baseline/2026-07-17](./performance/benchmark-reports/v4-baseline/2026-07-17/summary.md)

## 代表性结果

| 工作负载 | 结果 |
|----------|------|
| FASTQ 读取/写出 | 见 v4 raw JSON 与 median/CV 摘要 |
| 组合过滤处理 | 真实 Pipeline + trim/predicate 场景，见 v4 摘要 |
| 完整统计分析 | 真实 `FqStatisticWorker`，见 v4 摘要 |

## 解读

- v4 基线同时覆盖 plain、gzip-1/6/9、single/batch API，不能用旧的单一 Writer 数字代表所有场景。
- 当前 WSL2 sandbox 的 real-time 绝对值存在放大，跨环境比较必须以同一机器、同一命令为准；详见快照 `environment.md`。
- 只有 profile 达到 inclusive CPU ≥15% 且优化后 median 提升 ≥10% 时，才把热点纳入性能路线。

## 基准测试工具

benchmark 目标要求构建目录以 `-DBUILD_BENCHMARKS=ON` 配置（benchmark、nlohmann_json、GoogleTest 由 Conan 按需加入）：

```bash
# 构建并运行全部生产基准，raw JSON + median/CV 输出到
# docs/performance/benchmark-reports/results/
cmake --build build/clang-release --target run_benchmarks

# 自定义输出目录时直接调用 runner
python3 tools/benchmark/scripts/run_benchmarks.py \
  --build-dir build/clang-release \
  --output-dir docs/performance/benchmark-reports/v4-baseline/<YYYY-MM-DD> \
  --repetitions 5
```

基准套件在 `tools/benchmark/`，基于 Google Benchmark：

| 文件 | 测试内容 |
|------|----------|
| `fastq_io_benchmark.cpp` | 读写吞吐量 |
| `filter_benchmark.cpp` | filter 端到端 |
| `stat_benchmark.cpp` | stat 计算性能 |
| `pipeline_benchmark.cpp` | Sequential / oneTBB 同契约对照 |
| `concurrency_benchmark.cpp` | 真实 Pipeline 的线程数与 batch 大小扫描 |
| `object_pool_benchmark.cpp` | 对象池分配性能 |

## 历史 Backend 对照（仅归档）

v4 benchmark 只运行 Sequential 与 oneTBB；Taskflow backend 已移除，决策记录见 [移除 Taskflow 的笔记](../.agents/notes/implemented/simplification/2026-07-17-remove-taskflow-backend.md)。2026-07-13 原始快照（16 X 3193.9 MHz，100K × 150bp，plain FASTQ，Clang 21 Release + libc++，每 case 7 次重复）的完整 p50/p95/RSS 数据保留在 [`performance/benchmark-reports/backends/summary.md`](./performance/benchmark-reports/backends/summary.md)。

逐线程数对照（同工作负载同线程数取吞吐更高者为胜）：

| 工作负载 | oneTBB 最佳 | Taskflow 最佳 | 胜者 | 差值 |
| --- | --- | --- | --- | --- |
| CPU-only 2T | 1012.41 MiB/s | 835.00 MiB/s | oneTBB | +21% |
| CPU-only 4T | 1389.51 MiB/s | 1368.42 MiB/s | oneTBB | +1.5% |
| CPU-only 8T | 1186.69 MiB/s | 1267.43 MiB/s | Taskflow | +6.8% |
| ReadWrite 2T | 420.78 MiB/s | 385.32 MiB/s | oneTBB | +9.2% |
| ReadWrite 4T | 524.51 MiB/s | 783.84 MiB/s | Taskflow | +49.4% |
| ReadWrite 8T | 393.34 MiB/s | 383.36 MiB/s | oneTBB | +2.6% |

迁移门槛为“至少两个 CPU 密集负载吞吐提升 ≥10%、RSS 增长 ≤10%”。实测中 Taskflow 在 CPU-only 8T（+6.8%，未达 10%）和 ReadWrite 4T（+49.4%，达标且 RSS 更低 69 vs 76.7 MiB）两项胜出，但严格按门槛只有 ReadWrite 4T 一项满足 ≥10%，未达“至少两项”要求，因此 oneTBB 保持默认 backend。

值得注意的是 Taskflow 在 ReadWrite 4T 的大幅领先（+49.4%）与 8T 的回落表明其调度在特定并发度下有优势，但整体矩阵未稳定胜出。该快照只用于验证选型，正式结论仍应在目标机器和真实数据集上复现。

## 并发扩展扫描

固定 v4 基线之外，`benchmark_concurrency` 使用真实 `Pipeline` 测量四条探索路径：

- 不配置 mutator/predicate 的 CPU-only 基线，用于验证空算子 fast path；
- 组合过滤但不写出，用于观察并行处理本身的扩展性；
- 组合过滤并写出 plain FASTQ，用于观察顺序提交阶段的影响；
- `fq::statistics::Calculator`，用于观察生产统计命令的批处理与串行归约影响。

每条路径扫描 1/2/4/8 线程和 1K/10K/50K records per batch：

```bash
cmake --build build/clang-release --target benchmark_concurrency
./build/clang-release/tools/benchmark/benchmark_concurrency \
  --benchmark_format=json \
  --benchmark_out=/tmp/fastqtools-concurrency.json \
  --benchmark_repetitions=5 \
  --benchmark_min_time=0.001s
```

该矩阵是探索性测量，不与固定 v4 快照混合；重点观察线程数增加后的吞吐拐点、batch
大小敏感性和峰值 RSS，而不是把单次结果当作跨机器绝对性能。

## 与同类工具对比

本表不内置 fastp/seqkit 的直接同环境数字，因为对比结果高度依赖 CPU、磁盘、压缩级别、参数与数据集，写死容易误导。下面给出**量级定位**与**可复现方法**，供你在自己的硬件上补一行。

| 工具 | 典型量级（公开基准） | 与 FastQTools 的定位差异 |
| --- | --- | --- |
| fastp | 多线程下数十 M reads/min（150bp PE） | 功能面远大于 FastQTools；FastQTools 聚焦 stat+filter 热点路径，内核更现代 |
| seqkit | Go 实现，单线程数十 M reads/min | 算子面巨大；FastQTools 原生 C++、二进制更小、可作为 C++ 库链接 |
| FastQTools | 见 v4 生产基线快照 | 生产 C++ Reader/Writer/filter/stat |

**如何在本仓库跑一次对比**：

```bash
# 1. 构建 FastQTools 基准（需 -DBUILD_BENCHMARKS=ON 配置的构建目录）
cmake --build build/clang-release --target benchmarks

# 2. 跑 FastQTools 基准
./build/clang-release/tools/benchmark/benchmark_fastq_io --benchmark_format=json

# 3. 在同一台机器、同一份数据上跑 fastp / seqkit
#    例如：fastp -i sample.fastq.gz -o /dev/null --thread 8
#         seqkit stats -Q sample.fastq.gz

# 4. 记录环境（CPU、磁盘、压缩级别、参数）后填入上表
```

公开基准来源（量级参考，非本仓库实测）：
- fastp：[OpenGene/fastp](https://github.com/OpenGene/fastp) README 与 issue 中的用户基准
- seqkit：[shenwei356/seqkit](https://github.com/shenwei356/seqkit) README 性能小节

## 注意

这些数字是维护中的快照，用于判断量级。跨工具对比请在你自己的硬件上按上述方法复现，不要直接当作绝对结论。
