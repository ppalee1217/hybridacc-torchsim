#!/usr/bin/env python3

from __future__ import annotations

import argparse
import csv
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


FIELD_PATTERNS = {
    "windows": re.compile(r"\[SIM\] Cluster wave gap windows: (\d+)"),
    "dropped": re.compile(r"\[SIM\] Cluster wave gap partial windows dropped: (\d+)"),
    "cycles_total": re.compile(r"\[SIM\] Cluster wave gap cycles total: ([0-9.]+)"),
    "total": re.compile(r"\[SIM\] Cluster wave gap instructions total: (\d+)"),
    "mmio_total": re.compile(r"\[SIM\] Cluster wave gap MMIO configure instructions total: (\d+)"),
    "data_total": re.compile(r"\[SIM\] Cluster wave gap data compute instructions total: (\d+)"),
    "control_total": re.compile(r"\[SIM\] Cluster wave gap start/stop control instructions total: (\d+)"),
    "cycles_avg": re.compile(r"\[SIM\] Cluster wave gap cycles avg: ([0-9.]+)"),
    "total_avg": re.compile(r"\[SIM\] Cluster wave gap instructions avg: ([0-9.]+)"),
    "mmio_avg": re.compile(r"\[SIM\] Cluster wave gap MMIO configure instructions avg: ([0-9.]+)"),
    "data_avg": re.compile(r"\[SIM\] Cluster wave gap data compute instructions avg: ([0-9.]+)"),
    "control_avg": re.compile(r"\[SIM\] Cluster wave gap start/stop control instructions avg: ([0-9.]+)"),
    "cycles_min": re.compile(r"\[SIM\] Cluster wave gap cycles min: ([0-9.]+)"),
    "cycles_max": re.compile(r"\[SIM\] Cluster wave gap cycles max: ([0-9.]+)"),
    "total_min": re.compile(r"\[SIM\] Cluster wave gap instructions min: ([0-9.]+)"),
    "total_max": re.compile(r"\[SIM\] Cluster wave gap instructions max: ([0-9.]+)"),
    "boot_up_cycles": re.compile(r"\[SIM\] Cluster boot-up cycles: ([0-9.]+)"),
    "boot_up_instructions": re.compile(r"\[SIM\] Cluster boot-up instructions: ([0-9.]+)"),
    "drain_out_cycles": re.compile(r"\[SIM\] Cluster drain-out cycles: ([0-9.]+)"),
    "drain_out_instructions": re.compile(r"\[SIM\] Cluster drain-out instructions: ([0-9.]+)"),
}


@dataclass
class CaseStats:
    suite: str
    case: str
    status: str
    windows: int
    dropped: int
    cycles_total: float
    total: int
    mmio_total: int
    data_total: int
    control_total: int
    cycles_avg: float
    total_avg: float
    mmio_avg: float
    data_avg: float
    control_avg: float
    cycles_min: float
    cycles_max: float
    total_min: float
    total_max: float
    boot_up_cycles: float
    boot_up_instructions: float
    drain_out_cycles: float
    drain_out_instructions: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize cluster wave-gap instruction counts from result directories."
    )
    parser.add_argument(
        "--suite",
        action="append",
        nargs=2,
        metavar=("NAME", "DIR"),
        required=True,
        help="Suite name and result directory to scan. Repeat for multiple suites.",
    )
    parser.add_argument(
        "--csv-out",
        type=Path,
        required=True,
        help="Path to write per-case CSV summary.",
    )
    parser.add_argument(
        "--md-out",
        type=Path,
        required=True,
        help="Path to write Markdown summary.",
    )
    return parser.parse_args()


def parse_status(result_path: Path) -> str:
    if not result_path.exists():
        return "MISSING"
    text = result_path.read_text(errors="ignore")
    return text.split("\x1f", 1)[0].strip() or "UNKNOWN"


