# 快速开始

## 环境要求

- C++23 编译器（GCC 13+ 或 Clang 17+ 最低）
- CMake 3.28+
- Conan 2.x
- Linux x86-64 / ARM64（aarch64）原生支持；macOS 可用 `./scripts/core/install-deps`（Homebrew）后构建；Windows 用 Docker 或 WSL

## 安装依赖

`install-deps` 会安装 Clang / clang-format / clang-tidy、CMake（Kitware 官方归档，
满足 3.28+）、Ninja、Conan 2.x 以及 zlib 等系统库；root 环境下无需 `sudo`：

```bash
./scripts/core/install-deps
```

> 若偏好手动安装 Conan：`pipx install conan`（或 `pip install --user conan`），
> 然后 `conan profile detect`。ARM64 上构建脚本会自动选择
> `build-config/conan/profile-*-arm64`；ConanCenter 无 Linux armv8 预编译包，
> 首次构建会从源码编译 oneTBB/hwloc/zlib-ng/gtest。

## 构建

```bash
./scripts/core/build                  # 默认 Clang Release
./scripts/core/build --dev            # Debug + 详细输出
./scripts/core/build --compiler gcc   # GCC
./scripts/core/build --sanitizer asan # ASan
```

构建产物在 `build/clang-release/FastQTools`。如需构建 benchmark，见 [benchmark.md](./benchmark.md) 的「基准测试工具」一节。

## 首次运行

仓库不附带样本数据，先生成（默认 10,000 条 reads、80–150 bp，seed=42 可复现）：

```bash
python3 scripts/datagen/gen_fastq.py -o sample.fastq
gzip -kf sample.fastq   # 生成 sample.fastq.gz，供下面的示例使用
```

```bash
./build/clang-release/FastQTools --help

# 统计
./build/clang-release/FastQTools stat -i sample.fastq.gz -o sample.stats.txt

# 过滤 + 修剪
./build/clang-release/FastQTools filter \
  -i sample.fastq.gz \
  -o sample.filtered.fastq.gz \
  --min-quality 20 \
  --min-length 50 \
  --trim-quality 20 \
  --trim-mode both
```

## 下一步

- 命令参数细节：[cli-reference.md](./cli-reference.md)
- 在 C++ 项目中使用库：[api.md](./api.md)
- 架构与设计决策：[architecture.md](./architecture.md)
