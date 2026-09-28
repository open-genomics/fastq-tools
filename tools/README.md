# Tools 目录

开发辅助工具集，包含性能基准测试、模糊测试和测试数据生成。

## 目录结构

```
tools/
├── benchmark/              # 性能基准测试
│   ├── CMakeLists.txt      # CMake 构建配置
│   ├── *_benchmark.cpp     # C++ 基准测试源码（Google Benchmark）
│   └── scripts/            # 分析与报告脚本
│       ├── report_generator.py      # Markdown/图表报告生成
│       ├── regression_detector.py   # 性能回归检测
│       ├── badge_generator.py       # shields.io 徽章生成
│       ├── gen_benchmark_data.py    # 基准测试数据生成
│       └── run_baseline.sh          # 端到端基线测试
└── fuzz/                   # 模糊测试（LibFuzzer）
    ├── CMakeLists.txt      # CMake 构建配置
    ├── *_fuzzer.cpp         # Fuzzer 源码
    └── corpus/             # 种子文件
```

> 合成 FASTQ 数据生成器在 `scripts/datagen/gen_fastq.py`（本目录没有 data/ 子目录）。

## 快速使用

### 基准测试

```bash
# 构建基准测试
cmake --build build --target benchmarks

# 通过统一 CLI 运行
./scripts/dev/performance/benchmark run
./scripts/dev/performance/benchmark report
./scripts/dev/performance/benchmark compare baseline.json current.json
```

### 模糊测试

```bash
# 构建（需要 Clang + LibFuzzer）
cmake -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++ -B build-fuzz
cmake --build build-fuzz

# 运行
./build-fuzz/fuzzers/fastq_parser_fuzzer tools/fuzz/corpus/ -max_len=4096
```

### 测试数据生成

```bash
# 生成自定义 FASTQ 数据
python3 scripts/datagen/gen_fastq.py -o output.fastq -n 10000

# 生成完整 benchmark 数据集
python3 tools/benchmark/scripts/gen_benchmark_data.py --generate-dataset
```

## 与项目的集成

- **CMake**: 根 `CMakeLists.txt` 通过 `BUILD_BENCHMARKS` 和 `ENABLE_FUZZING` 选项控制构建
- **CI**: CI 不运行基准（重任务仅手动触发 fuzz/coverage/sanitizer 等套件，见 `ci.yml` 头部注释）；基准与回归检测按上文命令在本地运行
- **脚本**: `scripts/dev/performance/benchmark` 提供统一的命令行接口