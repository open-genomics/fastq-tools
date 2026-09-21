#!/usr/bin/env python3
"""Run the production benchmark suite and summarize repeated measurements.

Each executable is run with a fixed five-repetition contract. The raw Google
Benchmark JSON is retained next to a compact median/CV summary so a baseline
can be audited without rerunning the workload.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path
from statistics import median, pstdev
from typing import Any, Dict, Iterable, List, Optional


def benchmark_executables(build_dir: Path) -> List[Path]:
    directory = build_dir / "tools" / "benchmark"
    return sorted(path for path in directory.glob("benchmark_*") if path.is_file())


def run_executable(
    executable: Path, output_path: Path, repetitions: int, benchmark_filter: Optional[str]
) -> Optional[Dict[str, Any]]:
    command = [
        str(executable),
        "--benchmark_format=json",
        f"--benchmark_out={output_path}",
        f"--benchmark_repetitions={repetitions}",
        "--benchmark_min_time=0.001s",
        "--benchmark_report_aggregates_only=false",
    ]
    if benchmark_filter:
        command.append(f"--benchmark_filter={benchmark_filter}")
    completed = subprocess.run(command, check=False, capture_output=True, text=True)
    if completed.returncode != 0:
        output = f"{completed.stdout}\n{completed.stderr}"
        if "Failed to match any benchmarks" in output:
            output_path.unlink(missing_ok=True)
            print(f"Skipping {executable.name}: filter matched no benchmarks")
            return None
        print(output, file=sys.stderr, end="")
        raise subprocess.CalledProcessError(completed.returncode, command)
    if not output_path.exists() or output_path.stat().st_size == 0:
        output_path.unlink(missing_ok=True)
        return None
    return json.loads(output_path.read_text(encoding="utf-8"))


def sample_name(name: str) -> str:
    return re.sub(r"_(?:mean|median|stddev)$", "", name)


def is_iteration(entry: Dict[str, Any]) -> bool:
    run_type = entry.get("run_type")
    if run_type:
        return run_type == "iteration"
    name = str(entry.get("name", ""))
    return not name.endswith(("_mean", "_median", "_stddev"))


def values(entries: Iterable[Dict[str, Any]], key: str) -> List[float]:
    return [float(entry.get(key, 0.0) or 0.0) for entry in entries]


def counter(entry: Dict[str, Any], key: str) -> float:
    nested = entry.get("counters", {})
    return float(entry.get(key, nested.get(key, 0.0)) or 0.0)


def summarize(data: Dict[str, Any], source: str, repetitions: int) -> Dict[str, Any]:
    grouped: Dict[str, List[Dict[str, Any]]] = {}
    for entry in data.get("benchmarks", []):
        if is_iteration(entry):
            grouped.setdefault(sample_name(str(entry.get("name", ""))), []).append(entry)

    results: List[Dict[str, Any]] = []
    violations: List[str] = []
    for name, entries in sorted(grouped.items()):
        time_field = "cpu_time" if "stat" in name.lower() else "real_time"
        times = values(entries, time_field)
        reads = [counter(entry, "reads_per_s") for entry in entries]
        mib = [counter(entry, "mib_per_s") for entry in entries]
        memory = [counter(entry, "peak_memory_bytes") for entry in entries]
        mean_time = sum(times) / len(times) if times else 0.0
        cv_percent = (pstdev(times) / mean_time * 100.0) if len(times) > 1 and mean_time else 0.0
        lower_name = name.lower()
        target_cv = 10.0 if any(part in lower_name for part in ("reader", "writer", "filter")) else 5.0
        status = "pass" if cv_percent <= target_cv else "high-cv"
        if status != "pass":
            violations.append(name)

        results.append(
            {
                "name": name,
                "repetitions": len(entries),
                "median_time": median(times) if times else 0.0,
                "median_reads_per_s": median(reads) if reads else 0.0,
                "median_mib_per_s": median(mib) if mib else 0.0,
                "peak_memory_bytes": max(memory) if memory else 0.0,
                "time_source": time_field,
                "cv_percent": cv_percent,
                "target_cv_percent": target_cv,
                "cv_status": status,
            }
        )

    return {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "source": source,
        "repetitions_requested": repetitions,
        "benchmarks": results,
        "cv_violations": violations,
    }


def write_summary(summary: Dict[str, Any], path: Path) -> None:
    path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print("Benchmark summary:")
    print("name | median time | reads/s | MiB/s | peak memory | CV | target | status")
    for result in summary["benchmarks"]:
        print(
            f"{result['name']} | {result['median_time']:.3f} | "
            f"{result['median_reads_per_s']:.1f} | {result['median_mib_per_s']:.1f} | "
            f"{result['peak_memory_bytes']:.0f} | {result['cv_percent']:.2f}% | "
            f"{result['target_cv_percent']:.0f}% | {result['cv_status']}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--filter", help="Google Benchmark filter passed to each executable")
    parser.add_argument("--enforce-cv", action="store_true")
    args = parser.parse_args()

    if args.repetitions < 2:
        parser.error("--repetitions must be at least 2")

    executables = benchmark_executables(args.build_dir)
    if not executables:
        print(f"No benchmark executables found in {args.build_dir}", file=sys.stderr)
        return 1

    args.output_dir.mkdir(parents=True, exist_ok=True)
    summaries = []
    for executable in executables:
        raw_path = args.output_dir / f"{executable.name}.json"
        data = run_executable(executable, raw_path, args.repetitions, args.filter)
        if data is None:
            continue
        summary = summarize(data, executable.name, args.repetitions)
        write_summary(summary, args.output_dir / f"{executable.name}.summary.json")
        summaries.append(summary)

    violations = [name for summary in summaries for name in summary["cv_violations"]]
    if args.enforce_cv and violations:
        print(f"CV target exceeded: {', '.join(violations)}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
