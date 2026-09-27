# Agent Note: 依赖瘦身——移除 spdlog 与 libdeflate

Status: implemented

## Problem

日志与压缩各挂一个第三方重依赖，相对项目体量依赖面过深、构建变慢。两者都曾真实引入：libdeflate 于 2025-12-30 为 writer 接入（`718ba92`），spdlog 承担日志；2026-07-11 奥卡姆剃刀精简（`7e369eb`）将两者移除，残余引用（install-deps、release-build、valgrind suppressions）于 2026-07-24 清完（`1e10462`）。

## Decision

日志用基于 `fmt::print(stderr)` 的薄封装 `logging.h`（Debug/Info/Warn/Error/Off 五级，`info/warn/error` 便捷函数，fmt 风格格式串）。压缩统一走 zlib-ng 的 gz API，`fastq_writer.cpp` 单一压缩路径。决策理由与依赖对比的完整叙述见 `docs/architecture.md` 的「依赖瘦身」一节，本篇只记取舍边界。

## Alternatives considered

- **保留 spdlog** — 最强论据：生态事实标准，异步 sink、滚动文件、结构化输出开箱即用。被否：本项目日志需求只有五级加 fmt 格式串，一个薄封装已足够，spdlog 的功能面大部分是死重。
- **libdeflate 与 zlib-ng 并存** — 最强论据：libdeflate 的 gzip 解压/压缩吞吐高于 zlib 系实现，writer 曾实测受益。被否：两套压缩 API 并存扩大依赖面与认知负担，统一到 zlib-ng 后压缩路径单一。

## Consequences

- **收益**：依赖树浅，`conanfile.py` requirements 只剩 cxxopts/zlib-ng/fmt/onetbb/nlohmann_json（+可选 benchmark/gtest）；可执行文件不足 1MB；构建快。
- **代价与已知上限**：日志没有异步 sink、滚动、结构化能力——若需要按结构化字段检索日志或异步落盘，`logging.h` 不够，届时单项重引而非回引整套 spdlog；压缩路径上没有 libdeflate 的极端性能档——若 benchmark 实测 gzip 压缩成为明确热点（当前 gzip-6 writer 7,194 reads/s 已是实测下限，但瓶颈在压缩本身而非库选择面），可单独为压缩路径重评 libdeflate。

## Verification

`build-config/dependencies/conanfile.py` requirements 无 spdlog/libdeflate；`rg -i 'spdlog|libdeflate' src/ include/` 无命中。
