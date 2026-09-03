# AGENTS.md — FastQTools AI Agent Guide

> 面向在仓库中读写代码的 AI Agent，提供命令、风格、约束速查。

## 项目概述

**FastQTools** — C++23 FASTQ 质控工具，内置并行流水线内核。工程能力展示项目。

### 技术栈

| 类别 | 技术 | 用途 |
|------|------|------|
| 语言 | C++23 | 现代特性、概念、范围 |
| 并行 | Intel oneTBB | `tbb::parallel_pipeline` 流水线并行 |
| 构建 | CMake 3.28+ + Ninja | 增量构建 |
| 包管理 | Conan 2.x | 依赖管理 |
| 测试 | GoogleTest 1.17 | 单元/集成/E2E |
| CLI | cxxopts | 参数解析 |

## 构建命令

```bash
./scripts/core/build                             # 默认: Clang Release
./scripts/core/build --dev                        # Debug + 详细输出
./scripts/core/build --compiler gcc --type Debug  # GCC Debug
./scripts/core/build --sanitizer asan             # ASan 构建
./scripts/core/build --coverage                   # 覆盖率构建
```

## 测试命令

```bash
./scripts/core/test                       # 所有测试
./scripts/core/test --unit                # 仅单元测试
./scripts/core/test --integration         # 仅集成测试
./scripts/core/test --e2e                 # 仅端到端测试
./scripts/core/test --filter '^test_io$'  # 过滤特定测试
```

## Lint 命令

```bash
./scripts/core/lint check                  # 检查格式
./scripts/core/lint format                 # 自动格式化
./scripts/core/lint tidy -b build/clang-debug      # clang-tidy
./scripts/core/lint all -b build/clang-debug       # 完整检查
```

## 环境体检

```bash
./scripts/core/doctor                      # 检查本地工具链版本/遮蔽，给出对齐建议
```

预期版本单一来源为 `scripts/core/toolchain.env`（与 ci.yml 对齐）。
`lint format` 产出与 CI 相反的格式意见时，先跑 doctor 排查格式器版本漂移/遮蔽。

## 代码风格

### 基本规范

- C++23，`CMAKE_CXX_EXTENSIONS` 关闭。
- `.clang-format`: 列宽 100、4 空格缩进、`PointerAlignment: Left`、Attach 大括号。
- 不要手动对齐空格；直接运行 `./scripts/core/lint format`。
- 优先尾置返回类型：`auto foo() -> int`。
- 公共头文件 `#pragma once`；公共 API 用 `<fqtools/...>`，内部用引号。
- 避免头文件中 `using namespace`。

### 命名约定

| 类型 | 规则 | 示例 |
|------|------|------|
| 类 / 结构体 | PascalCase | `FastqBatch`, `StatCommand` |
| 函数 / 方法 | camelCase | `validateLengths()` |
| 变量 / 参数 | camelCase | `totalReads` |
| 私有成员 | camelCase_ | `config_`, `pipeline_` |
| 常量 / constexpr | kCamelCase | `kDefaultBatchSize` |
| 枚举值 | PascalCase | `CompressionType::Gzip` |
| 命名空间 | lower_case | `fq::processing` |
| 测试文件 | `test_<module>.cpp` | `test_io.cpp` |

### 类型与接口

- 公共接口在 `include/fqtools/`，实现在 `src/`。
- 查询函数、getter 优先加 `[[nodiscard]]`。
- 大量使用 `std::string_view`；修改时必须确认生命周期安全。
- 零拷贝 FASTQ 视图和批处理；不要无必要引入字符串复制。
- `tbb::parallel_pipeline` 并行；热点路径不要加串行瓶颈。

### 错误处理与日志

- 异常基类 `fq::error::FastQException`；子类 `IOError`、`FormatError`、`ConfigurationError`。
- 不静默吞异常；CLI 边界捕获并记录。
- 日志 `fq::logging::info/warn/error`，fmt 风格格式串。
- 不用 `std::endl`；统一 `"\n"`。

### 注释

- 公共 API 和不直观的逻辑用简洁中文注释。
- 重要接口用中文 Doxygen：`@brief`、`@param`、`@return`。

## 测试策略

| 目录 | 内容 | 框架 |
|------|------|------|
| `tests/unit/` | 单元测试，镜像 `src/` | GTest |
| `tests/integration/` | 跨模块集成测试 | GTest |
| `tests/e2e/` | CLI 端到端 | Bash + Python |
| `tests/utils/` | 测试工具库 | GTest |

命名：测试文件 `test_<module>.cpp`，测试类 `<Module>Test`，用例 `<Object>_<Scenario>_<Expected>`。

## Agent 约束

- 单人项目，默认在当前分支直接改动。
- 修改 C++ 源码后至少运行 `./scripts/core/lint format` 和相关测试。
- 不擅自重命名公共头文件、导出目标或 CLI 命令。
- 保持公共 API 精简：不为假设的扩展预留未用 API（函数、宏、枚举值）；新增公共面需有真实调用方或测试覆盖。
- commit message、代码注释优先中文。
- 提交规范：Conventional Commits `feat|fix|docs|refactor|test|chore(scope): subject`。
- 开始工作前检查 `issues/` 目录中的 open issue，了解待办上下文。
