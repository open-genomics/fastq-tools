# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

This root changelog is the maintained project history. Older granular work logs from the retired `changelog/` tree have been folded into the yearly and release summaries below.

---

## [Unreleased]

### Added
- `Pipeline::enableReadStatistics()`：过滤写出前对保留 read 做与 `stat` 同一套 QC 汇总，无需二次扫描。
- `filter --stat` / `--stat-json`：一趟扫描同时写出 FASTQ 与 QC 报告。
- `scripts/core/doctor`：本地环境体检——工具链版本下限校验、与 ci.yml 锁定版本的差异提示、PATH 命令遮蔽检测（uv/pip wheel 覆盖系统 clang-format 之类的真实事故场景）与优化建议。预期版本集中在新增的 `scripts/core/toolchain.env`（与 ci.yml 对齐的单一事实来源）。
- `scripts/core/install-deps`：LLVM 工具链（clang/clang-format/clang-tidy）改从 apt.llvm.org 安装固定大版本并 `update-alternatives` 钉住裸命令，与 CI 同源；发行版 apt 源只有旧版 LLVM，会与 CI 的 clang-format 意见相反。钉住后校验解析结果，被发行版同名包/用户目录副本遮蔽时给出告警。

### Fixed
- 修复 `.github/workflows/ci.yml` 的 job 级 `if` 非法使用 `matrix` 上下文导致的整体失效（所有 push 0 秒失败、0 个 job，包括 format gate）——改为 `resolve-matrix` job 用 Python 生成完整矩阵（含 build/test 命令等全部字段）+ `fromJSON` 注入，coverage 依赖同步调整。push/PR 仍只跑 format。
- `scripts/core/lint` 增加 clang-format 版本守卫：解析到非预期大版本时直接报错并指向 `doctor`，不再带着错误版本产出误导性格式结论。

---

## [4.1.0] - 2026-08-18

`stat`/`filter` 接入层补齐：JSON、stdin/stdout、`--trim-length`，以及 Linux Release workflow 与 macOS `install-deps`。同时收入 v4 之后的审计修复、API 收口与 CPU baseline。

### Added
- `filter --trim-length`：从 3' 端截断到指定长度（保留 5' 前缀）。与 `--max-length`（丢弃超长 read）区分；处理顺序为 adapter → poly → quality → length → predicates。
- `stat --json <path>`：与 TSV 同一指标集的机器可读 JSON（手写序列化，不把 nlohmann_json 拉进 CLI）。
- `-i -` / `-o -`：未压缩 stdin/stdout。gzip 输入请先 `gzip -dc`；gzip 输出请再管道给 `gzip`。不允许 TSV 与 JSON 同时写 stdout。
- `scripts/core/install-deps` 支持 macOS Homebrew（cmake/ninja/llvm）。
- GitHub Release workflow：`v*` tag 构建 portable Linux x86_64 二进制并上传。
- CPU baseline：`portable` / `x86-64-v3` / `native`（默认 portable）。
- `--batch-capacity-mb`、有界 head-kmer 合并、大规模保序回归、PR sanitizer/fuzz/coverage 门禁、`conan.lock`。

### Fixed — 全量审计修复（正确性 / 测试 / 基建 / 文档）

五维审计（代码逐文件通读、测试覆盖对照、构建/CI/打包、文档一致性、产品完整度）后的集中修复。Debug / Release / ASan 三配置 12/12 通过。

**正确性（两处静默数据错误）**
- `FastqWriter` 析构不再发布输出：异常展开时截断内容不会被 rename 成目标文件（旧行为与 `IWriter`"析构只做兜底清理"契约相反）；发布只能经显式 `finish()`。
- `QualityTrimmer` SIMD（AVX2）与标量路径语义统一为 `ceil`：修复小数阈值（如 `--trim-quality 20.5`）在 Release（x86-64-v3）与 Debug/ARM 构建下输出不同；修复 Phred+64 极端阈值下 `set1_epi8` 符号回绕导致"完全不剪"。
- `FqStatisticWorker` 质量统计以 `min(seq, qual)` 限界：自定义 I/O 注入不等长记录不再堆越界读。
- `FastqReader::nextBatch(maxRecords=0)` 改为抛 `std::invalid_argument`（旧行为静默假 EOF 或死循环）。
- 统计报告与 signature sidecar 写后校验流状态、临时文件 + rename 原子发布：磁盘满等失败抛 `IOError`，不再返回 0 产出半截报告。
- `IReader::nextBatch` 虚函数默认参数改为非虚重载转发（默认参数按静态类型解析，属经典陷阱）。
- `gzread`/`gzwrite` 分块防止 >4GB 缓冲静默截断；zlib 非 `Z_ERRNO` 错误携带 zlib 描述（旧 `strerror` 渲染无意义文本）；空序列记录报 "Empty sequence" 而非误导性 "mismatch: 0 vs 0"。
- 临时文件创建改为持有 `O_EXCL` fd 并经 `gzdopen` 复用，消除 TOCTOU 窗口。

