# Change Proposal: add-arm64-build-support

## Metadata

- Status: `Archived`
- Repository: `open-genomics/fastq-tools`
- Audit base: `59b8706` (master, 2026-10-26)
- Capability: `build-portability`
- Task IDs: `FQT-ARM-001`
- Decision IDs: none

## Why

构建脚本、Conan profile、CI 与 release 流水线此前都假定 x86-64。目标环境是一台
Kunpeng-920 aarch64 服务器（Ubuntu 22.04、cgroup 限制 21 核 / 200 GiB），既无法
按文档从源码构建，也没有可用的 aarch64 发布产物。

Conan 用 `armv8` 表示 64 位 ARM，x86 profile 会请求错误架构的包；ConanCenter
也没有 Linux armv8 的 oneTBB/hwloc/zlib-ng/gtest 预编译二进制，首次必须源码
构建。另一个直接故障是 Conan build context 与 host context 的编译器声明不一致：
默认 build profile 声明 clang，却调用 `/usr/bin/c++`（GCC），并向 GCC 注入
`-stdlib=libstdc++`，导致 ninja 等工具依赖编译失败。

## Changes

**Conan 与构建路径**

- 新增 `profile-clang-arm64` / `profile-gcc-arm64`，`arch=armv8`、`libstdc++11`。
- `scripts/core/build` 按主机架构选择 profile；原生构建时 host/build 上下文复用
  同一 profile，并同时覆盖两个上下文的 `compiler.version`。
- Linux ARM64 不套用 `clang-*` CMake preset（该 preset 强制 `-stdlib=libc++`），
  避免与 libstdc++ 依赖混用；显式传入 `clang-*` preset 时提前报错。
- profile 文件内容纳入 Conan toolchain 重装检测，切换/修改 profile 会重新生成依赖。

**工具链安装**

- `install-deps` 在 root 下不依赖 `sudo`，`--dry-run` 无 sudo 也可执行。
- CMake 从 Kitware 官方 x86_64/aarch64 归档安装，临时目录 + 官方
  `SHA-256.txt` 校验后解压。
- Conan 按 `python3 -m site --user-base` 的脚本目录修正 PATH，并复核实际解析到的
  主版本；`build`/`doctor` 也会自动探测该目录。

**CI 与发布**

- CI 新增 `ubuntu-24.04-arm` 构建/测试 job；缓存键加入 `runner.arch`。
- Release 的 x86_64/aarch64 并行构建并上传 artifact，`publish` job 在两者都成功后
  一次性创建 Release；runner 固定 22.04（glibc 2.35）以匹配目标机。
- 新增 `scripts/ci/package-binary`：复制非 glibc 运行库，RPATH 改为
  `$ORIGIN/lib`，打包后在屏蔽 `LD_LIBRARY_PATH` 的环境中启动验证。

**benchmark 元数据**

- CPU 型号优先用固定 C locale 的 `lscpu`（ARM MIDR 解码），回退 device tree/DMI。
- 核数取 `sched_getaffinity` 与 cgroup `cpu.max` 的较小值；内存按进程自身
  `/proc/self/cgroup` 路径读取限额，拒绝 `max` 与无限额哨兵。
- C++ `DataCollector` 与 `tools/benchmark/compiler_benchmark.py` 使用相同口径。

## Scope

- `.github/workflows/ci.yml`、`.github/workflows/release.yml`、`.github/actions/setup-cpp`
- `scripts/core/{build,install-deps,doctor}`、`scripts/ci/{release-build,package-binary}`、
  `scripts/lib/common.sh`、`scripts/core/toolchain.env`
- `build-config/conan/profile-*-arm64`
- `src/benchmark/data_collector.*`、`tools/benchmark/compiler_benchmark.py` 及相关单测
- README / getting-started / CHANGELOG / 决策笔记

## Out of scope

- NEON 优化（QualityTrimmer 在 ARM 上继续走标量回退）
- x86 主机交叉编译 aarch64
- macOS/Windows ARM 产物
- 运行时 SIMD dispatch
- 升级系统 GCC 或引入 ubuntu-toolchain-r/test PPA

## Compatibility and rollback

CLI 与数据语义不变；x86-64 的构建路径与 preset 行为保持原样。回滚只需移除
ARM profile/CI job 并恢复 release workflow；不会影响已有 x86 产物或 CLI。

## Approval

- Apply approval: `authorized by user request ("支持arm服务器，请你帮助我开发")`
