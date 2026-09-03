<h1 align="center">FastQTools</h1>

<p align="center">
  <b>C++23 FASTQ 质控工具，内置并行流水线内核。</b>
</p>

<p align="center">
  <a href="https://github.com/open-genomics/fastq-tools/actions/workflows/ci.yml">
    <img src="https://github.com/open-genomics/fastq-tools/actions/workflows/ci.yml/badge.svg" alt="CI Status">
  </a>
  <a href="https://github.com/open-genomics/fastq-tools/releases">
    <img src="https://img.shields.io/github/v/release/open-genomics/fastq-tools?label=Release&logo=github" alt="GitHub Release">
  </a>
  <a href="https://opensource.org/licenses/MIT">
    <img src="https://img.shields.io/badge/License-MIT-yellow.svg" alt="License: MIT">
  </a>
  <img src="https://img.shields.io/badge/C%2B%2B-23-blue.svg" alt="C++23">
  <img src="https://img.shields.io/badge/CMake-3.28%2B-064F8C?logo=cmake" alt="CMake 3.28+">
</p>

<p align="center">
  <a href="./docs/getting-started.md">快速开始</a> ·
  <a href="./docs/architecture.md">架构</a> ·
  <a href="https://github.com/open-genomics/fastq-tools/releases">发布版本</a>
</p>

---

## 简介

FastQTools 是一个 C++23 FASTQ 质控工具，内置并行流水线内核。它提供：

- **批处理执行层**：`ExecutionRuntime` + `ExecutionBackend`（Sequential / oneTBB），执行后端不感知数据格式，通过 Adapter 契约接入具体计算逻辑。
- **可扩展的算子体系**：`Predicate`（过滤器）、`Mutator`（修改器）接口，策略模式组合，依赖注入便于测试。
- **FASTQ 质控作为首要应用**：`stat`（统计）和 `filter`（过滤修剪）两个命令，覆盖测序数据日常 QC 的核心需求。

## 架构概览

```
  Reader                 Worker Pool              Writer
  ──────                ───────────              ──────
  serial_in_order  →    parallel        →        serial_in_order
  (gzip/zlib-ng)        (predicates + mutators)  (写出 + 归约)
```

### 模块分层

| 层 | 职责 | 扩展方式 |
|---|---|---|
| ExecutionRuntime | 接收任意 Adapter，选择后端，管理生命周期 | 内部设施，不对外暴露 |
| ExecutionBackend | 具体调度后端（Sequential / oneTBB） | 内部设施，不对外暴露 |
| ExecutionOperation | 类型擦除的批结果契约 | 内部契约，不对外暴露 |
| Pipeline（FASTQ） | FASTQ 专用管道，注册 Predicate / Mutator | 实现接口注入 |

### 核心要点

| 要点 | 说明 |
|------|------|
| 零拷贝 | `FastqRecord` 所有字段均为 `std::string_view`，直接指向 `FastqBatch` 内部缓冲区，无需内存拷贝；`clear()` 保留容量，避免逐批重复分配。 |
| 三级流水线 | 基于 `tbb::parallel_pipeline`：读取 → 并行处理 → 写出。读取和写出阶段串行保序，中间处理阶段并行无锁。 |
| 内存可控 | `maxLiveTokens` 限制在途批次数量，基于工作集分析推导上限；内存不足时主动抛出 `ConfigurationError`，避免静默 OOM。 |
| 错误边界 | 异常基类 `FastQException` 派生 `IOError`、`FormatError`、`ConfigurationError` 三类子异常；CLI 边界统一捕获并映射为对应的退出码。 |

详见 [架构文档](./docs/architecture.md)。

## 核心应用：FASTQ 质控

### `stat` — 统计

read 计数、最大读长、碱基组成、GC 含量、Q20/Q30。

### `filter` — 过滤与修剪

长度过滤、质量过滤、N 比例过滤、5'/3'/两端质量修剪、adapter 修剪、polyG/polyX 尾修剪，一趟扫描全部完成。可选 `--stat` / `--stat-json` 在同一趟里对保留的 read 写出与 `stat` 相同的 QC 报告。

## 扩展性

FastQTools 不是黑盒。你可以：

- **自定义过滤/修改逻辑**：实现 `ReadPredicateInterface` 或 `ReadMutatorInterface`，通过 `Pipeline::addReadPredicate()` / `addReadMutator()` 注册。
- **自定义 I/O**：实现 `IReader` / `IWriter`，通过 `Pipeline::setReader()` / `setWriter()` 注入。
（执行后端 `ExecutionRuntime` / `ExecutionBackend` 为内部设施，不作为公共扩展点。）

```cpp
// 自定义质量过滤器示例
class MyQualityFilter final : public fq::processing::ReadPredicateInterface {
public:
    [[nodiscard]] auto evaluate(const fq::io::FastqRecord& read) const -> bool override {
        // 你的过滤逻辑
        return true;
    }
};

fq::processing::Pipeline pipeline;
pipeline.setInputPath("input.fastq");
pipeline.setOutputPath("output.fastq");
pipeline.addReadPredicate(std::make_unique<MyQualityFilter>());
auto stats = pipeline.run();
```

