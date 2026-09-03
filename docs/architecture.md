# FastQTools 架构与技术决策

> 讲清楚每个关键决策的**为什么**，而不只是**是什么**。

## 项目定位

FastQTools 是一个 C++23 FASTQ 质控工具，覆盖测序数据日常 QC 的两个最高频操作：统计（`stat`）与过滤修剪（`filter`）。它同时提供一个最小 C++ 库 API。项目刻意把维护面收得很窄——不做比对、不做变异检测、不做可视化，也不做双端（paired-end/interleaved）处理、UMI/去重、质控前后对比报告——把工程精力集中在**把少数事做到极致**。`stat` 输出 TSV，并可用 `--json` 写出同一指标集的机器可读 JSON。

### 设计哲学

- **窄而深，而非宽而浅。** 与 fastp（全能 QC）、seqkit（全能工具箱）不同，FastQTools 只做 stat + filter + 库 API，但把这三件事的内核做到现代 C++23 的工程上限。
- **把预算投到看不见的地方。** 零拷贝、对象池、流水线保序、消毒剂 CI——这些是用户感知不到、但决定工程质量的细节。
- **约束即安全。** `FastqRecord` 不能逃逸批次、`FastqBatch::clear()` 不释放内存、流水线首尾串行——刻意约束换来零拷贝、确定顺序与有界内存。
- **CI 是质量合同，重任务本地优先。** push/PR 自动运行秒级 format 检查；静态分析、多编译器矩阵、sanitizer、fuzz、coverage 由 `workflow_dispatch` 手动触发（Actions 页选择 suite），等价命令见 `ci.yml` 头部注释。fuzzer target 位于 `tools/fuzz/`，手动 fuzz 冒烟对 4 个 target 各跑 60s。

### 与同类工具的定位

| 工具 | 范围 | 语言 | FastQTools 与之的差异 |
| --- | --- | --- | --- |
| fastp | 全能 QC + 报告 | C++11 | 维护面更窄；C++23 内核；库 API；更严格 CI |
| fastqc | QC 报告 | Java | 原生 CLI；无 JVM；C++ 库可链接 |
| seqkit | 全能工具箱 | Go | 聚焦 QC 热点路径；原生 C++ 吞吐；二进制更小 |

FastQTools 不是 fastp 的替代品，而是一个聚焦、现代内核的 QC 组件与 C++23 流式流水线参考实现。

技术选型上采用现代 C++ 工程实践：C++23、概念与范围、零拷贝视图、私有执行 backend（Sequential + Intel oneTBB）、Conan 2 依赖管理、GoogleTest 全层测试、clang-tidy/cppcheck/ASan/coverage 多重质量门。

## 模块拓扑

```
                 ┌─────────── src/cli ────────────┐
                 │  main → CommandRegistry         │
                 │   ├─ StatCommand                │
                 │   └─ FilterCommand              │
                 └────────┬────────────────────────┘
                          │ 组装 options + 注入依赖
          ┌───────────────┼───────────────┐
          ▼               ▼               ▼
   src/io            src/processing    src/statistics
   Reader/Writer     Pipeline          Calculator/Writer
   (gzip, 批量)      (backend + 规则)   (Q20/Q30, GC, 最大读长)
          │               │               │
          └───────────────┼───────────────┘
                          ▼
                   include/fqtools/  ← 公共 API Façade (fq.h)
```

| 层 | 职责 | 关键文件 |
|----|------|----------|
| `cli` | 参数解析、子命令分发、依赖装配 | `src/cli/main.cpp`, `command_registry.cpp`, `commands/*` |
| `io` | gzip 解压、批量读取、批量写入、零拷贝记录视图 | `include/fqtools/io/fastq_io.h`, `src/io/fastq_reader.cpp`, `fastq_writer.cpp` |
| `processing` | 规则组合、统一运行时配置、执行 backend 选择；可选对保留 read 做 QC reduce | `src/processing/execution_runtime.*`, `execution_backend.*` |
| `statistics` | 统计计算与报告输出（批次核 `fq_stat_core` 供 Pipeline 与 `Calculator` 共用） | `src/statistics/fq_statistic.cpp`, `statistics_report.cpp` |
| `error` | 类型化异常体系 | `include/fqtools/error/error.h` |

公共 API 入口是 `include/fqtools/fq.h`，一个 Façade 头文件，聚合所有对外接口。

## 关键技术决策

### 1. 零拷贝记录视图：`FastqRecord` 全 `string_view`

