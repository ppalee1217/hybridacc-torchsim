#!/usr/bin/env python3
from __future__ import annotations

import argparse
import math
import re
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Iterable

import matplotlib.pyplot as plt
from jinja2 import Template
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import getSampleStyleSheet
from reportlab.lib.units import inch
from reportlab.platypus import Image, Paragraph, SimpleDocTemplate, Spacer, Table, TableStyle

UNIT_TO_MW = {
    "W": 1000.0,
    "mW": 1.0,
    "uW": 1e-3,
    "nW": 1e-6,
    "pW": 1e-9,
}
NUMBER_RE = re.compile(r"^[+-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?$")


@dataclass
class PowerBreakdown:
    internal_mw: float = 0.0
    switching_mw: float = 0.0
    leakage_mw: float = 0.0
    total_mw: float = 0.0


@dataclass
class TimingPoint:
    slack: float = 0.0
    met: bool = False
    status: str = ""
    startpoint: str = ""
    endpoint: str = ""
    check_description: str = ""
    path_group: str = ""
    path_type: str = ""
    arrival_time: float = 0.0
    required_time: float = 0.0
    stages: list["TimingStage"] = field(default_factory=list)


@dataclass
class TimingStage:
    label: str
    incr: float
    path: float


@dataclass
class HierarchyPower:
    name: str
    internal_mw: float
    switching_mw: float
    leakage_mw: float
    total_mw: float
    percent: float | None = None
    depth: int = 0
    instance_name: str = ""
    module_name: str = ""
    parent_name: str | None = None


def is_number(token: str) -> bool:
    return bool(NUMBER_RE.match(token))


def to_mw(value: float, unit: str) -> float:
    if unit not in UNIT_TO_MW:
        raise ValueError(f"Unsupported power unit: {unit}")
    return value * UNIT_TO_MW[unit]


def read_text(path: Path) -> str:
    return path.read_text(errors="replace") if path.exists() else ""


def excerpt_lines(path: Path, limit: int = 12) -> list[str]:
    if not path.exists():
        return []
    lines: list[str] = []
    for raw_line in path.read_text(errors="replace").splitlines():
        line = raw_line.strip()
        if not line:
            continue
        lines.append(line)
        if len(lines) >= limit:
            break
    return lines


def infer_power_unit(text: str) -> str:
    return "W" if "Report : Averaged Power" in text else "mW"


def parse_power_report(path: Path) -> PowerBreakdown:
    text = read_text(path)
    if not text:
        return PowerBreakdown()

    default_unit = infer_power_unit(text)

    total_match = re.search(
        r"^Total\s+([\d.eE+-]+)(?:\s+(mW|uW|nW|pW|W))?\s+([\d.eE+-]+)(?:\s+(mW|uW|nW|pW|W))?\s+([\d.eE+-]+)(?:\s+(mW|uW|nW|pW|W))?\s+([\d.eE+-]+)(?:\s+(mW|uW|nW|pW|W))?",
        text,
        re.MULTILINE,
    )
    if total_match:
        return PowerBreakdown(
            internal_mw=to_mw(float(total_match.group(1)), total_match.group(2) or default_unit),
            switching_mw=to_mw(float(total_match.group(3)), total_match.group(4) or default_unit),
            leakage_mw=to_mw(float(total_match.group(5)), total_match.group(6) or default_unit),
            total_mw=to_mw(float(total_match.group(7)), total_match.group(8) or default_unit),
        )

    fields = {
        "internal_mw": r"Cell Internal Power\s+=\s+([\d.eE+-]+)(?:\s+(mW|uW|nW|pW|W))?",
        "switching_mw": r"Net Switching Power\s+=\s+([\d.eE+-]+)(?:\s+(mW|uW|nW|pW|W))?",
        "leakage_mw": r"Cell Leakage Power\s+=\s+([\d.eE+-]+)(?:\s+(mW|uW|nW|pW|W))?",
        "total_mw": r"Total(?: Dynamic)? Power\s+=\s+([\d.eE+-]+)(?:\s+(mW|uW|nW|pW|W))?",
    }
    values: dict[str, float] = {}
    for key, pattern in fields.items():
        match = re.search(pattern, text)
        if match:
            values[key] = to_mw(float(match.group(1)), match.group(2) or default_unit)
    if values:
        return PowerBreakdown(
            internal_mw=values.get("internal_mw", 0.0),
            switching_mw=values.get("switching_mw", 0.0),
            leakage_mw=values.get("leakage_mw", 0.0),
            total_mw=values.get("total_mw", values.get("internal_mw", 0.0) + values.get("switching_mw", 0.0) + values.get("leakage_mw", 0.0)),
        )

    return PowerBreakdown()