## 快速开始

**环境要求**：GCC 13+ / Clang 17+，CMake 3.28+，Conan 2.x。Linux 原生支持；macOS 可用 `./scripts/core/install-deps`（需 Homebrew）后构建；Windows 建议 WSL 或 Docker。

```bash
git clone https://github.com/open-genomics/fastq-tools.git
cd fastq-tools
./scripts/core/build
./build/clang-release/FastQTools --help

### CPU 架构基线

构建默认使用 `portable` 基线（不添加 `-march` 标志），确保二进制可在任何基线
x86-64 或 ARM CPU 上运行。如需针对特定 CPU 优化：

```bash
# portable（默认，最大兼容性）
./scripts/core/build --cpu-baseline portable

# x86-64-v3（AVX2+FMA+BMI，仅 x86-64）
./scripts/core/build --cpu-baseline x86-64-v3
# 或使用 preset: cmake --preset gcc-v3-release

# native（本地 CPU 优化，不用于发布）
./scripts/core/build --cpu-baseline native
```

> **注意**：项目不实现运行时 SIMD dispatch。选择的基线在编译时固定。
```

示例输入不随仓库附带，先用 `scripts/datagen/gen_fastq.py` 生成示例数据：`python3 scripts/datagen/gen_fastq.py -o sample.fastq && gzip -kf sample.fastq`。

统计：

```bash
./build/clang-release/FastQTools stat \
  -i sample.fastq.gz \
  -o sample.stats.txt
```

过滤并修剪：

```bash
./build/clang-release/FastQTools filter \
  -i sample.fastq.gz \
  -o sample.filtered.fastq.gz \
  --min-quality 20 \
  --min-length 50 \
  --trim-quality 20 \
  --trim-mode both
```

完整参数见 [CLI 参考](./docs/cli-reference.md)。

## 技术栈

| 层级 | 技术 |
|------|------|
| 语言 | C++23（GCC 13+ / Clang 17+） |
| 构建 | CMake 3.28+ · Ninja |
| 包管理 | Conan 2.x |
| 并行 | Intel oneTBB |
| 压缩 | zlib-ng |
| 格式化 | fmt |
| 测试 | GoogleTest |

## 性能

测试环境：AMD Ryzen 7 5800H（WSL2，8 核 16 线程），Clang 21 Release，1M reads × 150 bp，seed=42，重复 5 次取中位数。

| 场景 | 吞吐 |
|------|------|
| Reader | 202,903 reads/s（61.3 MiB/s） |
| plain writer（single API） | 117,444 reads/s（35.5 MiB/s） |
| gzip-6 writer（single API） | 7,194 reads/s（2.2 MiB/s） |
| filter 基线 | 125,623 reads/s（38.0 MiB/s） |

数据仅适用于同一机器、同一命令的横向对比。完整报告：[v4 基线](./docs/performance/benchmark-reports/v4-baseline/2026-07-17/summary.md)。

## 测试与质量保证

CI（GitHub Actions，push/PR 均触发，sanitizer 矩阵随 PR 与主分支运行）覆盖：

- 静态检查：clang-format、clang-tidy、cppcheck
- 多编译器：GCC + Clang，Release 模式
- Sanitizer：ASan、TSan、UBSan
- 测试：单元测试、集成测试、E2E 测试、gcovr 覆盖率
- Fuzzer：`tools/fuzz/` 对 FASTQ 解析器进行覆盖率导向的模糊测试

## 对比

| | FastQTools | fastp | fastqc |
|---|---|---|---|
| 定位 | FASTQ 质控工具 | 全能 FASTQ 质控 | 质量报告 |
| 统计 | Q20/Q30、GC、最大读长、碱基；TSV + JSON | 更丰富的 QC 指标 | 可视化 HTML 报告 |
| 过滤/修剪 | 一趟扫描，全部可配置 | 自动 adapter 检测 | 不支持 |
| Adapter 推断 | 不支持（需显式指定） | 内置自动检测 | 不支持 |
| 可视化 | 无 HTML | HTML + JSON 报告 | 交互式 HTML |
| 并行模型 | tbb::parallel_pipeline | 线程池 | 单线程 |
| 可嵌入 | 库 API（Pipeline / Calculator） | 命令行工具 | 命令行工具 |
| 语言 | C++23 | C++11 | Java |

需要自动 adapter 检测或可视化报告？请用 fastp 或 fastqc。需要确定性、可复现、高性能的统计与过滤？请用 FastQTools。

## 文档

| 目标 | 文档 |
| --- | --- |
| 命令参数 | [CLI 参考](./docs/cli-reference.md) |
| C++ API | [API 概览](./docs/api.md) |
| 架构与设计 | [架构文档](./docs/architecture.md) |
| 性能数据 | [性能总览](./docs/benchmark.md) |
| 待办与问题 | [Issue 跟踪](./issues/README.md) |
| 贡献 | [贡献指南](./CONTRIBUTING.md) |
| 变更记录 | [CHANGELOG.md](./CHANGELOG.md) |

## 许可证

[MIT License](LICENSE)。

## 作者

**shijiashuai** — [GitHub](https://github.com/LessUp)
