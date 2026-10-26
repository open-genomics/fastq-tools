# Tasks: add-arm64-build-support

## 1. Baseline

- [x] 1.1 记录环境与 HEAD
  - HEAD `59b8706`; Kunpeng-920 aarch64, Ubuntu 22.04, glibc 2.35
  - cgroup: 21 核 (`sched_getaffinity`), 200 GiB (`memory.max`)
- [x] 1.2 复现 x86-only 失败点
  - `get_conan_profile` 无 aarch64 映射；Conan 默认 build profile 声明 clang 却调用 GCC 11
  - 报错: `c++: error: unrecognized command-line option '-stdlib=libstdc++'`

## 2. Build and toolchain

- [x] 2.1 新增 `profile-clang-arm64` / `profile-gcc-arm64` (`arch=armv8`)
- [x] 2.2 `scripts/lib/common.sh` 按 OS + 架构选择 profile；macOS 不使用 Linux profile
- [x] 2.3 `scripts/core/build` 同时传 `-pr:h` / `-pr:b`，并覆盖两个上下文的 compiler.version
- [x] 2.4 将 profile 内容纳入 Conan toolchain 重装检测
- [x] 2.5 ARM 上拒绝 `clang-*` preset（强制 libc++）
- [x] 2.6 `install-deps` 支持 root/无 sudo、CMake 归档校验、Conan PATH 探测与版本复核、
      `--dry-run` 无 sudo
- [x] 2.7 `build` / `doctor` 自动探测 pip user scripts 目录

## 3. CI and release

- [x] 3.1 CI 矩阵新增 ARM64 Clang Release，缓存键加入 `runner.arch`
- [x] 3.2 Release x86_64/aarch64 并行构建，publish job 原子发布
- [x] 3.3 Release runner 固定 Ubuntu 22.04 / 22.04-arm（glibc 2.35）
- [x] 3.4 新增 `scripts/ci/package-binary`（运行库 + `$ORIGIN/lib` + 启动验证）
- [x] 3.5 `release-build` 复用 `toolchain.env` 版本、校验 CMake、同步 build context 版本

## 4. Benchmark metadata

- [x] 4.1 C++ CPU 型号：cpuinfo → lscpu (C locale) → device tree/DMI
- [x] 4.2 C++ 核数：affinity 与 `cpu.max` 取小
- [x] 4.3 C++ 内存：进程自身 cgroup v2/v1，拒绝 `max`/哨兵
- [x] 4.4 纯解析逻辑下沉 `data_collector_detail.h` + 表驱动单测
- [x] 4.5 Python benchmark 使用相同 CPU/内存/核数口径

## 5. Docs and verification

- [x] 5.1 README / getting-started / CHANGELOG / 决策笔记更新
- [x] 5.2 `./scripts/core/lint check` 通过
- [x] 5.3 `./scripts/core/build -c clang -t Release --no-lto` 通过（128/128）
- [x] 5.4 `./scripts/core/test --build-dir build/clang-release` 13/13 通过
- [x] 5.5 `./scripts/ci/package-binary ...` 成功并验证启动
- [x] 5.6 `./scripts/core/doctor`、`./scripts/core/notes verify`、YAML 解析通过
- [ ] 5.7 推送后确认 CI ARM job 与 release 两个 runner 实际通过（需要远端执行）