def parse_timing_stages(text: str) -> list[TimingStage]:
    stages: list[TimingStage] = []
    in_table = False
    pending_label: str | None = None
    for raw_line in text.splitlines():
        stripped = raw_line.rstrip()
        compact = stripped.strip()
        if not in_table:
            if compact.startswith("Point"):
                in_table = True
            continue
        if not compact or set(compact) == {"-"}:
            continue
        if compact.startswith("data arrival time"):
            break

        match = re.search(
            r"([+\-]?(?:\d+(?:\.\d*)?|\.\d+))\s+([+\-]?(?:\d+(?:\.\d*)?|\.\d+))\s*(?:[rf])?\s*$",
            stripped,
        )
        if match:
            label = stripped[: match.start()].strip() or pending_label or f"stage_{len(stages) + 1}"
            stages.append(
                TimingStage(
                    label=label,
                    incr=float(match.group(1)),
                    path=float(match.group(2)),
                )
            )
            pending_label = None
            continue

        pending_label = compact

    return stages


def pick_report_time(values: list[str]) -> float:
    numeric = [float(value) for value in values]
    if not numeric:
        return 0.0
    return max(numeric, key=lambda value: (abs(value), -numeric.index(value)))


def parse_timing_report(path: Path) -> TimingPoint:
    text = read_text(path)
    if not text:
        return TimingPoint()

    slack_match = re.search(
        r"slack\s+\(([^)]+)\)\s+([+\-]?(?:\d+(?:\.\d*)?|\.\d+))",
        text,
    )
    start_match = re.search(r"Startpoint:\s*(\S+)", text)
    end_match = re.search(r"Endpoint:\s*(\S+)(?:\s*\n\s*\(([^)]+)\))?", text)
    group_match = re.search(r"Path Group:\s*(.+)", text)
    type_match = re.search(r"Path Type:\s*(.+)", text)
    arrival_values = re.findall(r"data arrival time\s+([+\-]?(?:\d+(?:\.\d*)?|\.\d+))", text)
    required_values = re.findall(r"data required time\s+([+\-]?(?:\d+(?:\.\d*)?|\.\d+))", text)
    status = slack_match.group(1).strip() if slack_match else ""
    return TimingPoint(
        slack=float(slack_match.group(2)) if slack_match else 0.0,
        met=status.startswith("MET") if slack_match else False,
        status=status,
        startpoint=start_match.group(1) if start_match else "",
        endpoint=end_match.group(1) if end_match else "",
        check_description=end_match.group(2).strip() if end_match and end_match.group(2) else "",
        path_group=group_match.group(1).strip() if group_match else "",
        path_type=type_match.group(1).strip() if type_match else "",
        arrival_time=pick_report_time(arrival_values),
        required_time=pick_report_time(required_values),
        stages=parse_timing_stages(text),
    )


def parse_hierarchy_default_units(text: str) -> tuple[str, str, str, str]:
    match = re.search(
        r"Internal[^\n]*\((mW|uW|nW|pW|W)\)[^\n]*Switching[^\n]*\((mW|uW|nW|pW|W)\)[^\n]*Leakage[^\n]*\((mW|uW|nW|pW|W)\)[^\n]*Total[^\n]*\((mW|uW|nW|pW|W)\)",
        text,
    )
    if match:
        return match.group(1), match.group(2), match.group(3), match.group(4)
    default_unit = infer_power_unit(text)
    return (default_unit, default_unit, default_unit, default_unit)


def split_hierarchy_name(name: str) -> tuple[str, str]:
    match = re.match(r"(?P<instance>.+?)\s+\((?P<module>.+)\)$", name)
    if match:
        return match.group("instance").strip(), match.group("module").strip()
    stripped = name.strip()
    return stripped, stripped


def canonical_hierarchy_label(text: str) -> str:
    return re.sub(r"_[0-9a-fA-F]+(?:_[0-9a-fA-F]+)*$", "", text)


def parse_hierarchy_values(
    tokens: list[str],
    defaults: tuple[str, str, str, str],
) -> tuple[tuple[float, str], tuple[float, str], tuple[float, str], tuple[float, str], float | None, list[str]] | None:
    working = tokens.copy()
    percent: float | None = None
    if working and is_number(working[-1]):
        percent = float(working.pop())

    total = pop_power(working, defaults[3])
    leakage = pop_power(working, defaults[2])
    switching = pop_power(working, defaults[1])
    internal = pop_power(working, defaults[0])
    if not all((internal, switching, leakage, total)):
        return None

    return internal, switching, leakage, total, percent, working


def pop_power(tokens: list[str], default_unit: str) -> tuple[float, str] | None:
    if len(tokens) >= 2 and is_number(tokens[-2]) and tokens[-1] in UNIT_TO_MW:
        unit = tokens.pop()
        value = float(tokens.pop())
        return value, unit
    if tokens and is_number(tokens[-1]):
        return float(tokens.pop()), default_unit
    return None