`FastqRecord` 的五个字段（id、comment、seq、qual、plus）全部是 `std::string_view`，指向 `FastqBatch` 拥有的连续内存。读取一条记录不发生任何字符串拷贝。

**为什么**：FASTQ 文件动辄上百万条记录、每条 150bp。如果每条记录都做一次 `std::string` 分配，热点路径会被堆分配和拷贝主导。`string_view` 视图让解析退化为指针算术，吞吐量量级提升。

**代价**：`FastqRecord` 的生命周期严格绑定到所属 `FastqBatch`。这是刻意约束——批次处理完即清空复用，不允许记录逃逸出批次。`FastqBatch::clear()` 不释放内存，循环复用。

### 2. 批量 + 连续内存：`FastqBatch`

`FastqBatch` 持有一块连续 `buffer_`（默认 4MB）和一个 `records_` 向量。记录视图指向 buffer 内部偏移。

**为什么**：
- 连续内存 → 缓存友好，顺序遍历记录时预取命中率高
- 批量粒度 → 减少 I/O 系统调用次数，让 gzip 解压分摊成本
- 内存复用 → `clear()` 保留容量，下一批直接覆盖，避免反复 malloc/free

### 3. 可替换的三级执行 backend

`ExecutionRuntime` 只负责解析 `ResolvedRuntimeConfig`、创建 I/O adapter 和选择 backend。调度后端位于私有 implementation 中，不出现在公共头或调用方编译依赖里。

```text
ProcessingOptions
      │ resolveRuntimeConfig（唯一配置派生点）
      ▼
ResolvedRuntimeConfig ──→ 创建 Reader / Writer adapter
      │
      └──→ 选择 ExecutionBackend
              │
              └──→ read batch → batch operation → ordered commit/reduce
```

`ExecutionOperation` 对 filter/stat 的批结果做类型擦除。虚调用和结果装箱每批各发生一次；逐 read 的谓词、修改器和统计循环仍在具体 operation 内执行，不经过虚拟 backend interface。

默认 `OneTbbExecutionBackend` 用 `tbb::parallel_pipeline` 构造三级 filter：

```
serial_in_order (读取)  →  parallel (处理)  →  serial_in_order (写出 + 归约)
```

- 第一级 `serial_in_order`：从 Reader 读下一批，保证读取顺序
- 第二级 `parallel`：对每批应用谓词/修改器，CPU 密集，可并行
- 第三级 `serial_in_order`：写出 + 统计归约，保证输出顺序与统计一致性

**为什么用流水线而非线程池 + 任务队列**：
- 流水线天然保序：读取、提交和统计归约顺序确定
- TBB 负责任务调度、批粒度负载均衡和 filter 顺序
- `maxLiveTokens` 限制在途批数，控制内存峰值

同一 `ExecutionBackend` seam 还提供：

- `SequentialExecutionBackend`：单线程回退和契约基线
Sequential 与 oneTBB 共用 reader、writer、batch operation 和计量契约；历史 Taskflow 对照只保留在性能归档中，不参与 v4 构建。

#### Backend 选择规则

| 请求 | 选择 | 用途 |
| --- | --- | --- |
| `Automatic` + 单线程 | Sequential | 最小调度开销 |
| `Automatic` + 自定义 Reader/Writer | Sequential | 保持外部 adapter 的保守线程契约 |
| `Automatic` + 原生 I/O + 多线程 | oneTBB | `--threads > 1` 时的 CLI 与库并行路径 |
| 显式 Sequential / oneTBB | 对应 backend | 契约测试和公平基准 |
| Taskflow | v4 已移除 | 仅保留历史 benchmark/决策记录 |

CLI 始终使用 `Automatic`；实验 backend 选择不扩散到 CLI 参数或公共库 API。注意 CLI `--threads` 默认为 1，即**默认走 Sequential 串行**；显式传入大于 1 的线程数（且无自定义 Reader/Writer）才进入 oneTBB 并行路径。

#### 统一运行时配置

`ResolvedRuntimeConfig` 是 profile、内存上限和执行模式的唯一派生结果。旧的 `RuntimePolicy` / `PipelineExecutionPlan` 双轨已删除，避免相同规则在多个 module 漂移。配置集中包含：

- reader chunk、zlib buffer、batch capacity、writer buffer
- thread count、最大在途 token 数、执行模式
- `LowMemory` / `Default` / `HighThroughput` profile 的确定性默认值

