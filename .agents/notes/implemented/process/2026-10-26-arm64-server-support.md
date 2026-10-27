# Agent Note: ARM64（aarch64）服务器构建支持

Status: implemented

## Problem

FastQTools 的构建脚本、Conan profile、CI 与发布流水线此前都假定 x86-64：

- `scripts/core/build` 仅在 `uname -m == x86_64` 时选择 Conan profile；三个 profile 写死 `arch=x86_64`。
- Conan 用 `armv8` 表示 64 位 ARM，沿用 x86 profile 会请求错误架构的包。
- ConanCenter 对 onetbb/hwloc/zlib-ng/gtest 没有 Linux armv8 预编译二进制，首次必须在目标机源码构建。
- `clang-base` preset 无条件加 `-stdlib=libc++`，而 ARM 服务器通常只有系统 libstdc++；与 Conan 源码构建的依赖混用会带来 ABI 风险。
- `install-deps` 硬编码 `sudo`（容器内以 root 运行且没有 sudo 时直接失败），安装的是 Ubuntu 22.04 的 CMake 3.22（低于 3.28 下限），也不安装 Conan。
- benchmark 元数据在 ARM 上把 CPU 型号记为 `aarch64`，核数与内存取宿主值而非 cgroup 限额。
- release 只产出 `linux-x86_64` 二进制。

不解决这些点，ARM64 服务器既不能从源码构建，也没有可分发产物。

## Decision

**架构与 Conan profile**：新增 `build-config/conan/profile-clang-arm64` 与
`profile-gcc-arm64`，settings 使用 `arch=armv8`、`compiler.libcxx=libstdc++11`。
`scripts/lib/common.sh` 的 `get_conan_profile()` 按 `uname -m` 选择
`profile-<compiler>`（x86-64）或 `profile-<compiler>-arm64`（aarch64/arm64）；
`scripts/core/build` 直接调用它，`scripts/ci/release-build` 使用同一规则的
等价 `case` 映射（release 容器不依赖本地 common.sh）。

**构建路径**：Linux ARM64 上 `get_default_cmake_preset()` 返回非零，
`scripts/core/build` 改用显式 CMake 参数（不用 `clang-release` preset），因此
不会注入 `-stdlib=libc++`。ARM 上产物统一链接系统 libstdc++，与 Conan 源码
构建的依赖保持同一 ABI。x86-64 的 preset 行为不变。

**依赖构建**：首次 `conan install --build=missing` 在 ARM 上源码编译
oneTBB、hwloc、zlib-ng、gtest；`scripts/core/build` 用
`-c tools.build:jobs=${JOBS}` 把 Conan 并发限制到 `nproc`（容器里 Conan 可能
读到宿主 192 核，而 cpuset 只有 21 核）。`cmake` tool_require 仍下载官方
aarch64 二进制，不触发源码编译。原生构建时 host/build 两个上下文复用同一个
仓库 profile（`-pr:h` + `-pr:b`），并按本机编译器大版本同时覆盖
`compiler.version` 与 `compiler.version:build`——否则 Conan 的默认 build
profile 可能声明 clang 却调用 `cc/g++`，向 GCC 注入它不认识的
`-stdlib=libstdc++`，在源码构建 ninja 等工具依赖时直接失败。

**工具链安装**：`scripts/core/install-deps` 在 root 下不调用 sudo；
CMake 统一从 Kitware 官方 x86_64/aarch64 归档装到 `/opt` 并软链到
`/usr/local/bin`（满足 3.28+），下载到 `mktemp -d` 目录并用官方
`cmake-<ver>-SHA-256.txt` 校验后再以 root 解压；用
`python3 -m pip --user` 安装 `conan==2.27.1`，按
`python3 -m site --user-base` 推导 pip 脚本目录（Linux 为 `~/.local/bin`，
macOS 为 `~/Library/Python/X.Y/bin`），安装后复核 PATH 中实际解析到的 Conan
主版本，仅在缺少默认 profile 时执行 `conan profile detect`；`build`/`doctor`
也会自动探测该目录，避免 install-deps 的子进程 export 无法传递给父 shell。
Clang/clang++/clang-format/clang-tidy 仍从 apt.llvm.org 安装 LLVM 21，
并用优先级 200 + 显式 `update-alternatives --set` 钉住——发行版预装的
clang-18 也是 priority 100，仅 `--install` 不会切换。`.github/actions/setup-cpp`
同步此修复，避免 CI 实际用旧 clang 构建。

**版本单一来源**：`scripts/core/toolchain.env` 的 GCC/Clang 下限从 12/16
改为 13/17，与 `CMakeLists.txt` 的硬门禁一致，消除 doctor 对 GCC 12 的假阳性。
`scripts/core/doctor` 同时把 gcc/clang 视为二选一——CMake 门禁只作用于实际
使用的编译器，因此 ARM 上 Clang 21 + 发行版自带旧 GCC 不再被误判为失败。

**CI 与发布**：CI 矩阵新增 `ARM64 Clang Release` 条目，使用
`ubuntu-24.04-arm`（公共仓库免费）；该条目用显式 `--build-dir` 与
`profile-clang-arm64`，不走 libc++ preset。所有 Conan/ccache 缓存主键加入
`runner.arch`，避免 X64/ARM64 产物互相污染。release workflow 的
`linux-x86_64` 与 `linux-aarch64` 两个 job 并行构建并上传 artifact，
`publish` job 在两者都成功后才一次性创建 Release——避免 x86 先发布、ARM
失败时留下不完整版本。发布 runner 固定在 `ubuntu-22.04` /
`ubuntu-22.04-arm`（glibc 2.35），而不是 `ubuntu-latest` / `ubuntu-24.04-arm`：
portable 产物若在 24.04 上构建会要求 `GLIBC_2.39`，无法在 22.04 目标机启动。