def parse_hierarchy_report(path: Path) -> list[HierarchyPower]:
    text = read_text(path)
    if not text:
        return []

    defaults = parse_hierarchy_default_units(text)
    entries: list[HierarchyPower] = []
    stack: list[HierarchyPower] = []
    pending_name: str | None = None
    pending_depth = 0
    for raw_line in text.splitlines():
        stripped = raw_line.strip()
        if not stripped:
            continue
        if stripped.startswith(("*", "-", "Hierarchy", "Power Group", "Attributes", "Report", "Design", "Version", "Date", "Int", "Library", "Operating", "Wire")):
            continue
        if stripped.startswith("Total"):
            continue

        parsed_values = parse_hierarchy_values(stripped.split(), defaults)
        if parsed_values is None:
            pending_name = stripped
            pending_depth = (len(raw_line) - len(raw_line.lstrip(" "))) // 2
            continue

        internal, switching, leakage, total, percent, remaining_tokens = parsed_values
        if remaining_tokens:
            name = " ".join(remaining_tokens).strip()
            depth = (len(raw_line) - len(raw_line.lstrip(" "))) // 2
        elif pending_name is not None:
            name = pending_name
            depth = pending_depth
            pending_name = None
        else:
            continue

        if not name:
            continue

        instance_name, module_name = split_hierarchy_name(name)
        module_name = canonical_hierarchy_label(module_name)
        stack = stack[:depth]
        parent_name = stack[-1].name if stack else None

        entry = HierarchyPower(
            name=name,
            internal_mw=to_mw(internal[0], internal[1]),
            switching_mw=to_mw(switching[0], switching[1]),
            leakage_mw=to_mw(leakage[0], leakage[1]),
            total_mw=to_mw(total[0], total[1]),
            percent=percent,
            depth=depth,
            instance_name=instance_name,
            module_name=module_name,
            parent_name=parent_name,
        )
        entries.append(entry)
        stack.append(entry)

    return entries


def count_relevant_lines(path: Path) -> int:
    if not path.exists():
        return 0
    count = 0
    for raw_line in path.read_text(errors="replace").splitlines():
        line = raw_line.strip()
        if not line or line.startswith(("-", "Report", "Net", "Object", "Clock", "Info")):
            continue
        count += 1
    return count


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def fmt_mw(value: float) -> str:
    return f"{value:.4f} mW"


def fmt_chart_power(value_mw: float) -> str:
    abs_value = abs(value_mw)
    if abs_value >= 1000.0:
        return f"{value_mw / 1000.0:.3f} W"
    if abs_value >= 1.0:
        return f"{value_mw:.1f} mW"
    if abs_value >= 1e-3:
        return f"{value_mw * 1000.0:.1f} uW"
    return f"{value_mw * 1_000_000.0:.1f} nW"


def compact_stage_label(label: str) -> str:
    normalized = re.sub(r"\s+\([^)]*\)$", "", label.strip())
    replacements = {
        "clock clk (rise edge)": "Clock edge",
        "clock network delay (ideal)": "Clock network",
        "input external delay": "Input delay",
        "data required time": "Required time",
        "data arrival time": "Arrival time",
    }
    if normalized in replacements:
        normalized = replacements[normalized]
    elif "/" in normalized:
        parts = normalized.split("/")
        normalized = "/".join(parts[-2:])

    words = normalized.replace("_", " ").split()
    if not words:
        return normalized

    lines: list[str] = []
    current: list[str] = []
    current_len = 0
    for word in words:
        extra = len(word) + (1 if current else 0)
        if current and current_len + extra > 16:
            lines.append(" ".join(current))
            current = [word]
            current_len = len(word)
        else:
            current.append(word)
            current_len += extra
    if current:
        lines.append(" ".join(current))
    return "\n".join(lines)


def wrap_summary_text(text: str, width: int = 58) -> str:
    if len(text) <= width:
        return text

    chunks: list[str] = []
    current = ""
    for part in text.split("/"):
        candidate = f"{current}/{part}" if current else part
        if current and len(candidate) > width:
            chunks.append(current)
            current = part
        else:
            current = candidate
    if current:
        chunks.append(current)
    return "\n".join(chunks)


def fmt_slack(point: TimingPoint) -> str:
    status = point.status or ("MET" if point.met else "VIOLATED")
    return f"{point.slack:+.4f} ns ({status})"


