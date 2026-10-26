# Design: add-arm64-build-support

## Native build, no cross-compilation

目标是在 aarch64 服务器上原生开发、测试与运行。交叉编译无法验证目标机的测试和
性能，也需要额外 sysroot，因此不采用。ARM 上统一使用系统 libstdc++，不安装
libc++；Conan 依赖与主程序保持同一 ABI。

## Conan host/build contexts

| Context | Profile | 关键设置 |
|---|---|---|
| host | `build-config/conan/profile-<compiler>-arm64` | `arch=armv8`、`compiler.libcxx=libstdc++11` |
| build | 复用 host profile | `tools.build:compiler_executables` 指向 clang/gcc |

原生构建时两个上下文使用同一 profile 是安全的。若 build context 使用默认 profile，
Conan 可能按 clang 生成 `-stdlib=libstdc++`，但实际执行 `cc`/`c++`（GCC），
在源码构建工具依赖时直接失败。因此 `scripts/core/build` 同时传 `-pr:h` 与
`-pr:b`，并用 `-s`/`-s:b` 对齐本机编译器大版本。

## Release glibc baseline

portable 的含义不仅是去掉 `-march`，还包括可运行的最低 glibc。若在
`ubuntu-24.04-arm` 上构建，产物会要求 `GLIBC_2.39`，无法在目标 Ubuntu 22.04
（glibc 2.35）启动。release 的 x86_64 与 aarch64 job 因此都固定在 22.04。

## Packaging

oneTBB 在 Conan 中是 shared-only，hwloc 被强制 shared。直接复制构建目录中的
二进制会把 Conan 缓存绝对 RPATH 带入产物。`package-binary` 复制非 glibc 核心
运行库到 `lib/`，把可执行文件 RPATH 改为 `$ORIGIN/lib`、库 RPATH 改为
`$ORIGIN`，然后在清空 `LD_LIBRARY_PATH` 的环境中实际启动一次。

## Benchmark metadata

容器内 `/proc/cpuinfo` 常常没有 CPU 型号，`sysinfo`/`MemTotal` 报宿主资源。
采集顺序固定为：

1. `/proc/cpuinfo` 的 `model name` / `Model` / `Hardware`；
2. `LC_ALL=C LANGUAGE= lscpu`（ARM MIDR 解码）；
3. device tree / DMI 整机型号。

核数取 `sched_getaffinity`（cpuset）与 cgroup `cpu.max`（CFS 配额）的较小值；
内存按 `/proc/self/cgroup` 定位进程自身 cgroup 后读取 v2/v1 限额。纯解析函数
下沉到 `data_collector_detail.h`，用表驱动单测覆盖 `max`、哨兵、非法数字、
CRLF/NUL 等边界。

## Allowed surface

- 构建/发布脚本与 workflow、Conan profile、benchmark 采集实现及其单测、文档。
- 不修改 CLI 命令、公共头文件导出面或 FASTQ 处理语义。

## Verification strategy

1. aarch64 上 `./scripts/core/build` 成功，产出 `build/clang-release/FastQTools`；
2. `./scripts/core/test` 全部通过；
3. `package-binary` 产物在屏蔽 `LD_LIBRARY_PATH` 后能启动；
4. `lint check`、`notes verify`、YAML 解析通过；
5. CI/release 的实际 runner 执行在推送后验证。

## Risk

- ARM 首次构建需源码编译四个依赖，耗时和磁盘占用高于 x86。
- 显式 `--preset clang-*` 在 ARM 上被拒绝；需要 libc++ 的用户应自备 profile/preset。
- TSan 在受限 seccomp 容器中可能不可用，属于环境限制。