**性能**
- 谓词逐 read 原子计数改为分片计数器（`detail::ShardedCounter`，线程轮转分片、热路径零开销），消除并行模式下全部 TBB worker 在同一 cache line 的竞争。
- Reader 残片处理改为拷入 batch buffer 头部，保留对象池预分配容量（旧 `std::move(remainder)` 会丢弃池缓冲导致每批重新分配）。

**复审补丁（code review）**
- 删除 `FastqWriter::Impl::finishFailed` 死字段：析构改为只看 `!finished` 后该字段只写不读。
- `fq_statistic.cpp` `writeAtomically` 用 try/catch 统一清理 `.tmp`：`writeBody` 抛异常时不再遗留半截临时文件（原仅 `good()`/rename 失败路径有清理）。
- `architecture.md` 两处 fuzzer 表述同步为"CI 每 PR 跑 60s fuzz 冒烟"（原文误写"CI 集成待定"）。
- `conan.lock` 此前仅放行 `.gitignore` 却未 `git add`，现已暂存入库并经 `--lockfile` 集成进 build 流程（见下方复审第二轮）。

**复审第二轮（code review 后续修复）**
- `scripts/core/build` 集成 `--lockfile`/`--lockfile-out`：入库的 `conan.lock` 真正生效（此前只提交未消费，对可复现性零作用且会静默漂移）；lockfile 纳入 hash 变更检测（回写经实测字节稳定，无重装循环）；失配时以非零退出并提示 `conan lock create` 再生成。
- `scripts/core/build` toolchain 检查修复两处既有 bug：相对路径与 CMakeCache 绝对路径比较、`:FILEPATH=` 类型假设（实际条目为 `:UNINITIALIZED=`）——此前每次运行都误判"toolchain 变化"而清空整个构建目录重建。
- `fq_statistic.cpp` `writeAtomically` 临时文件改 pid+counter 唯一后缀（对齐 `FastqWriter`），消除固定 `.tmp` 在多进程写同一目标时的互相覆盖。
- CLI `--batch-capacity-mb` / `--memory-limit-gb` 超大值在字节换算前拒绝（此前 `size_t` 乘法回绕成无意义小值），补 3 个单元测试（TDD，RED→GREEN）。
- 补空序列拒绝（`Empty sequence`）回归测试，锚定 reader 契约。

### Added
- 大规模保序回归测试：10k 唯一 ID 记录、4 线程、小批次，读回输出逐条比对（旧保序测试仅 2 条记录，任何乱序回归都无法显现）。
- `--batch-capacity-mb` CLI 选项 / `ProcessingOptions::batchCapacityBytes`：覆盖档位批缓冲上限；ONT 超长 read 不再整轮失败且无旋钮可调。
- head kmer 全局合并有界化（top-4096 懒剪枝），防止超大输入下 map 无界膨胀。
- `IReader` / `ReadPredicateInterface` / `ReadMutatorInterface` 文档补充线程安全契约。
- CI：sanitizer（ASan/UBSan）移入 PR 门禁并加 ccache（TSan 保留仅主分支）；新增每 PR 60s fuzz smoke job；覆盖率强制阈值检查；全部 actions pin 到 SHA + 新增 dependabot。
- 提交 `conan.lock` 锁定依赖图；`scripts/core/build` 按 hash 检测 conanfile 变更触发重装。

### Changed
- 脚本修复：`scripts/core/test` 默认不再以 `--timeout 60` 覆盖测试自身 TIMEOUT 属性；`run-fuzzer` 的 `((x++))` 在 `set -e` 下自杀 bug；`lint` 不再把 clang-tidy 崩溃误判为通过，删除死的 `-j` 选项。
- 构建修复：AppleClang 不再被 `MATCHES "GNU|Clang"` 误匹配（macOS 链接必败）；`-O3`/march 仅 Release 启用；`verify_consumer.cmake` 对 GCC 用 `-flto=auto`。
- 文档修复：夸大表述（"长度分布"→最大读长、`stat -o` 实为必填）；architecture.md 同步已删除的 `FQ_THROW_*` 宏与日志级别；getting-started 补 Conan 安装与样本生成步骤；paired-end / JSON 输出 / 去重等立场写入"刻意不做"清单并落 issues 条目。
- 测试卫生：删除死测试、弱断言改为精确计数（并发计数 400/400）、`unit_tests` 聚合目标补 `test_cli`。

