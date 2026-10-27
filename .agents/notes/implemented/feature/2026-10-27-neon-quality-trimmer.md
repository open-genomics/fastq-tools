# Agent Note: QualityTrimmer ARM64 NEON 质量扫描

Status: implemented

## Problem

ARM64 没有 AVX2，`QualityTrimmer` 的 5' 扫描在 ARM 上完全走标量，改动前 3'
扫描在所有平台都是标量。长低质量尾部（例如 149 个 q20 + 1 个 q21，阈值 25）
每 read 约 176–193 ns，是过滤流水线的可见成本；典型高质量 read 只需约 8 ns。

## Decision

- 为 aarch64/ARM NEON 增加 16 字节有符号字节比较：
  - 5'：找第一个 `(int8_t)quality[i] >= ceil(threshold) + encoding`。
  - 3'：从尾部按块找最后一个高质量碱基，返回 `index + 1`。
- 先做单字节标量快速路径（首字节/尾字节达标即返回），避免典型高质量 read
  为 SIMD 固定开销买单；未命中才进入 NEON。
- 阈值在构造时预计算为 `qualityTarget_` 与 `allLow_`/`allHigh_` 标志：热路径
  只读成员，不再每次 read 做 `ceil` 和边界判断；同时消除巨大/NaN 阈值下
  `int` 转换的未定义行为。标量 `isHighQuality()` 与 SIMD 共用同一套判断。
- 块内用 `vst1q_u8` 按内存顺序取 mask，避免大小端分支；无命中时用
  `vgetq_lane_u64` 的 OR 快速跳过，不做 16 次逐字节扫描。
- x86 AVX2 路径的扫描逻辑不变，仅复用同一套预计算阈值。

## Alternatives considered

- **纯 NEON，无标量快速路径**：最坏情况最快（约 24 ns），但典型高质量 read
  从 8 ns 退化到约 10.6–12 ns（+29%~44%）。
- **4/8 字节标量前缀 + NEON**：最坏情况约 29–43 ns，典型仍约 11–12 ns；
  单字节快速路径把典型拉回 7.4–8.1 ns。
- **依赖 `-O3 -ftree-vectorize` 自动向量化**：5'/3' 都是带早退的搜索，
  编译器不会生成向量化的早退扫描；实测仍约 172–193 ns。
- **SVE**：Kunpeng-920 是 ARMv8.2，无 SVE；NEON 是 aarch64 基线，可移植性
  更好。
- **同时改 AVX2 的 3' 路径**：x86 侧没有可复现的 ARM 目标收益，且会扩大
  本次改动面；留作独立优化。

## Consequences

- 典型高质量 read：7.3–7.5 ns/read（与标量 7.6–8.3 ns 持平或略好）。
- 全低质量/长低质量尾部：24–29 ns/read，较标量 172–193 ns 快约 6–7 倍。
- 端到端 `benchmark_filter/combined_with_trim`（1M×150bp，单线程）从约
  1173 ms 降到约 1083 ms（约 7.7%，受机器负载噪声影响）。
- 不需要运行时 dispatch：aarch64 的 NEON 是基线；非 NEON 构建仍走标量。
- 未引入 SVE；未来若目标机支持 SVE 且需要更高吞吐，可另开 profile 评估。

## Verification

```bash
./scripts/core/lint check
./scripts/core/build -c clang -t Release --no-lto
./scripts/core/test --build-dir build/clang-release
```

- 13/13 tests 通过；新增 6 个用例覆盖 3' 长串小数阈值、Phred+64 极端阈值
  3' 全剪、极低阈值保留 read、巨大阈值/NaN 阈值、随机质量差分比对。
- 微基准（临时独立 harness，1M 次/场景，150 bp）：
  - all_high：7.4 ns（标量 8.3）
  - all_low：24.4 ns（标量 172.9）
  - 5' worst：26.6–27.2 ns（标量 176.4）
  - 3' worst：28.3–28.5 ns（标量 193.0）
  - random：7.3–7.5 ns（标量 7.6）
- 端到端基准可用
  `./build/bench-arm64/tools/benchmark/benchmark_filter --benchmark_filter=combined_with_trim`
  复现；该构建目录需 `BUILD_BENCHMARKS=ON`。
