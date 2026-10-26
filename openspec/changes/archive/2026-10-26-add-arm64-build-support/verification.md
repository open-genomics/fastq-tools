# Verification: add-arm64-build-support

- Status: `Completed`（本地验证；远端 CI/release 待推送后确认）
- Ready to archive: `yes`
- Verifier: `implementing agent (self-verified)`
- Date: 2026-10-26

## Environment

- HEAD: `59b8706` (master)
- Host: aarch64, Kunpeng-920, Ubuntu 22.04, glibc 2.35
- CPU: 21 (cpuset 50-70), memory limit: 200 GiB (cgroup v2)
- Clang 21.1.8, GCC 11.4.0 (system), CMake 4.3.4, Ninja 1.13.2, Conan 2.27.1

## Requirement -> Evidence Matrix

| Requirement | Scenario | Evidence | Result |
|---|---|---|---|
| ARM64 selects native Conan profile | `./scripts/core/build` on aarch64 | `Conan host profile: build-config/conan/profile-clang-arm64`; build 128/128 | passed |
| Host/build compiler contexts agree | Source-build tool requirements | `-pr:h`/`-pr:b` same profile + `-s`/`-s:b` version; ninja/pkgconf build succeeded | passed |
| ARM rejects libc++ presets | `--preset clang-release` on ARM | Early error with alternative | passed |
| Toolchain installs without sudo | root container, no sudo | `install-deps --dry-run` and root install path | passed |
| Portable release runs on 22.04 | aarch64 tarball | Runner pinned `ubuntu-22.04-arm`; local 22.04 build/run | passed (local) |
| Packaged binary resolves libs | `package-binary` | 3 libs bundled; `ldd` no `not found`; `./FastQTools --help` succeeded | passed |
| Benchmark metadata reflects cgroup | ARM container | C++ unit tests + Python returns Kunpeng-920 / 21 / 200 GiB | passed |
| x86-64 path unchanged | x86 profile/preset mapping | `get_conan_profile` x86 branch unchanged; CI matrix retains x86 entries | passed (static) |

## Command Results

| Command | Exit status | Summary |
|---|---|---|
| `./scripts/core/build -c clang -t Release --no-lto` | 0 | 128/128 targets; `build/clang-release/FastQTools` (852K) |
| `./scripts/core/test --build-dir build/clang-release` | 0 | 13/13 tests passed (unit/integration/e2e/packaging) |
| `./scripts/ci/package-binary build/clang-release/FastQTools fastqtools-linux-aarch64 dist` | 0 | `dist/fastqtools-linux-aarch64.tar.gz` + sha256; 3 runtime libs |
| `./scripts/core/lint check` | 0 | Format check passed |
| `./scripts/core/doctor` | 0 | 7 pass, 2 warnings (GCC 11 below, cppcheck missing) |
| `./scripts/core/notes verify` | 0 | 6 notes verified |
| YAML parse (`ci.yml`, `release.yml`, `setup-cpp/action.yml`) | 0 | All parse successfully |
| Python metadata smoke | 0 | cores=21, memory=214748364800, cpu=Kunpeng-920 |

## Notes

- Conan dependency source build (oneTBB/hwloc/zlib-ng/gtest) succeeded on aarch64.
- `conan.lock` was not modified by the build.
- Remote CI/release jobs are configuration-validated only; they require a push/tag
  to execute on GitHub-hosted runners.
- New files must be staged before commit: ARM profiles, `package-binary`,
  `data_collector_detail.h`, benchmark test, decision note, and this change package.