### Refactor: 收回框架定位，追求轻灵巧

定位从"并行流水线框架"收回为"FASTQ 质控工具 + 流水线参考实现"，删除为假设扩展预留的抽象与未用 API 表面。净减约 200 行（增 161 / 删 362）。

- **定位收回**：README / AGENTS / architecture / CMake DESCRIPTION 的"并行流水线框架"统一改为"FASTQ 质控工具"；删除"替换批类型接入其他格式""自定义后端"等不真实承诺。
- **CMake 依赖传播修正**：`fq_statistics` 的 `fq_processing` 改 PUBLIC、`fq_cli` 的 `cxxopts` 改 PUBLIC；`fq_error` 的 `fmt`、`fq_processing` 的 `fq_error` 改 PRIVATE；删除 `FastqTools` / `test_cli` 显式补链、benchmark `nlohmann_json` 冗余、可执行文件冗余 `cxx_std_23`；修正 `src/CMakeLists.txt` 注释矛盾。
- **死 API 清理**：删除 `logging` 的 `trace/debug/critical/init`、`LogOptions`、`Level::Trace/Critical`；`error` 的 `isRecoverable` 与 `FQ_THROW_*` 三宏；`CommandRegistry::getCommandNames`。保留 `message/severity`（测试在用）与 `ErrorCategory` 扩展枚举（公共契约）。
- **间接层削减**：删除 `FilterRuntimeAdapter` 与 3 个 `std::function`，`Pipeline::Impl` 直接实现 `ExecutionRuntime` Adapter 契约；`FastqStatisticCalculator` 合并入 `Calculator::Impl`，删除 `factory.cpp`。
- **杂项清理**：删除未用 include（`<iomanip>` / `<numeric>` / `<memory>` / `<vector>` / cxxopts 前向声明）；`main.cpp` / `filter_command.h` 的 `fq.h` 改精确头；`statistics_report.cpp` 三处重复格式化提 `fixedStream()` helper；`LengthTrimmer` 删除 `FixedLength` / `FromEnd` 冗余枚举，简化 `process`。
- **Sanitizer 配置**：`tsan.supp` 增加 tbbmalloc `BootStrapBlocks::allocate` 误报抑制；`add_fq_test` 在 TSan 下 timeout ×3。

ASan / TSan（clang-debug + clang-release）12/12 通过，packaging install 传播验证通过。

### Changed
- Consolidated the maintained repository history into this file and retired the legacy `changelog/` directory.
- Retired docs-site changelog publication in favor of GitHub Releases plus the root `CHANGELOG.md`.

### Architecture and runtime
- Continued the post-v3.1.0 architecture cleanup with injectable I/O abstractions, shared CLI/runtime configuration helpers, centralized statistics reporting, and clearer execution-runtime seams.
- Reduced shallow wrappers and false seams, tightened error-handling boundaries, and fixed follow-up CI workflow regressions plus a CLI test regression during the refactor cycle.

### Documentation and project presentation
- Rebuilt the documentation whitepaper surface with stronger information architecture, bilingual narrative pages, theme-aware diagrams, research and algorithms routing fixes, and responsive homepage/layout follow-ups.
- Cleaned publication rules so internal asset README files are not treated as user-facing documentation.

---

## [4.0.0] - 2026-07-17

FastQTools v4 is an intentional breaking release that narrows the public surface and makes runtime/resource contracts explicit.

### Breaking
- Replaced `createProcessingPipeline()` and `createStatisticCalculator()` with move-only PIMPL classes `fq::processing::Pipeline` and `fq::statistics::Calculator`.
- Renamed the statistics namespace from `fq::statistic` to `fq::statistics`.
- Added `IWriter::finish()`; `write()` only accepts data, while `finish()` reports flush, compression-close, and publication errors.
- Removed config, ErrorHandler, legacy Logger, public object pools, and the Taskflow backend. No v3 compatibility layer is provided.
- Consolidated the installed CMake consumer target to `FastQTools::FastQTools`; CLI-only dependencies remain private.

