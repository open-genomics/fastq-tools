# fastq-tools - Project Context

## Identity

- **Canonical repository**: `open-genomics/fastq-tools`
- **Lifecycle**: active development
- **Language**: C++23 (CMake + Conan + Ninja)

## Core contracts

| Capability | Path | Description |
|---|---|---|
| `build-portability` | `openspec/specs/build-portability/` | CPU baseline profiles, portable/v3/native selection |

## Validation commands

```bash
./scripts/core/lint check
./scripts/core/build --dev
./scripts/core/test
```

## Archived changes

Completed, merged, and moved under `openspec/changes/archive/`:

| Date | Change | Capability |
|---|---|---|
| 2026-08-19 | `define-cpu-build-profiles` | `build-portability` |
| 2026-10-26 | `add-arm64-build-support` | `build-portability` |

## Authority rules

- `src/` is the implementation source of truth.
- Models must not commit, push, create PRs, or publish without explicit authorization.
- Default build is `portable` (no `-march`); v3/native require explicit opt-in.