内存上限按每个在途 token 的完整工作集计算：`batchCapacityBytes`、`FastqRecord` vector、reader remainder、writer buffer 与 zlib buffer。profile 会把 batch capacity 和 batch size 传入对象池；`maxLiveTokens` 据此裁剪。预算不足以容纳一个最小工作集时抛 `ConfigurationError`，不会静默突破上限。

#### Backend 契约不变量

| 不变量 | 约束 |
| --- | --- |
| 保序 | batch 按读取顺序进入 commit/reduce，输出 FASTQ 不乱序 |
| 背压 | 并行 backend 的在途 batch 不超过 `maxLiveTokens` |
| 计量 | 只有成功进入 commit 的 batch 才增加 `batchCount`；`committedBytes` 累加 writer 返回值 |
| 错误 | reader、operation、writer 的异常同步传播到 `run()` 调用方，不静默降级或重试 |
| 资源 | backend 返回前完成 worker；Reader/Writer 析构不会与最后一个任务并发 |

默认 `FastqWriter` 将输出写入目标同目录临时文件，`finish()` 成功后原子 rename；写入、关闭或 rename 失败时删除临时文件并保留已有目标。对自定义 Writer，runtime 只保证调用一次 `finish()` 并传播异常，事务语义由 adapter 自己定义。

**为什么第一级和第三级是串行**：
- 读取串行：gzip 流是顺序格式，无法随机访问
- 写出串行：输出 FASTQ 必须保序，且 gzip 流需顺序写入
- 中间级并行：这才是 CPU 瓶颈所在（过滤、修剪、统计计算）

oneTBB 的串行 stage 提供互斥语义；implementation 保留 reader/writer mutex，为 ThreadSanitizer 建立明确的 happens-before。锁位于串行 stage，正常执行不产生竞争，不改变流水线并行度。

### 4. 接口最小化：3 个 interfaces.h

每个领域的全部抽象聚合到一个 `interfaces.h`（io/processing/statistics），读一个文件就能理解该领域的契约。

**为什么**：细粒度接口在没有多实现时是纯成本——多一层间接、多一处维护、多一个理解单元。接口仍支持依赖注入（`Pipeline::setReader/setWriter` 接受 `unique_ptr<IReader/IWriter>`），测试可注入 mock。

I/O seam 直接承载 runtime 需要的完整契约，不再探测具体类型：

- `IReader::nextBatch(batch, maxRecords)`：所有 reader 都接收批记录上限
- `IWriter::write(batch) -> uint64_t`：返回本批接受的未压缩 FASTQ 序列化字节数
- `IWriter::finish()`：显式完成 flush、压缩流关闭和输出发布；析构不承担唯一错误报告职责

这里的“提交字节”表示 writer 已接受到自身 buffer/压缩流，不等同于执行 `fsync` 后的持久化字节。准确计量属于 adapter 契约，因此 runtime 不再对 `FastqReader` / `FastqWriter` 使用 `dynamic_cast`。

自定义 Reader 是单次消费资源：一次 `run()` 后必须重新注入才能再次运行。自定义 Writer 使用共享所有权，可由调用者控制最终 flush 生命周期。

### 5. 依赖瘦身：不引入 spdlog 和 libdeflate

日志和压缩用轻量方案而非第三方重依赖：
- **spdlog** → 基于 `fmt::print(stderr)` 的轻量 `logging.h`
- **libdeflate** → `fastq_writer.cpp` 用 zlib-ng 的 gz API 统一压缩库

**为什么**：
- spdlog 在这个项目体量下是过度依赖：日志需求简单（Debug/Info/Warn/Error/Off 五级、info/warn/error 三个便捷函数、fmt 风格格式串），一个薄封装足够
- libdeflate 与 zlib-ng 并存导致两套压缩 API，统一到 zlib-ng 减少依赖面和认知负担
- 结果：可执行文件不足 1MB，依赖树浅，构建快

### 6. 错误边界：异常基类 + 子类 + CLI 边界捕获

- 异常基类 `fq::error::FastQException`（携带 `ErrorCategory` 与 `ErrorSeverity`），子类 `IOError`/`FormatError`/`ConfigurationError`
- 各模块直接抛出子类异常，不再有宏封装层
- 库内部不静默吞异常；CLI 边界（`main.cpp`）捕获并记录后退出

退出码在 CLI 边界稳定映射：参数/配置错误为 2，FASTQ 格式错误为 3，I/O 错误为 4，其它运行时错误为 1。库层只抛出类型化异常，不重复记录同一错误。

