# Agent Note: 用 std::expected / 错误码替代异常做错误边界

Status: rejected — 错误边界采用类型化异常体系（FastQException 基类 + 子类 + CLI 边界捕获）

## Problem

reader/writer/operation 各层产生的错误（文件打不开、FASTQ 格式损坏、配置非法）需要统一模型传播到 CLI 边界，并稳定映射到退出码。

## Proposal

用 `std::expected` 或错误码替代异常：错误通道显式写在返回类型里，无栈展开开销，函数签名即错误文档。

## Alternatives considered

- **类型化异常体系（采纳的方向）** — `fq::error::FastQException` 携带 `ErrorCategory`/`ErrorSeverity`，子类 `IOError`/`FormatError`/`ConfigurationError` 各模块直接抛出；库内部不吞异常，CLI 边界（`main.cpp`）捕获、记录并映射退出码（参数/配置 2、格式 3、I/O 4、其它 1）。
- **维持现状方案**：FASTQ 处理的错误是例外路径而非正常路径的一部分——正常路径占绝大多数，异常让正常代码保持线性；`std::expected` 要求每层调用显式 unwrap，正常路径被错误处理语法污染。expected 在需要多错误码分类逐层决策的场景才更划算，本项目错误只在边界统一处理，分类需求在异常类型体系里已解决。

## Risks

若错误语义变化——例如「坏记录跳过并计数」成为一等功能、需要逐 read/逐 batch 的可恢复错误分类——`std::expected` 的分类价值上升，可对相关 API 局部重评；全局替换异常的提案不应在无此类驱动时重提。