### Correctness and runtime
- Filter order is fixed to adapter trim → poly-G/poly-X trim → quality trim → predicates; all predicates use the final trimmed read.
- Default file output is staged in a same-directory temporary file and atomically renamed after a successful `finish()`; failed writes preserve the previous target.
- Memory profiles account for batch storage, record vectors, reader remainder, and writer/zlib buffers. Limits below one runtime working set raise `ConfigurationError`.
- Sequential and oneTBB paths share deterministic output/statistics contracts; CLI is the single exception/logging/exit-code boundary.

### Performance and verification
- Rebuilt production benchmarks around a fixed seed=42, 1M×150 bp dataset, real Reader/Writer/Pipeline/StatisticWorker code, and five repetitions with median/CV summaries.
- Writer measurements cover plain and gzip-1/6/9, each with single and batch APIs; unstable WSL2 clock data did not meet the evidence threshold for speculative Writer/statistics optimization.
- Manual CI runs format, static analysis, GCC/Clang, ASan, TSan, UBSan, and coverage jobs when dispatched from GitHub Actions.

---

## [3.2.0] - 2026-04-30

### Added
- Advanced preprocessing capabilities called out by the `v3.2.0` release tag, including quality trimming, poly-tail trimming, adapter trimming, and related CLI options such as `--adapter-seq`, `--trim-poly-g`, and `--trim-poly-x`.
- QC signature reporting called out by the `v3.2.0` release tag, including optional signature sidecar output, duplicate estimation, and head-kmer statistics.

### Changed
- Repository history at the `v3.2.0` tag shows accompanying cleanup around CI simplification plus documentation, GitHub Pages, and AI-governance reorganization.

---

## [3.1.0] - 2026-04-16

### Added
- First stable bilingual documentation release across the README, documentation site, and release materials.
- Production-ready FASTQ processing commands: `stat` for statistical analysis and `filter` for read filtering and quality trimming.
- High-performance processing foundations including TBB `parallel_pipeline`, zero-copy FASTQ record views, and libdeflate-backed compression support.
- Broader quality tooling with unit/integration/e2e tests, Google Benchmark, fuzzing support, sanitizers, Valgrind checks, Docker images, and DevContainer setup.

### Changed
- Standardized the project on C++23, modern CMake presets, and Conan 2.x dependency management.
- Completed the public API/interface split, unified logging and error handling, and tightened naming/documentation conventions across the codebase.
- Reworked the documentation structure, bilingual navigation, and release presentation for a stable public release.

### Fixed
- Resolved documentation workflow issues around i18n, static-site configuration, and release-note consistency.
- Addressed code quality findings affecting CLI logging consistency and public API annotations.

---

## 2026 Development Summary

- **Toolchain and quality**: unified GCC 15 / Clang 21 expectations, completed the C++23 migration, simplified selected dependency configuration (including header-only `fmt` usage), added benchmark and coverage improvements, and hardened build/test workflows.
- **Developer environment**: iterated on DevContainer and Docker support, remote/AI-assisted development integration, and local tooling guidance so contributors could build and validate the project more reliably.
- **Repository convergence**: cleaned repository structure, aligned baseline specifications with implementation reality, improved workflow reliability, and refined release/readme/docs presentation leading into and after v3.1.0.
- **Post-release maintenance**: May 2026 focused on architecture deepening and whitepaper/content refinement; the key outcomes of that work are reflected in the Unreleased section above.

---

## [3.0.0] - 2025-07-31

### Added
- Initial modular implementation of the core FASTQ processing functionality.
- Modern CMake-based build system and Conan-managed dependency setup.

### Changed
- Reverted from experimental C++20 modules to the traditional header/source layout used by the project today.
- Reorganized the codebase around clearer module boundaries for I/O, processing, statistics, and CLI concerns.

---

## 2025 Development Summary

- **Project bootstrap**: established the repository layout, build scripts, Docker/devcontainer setup, CI/CD foundations, and early documentation structure.
- **Core implementation**: built FASTQ I/O, processing pipelines, statistics analysis, and early high-performance refactors around the TBB-based architecture.
- **Quality and packaging**: expanded tests, multi-platform release automation, consumer/package verification, install fixes, and dependency cleanup.
- **Repository maturation**: completed major docs restructures, naming/style unification, public header relocation, script cleanup, and end-of-year configuration alignment that set up the 2026 stabilization work.

---

## [2.0.0] - 2024-07-29

### Added
- Initial public release of FastQTools.
- FASTQ statistical analysis, multi-threaded processing, compressed file support, and a command-line interface.
