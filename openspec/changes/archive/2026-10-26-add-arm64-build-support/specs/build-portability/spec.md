# Build Portability Change Specification

## ADDED Requirements

### Requirement: ARM64 uses native Conan profiles
On Linux aarch64/arm64, the build SHALL select a repository Conan profile with
`arch=armv8` and SHALL NOT pass an x86-64 profile.

#### Scenario: Build on aarch64
- **WHEN** a user runs `./scripts/core/build` on Linux aarch64/arm64
- **THEN** the selected host profile SHALL be
  `build-config/conan/profile-<compiler>-arm64`
- **AND** the dependency graph SHALL use `arch=armv8`

#### Scenario: Apple Silicon is not treated as Linux ARM
- **WHEN** the host OS is macOS on arm64
- **THEN** the build SHALL use Conan's default profile
- **AND** SHALL NOT apply a Linux `armv8` profile

### Requirement: Host and build contexts use the same native compiler
For native builds with a repository profile, the Conan build context SHALL use
the same profile as the host context, and compiler version overrides SHALL apply
to both contexts.

#### Scenario: Source-build a tool requirement
- **WHEN** Conan builds a tool requirement such as ninja from source on aarch64
- **THEN** the compiler executable SHALL be the configured clang/gcc
- **AND** the build SHALL NOT inject a `-stdlib` option unsupported by that compiler

### Requirement: ARM builds reject libc++ presets
On Linux ARM64, the build wrapper SHALL reject explicit `clang-*` CMake presets
because they force `-stdlib=libc++` while ARM dependencies use libstdc++.

#### Scenario: Explicit clang preset on ARM
- **WHEN** a user passes `--preset clang-release` on Linux ARM64
- **THEN** the build SHALL fail before configuration with a message pointing to
  the default build path or GCC

### Requirement: Release artifacts target the oldest supported glibc
Published x86_64 and aarch64 binaries SHALL be built on Ubuntu 22.04
(glibc 2.35) or an equivalent older baseline, not on a newer runner image.

#### Scenario: Build release on GitHub-hosted runners
- **WHEN** a `v*` tag triggers the release workflow
- **THEN** x86_64 SHALL run on `ubuntu-22.04`
- **AND** aarch64 SHALL run on `ubuntu-22.04-arm`
- **AND** a single `publish` job SHALL create the Release only after both succeed

### Requirement: Release binaries are relocatable
Published tarballs SHALL bundle non-glibc runtime libraries and resolve them
relative to the executable, without depending on the build machine's Conan cache.

#### Scenario: Run packaged binary on another host
- **WHEN** the tarball is extracted and `FastQTools` is started with
  `LD_LIBRARY_PATH` unset
- **THEN** `ldd` SHALL report no missing libraries
- **AND** `FastQTools --help` SHALL exit successfully

### Requirement: Benchmark metadata reflects the process cgroup
On Linux, benchmark metadata SHALL report the CPU model, the effective core
count, and the memory limit visible to the process, not the host's raw values.

#### Scenario: Collect metadata in a limited container
- **WHEN** the process is confined by `sched_getaffinity` and a cgroup memory limit
- **THEN** core count SHALL NOT exceed the affinity/quota minimum
- **AND** memory bytes SHALL NOT exceed the cgroup limit
- **AND** the CPU model SHALL NOT be reported as the architecture name
  (`aarch64`/`arm64`)