**benchmark 元数据**：`DataCollector::getCpuModel()` 在 `/proc/cpuinfo` 无
`model name` 时优先用固定 C locale 的 `lscpu`（把 ARM MIDR 解码成
Kunpeng-920 等型号），再回退到 device tree / DMI 整机型号；`getCoreCount()`
取 `sched_getaffinity`（cpuset）与 `cpu.max`/CFS 配额（ceil(quota/period)）的
较小值；`getMemoryBytes()` 先按 `/proc/self/cgroup` 定位进程自身 cgroup 路径，
再读 v2/v1 内存限额，并拒绝 `max` 与无限额哨兵。纯解析逻辑下沉到
`src/benchmark/data_collector_detail.h`，单测用表驱动覆盖空值、`max`、哨兵、
非法数字、CRLF/NUL 等边界。`tools/benchmark/compiler_benchmark.py` 同步相同的
CPU 型号回退、cgroup 内存与 `sched_getaffinity` 核数口径。

**发布产物可搬运性**：oneTBB 在 Conan 中是 shared-only、hwloc 被强制
shared，直接复制构建目录里的二进制会把构建机的 Conan 缓存绝对 RPATH 带进
产物。新增 `scripts/ci/package-binary`：复制非 glibc 核心的运行库到 `lib/`，
用 `patchelf` 把 RPATH 改为 `$ORIGIN/lib`，再打成 tar.gz；打包后会在屏蔽
`LD_LIBRARY_PATH` 的环境里实际启动一次验证。x86_64 与 aarch64 发布共用该脚本。

## Alternatives considered

- **GCC 15 + ubuntu-toolchain-r/test PPA** — 安装路径最短，`gcc-release` preset 也无需 libc++；但 PPA 的 `libstdc++-15-dev` 会把系统 `libstdc++6` 升到 PPA 的 16 版本，而本机同时运行 CANN/ATB/bisheng 等自带 C++ 运行时的组件，宿主级运行时变更风险不可控。因此选择 Clang 21 + 系统 libstdc++。
- **在 ARM 上继续使用 `clang-release` preset 并安装 libc++-21-dev** — 能保持与 x86 完全相同的 preset；但需要让 Conan 依赖也按 libc++ 重建，且产物引入 libc++ 运行期依赖。ARM 服务器上系统 libstdc++ 更普遍，收益不抵复杂度。
- **交叉编译（x86 主机产出 aarch64 二进制）** — 适合发布流水线；但用户的核心诉求是在鲲鹏服务器上原生开发与运行，交叉编译无法验证目标机上的测试与性能，且需要额外 sysroot/模拟器。选择原生构建。
- **下调 `CMakeLists.txt` 的 GCC 13/Clang 17 门禁到 C++20/GCC 11** — 源码实际最高只用到 C++20 的 `std::string::starts_with`，本机 GCC 11.4 能编译代表性 TU；但依赖库、sanitizer 与 CI 的验证成本高，且会削弱"现代 C++23 工程"的项目定位。保留门禁，改为在 ARM 上安装 Clang 21。
- **当时暂缓、后续已实现：QualityTrimmer NEON** — ARM 上标量回退已验证与 AVX2 语义逐字节等价，功能正确；NEON 属于性能优化，需要独立的基准与跨架构回归，和本次"可构建、可测试、可发布"的目标分开。后续已在 [QualityTrimmer ARM64 NEON 质量扫描](../feature/2026-10-27-neon-quality-trimmer.md) 中补齐。
- **使用自建 ARM runner** — 可以覆盖私有环境与性能测试；但需要长期运维，公共仓库已有免费的 `ubuntu-24.04-arm` 托管 runner，先用托管方案。

## Consequences

- **收益**：ARM64 服务器可直接用 `./scripts/core/install-deps && ./scripts/core/build` 构建；CI 在原生 ARM runner 上跑单测与 e2e；发布同时提供 x86_64 与 aarch64 二进制；benchmark 元数据能反映 cgroup 实际配额。
- **代价与已知上限**：ARM 首次构建需源码编译四个依赖，耗时和磁盘占用高于 x86；ConanCenter 后续若仍无 armv8 二进制，这一成本会持续。ARM 上不提供 libc++ 构建，显式 `--preset clang-release` 仍会失败（需改用默认路径或自备 libc++）。TSan 在受限 seccomp 容器中不可用，属环境限制。NEON 优化已在后续 [QualityTrimmer ARM64 NEON 质量扫描](../feature/2026-10-27-neon-quality-trimmer.md) 中实现；x86 AVX2 路径不变。`zlib-ng` 因 `find_package(ZLIB)` 走模块模式仍未被实际链接（既有问题），ARM 首次构建会编译它但不使用，运行时链接系统 zlib；`install-deps` 已安装 `zlib1g-dev`。
- **重访信号**：ConanCenter 开始发布 Linux armv8 二进制、ARM runner 标签或免费策略变化、或需要 SVE/更激进的 SIMD 优化时，重新评估本决定。

## Verification

```bash
./scripts/core/doctor
./scripts/core/build                 # ARM64 上自动选 profile-*-arm64，产物 build/clang-release/FastQTools
./scripts/core/test --build-dir build/clang-release
./scripts/ci/package-binary build/clang-release/FastQTools FastQTools-test-linux-aarch64 dist
./scripts/core/notes verify
```

`tests/unit/benchmark/test_data_collector.cpp` 覆盖 CPU 型号非架构名、核数不超过
`hardware_concurrency`、内存为正、元数据核心字段非空。