**为什么用异常而非错误码**：FASTQ 处理是"正常路径占绝大多数"的场景，错误（文件打不开、格式损坏）是例外。异常让正常路径代码保持线性，错误处理集中在边界。C++23 没有稳定的错误类型，`std::expected` 在需要多错误码分类时才更合适，这里异常是更轻量的选择。

### 7. 质量门：CI 矩阵 + 消毒剂 + 模糊测试

CI（`.github/workflows/ci.yml`）的触发策略是**轻量自动、重任务手动**（见 `ci.yml` 头部注释）：
- push/PR 自动：clang-format 格式检查（秒级，无构建）
- `workflow_dispatch` 手动（Actions 页选择 suite：full/quick/sanitizers/coverage/fuzz）：
  - clang-tidy + cppcheck 静态分析
  - GCC Release / Clang Release / Clang ASan / Clang TSan / Clang UBSan 矩阵构建 + 测试
  - fuzzer target 在 `tools/fuzz/`（fastq_parser_fuzzer 等）跑 60s fuzz 冒烟（fuzz-smoke job）
  - 覆盖率报告

**为什么**：单人项目下重任务（全矩阵构建 + sanitizer）单次十余分钟，随每个 push 自动跑的 Actions 成本收益失衡，故降为手动触发、重任务优先在本地运行（`./scripts/core/build --sanitizer asan && ./scripts/core/test` 与 CI 等价，合并前本地跑完）。C++ 内存安全是核心风险，ASan 常态化运行能在合并前抓到越界和 UAF；fuzzer 针对解析器入口，因为 FASTQ 输入是外部不可信数据，本地可用 libFuzzer 长跑深挖。静态分析抓 API 误用和现代 C++ 反模式。

## 性能特征

v4 性能快照固定为 1M reads × 150 bp、seed=42、5 次重复，并直接调用生产 Reader/Writer/Pipeline/`FqStatisticWorker`：

| 工作负载 | 结果 |
|----------|------|
| FASTQ reader | 202,903 reads/s（61.3 MiB/s） |
| plain writer，single API | 117,444 reads/s（35.5 MiB/s） |
| gzip-6 writer，single API | 7,194 reads/s（2.2 MiB/s） |
| filter baseline | 125,623 reads/s（38.0 MiB/s） |

这些数值仅用于同机器、同命令的相对比较；当前 WSL2 的 `real_time` 被放大，stat CPU clock 的重复 CV 也超过 5%，因此没有据此实施 Writer 或统计热点优化。完整原始 JSON、环境和优化门槛见 [`docs/performance`](./performance/README.md)。

#### 已知限制：串行段的吞吐天花板

三级流水线的首尾两级是串行的（见"为什么第一级和第三级是串行"），这带来两个如实承认的天花板：

- **解析与 gzip 解压在串行段。** FASTQ 解析（行定位、字段切分、校验）随读取执行，gzip 解压同样只能顺序进行。轻过滤负载（谓词/修改器开销小）下并行段很快空闲，加速比受串行解析钳制；重负载（`--stat` 的逐碱基/逐位置统计）下并行段占主导，天花板不明显。
- **gzip 压缩在串行段。** 写出侧用 zlib-ng gz API 顺序压缩，`filter -o out.fq.gz` 的吞吐受压缩速度钳制（见上表 gzip-6 writer 基准），没有 bgzip/pigz 式的块级并行压缩。

两者的共同出路是 chunk 级并行（记录边界同步解析、块级并行压缩），但会改变"单缓冲零拷贝批次"模型，当前刻意不纳入。

## 测试策略

| 层 | 目录 | 框架 |
|----|------|------|
| 单元 | `tests/unit/` | GoogleTest，镜像 `src/` 结构 |
| 集成 | `tests/integration/` | GoogleTest，跨模块 |
| 端到端 | `tests/e2e/` | Bash + Python，CLI 黑盒 |
| 工具 | `tests/utils/` | 测试工具库 |

命名约定：`test_<module>.cpp` / `<Module>Test` / `<Object>_<Scenario>_<Expected>`。

## 构建与依赖

- C++23，`CMAKE_CXX_EXTENSIONS` 关闭（纯标准，无 GNU 扩展）
- CMake 3.28+ + Ninja + Conan 2.x
- 关键依赖：Intel oneTBB（默认并行 backend）、fmt（格式化）、zlib-ng（gzip）；cxxopts 仅为 CLI 私有依赖。
- benchmark、nlohmann_json 和 GoogleTest 按构建选项启用；Taskflow backend 已在 v4 移除，历史对照报告保留在性能归档中。
- 构建脚本：`./scripts/core/build`（默认 Clang Release）