def parse_case_stats(suite: str, case_dir: Path) -> CaseStats | None:
    sim_log = case_dir / "sim.log"
    if not sim_log.exists():
        return None

    text = sim_log.read_text(errors="ignore")
    values: dict[str, int | float] = {}
    for field, pattern in FIELD_PATTERNS.items():
        match = pattern.search(text)
        if match is None:
            return None
        if field.endswith(("avg", "min", "max", "cycles", "instructions")):
            values[field] = float(match.group(1))
        else:
            values[field] = int(match.group(1))

    return CaseStats(
        suite=suite,
        case=case_dir.name,
        status=parse_status(case_dir / ".e2e.result"),
        windows=int(values["windows"]),
        dropped=int(values["dropped"]),
        cycles_total=float(values["cycles_total"]),
        total=int(values["total"]),
        mmio_total=int(values["mmio_total"]),
        data_total=int(values["data_total"]),
        control_total=int(values["control_total"]),
        cycles_avg=float(values["cycles_avg"]),
        total_avg=float(values["total_avg"]),
        mmio_avg=float(values["mmio_avg"]),
        data_avg=float(values["data_avg"]),
        control_avg=float(values["control_avg"]),
        cycles_min=float(values["cycles_min"]),
        cycles_max=float(values["cycles_max"]),
        total_min=float(values["total_min"]),
        total_max=float(values["total_max"]),
        boot_up_cycles=float(values["boot_up_cycles"]),
        boot_up_instructions=float(values["boot_up_instructions"]),
        drain_out_cycles=float(values["drain_out_cycles"]),
        drain_out_instructions=float(values["drain_out_instructions"]),
    )


def collect_cases(suites: Iterable[tuple[str, Path]]) -> list[CaseStats]:
    rows: list[CaseStats] = []
    for suite_name, suite_dir in suites:
        for case_dir in sorted(path for path in suite_dir.iterdir() if path.is_dir()):
            stats = parse_case_stats(suite_name, case_dir)
            if stats is not None:
                rows.append(stats)
    return rows


