# CLI 参考

两个子命令：`stat` 和 `filter`。共享参数一致，特有参数分别列出。

## 共享参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-i, --input` | 输入 FASTQ 文件（支持 `.gz`；`-` 表示未压缩 stdin） | 必填 |
| `-o, --output` | 输出文件（`-` 表示未压缩 stdout） | 必填 |
| `-t, --threads` | 线程数 | 1 |
| `--batch-size` | 每批记录数 | 10000 |
| `--profile` | 性能档位：`default`/`lowMemory`/`highThroughput` | default |
| `--memory-limit-gb` | 内存上限 GB（0=不限） | 0 |
| `--batch-capacity-mb` | 单批缓冲上限 MB（0=档位默认） | 0 |
| `--quality-encoding` | 质量编码偏移（33 或 64） | 33 |

`--profile` 只调整运行时资源预算：`lowMemory` 使用较小的 batch/缓冲区，`highThroughput` 使用较大的 batch/缓冲区。`--memory-limit-gb` 会把 batch buffer、record vector、reader remainder、writer/zlib buffer 一并计入；不足以容纳最小运行集时命令以配置错误退出。`--batch-capacity-mb` 覆盖档位的批缓冲默认值（default 4MB / lowMemory 1MB / highThroughput 16MB）；单条记录超过该上限时命令以格式错误退出——处理 ONT 等超长 read 时需要调大。

全局选项（`main.cpp`）：`--verbose`/`-v`、`--quiet`/`-q`、`--log-level=<level>`。

`-` 只接受未压缩流。gzip 输入请先 `gzip -dc` / `zcat`；gzip 输出请再管道给 `gzip`。

## stat — 统计

```bash
FastQTools stat -i sample.fastq.gz -o sample.stats.txt
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--signature-report` | 可选签名 sidecar 报告（TSV） | 关 |
| `--signature-kmer-size` | 签名 head k-mer 大小 | 15 |
| `--signature-limit` | 签名最大行数 | 20 |
| `--duplicate-sample-modulo` | 重复估计采样模数（1=精确） | 1024 |
| `--json` | 可选 JSON 报告路径（与 TSV 同一指标集；`-` 写 stdout） | 关 |

输出指标（TSV 摘要行）：文件名、质量编码、读段数、重复估计（`#DuplicateEstimate` 与 `#DuplicateEstimateRate`）、最大读长、总碱基数、Q20/Q30、碱基组成（A/C/G/T/N）与 GC 含量。摘要之后是逐位置明细表：`#Pos`、A/C/G/T/N 计数、`AvgQual`、`ErrRate`。`--json` 写出同一指标集的 JSON；不能与 `-o -` 同时指向 stdout。

## filter — 过滤与修剪

```bash
FastQTools filter \
  -i sample.fastq.gz \
  -o sample.filtered.fastq.gz \
  --min-quality 20 \
  --min-length 50 \
  --trim-quality 20 \
  --trim-mode both
```

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--min-quality` | 最低平均质量阈值 | — |
| `--min-length` | 最短读长（不足则丢弃） | — |
| `--max-length` | 最长读长（超过则丢弃） | — |
| `--trim-length` | 从 3' 端截断到此长度（保留 5' 前缀，不丢弃） | — |
| `--max-n-ratio` | 最大 N 比例（0.0-1.0） | — |
| `--trim-quality` | 修剪低于此质量的碱基 | — |
| `--trim-mode` | 修剪方向：`both`/`five`/`three` | both |
| `--adapter-seq` | 3' 端 adapter 序列（可重复） | — |
| `--adapter-min-overlap` | adapter 最小重叠 | 3 |
| `--adapter-max-mismatches` | adapter 最大错配数 | 1 |
| `--trim-poly-g` | 修剪 polyG 尾（最小连续长度） | — |
| `--trim-poly-x` | 修剪低复杂度 polyX 尾（最小连续长度） | — |
| `--stat` | 对保留的 read 写 TSV QC 报告（与 `stat` 同一指标集） | 关 |
| `--stat-json` | 对保留的 read 写 JSON QC 报告 | 关 |

过滤与修剪在同一条处理链中完成，单遍扫描。`--stat` / `--stat-json` 在写出前汇总通过谓词的 read，不再二次扫描。不能把 FASTQ 与 QC 报告同时写到 stdout。

固定处理顺序为 adapter trim → poly-G/poly-X trim → quality trim → length trim → predicates。`--trim-length` 截断超长 read；`--max-length` 丢弃超长 read。长度、平均质量和 N 比例都基于最终修剪后的 read；任一修剪产生空 read 时整条 read 被过滤。

## 输出与退出码

`filter` 的默认 `FastqWriter` 先写同目录临时文件，成功完成后原子替换目标；写入、gzip close 或 rename 失败会删除临时文件并保留原目标。处理中途异常不会发布半成品。`-o -` 直接写 stdout，不做原子 rename。

CLI 在唯一边界捕获并记录异常，退出码稳定为：

| 退出码 | 含义 |
|--------|------|
| 0 | 成功 |
| 1 | 其它运行时错误 |
| 2 | 参数或配置错误 |
| 3 | FASTQ 格式错误 |
| 4 | 输入/输出 I/O 错误 |