def create_primetime_delay_chart(output_dir: Path, point: TimingPoint) -> list[Path]:
    if not point.stages:
        return []

    chart_path = output_dir / "primetime_sta_delay_profile.png"
    positions = list(range(len(point.stages)))
    increments = [stage.incr for stage in point.stages]
    cumulative = [stage.path for stage in point.stages]
    labels = [compact_stage_label(stage.label) for stage in point.stages]

    fig, ax_incr = plt.subplots(figsize=(14, 7), facecolor="white")
    ax_path = ax_incr.twinx()

    ax_incr.bar(
        positions,
        increments,
        width=0.64,
        color="#F97316",
        alpha=0.88,
        label="Incremental delay",
    )
    ax_path.plot(
        positions,
        cumulative,
        color="#1D4ED8",
        marker="o",
        linewidth=2.4,
        markersize=4.8,
        label="Cumulative arrival",
    )
    if point.required_time:
        ax_path.axhline(
            point.required_time,
            color="#DC2626",
            linestyle="--",
            linewidth=2.0,
            label="Required time",
        )
    if point.arrival_time:
        ax_path.axhline(
            point.arrival_time,
            color="#0F766E",
            linestyle=":",
            linewidth=1.8,
            label="Arrival time",
        )

    ax_incr.set_xticks(positions)
    ax_incr.set_xticklabels(labels, rotation=34, ha="right", fontsize=8.5)
    ax_incr.set_ylabel("Incremental delay (ns)", color="#9A3412", fontsize=11)
    ax_path.set_ylabel("Cumulative path delay (ns)", color="#1E3A8A", fontsize=11)
    ax_incr.tick_params(axis="y", labelcolor="#9A3412")
    ax_path.tick_params(axis="y", labelcolor="#1E3A8A")
    ax_incr.grid(axis="y", linestyle=":", linewidth=0.8, alpha=0.35)

    title_suffix = point.check_description or point.path_type or "max path"
    ax_incr.set_title(
        f"PrimeTime STA Delay Profile - Worst Max Path ({title_suffix})",
        fontsize=14,
        fontweight="bold",
        color="#111827",
        pad=14,
    )

    summary_lines = [
        f"Start: {wrap_summary_text(point.startpoint)}",
        f"End: {wrap_summary_text(point.endpoint)}",
    ]
    if point.path_group:
        summary_lines.append(f"Group: {point.path_group}")
    if point.required_time:
        summary_lines.append(f"Required: {point.required_time:.4f} ns")
    if point.arrival_time:
        summary_lines.append(f"Arrival: {point.arrival_time:.4f} ns")
    summary_lines.append(f"Slack: {fmt_slack(point)}")
    fig.text(
        0.985,
        0.965,
        "\n".join(summary_lines),
        ha="right",
        va="top",
        fontsize=10,
        color="#111827",
        bbox={"boxstyle": "round,pad=0.5", "facecolor": "#F8FAFC", "edgecolor": "#CBD5E1"},
    )

    handles_incr, labels_incr = ax_incr.get_legend_handles_labels()
    handles_path, labels_path = ax_path.get_legend_handles_labels()
    ax_incr.legend(
        handles_incr + handles_path,
        labels_incr + labels_path,
        loc="upper left",
        frameon=False,
        fontsize=10,
    )

    fig.tight_layout(rect=(0.03, 0.07, 0.97, 0.90))
    fig.savefig(chart_path, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close(fig)
    return [chart_path]


def create_primepower_charts(
    output_dir: Path,
    baseline: PowerBreakdown,
    measured: PowerBreakdown,
    hierarchy_entries: list[HierarchyPower],
) -> list[Path]:
    chart_paths: list[Path] = []

    for stale_name in [
        "primepower_hierarchy_pie.png",
        "primepower_hierarchy_level0_overall_pie.png",
        "primepower_hierarchy_level1_core_control_pie.png",
        "primepower_hierarchy_level1_cluster_pie.png",
        "primepower_hierarchy_level2_noc_pie.png",
        "primepower_vs_synth_bar.png",
    ]:
        stale_path = output_dir / stale_name
        if stale_path.exists():
            stale_path.unlink()

    def plot_pie_chart(filename: str, title: str, slices: list[tuple[str, float]]) -> None:
        if not slices:
            return
        pie_path = output_dir / filename
        labels = [label for label, _ in slices]
        values = [value for _, value in slices]
        total_value = sum(values)
        if total_value <= 0.0:
            return

        palette = [
            "#3B82F6",
            "#E24635",
            "#F4C20D",
            "#16A34A",
            "#F97316",
            "#4CB6C0",
            "#A935C7",
            "#C7C214",
            "#7C3AED",
            "#0EA5A4",
            "#F59E0B",
            "#DC2626",
        ]

        def humanize_label(label: str) -> str:
            overrides = {
                "CoreController": "Core Controller",
                "ComputeCluster": "Compute Cluster",
                "CoreLocalIrq": "Core Local IRQ",
                "ClusterDataFabric": "Cluster Data Fabric",
                "SectionLoader": "Section Loader",
                "DataSram": "Data SRAM",
                "BootHostIf": "Boot Host IF",
                "CoreMcu": "Core MCU",
                "ScratchpadMemory": "Scratchpad Memory",
                "NetworkOnChip": "Network On Chip",
                "HybridDataDeliverUnit": "Hybrid Data Deliver Unit",
                "ClusterControlUnit": "Cluster Control Unit",
                "ProcessElement": "Process Element",
                "NoCRouter": "NoC Router",
            }
            if label in overrides:
                return overrides[label]
            spaced = re.sub(r"(?<=[A-Z])(?=[A-Z][a-z])", " ", label)
            spaced = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", " ", spaced)
            return spaced.replace(" Irq", " IRQ").replace(" Sram", " SRAM")

        def wrap_label(label: str, max_chars: int = 15) -> str:
            words = label.split()
            if not words:
                return label
            lines: list[str] = []
            current: list[str] = []
            current_len = 0
            for word in words:
                extra = len(word) + (1 if current else 0)
                if current and current_len + extra > max_chars:
                    lines.append(" ".join(current))
                    current = [word]
                    current_len = len(word)
                else:
                    current.append(word)
                    current_len += extra
            if current:
                lines.append(" ".join(current))
            return "\n".join(lines)

        def place_callouts(side_items: list[dict[str, float | int | str]]) -> None:
            if not side_items:
                return
            side_items.sort(key=lambda item: float(item["target_y"]))
            y_min = -1.14
            y_max = 1.14
            placed: list[float] = []
            heights: list[float] = []
            for index, item in enumerate(side_items):
                text_height = 0.12 + 0.095 * (int(item["line_count"]) - 1)
                heights.append(text_height)
                target_y = float(item["target_y"])
                min_center = y_min + text_height / 2.0
                if index == 0:
                    placed_y = max(target_y, min_center)
                else:
                    min_gap = (heights[index - 1] + text_height) / 2.0 + 0.04
                    placed_y = max(target_y, placed[-1] + min_gap)
                placed.append(placed_y)
            top_overflow = placed[-1] + heights[-1] / 2.0 - y_max
            if top_overflow > 0:
                shift = top_overflow
                placed = [value - shift for value in placed]
            bottom_overflow = y_min - (placed[0] - heights[0] / 2.0)
            if bottom_overflow > 0:
                shift = bottom_overflow
                placed = [value + shift for value in placed]
            for item, placed_y in zip(side_items, placed):
                item["placed_y"] = placed_y

        fig, ax = plt.subplots(figsize=(11.2, 6.4), facecolor="white")
        wedges, _ = ax.pie(
            values,
            labels=None,
            colors=[palette[index % len(palette)] for index in range(len(values))],
            startangle=90,
            counterclock=False,
            radius=0.92,
            wedgeprops={"linewidth": 0.0, "edgecolor": "white"},
        )

        left_items: list[dict[str, float | int | str]] = []
        right_items: list[dict[str, float | int | str]] = []
        for wedge, raw_label, value in zip(wedges, labels, values):
            mid_angle = (wedge.theta1 + wedge.theta2) / 2.0
            angle_rad = math.radians(mid_angle)
            x = math.cos(angle_rad)
            y = math.sin(angle_rad)
            side = 1 if x >= 0 else -1
            share = value / total_value * 100.0
            wrapped_label = wrap_label(humanize_label(raw_label), max_chars=13)
            callout = {
                "x": x,
                "y": y,
                "side": side,
                "target_y": 1.06 * y,
                "text": f"{wrapped_label}\n{fmt_chart_power(value)} ({share:.1f}%)",
                "line_count": wrapped_label.count("\n") + 2,
            }
            if side > 0:
                right_items.append(callout)
            else:
                left_items.append(callout)

        place_callouts(left_items)
        place_callouts(right_items)

        connector_color = "#8A9099"
        for item in left_items + right_items:
            side = int(item["side"])
            x = float(item["x"])
            y = float(item["y"])
            placed_y = float(item["placed_y"])
            x0 = 0.84 * x
            y0 = 0.84 * y
            elbow_x = 1.06 * side
            text_x = 1.50 * side
            text_anchor_x = text_x - 0.05 * side
            ax.plot([x0, elbow_x], [y0, placed_y], color=connector_color, linewidth=1.6)
            ax.plot([elbow_x, text_anchor_x], [placed_y, placed_y], color=connector_color, linewidth=1.6)
            ax.text(
                text_x,
                placed_y,
                str(item["text"]),
                ha="left" if side > 0 else "right",
                va="center",
                fontsize=11.8,
                fontweight="semibold",
                color="#111827",
                linespacing=1.15,
            )

        ax.set_title(title, fontsize=13, fontweight="bold", color="#111827", pad=6)
        ax.set_aspect("equal")
        ax.set_xlim(-1.86, 1.86)
        ax.set_ylim(-1.32, 1.32)
        ax.axis("off")
        fig.tight_layout(pad=0.6)
        fig.savefig(pie_path, dpi=180, bbox_inches="tight", facecolor="white")
        plt.close(fig)
        chart_paths.append(pie_path)

    def aggregate_by_labels(entries: list[HierarchyPower], labels: list[str]) -> list[tuple[str, float]]:
        totals = {label: 0.0 for label in labels}
        extras = 0.0
        for entry in entries:
            label = entry.module_name or canonical_hierarchy_label(entry.name)
            if label in totals:
                totals[label] += entry.total_mw
            else:
                extras += entry.total_mw
        slices = [(label, totals[label]) for label in labels if totals[label] > 0.0]
        if extras > 0.0:
            slices.append(("Others", extras))
        return slices

    def find_entry(label: str, depth: int | None = None, parent_name: str | None = None) -> HierarchyPower | None:
        for entry in hierarchy_entries:
            entry_label = entry.module_name or canonical_hierarchy_label(entry.name)
            if entry_label != label:
                continue
            if depth is not None and entry.depth != depth:
                continue
            if parent_name is not None and entry.parent_name != parent_name:
                continue
            return entry
        return None

    def children_of(parent: HierarchyPower | None) -> list[HierarchyPower]:
        if parent is None:
            return []
        return [entry for entry in hierarchy_entries if entry.parent_name == parent.name]

    root_entry = hierarchy_entries[0] if hierarchy_entries else None
    if root_entry is not None:
        plot_pie_chart(
            "primepower_hierarchy_level0_overall_pie.png",
            "PrimePower Hierarchy Level 0 - Overall",
            aggregate_by_labels(children_of(root_entry), ["CoreController", "ComputeCluster"]),
        )

        core_controller = find_entry("CoreController", depth=1, parent_name=root_entry.name)
        plot_pie_chart(
            "primepower_hierarchy_level1_core_control_pie.png",
            "PrimePower Hierarchy Level 1 - Core Control",
            aggregate_by_labels(
                children_of(core_controller),
                [
                    "Isram",
                    "DmaEngine",
                    "CoreLocalIrq",
                    "ClusterDataFabric",
                    "SectionLoader",
                    "DataSram",
                    "Plic",
                    "BootHostIf",
                    "CmdFabric",
                    "CoreMcu",
                ],
            ),
        )

        compute_cluster = find_entry("ComputeCluster", depth=1, parent_name=root_entry.name)
        plot_pie_chart(
            "primepower_hierarchy_level1_cluster_pie.png",
            "PrimePower Hierarchy Level 1 - Cluster",
            aggregate_by_labels(
                children_of(compute_cluster),
                ["ScratchpadMemory", "NetworkOnChip", "HybridDataDeliverUnit", "ClusterControlUnit"],
            ),
        )

        noc_entry = find_entry("NetworkOnChip", depth=2, parent_name=compute_cluster.name if compute_cluster else None)
        plot_pie_chart(
            "primepower_hierarchy_level2_noc_pie.png",
            "PrimePower Hierarchy Level 2 - NoC",
            aggregate_by_labels(children_of(noc_entry), ["ProcessElement", "MBUS", "NoCRouter"]),
        )

    bar_path = output_dir / "primepower_vs_synth_bar.png"
    categories = ["Internal", "Switching", "Leakage", "Total"]
    synth_values = [baseline.internal_mw, baseline.switching_mw, baseline.leakage_mw, baseline.total_mw]
    measured_values = [measured.internal_mw, measured.switching_mw, measured.leakage_mw, measured.total_mw]
    positions = range(len(categories))
    width = 0.35
    plt.figure(figsize=(9, 5))
    plt.bar([pos - width / 2 for pos in positions], synth_values, width=width, label="Synthesis baseline")
    plt.bar([pos + width / 2 for pos in positions], measured_values, width=width, label="PrimePower")
    plt.xticks(list(positions), categories)
    plt.ylabel("Power (mW)")
    plt.title("PrimePower vs Synthesis Baseline")
    plt.legend()
    plt.tight_layout()
    plt.savefig(bar_path, dpi=180)
    plt.close()
    chart_paths.append(bar_path)

    return chart_paths


def render_html(title: str, summary_rows: list[tuple[str, str]], sections: list[tuple[str, list[str]]], table_rows: list[list[str]], chart_paths: list[Path]) -> str:
    template = Template(
        """
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <title>{{ title }}</title>
  <style>
    body { font-family: Arial, sans-serif; margin: 32px; color: #1f2933; }
    h1, h2 { color: #102a43; }
    table { border-collapse: collapse; width: 100%; margin: 16px 0 24px; }
    th, td { border: 1px solid #d9e2ec; padding: 8px 10px; text-align: left; }
    th { background: #f0f4f8; }
    .meta { margin-bottom: 24px; }
    .section { margin-bottom: 28px; }
    img { max-width: 100%; margin: 12px 0 24px; border: 1px solid #d9e2ec; }
    ul { padding-left: 20px; }
    code { background: #f0f4f8; padding: 1px 4px; }
  </style>
</head>
<body>
  <h1>{{ title }}</h1>
  <div class="meta">
    <table>
      <tbody>
      {% for label, value in summary_rows %}
        <tr><th>{{ label }}</th><td>{{ value }}</td></tr>
      {% endfor %}
      </tbody>
    </table>
  </div>
  {% for heading, lines in sections %}
  <div class="section">
    <h2>{{ heading }}</h2>
    <ul>
      {% for line in lines %}
      <li>{{ line }}</li>
      {% endfor %}
    </ul>
  </div>
  {% endfor %}
  {% if table_rows %}
  <div class="section">
    <h2>Top Entries</h2>
    <table>
      <thead>
        <tr>
          {% for cell in table_rows[0] %}
          <th>{{ cell }}</th>
          {% endfor %}
        </tr>
      </thead>
      <tbody>
        {% for row in table_rows[1:] %}
        <tr>
          {% for cell in row %}
          <td>{{ cell }}</td>
          {% endfor %}
        </tr>
        {% endfor %}
      </tbody>
    </table>
  </div>
  {% endif %}
  {% for chart in chart_paths %}
  <div class="section">
    <img src="{{ chart }}" alt="chart">
  </div>
  {% endfor %}
</body>
</html>
        """
    )
    return template.render(
        title=title,
        summary_rows=summary_rows,
        sections=sections,
        table_rows=table_rows,
        chart_paths=[path.name for path in chart_paths],
    )


def render_markdown(
    title: str,
    summary_rows: list[tuple[str, str]],
    sections: list[tuple[str, list[str]]],
    table_rows: list[list[str]],
    chart_paths: list[Path],
) -> str:
    lines = [f"# {title}", "", "## Summary", "", "| Metric | Value |", "| --- | --- |"]
    for label, value in summary_rows:
        safe_label = label.replace("|", "\\|")
        safe_value = value.replace("|", "\\|")
        lines.append(f"| {safe_label} | {safe_value} |")

    for heading, entries in sections:
        lines.extend(["", f"## {heading}", ""])
        for entry in entries:
            lines.append(f"- {entry}")

    if table_rows:
        header = table_rows[0]
        lines.extend(["", "## Detail Table", "", f"| {' | '.join(header)} |", f"| {' | '.join(['---'] * len(header))} |"])
        for row in table_rows[1:]:
            lines.append(f"| {' | '.join(row)} |")

    if chart_paths:
        lines.extend(["", "## Figures", ""])
        for chart_path in chart_paths:
            title_text = chart_path.stem.replace("_", " ").title()
            lines.extend([f"### {title_text}", "", f"![{title_text}]({chart_path.name})", ""])

    return "\n".join(lines).rstrip() + "\n"


def write_pdf(title: str, summary_rows: list[tuple[str, str]], sections: list[tuple[str, list[str]]], table_rows: list[list[str]], chart_paths: list[Path], output_path: Path) -> None:
    styles = getSampleStyleSheet()
    document = SimpleDocTemplate(str(output_path), pagesize=A4)
    story = [Paragraph(title, styles["Title"]), Spacer(1, 0.2 * inch)]

    summary_table = Table([[label, value] for label, value in summary_rows], colWidths=[2.2 * inch, 4.8 * inch])
    summary_table.setStyle(
        TableStyle(
            [
                ("BACKGROUND", (0, 0), (0, -1), colors.HexColor("#f0f4f8")),
                ("GRID", (0, 0), (-1, -1), 0.5, colors.HexColor("#d9e2ec")),
                ("VALIGN", (0, 0), (-1, -1), "TOP"),
            ]
        )
    )
    story.extend([summary_table, Spacer(1, 0.25 * inch)])

    for heading, lines in sections:
        story.append(Paragraph(heading, styles["Heading2"]))
        for line in lines:
            story.append(Paragraph(line, styles["BodyText"]))
        story.append(Spacer(1, 0.15 * inch))

    if table_rows:
        story.append(Paragraph("Top Entries", styles["Heading2"]))
        body_table = Table(table_rows, repeatRows=1)
        body_table.setStyle(
            TableStyle(
                [
                    ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#f0f4f8")),
                    ("GRID", (0, 0), (-1, -1), 0.5, colors.HexColor("#d9e2ec")),
                    ("VALIGN", (0, 0), (-1, -1), "TOP"),
                ]
            )
        )
        story.extend([body_table, Spacer(1, 0.2 * inch)])

    for chart_path in chart_paths:
        if chart_path.exists():
            story.extend([Image(str(chart_path), width=6.5 * inch, height=3.8 * inch), Spacer(1, 0.2 * inch)])

    document.build(story)


def generate_primetime_report(report_dir: Path, synth_dir: Path, output_dir: Path) -> None:
    pt_max = parse_timing_report(report_dir / "timing_max.rpt")
    pt_min = parse_timing_report(report_dir / "timing_min.rpt")
    synth_max = parse_timing_report(synth_dir / "timing_max_rpt_HybridAcc.txt")
    synth_min = parse_timing_report(synth_dir / "timing_min_rpt_HybridAcc.txt")
    chart_paths = create_primetime_delay_chart(output_dir, pt_max)

    summary_rows = [
        ("Generated", datetime.now().strftime("%Y-%m-%d %H:%M:%S")),
        ("PrimeTime max slack", fmt_slack(pt_max)),
        ("PrimeTime min slack", fmt_slack(pt_min)),
        ("Synthesis max slack", fmt_slack(synth_max)),
        ("Synthesis min slack", fmt_slack(synth_min)),
        ("PrimeTime report dir", str(report_dir)),
        ("Synthesis baseline dir", str(synth_dir)),
    ]

    sections = [
        (
            "Delta Summary",
            [
                f"Setup slack delta: {pt_max.slack - synth_max.slack:+.4f} ns",
                f"Hold slack delta: {pt_min.slack - synth_min.slack:+.4f} ns",
                f"Worst max path group: {pt_max.path_group or 'n/a'}",
                f"Worst max check: {pt_max.check_description or pt_max.path_type or 'n/a'}",
                f"Worst max arrival vs required: {pt_max.arrival_time:.4f} ns vs {pt_max.required_time:.4f} ns",
                f"PrimeTime worst max path: {pt_max.startpoint} -> {pt_max.endpoint}",
                f"PrimeTime worst min path: {pt_min.startpoint} -> {pt_min.endpoint}",
            ],
        ),
        ("Coverage Excerpt", excerpt_lines(report_dir / "analysis_coverage.rpt") or ["No analysis_coverage.rpt content found."]),
        ("Constraint Excerpt", excerpt_lines(report_dir / "constraint_violators.rpt") or ["No constraint_violators.rpt content found."]),
    ]

    table_rows = [
        ["Metric", "PrimeTime", "Synthesis", "Delta"],
        ["Max slack (ns)", f"{pt_max.slack:+.4f}", f"{synth_max.slack:+.4f}", f"{pt_max.slack - synth_max.slack:+.4f}"],
        ["Min slack (ns)", f"{pt_min.slack:+.4f}", f"{synth_min.slack:+.4f}", f"{pt_min.slack - synth_min.slack:+.4f}"],
    ]

    html = render_html("PrimeTime Analysis Report", summary_rows, sections, table_rows, chart_paths)
    (output_dir / "index.html").write_text(html, encoding="utf-8")
    (output_dir / "report.md").write_text(
        render_markdown("PrimeTime Analysis Report", summary_rows, sections, table_rows, chart_paths),
        encoding="utf-8",
    )
    write_pdf("PrimeTime Analysis Report", summary_rows, sections, table_rows, chart_paths, output_dir / "report.pdf")


def hierarchy_table_rows(entries: Iterable[HierarchyPower]) -> list[list[str]]:
    rows = [["Hierarchy", "Internal", "Switching", "Leakage", "Total", "Share"]]
    for entry in entries:
        share = f"{entry.percent:.2f}%" if entry.percent is not None else "-"
        rows.append(
            [
                entry.name,
                fmt_mw(entry.internal_mw),
                fmt_mw(entry.switching_mw),
                fmt_mw(entry.leakage_mw),
                fmt_mw(entry.total_mw),
                share,
            ]
        )
    return rows


def generate_primepower_report(report_dir: Path, synth_dir: Path, output_dir: Path) -> None:
    measured = parse_power_report(report_dir / "power_summary.rpt")
    baseline = parse_power_report(synth_dir / "power_rpt_HybridAcc.txt")
    hierarchy_entries = parse_hierarchy_report(report_dir / "power_hierarchy.rpt")
    top_entries = sorted(hierarchy_entries[1:] if len(hierarchy_entries) > 1 else hierarchy_entries, key=lambda item: item.total_mw, reverse=True)[:10]
    chart_paths = create_primepower_charts(output_dir, baseline, measured, hierarchy_entries)
    unannotated_count = count_relevant_lines(report_dir / "unannotated_activity.rpt")

    summary_rows = [
        ("Generated", datetime.now().strftime("%Y-%m-%d %H:%M:%S")),
        ("PrimePower total", fmt_mw(measured.total_mw)),
        ("Synthesis baseline total", fmt_mw(baseline.total_mw)),
        ("Delta", fmt_mw(measured.total_mw - baseline.total_mw)),
        ("Unannotated activity lines", str(unannotated_count)),
        ("PrimePower report dir", str(report_dir)),
        ("Synthesis baseline dir", str(synth_dir)),
    ]

    sections = [
        (
            "Power Comparison",
            [
                f"Internal power delta: {measured.internal_mw - baseline.internal_mw:+.4f} mW",
                f"Switching power delta: {measured.switching_mw - baseline.switching_mw:+.4f} mW",
                f"Leakage power delta: {measured.leakage_mw - baseline.leakage_mw:+.4f} mW",
                f"Total power delta: {measured.total_mw - baseline.total_mw:+.4f} mW",
            ],
        ),
        ("Clock Gate Savings Excerpt", excerpt_lines(report_dir / "clock_gate_savings.rpt") or ["No clock_gate_savings.rpt content found."]),
        ("Unannotated Activity Excerpt", excerpt_lines(report_dir / "unannotated_activity.rpt") or ["No unannotated_activity.rpt content found."]),
    ]

    table_rows = hierarchy_table_rows(top_entries)
    html = render_html("PrimePower Analysis Report", summary_rows, sections, table_rows, chart_paths)
    (output_dir / "index.html").write_text(html, encoding="utf-8")
    (output_dir / "report.md").write_text(
        render_markdown("PrimePower Analysis Report", summary_rows, sections, table_rows, chart_paths),
        encoding="utf-8",
    )
    write_pdf("PrimePower Analysis Report", summary_rows, sections, table_rows, chart_paths, output_dir / "report.pdf")


def main() -> int:
    parser = argparse.ArgumentParser(description="Analyze PrimeTime/PrimePower reports and compare with synthesis baselines.")
    parser.add_argument("--mode", choices=["primetime", "primepower"], required=True)
    parser.add_argument("--report-dir", required=True)
    parser.add_argument("--synthesis-report-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    report_dir = Path(args.report_dir).resolve()
    synth_dir = Path(args.synthesis_report_dir).resolve()
    output_dir = Path(args.output_dir).resolve()
    ensure_dir(output_dir)

    if args.mode == "primetime":
        generate_primetime_report(report_dir, synth_dir, output_dir)
    else:
        generate_primepower_report(report_dir, synth_dir, output_dir)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