def write_csv(rows: list[CaseStats], out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", newline="") as handle:
        writer = csv.writer(handle)
        writer.writerow(
            [
                "suite",
                "case",
                "status",
                "windows",
                "dropped",
                "cycles_total",
                "total",
                "mmio_total",
                "data_total",
                "control_total",
                "cycles_avg",
                "total_avg",
                "mmio_avg",
                "data_avg",
                "control_avg",
                "cycles_min",
                "cycles_max",
                "total_min",
                "total_max",
                "boot_up_cycles",
                "boot_up_instructions",
                "drain_out_cycles",
                "drain_out_instructions",
            ]
        )
        for row in rows:
            writer.writerow(
                [
                    row.suite,
                    row.case,
                    row.status,
                    row.windows,
                    row.dropped,
                    f"{row.cycles_total:.2f}",
                    row.total,
                    row.mmio_total,
                    row.data_total,
                    row.control_total,
                    f"{row.cycles_avg:.2f}",
                    f"{row.total_avg:.2f}",
                    f"{row.mmio_avg:.2f}",
                    f"{row.data_avg:.2f}",
                    f"{row.control_avg:.2f}",
                    f"{row.cycles_min:.2f}",
                    f"{row.cycles_max:.2f}",
                    f"{row.total_min:.2f}",
                    f"{row.total_max:.2f}",
                    f"{row.boot_up_cycles:.2f}",
                    f"{row.boot_up_instructions:.2f}",
                    f"{row.drain_out_cycles:.2f}",
                    f"{row.drain_out_instructions:.2f}",
                ]
            )


def suite_rows(rows: list[CaseStats], suite_name: str, include_failures: bool) -> list[CaseStats]:
    suite_only = [row for row in rows if row.suite == suite_name]
    if include_failures:
        return suite_only
    return [row for row in suite_only if row.status == "PASS"]


def summarize(rows: list[CaseStats]) -> str:
    suite_names = sorted({row.suite for row in rows})
    lines: list[str] = []
    lines.append("# Wave Gap Instruction Summary")
    lines.append("")
    lines.append("Counts are taken from retired core instructions between a cluster HDDU STOP MMIO write and the next HDDU START MMIO write.")
    lines.append("Cycle fields count core-enable-relative cycles reported by the simulator probe.")
    lines.append("Buckets:")
    lines.append("- MMIO configure: retired MMIO instructions excluding the START/STOP control writes.")
    lines.append("- Data compute: all retired non-MMIO firmware instructions in the gap.")
    lines.append("- Start/stop cluster control: the retired HDDU STOP and HDDU START MMIO writes.")
    lines.append("- Boot-up: core enable to the first cluster START.")
    lines.append("- Drain-out: final cluster STOP to MCU program end.")
    lines.append("")

    for suite_name in suite_names:
        suite_all = suite_rows(rows, suite_name, include_failures=True)
        suite_pass = suite_rows(rows, suite_name, include_failures=False)
        if not suite_all:
            continue

        def emit_aggregate(title: str, suite_subset: list[CaseStats]) -> None:
            windows = sum(row.windows for row in suite_subset)
            cycles_total = sum(row.cycles_total for row in suite_subset)
            total = sum(row.total for row in suite_subset)
            mmio_total = sum(row.mmio_total for row in suite_subset)
            data_total = sum(row.data_total for row in suite_subset)
            control_total = sum(row.control_total for row in suite_subset)
            avg_cycles = (cycles_total / windows) if windows else 0.0
            avg_total = (total / windows) if windows else 0.0
            avg_mmio = (mmio_total / windows) if windows else 0.0
            avg_data = (data_total / windows) if windows else 0.0
            avg_control = (control_total / windows) if windows else 0.0
            avg_boot_up_cycles = sum(row.boot_up_cycles for row in suite_subset) / len(suite_subset)
            avg_boot_up_instructions = sum(row.boot_up_instructions for row in suite_subset) / len(suite_subset)
            avg_drain_out_cycles = sum(row.drain_out_cycles for row in suite_subset) / len(suite_subset)
            avg_drain_out_instructions = sum(row.drain_out_instructions for row in suite_subset) / len(suite_subset)
            lines.append(f"## {suite_name} {title}")
            lines.append("")
            lines.append(
                f"- cases: {len(suite_subset)}"
            )
            lines.append(
                f"- completed_windows: {windows}"
            )
            lines.append(
                f"- total_cycles: {cycles_total:.2f}"
            )
            lines.append(
                f"- total_instructions: {total}"
            )
            lines.append(
                f"- mmio_config_total: {mmio_total}"
            )
            lines.append(
                f"- data_compute_total: {data_total}"
            )
            lines.append(
                f"- control_total: {control_total}"
            )
            lines.append(
                f"- avg_per_window_cycles: {avg_cycles:.2f}"
            )
            lines.append(
                f"- avg_per_window_instructions: {avg_total:.2f} = {avg_mmio:.2f} MMIO + {avg_data:.2f} data + {avg_control:.2f} control"
            )
            lines.append(
                f"- avg_boot_up: {avg_boot_up_cycles:.2f} cycles / {avg_boot_up_instructions:.2f} instructions"
            )
            lines.append(
                f"- avg_drain_out: {avg_drain_out_cycles:.2f} cycles / {avg_drain_out_instructions:.2f} instructions"
            )
            lines.append("")

        emit_aggregate("(all cases)", suite_all)
        if len(suite_pass) != len(suite_all):
            emit_aggregate("(PASS-only)", suite_pass)

        lines.append(f"## {suite_name} Per-case")
        lines.append("")
        lines.append("| case | status | windows | cycles_total | total | mmio | data | control | cycles_avg | avg | cycles_min | cycles_max | min | max | boot_up | drain_out |")
        lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        for row in suite_all:
            lines.append(
                f"| {row.case} | {row.status} | {row.windows} | {row.cycles_total:.2f} | {row.total} | {row.mmio_total} | {row.data_total} | {row.control_total} | {row.cycles_avg:.2f} | {row.total_avg:.2f} | {row.cycles_min:.2f} | {row.cycles_max:.2f} | {row.total_min:.2f} | {row.total_max:.2f} | {row.boot_up_cycles:.2f}/{row.boot_up_instructions:.2f} | {row.drain_out_cycles:.2f}/{row.drain_out_instructions:.2f} |"
            )
        lines.append("")

    return "\n".join(lines)


def main() -> None:
    args = parse_args()
    suites = [(name, Path(path)) for name, path in args.suite]
    rows = collect_cases(suites)
    if not rows:
        raise SystemExit("No parsable sim.log files were found.")

    write_csv(rows, args.csv_out)
    args.md_out.parent.mkdir(parents=True, exist_ok=True)
    args.md_out.write_text(summarize(rows))


if __name__ == "__main__":
    main()