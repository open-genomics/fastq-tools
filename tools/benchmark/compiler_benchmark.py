#!/usr/bin/env python3
"""
Compiler benchmark runner for FastQTools.

Builds GCC/Clang release binaries, runs benchmark executables, and writes a
comparison JSON report for visualization.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple


DEFAULT_COMPILERS = ("gcc", "clang")


@dataclass
class BenchmarkSummary:
    mean_time_ms: float
    throughput_mbps: float
    throughput_reads_per_sec: float
    max_rss_kb: int
    binary_size_bytes: int


@dataclass
class CompilerReport:
    compiler: str
    compiler_version: str
    build_dir: str
    benchmark_executables: List[str]
    binary_sizes: Dict[str, int]
    max_rss_kb: int
    summary: BenchmarkSummary
    benchmarks: List[Dict[str, Any]]
    raw_results: Dict[str, Any]


def run_command(
    cmd: List[str],
    cwd: Optional[Path] = None,
    capture_output: bool = True,
    check: bool = True,
) -> subprocess.CompletedProcess:
    result = subprocess.run(
        cmd,
        cwd=str(cwd) if cwd else None,
        capture_output=capture_output,
        text=True,
        check=False,
    )
    if check and result.returncode != 0:
        raise RuntimeError(
            f"Command failed ({result.returncode}): {' '.join(cmd)}\n"
            f"stdout: {result.stdout}\n"
            f"stderr: {result.stderr}"
        )
    return result


def get_project_root() -> Path:
    return Path(__file__).resolve().parents[2]


def get_git_info(project_root: Path) -> Tuple[str, str]:
    commit = "unknown"
    branch = "unknown"
    try:
        commit = run_command(
            ["git", "rev-parse", "--short", "HEAD"], cwd=project_root
        ).stdout.strip()
        branch = run_command(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=project_root
        ).stdout.strip()
    except Exception:
        pass
    return commit or "unknown", branch or "unknown"


def get_cpu_model() -> str:
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text(encoding="ascii", errors="ignore").splitlines():
            name, _, value = line.partition(":")
            if name.strip() in {"model name", "Model", "Hardware", "cpu model"}:
                model = value.strip()
                if model:
                    return model

    # ARM64 服务器通常不在 /proc/cpuinfo 暴露 model name：
    # lscpu（MIDR 解码，固定 C locale）→ device tree / DMI 整机型号。
    try:
        lscpu = subprocess.run(
            ["lscpu"],
            capture_output=True,
            text=True,
            check=False,
            env={**os.environ, "LC_ALL": "C", "LANGUAGE": ""},
        )
    except OSError:
        lscpu = None
    if lscpu is not None:
        for line in lscpu.stdout.splitlines():
            if line.startswith("Model name:"):
                model = line.split(":", 1)[1].strip()
                if model and model not in {"-", "N/A", "Not Specified"}:
                    return model

    for path in ("/proc/device-tree/model", "/sys/firmware/devicetree/base/model"):
        device_tree = Path(path)
        if device_tree.exists():
            model = device_tree.read_bytes().replace(b"\0", b"").decode("utf-8", "ignore").strip()
            if model:
                return model

    dmi = Path("/sys/devices/virtual/dmi/id/product_name")
    if dmi.exists():
        model = dmi.read_text(encoding="utf-8", errors="ignore").strip()
        if model:
            return model

    return platform.processor() or platform.machine() or "unknown"


CGROUP_UNLIMITED = 1 << 60


def _read_cgroup_text(path: Path) -> str:
    try:
        return path.read_text(encoding="ascii", errors="ignore").strip()
    except OSError:
        return ""


def _cgroup_path(controller: Optional[str]) -> Optional[str]:
    """返回进程自身 cgroup 路径；controller=None 表示 cgroup v2。"""
    content = _read_cgroup_text(Path("/proc/self/cgroup"))
    for line in content.splitlines():
        parts = line.split(":", 2)
        if len(parts) != 3:
            continue
        _, controllers, relative = parts
        if controller is None:
            if parts[0] == "0" and controllers == "":
                return relative
        elif controller in controllers.split(","):
            return relative
    return None


def _cgroup_candidates(controller: Optional[str], mount: str, leaf: str) -> List[Path]:
    base = Path("/sys/fs/cgroup") / mount if mount else Path("/sys/fs/cgroup")
    candidates: List[Path] = []
    relative = _cgroup_path(controller)
    if relative is not None:
        candidates.append(base / relative.lstrip("/") / leaf)
    # /proc/self/cgroup 不可读或旧布局时的挂载根兜底
    candidates.append(base / leaf)
    return candidates


def _parse_cgroup_limit(raw: str) -> Optional[int]:
    text = raw.strip()
    if not text or text == "max" or not text.isdigit():
        return None
    value = int(text)
    # cgroup v1 用接近 2^63 的哨兵表示无限额
    if value == 0 or value >= CGROUP_UNLIMITED:
        return None
    return value


def get_memory_bytes() -> int:
    host_bytes = 0
    meminfo = Path("/proc/meminfo")
    if meminfo.exists():
        for line in meminfo.read_text(encoding="ascii", errors="ignore").splitlines():
            if line.startswith("MemTotal"):
                parts = line.split()
                if len(parts) >= 2:
                    host_bytes = int(parts[1]) * 1024
                    break

    # 容器内 MemTotal 报宿主内存；cgroup 限额才是实际可用预算。
    limit: Optional[int] = None
    for path in _cgroup_candidates(None, "", "memory.max"):
        limit = _parse_cgroup_limit(_read_cgroup_text(path))
        if limit is not None:
            break
    if limit is None:
        for path in _cgroup_candidates("memory", "memory", "memory.limit_in_bytes"):
            limit = _parse_cgroup_limit(_read_cgroup_text(path))
            if limit is not None:
                break
    if limit is not None and (host_bytes == 0 or limit < host_bytes):
        return limit
    return host_bytes


def _parse_cpu_quota(quota_raw: str, period_raw: str) -> Optional[int]:
    quota_text = quota_raw.strip()
    period_text = period_raw.strip()
    if not quota_text or quota_text in {"max", "-1"}:
        return None
    if not quota_text.isdigit() or not period_text.isdigit():
        return None
    quota = int(quota_text)
    period = int(period_text)
    if quota <= 0 or period <= 0:
        return None
    return (quota + period - 1) // period  # ceil(quota / period)


def _read_cpu_quota() -> Optional[int]:
    for path in _cgroup_candidates(None, "", "cpu.max"):
        parts = _read_cgroup_text(path).split()
        if len(parts) == 2:
            quota = _parse_cpu_quota(parts[0], parts[1])
            if quota is not None:
                return quota
    for path in _cgroup_candidates("cpu", "cpu", "cpu.cfs_quota_us"):
        quota = _parse_cpu_quota(
            _read_cgroup_text(path),
            _read_cgroup_text(path.with_name("cpu.cfs_period_us")),
        )
        if quota is not None:
            return quota
    return None


def get_core_count() -> int:
    try:
        affinity = len(os.sched_getaffinity(0))
    except AttributeError:
        affinity = os.cpu_count() or 0
    quota = _read_cpu_quota()
    if affinity > 0:
        return min(affinity, quota) if quota is not None else affinity
    return quota or 0


def get_os_version() -> str:
    os_release = Path("/etc/os-release")
    if os_release.exists():
        for line in os_release.read_text(encoding="ascii", errors="ignore").splitlines():
            if line.startswith("PRETTY_NAME="):
                return line.split("=", 1)[1].strip().strip('"')
    return platform.platform()


def get_compiler_version(compiler: str) -> str:
    try:
        output = run_command([compiler, "--version"], capture_output=True).stdout
        first_line = output.splitlines()[0].strip() if output else ""
        return first_line or compiler
    except Exception:
        return compiler


def get_default_build_dir(project_root: Path, compiler: str, build_type: str) -> Path:
    return project_root / f"build-{compiler}-{build_type.lower()}"


def ensure_benchmark_targets(project_root: Path, build_dir: Path) -> None:
    if not (build_dir / "CMakeCache.txt").exists():
        raise RuntimeError(f"Build directory not configured: {build_dir}")
    run_command(
        [
            "cmake",
            "-S",
            str(project_root),
            "-B",
            str(build_dir),
            "-DBUILD_BENCHMARKS=ON",
        ],
        cwd=project_root,
    )
    run_command(["cmake", "--build", str(build_dir), "--target", "benchmarks"], cwd=project_root)


def list_benchmark_executables(build_dir: Path) -> List[Path]:
    bench_dir = build_dir / "tools" / "benchmark"
    if not bench_dir.exists():
        return []
    executables = sorted(p for p in bench_dir.glob("benchmark_*") if os.access(p, os.X_OK))
    return executables


def parse_max_rss_kb(stderr: str) -> int:
    match = re.search(r"Maximum resident set size \(kbytes\):\s*(\d+)", stderr)
    if match:
        return int(match.group(1))
    return 0


def run_benchmark_executable(
    exe_path: Path,
    output_path: Path,
    benchmark_filter: Optional[str],
    repetitions: Optional[int],
    time_command: Optional[str],
) -> int:
    cmd = [str(exe_path), "--benchmark_format=json", f"--benchmark_out={output_path}"]
    if benchmark_filter:
        cmd.append(f"--benchmark_filter={benchmark_filter}")
    if repetitions:
        cmd.append(f"--benchmark_repetitions={repetitions}")

    if time_command:
        cmd = [time_command, "-v", *cmd]
    result = run_command(cmd, capture_output=True, check=True)
    return parse_max_rss_kb(result.stderr)


def merge_benchmark_results(results: Iterable[Dict[str, Any]]) -> Dict[str, Any]:
    context: Dict[str, Any] = {}
    benchmarks: List[Dict[str, Any]] = []
    for data in results:
        if not context and data.get("context"):
            context = dict(data["context"])
        benchmarks.extend(data.get("benchmarks", []))
    return {"context": context, "benchmarks": benchmarks}


def extract_benchmark_metrics(data: Dict[str, Any]) -> List[Dict[str, Any]]:
    metrics = []
    for entry in data.get("benchmarks", []):
        name = entry.get("name", "")
        real_time = float(entry.get("real_time", 0.0))
        bytes_per_second = float(entry.get("bytes_per_second", 0.0) or 0.0)
        items_per_second = float(entry.get("items_per_second", 0.0) or 0.0)
        metrics.append(
            {
                "name": name,
                "iterations": entry.get("iterations", 0),
                "mean_time_ms": real_time,
                "throughput_mbps": bytes_per_second / 1e6 if bytes_per_second else 0.0,
                "throughput_reads_per_sec": items_per_second,
            }
        )
    return metrics


def summarize_metrics(metrics: List[Dict[str, Any]], max_rss_kb: int, binary_size_bytes: int) -> BenchmarkSummary:
    def avg(values: List[float]) -> float:
        return sum(values) / len(values) if values else 0.0

    mean_time_ms = avg([m["mean_time_ms"] for m in metrics if m["mean_time_ms"] > 0])
    throughput_mbps = avg([m["throughput_mbps"] for m in metrics if m["throughput_mbps"] > 0])
    throughput_reads = avg(
        [m["throughput_reads_per_sec"] for m in metrics if m["throughput_reads_per_sec"] > 0]
    )

    return BenchmarkSummary(
        mean_time_ms=mean_time_ms,
        throughput_mbps=throughput_mbps,
        throughput_reads_per_sec=throughput_reads,
        max_rss_kb=max_rss_kb,
        binary_size_bytes=binary_size_bytes,
    )


def compute_binary_sizes(build_dir: Path, benchmark_executables: List[Path]) -> Dict[str, int]:
    sizes: Dict[str, int] = {}
    main_binary = build_dir / "FastQTools"
    if main_binary.exists():
        sizes["FastQTools"] = main_binary.stat().st_size
    for exe in benchmark_executables:
        sizes[exe.name] = exe.stat().st_size
    return sizes


def compute_comparison(
    gcc_report: CompilerReport,
    clang_report: CompilerReport,
) -> Dict[str, Any]:
    summary_metrics = [
        ("throughput_mbps", True),
        ("mean_time_ms", False),
        ("max_rss_kb", False),
        ("binary_size_bytes", False),
    ]

    summary: Dict[str, Any] = {}
    for metric, higher_is_better in summary_metrics:
        gcc_value = getattr(gcc_report.summary, metric)
        clang_value = getattr(clang_report.summary, metric)
        delta = (clang_value - gcc_value) / gcc_value * 100 if gcc_value else 0.0
        summary[metric] = {
            "gcc": gcc_value,
            "clang": clang_value,
            "delta_percent": delta,
            "higher_is_better": higher_is_better,
        }

    gcc_metrics = {m["name"]: m for m in gcc_report.benchmarks}
    clang_metrics = {m["name"]: m for m in clang_report.benchmarks}
    benchmark_comparisons: List[Dict[str, Any]] = []

    for name in sorted(set(gcc_metrics) & set(clang_metrics)):
        gcc_entry = gcc_metrics[name]
        clang_entry = clang_metrics[name]
        metrics = {}
        for metric, higher_is_better in [
            ("mean_time_ms", False),
            ("throughput_mbps", True),
            ("throughput_reads_per_sec", True),
        ]:
            gcc_val = float(gcc_entry.get(metric, 0.0))
            clang_val = float(clang_entry.get(metric, 0.0))
            delta = (clang_val - gcc_val) / gcc_val * 100 if gcc_val else 0.0
            metrics[metric] = {
                "gcc": gcc_val,
                "clang": clang_val,
                "delta_percent": delta,
                "higher_is_better": higher_is_better,
            }
        benchmark_comparisons.append({"name": name, "metrics": metrics})

    return {"baseline": "gcc", "summary": summary, "benchmarks": benchmark_comparisons}


def build_compiler_report(
    compiler: str,
    project_root: Path,
    build_dir: Path,
    benchmark_filter: Optional[str],
    repetitions: Optional[int],
    clean_build: bool,
    skip_build: bool,
    time_command: Optional[str],
) -> CompilerReport:
    build_script = project_root / "scripts" / "core" / "build"
    if not skip_build:
        build_cmd = [str(build_script), "-c", compiler, "-t", "Release"]
        if clean_build:
            build_cmd.append("--clean")
        run_command(build_cmd, cwd=project_root, capture_output=False)

    ensure_benchmark_targets(project_root, build_dir)

    benchmark_executables = list_benchmark_executables(build_dir)
    if not benchmark_executables:
        raise RuntimeError(f"No benchmark executables found in {build_dir}")

    combined_results: List[Dict[str, Any]] = []
    max_rss_kb = 0

    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir_path = Path(tmpdir)
        for exe in benchmark_executables:
            output_path = tmpdir_path / f"{compiler}_{exe.name}.json"
            rss_kb = run_benchmark_executable(
                exe, output_path, benchmark_filter, repetitions, time_command
            )
            max_rss_kb = max(max_rss_kb, rss_kb)
            with output_path.open("r", encoding="utf-8") as handle:
                combined_results.append(json.load(handle))

    merged = merge_benchmark_results(combined_results)
    metrics = extract_benchmark_metrics(merged)
    binary_sizes = compute_binary_sizes(build_dir, benchmark_executables)
    summary = summarize_metrics(metrics, max_rss_kb, binary_sizes.get("FastQTools", 0))

    return CompilerReport(
        compiler=compiler,
        compiler_version=get_compiler_version(compiler),
        build_dir=str(build_dir),
        benchmark_executables=[str(path) for path in benchmark_executables],
        binary_sizes=binary_sizes,
        max_rss_kb=max_rss_kb,
        summary=summary,
        benchmarks=metrics,
        raw_results=merged,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Run GCC vs Clang compiler benchmarks")
    parser.add_argument(
        "--data-dir", type=Path, default=Path("docs/performance/benchmark-reports/data"))
    parser.add_argument("--output", "-o", type=Path, help="Output JSON path")
    parser.add_argument("--benchmark-filter", help="Google Benchmark regex filter")
    parser.add_argument("--repetitions", type=int, help="Benchmark repetitions")
    parser.add_argument("--skip-build", action="store_true", help="Skip building binaries")
    parser.add_argument("--clean", action="store_true", help="Clean build directories")
    parser.add_argument("--compiler", action="append", help="Compiler to run (gcc/clang)")
    parser.add_argument("--no-time", action="store_true", help="Disable max RSS collection")

    args = parser.parse_args()

    project_root = get_project_root()
    output_dir = project_root / "docs" / "benchmark-reports" / "compiler"
    output_dir.mkdir(parents=True, exist_ok=True)

    compilers = args.compiler or list(DEFAULT_COMPILERS)
    invalid = [c for c in compilers if c not in DEFAULT_COMPILERS]
    if invalid:
        raise RuntimeError(f"Unsupported compiler(s): {', '.join(invalid)}")

    if not args.data_dir.exists():
        print(
            f"Warning: data dir not found: {args.data_dir}. "
            "Benchmarks will use synthetic data.",
            file=sys.stderr,
        )

    time_command = None if args.no_time else shutil.which("/usr/bin/time") or shutil.which("time")
    if time_command and time_command.endswith("time") and "bash" in time_command:
        time_command = None

    timestamp = datetime.utcnow().strftime("%Y-%m-%d_%H-%M-%S")
    commit, branch = get_git_info(project_root)
    output_path = args.output or (output_dir / f"{timestamp}_{commit}_compiler.json")

    reports: Dict[str, CompilerReport] = {}
    for compiler in compilers:
        build_dir = get_default_build_dir(project_root, compiler, "Release")
        reports[compiler] = build_compiler_report(
            compiler=compiler,
            project_root=project_root,
            build_dir=build_dir,
            benchmark_filter=args.benchmark_filter,
            repetitions=args.repetitions,
            clean_build=args.clean,
            skip_build=args.skip_build,
            time_command=time_command,
        )

    if "gcc" not in reports or "clang" not in reports:
        raise RuntimeError("Both gcc and clang reports are required for comparison")

    metadata = {
        "timestamp": datetime.utcnow().isoformat(timespec="seconds") + "Z",
        "git_commit": commit,
        "git_branch": branch,
        "cpu_model": get_cpu_model(),
        "core_count": get_core_count(),
        "memory_bytes": get_memory_bytes(),
        "os_version": get_os_version(),
        "data_dir": str(args.data_dir),
        "benchmarks": [Path(path).name for path in reports["gcc"].benchmark_executables],
    }

    comparison = compute_comparison(reports["gcc"], reports["clang"])

    report_payload = {
        "metadata": metadata,
        "compilers": {
            name: {
                "compiler": report.compiler,
                "compiler_version": report.compiler_version,
                "build_dir": report.build_dir,
                "benchmark_executables": report.benchmark_executables,
                "binary_sizes": report.binary_sizes,
                "max_rss_kb": report.max_rss_kb,
                "summary": {
                    "mean_time_ms": report.summary.mean_time_ms,
                    "throughput_mbps": report.summary.throughput_mbps,
                    "throughput_reads_per_sec": report.summary.throughput_reads_per_sec,
                    "max_rss_kb": report.summary.max_rss_kb,
                    "binary_size_bytes": report.summary.binary_size_bytes,
                },
                "benchmarks": report.benchmarks,
                "raw_results": report.raw_results,
            }
            for name, report in reports.items()
        },
        "comparison": comparison,
    }

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8") as handle:
        json.dump(report_payload, handle, indent=2)

    latest_path = output_dir / "latest.json"
    shutil.copyfile(output_path, latest_path)

    print(f"Compiler benchmark report written to: {output_path}")


if __name__ == "__main__":
    main()
