#!/usr/bin/env python3
"""Run tm_ring_demo cases and summarize correctness and performance."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


DEFAULT_CASES = (
    "single_rw",
    "multi_master",
    "multi_target_linear",
    "backpressure",
)


def parse_key_value_line(line: str) -> tuple[str, dict[str, str]] | None:
    fields = line.strip().split()
    if not fields or not fields[0].startswith("TEST_"):
        return None
    values: dict[str, str] = {}
    for field in fields[1:]:
        if "=" in field:
            key, value = field.split("=", 1)
            values[key] = value
    return fields[0], values


def run_case(
    executable: str, config: str, case: str, sim_args: list[str]
) -> dict[str, object]:
    command = [executable, config, case] + sim_args
    completed = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        check=False,
    )
    parsed: dict[str, dict[str, str]] = {}
    for line in completed.stdout.splitlines():
        item = parse_key_value_line(line)
        if item is not None:
            name, values = item
            parsed[name] = values

    result = parsed.get("TEST_RESULT", {})
    status = result.get("status", "NO_RESULT")
    if completed.returncode != 0 and status == "PASS":
        status = "PROCESS_ERROR"
    return {
        "case": case,
        "status": status,
        "returncode": completed.returncode,
        "metrics": parsed,
        "output": completed.stdout,
        "command": command,
    }


def metric(run: dict[str, object], group: str, name: str) -> str:
    metrics = run["metrics"]
    assert isinstance(metrics, dict)
    values = metrics.get(group, {})
    return values.get(name, "-")


def numeric_metric(
    run: dict[str, object], group: str, name: str
) -> float | None:
    value = metric(run, group, name)
    try:
        return float(value)
    except ValueError:
        return None


def print_summary(runs: list[dict[str, object]]) -> None:
    headers = (
        "case",
        "status",
        "cycles",
        "payload_B/c",
        "payload_GB/s",
        "util_%",
        "target_met",
        "read_lat",
        "write_lat",
        "stalls",
        "bottleneck",
        "fairness",
    )
    rows = []
    for run in runs:
        rows.append(
            (
                str(run["case"]),
                str(run["status"]),
                metric(run, "TEST_PERF", "completion_cycles"),
                metric(run, "TEST_PERF", "total_payload_bytes_per_cycle"),
                metric(run, "TEST_PERF", "total_payload_bandwidth_GBps"),
                metric(run, "TEST_UTILIZATION", "utilization_pct"),
                metric(run, "TEST_UTILIZATION", "target_met"),
                metric(run, "TEST_LATENCY", "read_avg_cycles"),
                metric(run, "TEST_LATENCY", "write_avg_cycles"),
                metric(run, "TEST_STALLS", "total"),
                metric(run, "TEST_BOTTLENECK", "dominant"),
                metric(run, "TEST_FAIRNESS", "jain_index"),
            )
        )

    widths = [len(header) for header in headers]
    for row in rows:
        for index, value in enumerate(row):
            widths[index] = max(widths[index], len(value))

    def render(row: tuple[str, ...]) -> str:
        return "  ".join(
            value.ljust(widths[index]) for index, value in enumerate(row)
        )

    print(render(headers))
    print(render(tuple("-" * width for width in widths)))
    for row in rows:
        print(render(row))


def print_ratio(
    label: str,
    numerator: dict[str, object],
    denominator: dict[str, object],
    group: str,
    name: str,
) -> None:
    lhs = numeric_metric(numerator, group, name)
    rhs = numeric_metric(denominator, group, name)
    if lhs is None or rhs is None or rhs == 0.0:
        return
    print(f"{label}={lhs / rhs:.3f}x")


def print_comparisons(runs: list[dict[str, object]]) -> None:
    by_case = {str(run["case"]): run for run in runs}
    multi_master = by_case.get("multi_master")
    multi_target = by_case.get("multi_target_linear")
    backpressure = by_case.get("backpressure")

    comparisons = []
    if multi_master is not None and multi_target is not None:
        comparisons.append(
            (
                "multi_target throughput / multi_master",
                multi_target,
                multi_master,
                "TEST_PERF",
                "total_payload_bytes_per_cycle",
            )
        )
        comparisons.append(
            (
                "multi_target cycles / multi_master",
                multi_target,
                multi_master,
                "TEST_PERF",
                "completion_cycles",
            )
        )
    if multi_target is not None and backpressure is not None:
        comparisons.append(
            (
                "backpressure throughput / multi_target",
                backpressure,
                multi_target,
                "TEST_PERF",
                "total_payload_bytes_per_cycle",
            )
        )
        comparisons.append(
            (
                "backpressure cycles / multi_target",
                backpressure,
                multi_target,
                "TEST_PERF",
                "completion_cycles",
            )
        )
        comparisons.append(
            (
                "backpressure read latency / multi_target",
                backpressure,
                multi_target,
                "TEST_LATENCY",
                "read_avg_cycles",
            )
        )

    if comparisons:
        print("\nrelative performance (same 4x256 workload):")
        for comparison in comparisons:
            print_ratio(*comparison)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", help="path to test_prj")
    parser.add_argument("config", help="path to pem_config_cloud.toml")
    parser.add_argument(
        "cases",
        nargs="*",
        default=list(DEFAULT_CASES),
        help="cases to run (default: all four)",
    )
    parser.add_argument("--json", dest="json_path", help="write full results JSON")
    parser.add_argument(
        "--sim-arg",
        action="append",
        default=[],
        help="extra simulator option; repeat as needed",
    )
    parser.add_argument(
        "--show-failures",
        action="store_true",
        help="print complete simulator output for failed cases",
    )
    args = parser.parse_args()

    runs = []
    for case in args.cases:
        print(f"running {case} ...", flush=True)
        try:
            runs.append(
                run_case(args.executable, args.config, case, args.sim_arg)
            )
        except OSError as error:
            print(f"failed to start {args.executable}: {error}", file=sys.stderr)
            return 2

    print()
    print_summary(runs)
    print_comparisons(runs)

    if args.json_path:
        output_path = Path(args.json_path)
        output_path.write_text(
            json.dumps(runs, indent=2, ensure_ascii=False), encoding="utf-8"
        )
        print(f"\nfull results: {output_path}")

    failed = [run for run in runs if run["status"] != "PASS"]
    if args.show_failures:
        for run in failed:
            print(f"\n===== {run['case']} ({run['status']}) =====")
            print(run["output"], end="")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
