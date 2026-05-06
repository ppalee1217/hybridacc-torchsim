from __future__ import annotations

from dataclasses import dataclass, field
import json
from pathlib import Path
import re
from typing import Any

import matplotlib
import pandas as pd

matplotlib.use("Agg")

import matplotlib.pyplot as plt


POWER_TO_MW = {
    "W": 1000.0,
    "mW": 1.0,
    "uW": 1e-3,
    "nW": 1e-6,
    "pW": 1e-9,
}

NUMERIC_RE = re.compile(r"^[+\-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+\-]?\d+)?$")
NUMERIC_PREFIX_RE = re.compile(
    r"^([+\-]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+\-]?\d+)?)\s*(.*)$"
)
GENERATED_SEGMENT_INDEX_RE = re.compile(r"_(\d+)(?=__)")
SAFE_ARTIFACT_NAME_RE = re.compile(r"[^A-Za-z0-9_.-]+")
SMALL_SLICE_PCT_THRESHOLD = 2.0
AREA_MM2_SWITCH_THRESHOLD_UM2 = 100000.0
UM2_PER_MM2 = 1000000.0

AREA_KEYS = {
    "Number of ports": "num_ports",
    "Number of nets": "num_nets",
    "Number of cells": "num_cells",
    "Number of combinational cells": "num_combinational_cells",
    "Number of sequential cells": "num_sequential_cells",
    "Number of macros/black boxes": "num_macros_black_boxes",
    "Number of buf/inv": "num_buf_inv",
    "Number of references": "num_references",
    "Combinational area": "combinational_area",
    "Buf/Inv area": "buf_inv_area",
    "Noncombinational area": "noncombinational_area",
    "Macro/Black Box area": "macro_black_box_area",
    "Net Interconnect area": "net_interconnect_area",
    "Total cell area": "total_cell_area",
    "Total area": "total_area",
}

CELL_ATTR_CODES = {"b", "cg", "h", "mo", "n", "p", "r", "so", "u"}
LIBRARY_TOKEN_PREFIXES = ("N16ADFP_",)


@dataclass(frozen=True)
class ReportFiles:
    report_dir: Path
    design: str
    area: Path
    power: Path
    cell: Path
    timing_max: Path | None = None
    timing_min: Path | None = None


@dataclass
class ModuleNode:
    path: str
    parent_path: str | None
    children: set[str] = field(default_factory=set)
    direct_leaf_area: float = 0.0
    direct_leaf_power_mw: float = 0.0
    direct_leaf_cell_count: int = 0
    total_area: float = 0.0
    total_estimated_power_mw: float = 0.0
    allocated_direct_leaf_power_mw: float = 0.0
    allocated_total_power_mw: float = 0.0
    total_leaf_cell_count: int = 0
    has_explicit_module_row: bool = False
    reported_module_area: float | None = None
    reported_module_power_mw: float | None = None
    module_reference: str | None = None
    module_library: str | None = None
    module_attributes: str | None = None


class SynReportParser:
    """Parse Synopsys synthesis reports and render summary artifacts."""

    def __init__(self) -> None:
        self._last_module_nodes: dict[str, ModuleNode] | None = None
        self._last_design_name: str | None = None

    def discover_report_files(self, report_root: str | Path) -> ReportFiles:
        root = Path(report_root).expanduser().resolve()
        if not root.exists():
            raise FileNotFoundError(f"Report path not found: {root}")

        candidate_dirs = [root]
        if root.is_dir():
            candidate_dirs.extend(sorted(path for path in root.iterdir() if path.is_dir()))

        matches: list[ReportFiles] = []
        for candidate in candidate_dirs:
            report_files = self._try_build_report_files(candidate)
            if report_files is not None:
                matches.append(report_files)

        if not matches:
            raise FileNotFoundError(
                f"Could not find a complete report set under: {root}"
            )
        if len(matches) > 1:
            joined = ", ".join(str(match.report_dir) for match in matches)
            raise ValueError(
                "Found multiple report directories. Please point --report-dir at one design folder: "
                f"{joined}"
            )
        return matches[0]

    def parse_report_set(
        self,
        report_root: str | Path,
        focus_module: str | None = None,
        top_n: int = 8,
        include_others: bool = True,
    ) -> dict[str, Any]:
        report_files = self.discover_report_files(report_root)
        area_summary = self.parse_area_report(report_files.area)
        power_summary = self.parse_power_report(report_files.power)
        cell_df = self.parse_cell_report(report_files.cell)
        timing: dict[str, Any] = {}

        if report_files.timing_max is not None:
            timing["max"] = self.parse_timing_report(report_files.timing_max)
        if report_files.timing_min is not None:
            timing["min"] = self.parse_timing_report(report_files.timing_min)

        cell_df = self._annotate_power_groups(cell_df, power_summary.get("power_groups", {}))
        module_nodes = self.build_module_hierarchy(
            cell_df=cell_df,
            design_name=report_files.design,
        )
        self._last_module_nodes = module_nodes
        self._last_design_name = report_files.design
        hierarchy_df = self.module_hierarchy_to_frame(
            nodes=module_nodes,
            design_name=report_files.design,
        )
        breakdown_df, resolved_focus_module = self.build_module_breakdown(
            nodes=module_nodes,
            design_name=report_files.design,
            focus_module=focus_module,
            top_n=top_n,
            include_others=include_others,
        )

        result = {
            "design": report_files.design,
            "report_dir": str(report_files.report_dir),
            "report_files": {
                "area": str(report_files.area),
                "power": str(report_files.power),
                "cell": str(report_files.cell),
                "timing_max": str(report_files.timing_max) if report_files.timing_max else None,
                "timing_min": str(report_files.timing_min) if report_files.timing_min else None,
            },
            "area": area_summary,
            "power": power_summary,
            "timing": timing,
            "module_report": {
                "total_cell_rows": int(len(cell_df)),
                "total_module_rows": int(len(hierarchy_df)),
                "focus_module": resolved_focus_module,
                "power_estimation_method": (
                    "estimated from report_power power-group totals, distributed by "
                    "cell-report area share within each power group, then summed to RTL hierarchy"
                ),
                "power_group_area_totals": self._frame_to_records(
                    cell_df.groupby("power_group", dropna=False, as_index=False)
                    .agg(area=("area", "sum"))
                    .sort_values("area", ascending=False)
                ),
                "breakdown_options": {
                    "top_n": top_n,
                    "include_others": include_others,
                    "generated_sibling_fusion": True,
                },
                "hierarchy": self._frame_to_records(hierarchy_df),
                "breakdown": self._frame_to_records(breakdown_df),
            },
        }
        return result

    def write_artifacts(self, result: dict[str, Any], out_dir: str | Path) -> dict[str, Path]:
        output_dir = Path(out_dir).expanduser().resolve()
        output_dir.mkdir(parents=True, exist_ok=True)

        summary_path = output_dir / "summary.json"
        hierarchy_path = output_dir / "module_hierarchy.csv"
        breakdown_path = output_dir / "module_breakdown.csv"
        area_pie_path = output_dir / "module_area_breakdown.png"
        power_pie_path = output_dir / "module_power_breakdown.png"
        recursive_breakdown_dir = output_dir / "all_module_breakdowns"
        recursive_breakdown_index_path = output_dir / "module_breakdown_index.csv"

        summary_path.write_text(json.dumps(result, indent=2), encoding="utf-8")

        hierarchy_df = pd.DataFrame(result["module_report"]["hierarchy"])
        hierarchy_df.to_csv(hierarchy_path, index=False)

        breakdown_df = pd.DataFrame(result["module_report"]["breakdown"])
        breakdown_df.to_csv(breakdown_path, index=False)

        artifacts = {
            "summary_json": summary_path,
            "hierarchy_csv": hierarchy_path,
            "breakdown_csv": breakdown_path,
        }

        if not breakdown_df.empty and (breakdown_df["area"] > 0).any():
            self.save_pie_chart(
                df=breakdown_df,
                value_column="area",
                label_column="label",
                title=f"{result['module_report']['focus_module']} module area breakdown",
                value_unit="area",
                output_path=area_pie_path,
            )
            artifacts["area_pie"] = area_pie_path
        if not breakdown_df.empty and (breakdown_df["estimated_power_mw"] > 0).any():
            self.save_pie_chart(
                df=breakdown_df,
                value_column="estimated_power_mw",
                label_column="label",
                title=f"{result['module_report']['focus_module']} estimated module power breakdown",
                value_unit="mW",
                output_path=power_pie_path,
            )
            artifacts["power_pie"] = power_pie_path

        if self._last_module_nodes is not None and self._last_design_name == result["design"]:
            breakdown_options = result["module_report"].get("breakdown_options", {})
            recursive_index_df = self.export_recursive_breakdowns(
                nodes=self._last_module_nodes,
                design_name=result["design"],
                output_dir=recursive_breakdown_dir,
                top_n=int(breakdown_options.get("top_n", 0) or 0),
                include_others=bool(breakdown_options.get("include_others", True)),
            )
            recursive_index_df.to_csv(recursive_breakdown_index_path, index=False)
            artifacts["breakdown_index_csv"] = recursive_breakdown_index_path
            artifacts["breakdown_dir"] = recursive_breakdown_dir

        return artifacts

    def parse_area_report(self, file_path: str | Path) -> dict[str, Any]:
        text = self._read_text(file_path)
        summary: dict[str, Any] = {
            "design": self._extract_text(text, r"^Design\s*:\s*(.+)$"),
            "date": self._extract_text(text, r"^Date\s*:\s*(.+)$"),
            "metrics": {},
        }

        for raw_label, key in AREA_KEYS.items():
            summary["metrics"][key] = self._extract_optional_float(
                text,
                rf"^{re.escape(raw_label)}:\s*(undefined|[+\-0-9.eE]+)",
            )

        subtotal = re.search(
            r"^Subtotal of datapath\(DP_OP\) cell area:\s*([+\-0-9.eE]+)\s+([+\-0-9.eE]+)%",
            text,
            flags=re.MULTILINE,
        )
        total_synth = re.search(
            r"^Total synthetic cell area:\s*([+\-0-9.eE]+)\s+([+\-0-9.eE]+)%",
            text,
            flags=re.MULTILINE,
        )
        summary["synthetic_area"] = {
            "dp_op_area": float(subtotal.group(1)) if subtotal else None,
            "dp_op_percent": float(subtotal.group(2)) if subtotal else None,
            "total_synthetic_area": float(total_synth.group(1)) if total_synth else None,
            "total_synthetic_percent": float(total_synth.group(2)) if total_synth else None,
        }
        return summary

    def parse_power_report(self, file_path: str | Path) -> dict[str, Any]:
        text = self._read_text(file_path)
        summary: dict[str, Any] = {
            "design": self._extract_text(text, r"^Design\s*:\s*(.+)$"),
            "date": self._extract_text(text, r"^Date\s*:\s*(.+)$"),
            "global_voltage_v": self._extract_optional_float(
                text,
                r"^Global Operating Voltage\s*=\s*([+\-0-9.eE]+)",
            ),
            "cell_internal_mw": self._extract_power_mw(text, r"Cell Internal Power\s*=\s*([+\-0-9.eE]+)\s*(\w+)"),
            "net_switching_mw": self._extract_power_mw(text, r"Net Switching Power\s*=\s*([+\-0-9.eE]+)\s*(\w+)"),
            "total_dynamic_mw": self._extract_power_mw(text, r"Total Dynamic Power\s*=\s*([+\-0-9.eE]+)\s*(\w+)"),
            "cell_leakage_mw": self._extract_power_mw(text, r"Cell Leakage Power\s*=\s*([+\-0-9.eE]+)\s*(\w+)"),
            "power_groups": {},
        }

        total_row = re.search(
            r"^Total\s+([+\-0-9.eE]+)\s*(\w+)\s+([+\-0-9.eE]+)\s*(\w+)\s+([+\-0-9.eE]+)\s*(\w+)\s+([+\-0-9.eE]+)\s*(\w+)$",
            text,
            flags=re.MULTILINE,
        )
        if total_row:
            summary["table_total_mw"] = self._convert_power_to_mw(total_row.group(7), total_row.group(8))
        else:
            leakage = summary["cell_leakage_mw"] or 0.0
            dynamic = summary["total_dynamic_mw"] or 0.0
            summary["table_total_mw"] = dynamic + leakage

        power_group_header = "Power Group      Power            Power               Power              Power"
        lines = text.splitlines()
        if power_group_header in text:
            start_idx = next(
                index for index, line in enumerate(lines) if line.startswith("Power Group")
            )
            group_row_re = re.compile(
                r"^\s*(?P<group>\S+)\s+"
                r"(?P<internal>[+\-0-9.eE]+)\s+"
                r"(?P<switching>[+\-0-9.eE]+)\s+"
                r"(?P<leakage>[+\-0-9.eE]+)\s+"
                r"(?P<total>[+\-0-9.eE]+)\s+"
                r"\(\s*(?P<percent>[+\-0-9.eE]+)%\s*\)"
                r"(?:\s+(?P<attrs>.*))?$"
            )
            for line in lines[start_idx + 2 :]:
                stripped = line.strip()
                if not stripped or stripped == "1":
                    if summary["power_groups"]:
                        break
                    continue
                if line.startswith("---"):
                    continue
                if stripped.startswith("Total "):
                    break
                match = group_row_re.match(line)
                if not match:
                    continue
                group_name = match.group("group")
                leakage_nw = float(match.group("leakage"))
                summary["power_groups"][group_name] = {
                    "internal_power_mw": float(match.group("internal")),
                    "switching_power_mw": float(match.group("switching")),
                    "leakage_power_mw": leakage_nw * POWER_TO_MW["nW"],
                    "total_power_mw": float(match.group("total")),
                    "percent": float(match.group("percent")),
                    "attrs": (match.group("attrs") or "").strip() or None,
                }

        return summary

    def parse_timing_report(self, file_path: str | Path) -> dict[str, Any]:
        text = self._read_text(file_path)
        arrival_values = re.findall(r"^\s*data arrival time\s+([+\-0-9.eE]+)$", text, flags=re.MULTILINE)
        required_values = re.findall(r"^\s*data required time\s+([+\-0-9.eE]+)$", text, flags=re.MULTILINE)
        slack = re.search(r"^\s*slack \(([^)]+)\)\s+([+\-0-9.eE]+)$", text, flags=re.MULTILINE)

        return {
            "design": self._extract_text(text, r"^Design\s*:\s*(.+)$"),
            "date": self._extract_text(text, r"^Date\s*:\s*(.+)$"),
            "startpoint": self._extract_text(text, r"^\s*Startpoint:\s*(.+)$"),
            "endpoint": self._extract_text(text, r"^\s*Endpoint:\s*(.+)$"),
            "path_group": self._extract_text(text, r"^\s*Path Group:\s*(.+)$"),
            "path_type": self._extract_text(text, r"^\s*Path Type:\s*(.+)$"),
            "data_arrival_ns": float(arrival_values[0]) if arrival_values else None,
            "data_required_ns": float(required_values[0]) if required_values else None,
            "slack_status": slack.group(1) if slack else None,
            "slack_ns": float(slack.group(2)) if slack else None,
        }

    def parse_cell_report(self, file_path: str | Path) -> pd.DataFrame:
        text = self._read_text(file_path)
        lines = text.splitlines()
        header = next(
            (line for line in lines if line.strip().startswith("Cell") and "Reference" in line),
            None,
        )
        if header is None:
            raise ValueError(f"Could not locate cell table header in {file_path}")

        header_idx = lines.index(header)
        start_idx = header_idx + 2

        blocks: list[list[str]] = []
        current_block: list[str] = []
        for line in lines[start_idx:]:
            stripped = line.strip()
            if not stripped or stripped == "1":
                continue
            if line.startswith("---"):
                continue
            if stripped.startswith("Total "):
                break

            if current_block and line and not line[0].isspace():
                blocks.append(current_block)
                current_block = [line]
                continue

            current_block.append(line)

        if current_block:
            blocks.append(current_block)

        rows: list[dict[str, Any]] = []
        for block in blocks:
            parsed = self._parse_cell_block(block)
            if parsed is not None:
                rows.append(parsed)

        if not rows:
            raise ValueError(f"No cell rows parsed from {file_path}")

        return pd.DataFrame(rows)

    def build_module_hierarchy(
        self,
        cell_df: pd.DataFrame,
        design_name: str,
    ) -> dict[str, ModuleNode]:
        nodes: dict[str, ModuleNode] = {}
        self._ensure_module_node(nodes, "")

        all_paths = {str(cell) for cell in cell_df["cell"].fillna("") if cell}
        parent_prefixes: set[str] = set()
        for path in all_paths:
            parent_prefixes.update(self._iter_parent_paths(path))

        module_row_paths: set[str] = set()
        for row in cell_df.itertuples(index=False):
            cell_path = str(row.cell)
            attrs = self._attribute_tokens(row.attributes)
            reference = str(row.reference or "")
            has_library = pd.notna(row.library) and str(row.library).strip() != ""
            is_clock_gate = "cg" in attrs or reference.upper().startswith("SNPS_CLOCK_GATE")
            has_descendants = cell_path in parent_prefixes
            if has_descendants or ("h" in attrs and not is_clock_gate and not has_library):
                module_row_paths.add(cell_path)

        all_module_paths = set(parent_prefixes) | module_row_paths
        for module_path in sorted(all_module_paths):
            self._ensure_module_node(nodes, module_path)

        for row in cell_df.itertuples(index=False):
            cell_path = str(row.cell)
            if cell_path in module_row_paths:
                node = self._ensure_module_node(nodes, cell_path)
                node.has_explicit_module_row = True
                node.reported_module_area = float(row.area)
                node.reported_module_power_mw = float(row.estimated_power_mw)
                node.module_reference = row.reference
                node.module_library = row.library
                node.module_attributes = row.attributes
                continue

            parent_path = self._parent_module_path(cell_path)
            parent_node = self._ensure_module_node(nodes, parent_path)
            parent_node.direct_leaf_area += float(row.area)
            parent_node.direct_leaf_power_mw += float(row.estimated_power_mw)
            parent_node.direct_leaf_cell_count += 1

        self._finalize_module_totals(nodes, "")
        self._allocate_unexpanded_module_power(nodes, "")
        return nodes

    def module_hierarchy_to_frame(
        self,
        nodes: dict[str, ModuleNode],
        design_name: str,
    ) -> pd.DataFrame:
        root = nodes[""]
        total_area = root.total_area
        total_power = root.total_estimated_power_mw
        records: list[dict[str, Any]] = []

        for path, node in nodes.items():
            module_path = self._display_module_path(path, design_name)
            parent_path = None
            if node.parent_path is not None:
                parent_path = self._display_module_path(node.parent_path, design_name)
            records.append(
                {
                    "module_key": path,
                    "module_path": module_path,
                    "parent_key": node.parent_path,
                    "parent_path": parent_path,
                    "module_name": self._module_name(path, design_name),
                    "depth": 0 if path == "" else path.count("/") + 1,
                    "child_module_count": len(node.children),
                    "direct_leaf_cell_count": node.direct_leaf_cell_count,
                    "total_leaf_cell_count": node.total_leaf_cell_count,
                    "direct_leaf_area": node.direct_leaf_area,
                    "total_area": node.total_area,
                    "area_pct_of_design": 100.0 * node.total_area / total_area if total_area else 0.0,
                    "direct_leaf_power_mw": node.allocated_direct_leaf_power_mw,
                    "total_estimated_power_mw": node.allocated_total_power_mw,
                    "power_pct_of_design": (
                        100.0 * node.allocated_total_power_mw / total_power if total_power else 0.0
                    ),
                    "raw_direct_leaf_power_mw": node.direct_leaf_power_mw,
                    "raw_total_estimated_power_mw": node.total_estimated_power_mw,
                    "has_explicit_module_row": node.has_explicit_module_row,
                    "reported_module_area": node.reported_module_area,
                    "reported_module_power_mw": node.reported_module_power_mw,
                    "module_reference": node.module_reference,
                    "module_library": node.module_library,
                    "module_attributes": node.module_attributes,
                }
            )

        return pd.DataFrame(records).sort_values(
            by=["depth", "total_area", "module_path"],
            ascending=[True, False, True],
        )

    def build_module_breakdown(
        self,
        nodes: dict[str, ModuleNode],
        design_name: str,
        focus_module: str | None,
        top_n: int,
        include_others: bool,
    ) -> tuple[pd.DataFrame, str]:
        focus_key = self._resolve_focus_module(nodes, design_name, focus_module)
        breakdown = self._build_grouped_scope_breakdown(
            nodes=nodes,
            design_name=design_name,
            scope_key=focus_key,
            source_module_keys=[focus_key],
        )
        if breakdown.empty:
            breakdown = pd.DataFrame(
                columns=[
                    "label",
                    "component_type",
                    "module_key",
                    "module_path",
                    "matched_cells",
                    "area",
                    "estimated_power_mw",
                    "source_module_count",
                    "source_module_keys",
                    "source_module_paths",
                    "is_fused",
                    "area_pct",
                    "estimated_power_pct",
                ]
            )
            return breakdown, self._display_module_path(focus_key, design_name)

        breakdown = self._limit_breakdown_rows(
            breakdown=breakdown,
            top_n=top_n,
            include_others=include_others,
        )
        breakdown = self._add_breakdown_percentages(breakdown)
        return breakdown, self._display_module_path(focus_key, design_name)

    def export_recursive_breakdowns(
        self,
        nodes: dict[str, ModuleNode],
        design_name: str,
        output_dir: str | Path,
        top_n: int,
        include_others: bool,
    ) -> pd.DataFrame:
        breakdown_dir = Path(output_dir).expanduser().resolve()
        breakdown_dir.mkdir(parents=True, exist_ok=True)

        index_records: list[dict[str, Any]] = []
        self._export_breakdown_scope(
            nodes=nodes,
            design_name=design_name,
            breakdown_dir=breakdown_dir,
            scope_key="",
            scope_path=design_name,
            scope_label=design_name,
            source_module_keys=[""],
            depth=0,
            is_fused_scope=False,
            top_n=top_n,
            include_others=include_others,
            index_records=index_records,
        )

        if not index_records:
            return pd.DataFrame(
                columns=[
                    "scope_key",
                    "scope_path",
                    "scope_label",
                    "depth",
                    "scope_type",
                    "source_module_count",
                    "source_module_keys",
                    "source_module_paths",
                    "breakdown_csv",
                    "area_pie",
                    "power_pie",
                    "breakdown_row_count",
                    "chart_row_count",
                ]
            )

        return pd.DataFrame(index_records).sort_values(
            by=["depth", "scope_path"],
            ascending=[True, True],
        )

    def _export_breakdown_scope(
        self,
        nodes: dict[str, ModuleNode],
        design_name: str,
        breakdown_dir: Path,
        scope_key: str,
        scope_path: str,
        scope_label: str,
        source_module_keys: list[str],
        depth: int,
        is_fused_scope: bool,
        top_n: int,
        include_others: bool,
        index_records: list[dict[str, Any]],
    ) -> None:
        full_breakdown = self._build_grouped_scope_breakdown(
            nodes=nodes,
            design_name=design_name,
            scope_key=scope_key,
            source_module_keys=source_module_keys,
        )
        if not self._has_child_module_rows(full_breakdown):
            return

        chart_breakdown = self._limit_breakdown_rows(
            breakdown=full_breakdown,
            top_n=top_n,
            include_others=include_others,
        )
        full_breakdown = self._add_breakdown_percentages(full_breakdown)
        chart_breakdown = self._add_breakdown_percentages(chart_breakdown)

        artifact_stem = self._artifact_stem(scope_key, design_name)
        breakdown_csv_path = breakdown_dir / f"{artifact_stem}.csv"
        area_pie_path = breakdown_dir / f"{artifact_stem}__area.png"
        power_pie_path = breakdown_dir / f"{artifact_stem}__power.png"

        self._serialize_breakdown_frame(full_breakdown).to_csv(breakdown_csv_path, index=False)

        area_pie_written = False
        power_pie_written = False
        if (chart_breakdown["area"] > 0).sum() > 1:
            self.save_pie_chart(
                df=chart_breakdown,
                value_column="area",
                label_column="label",
                title=f"{scope_path} module area breakdown",
                value_unit="area",
                output_path=area_pie_path,
            )
            area_pie_written = True
        if (chart_breakdown["estimated_power_mw"] > 0).sum() > 1:
            self.save_pie_chart(
                df=chart_breakdown,
                value_column="estimated_power_mw",
                label_column="label",
                title=f"{scope_path} estimated module power breakdown",
                value_unit="mW",
                output_path=power_pie_path,
            )
            power_pie_written = True

        index_records.append(
            {
                "scope_key": scope_key,
                "scope_path": scope_path,
                "scope_label": scope_label,
                "depth": depth,
                "scope_type": "fused_group" if is_fused_scope else "module",
                "source_module_count": len(source_module_keys),
                "source_module_keys": source_module_keys,
                "source_module_paths": [
                    self._display_module_path(path, design_name) for path in source_module_keys
                ],
                "breakdown_csv": str(breakdown_csv_path),
                "area_pie": str(area_pie_path) if area_pie_written else None,
                "power_pie": str(power_pie_path) if power_pie_written else None,
                "breakdown_row_count": int(len(full_breakdown)),
                "chart_row_count": int(len(chart_breakdown)),
            }
        )

        child_rows = full_breakdown[
            full_breakdown["component_type"].isin(["module", "fused_module_group"])
        ]
        for row in child_rows.itertuples(index=False):
            child_source_keys = list(row.source_module_keys or [])
            if not child_source_keys:
                continue
            if not any(nodes[child_key].children for child_key in child_source_keys):
                continue
            child_scope_key = str(row.module_key or self._join_module_path(scope_key, row.label))
            child_scope_path = str(row.module_path or self._display_module_path(child_scope_key, design_name))
            self._export_breakdown_scope(
                nodes=nodes,
                design_name=design_name,
                breakdown_dir=breakdown_dir,
                scope_key=child_scope_key,
                scope_path=child_scope_path,
                scope_label=str(row.label),
                source_module_keys=child_source_keys,
                depth=depth + 1,
                is_fused_scope=bool(row.is_fused),
                top_n=top_n,
                include_others=include_others,
                index_records=index_records,
            )

    def _build_grouped_scope_breakdown(
        self,
        nodes: dict[str, ModuleNode],
        design_name: str,
        scope_key: str,
        source_module_keys: list[str],
    ) -> pd.DataFrame:
        grouped_children: dict[str, dict[str, Any]] = {}
        local_cells = {
            "matched_cells": 0,
            "area": 0.0,
            "estimated_power_mw": 0.0,
        }

        for source_module_key in source_module_keys:
            source_node = nodes[source_module_key]
            local_cells["matched_cells"] += source_node.direct_leaf_cell_count
            local_cells["area"] += source_node.direct_leaf_area
            local_cells["estimated_power_mw"] += source_node.allocated_direct_leaf_power_mw

            for child_path in sorted(source_node.children):
                child = nodes[child_path]
                raw_label = self._module_name(child_path, design_name)
                normalized_label = self._normalize_generated_module_name(raw_label)
                entry = grouped_children.setdefault(
                    normalized_label,
                    {
                        "matched_cells": 0,
                        "area": 0.0,
                        "estimated_power_mw": 0.0,
                        "source_module_count": 0,
                        "raw_labels": [],
                        "source_module_keys": [],
                        "source_module_paths": [],
                    },
                )
                entry["matched_cells"] += child.total_leaf_cell_count
                entry["area"] += child.total_area
                entry["estimated_power_mw"] += child.allocated_total_power_mw
                entry["source_module_count"] += 1
                entry["raw_labels"].append(raw_label)
                entry["source_module_keys"].append(child_path)
                entry["source_module_paths"].append(self._display_module_path(child_path, design_name))

        rows = []
        for normalized_label, entry in grouped_children.items():
            raw_labels = sorted(entry["raw_labels"])
            source_module_keys_sorted = sorted(entry["source_module_keys"])
            source_module_paths_sorted = sorted(entry["source_module_paths"])
            if len(source_module_keys_sorted) == 1:
                final_label = raw_labels[0]
                module_key = source_module_keys_sorted[0]
                module_path = source_module_paths_sorted[0]
                component_type = "module"
                is_fused = False
            else:
                final_label = raw_labels[0] if len(set(raw_labels)) == 1 else normalized_label
                module_key = self._join_module_path(scope_key, final_label)
                module_path = self._display_module_path(module_key, design_name)
                component_type = "fused_module_group"
                is_fused = True

            rows.append(
                {
                    "label": final_label,
                    "component_type": component_type,
                    "module_key": module_key,
                    "module_path": module_path,
                    "matched_cells": int(entry["matched_cells"]),
                    "area": float(entry["area"]),
                    "estimated_power_mw": float(entry["estimated_power_mw"]),
                    "source_module_count": int(entry["source_module_count"]),
                    "source_module_keys": source_module_keys_sorted,
                    "source_module_paths": source_module_paths_sorted,
                    "is_fused": is_fused,
                }
            )

        if any(local_cells.values()):
            rows.append(
                {
                    "label": "__local_cells__",
                    "component_type": "local_cells",
                    "module_key": None,
                    "module_path": None,
                    "matched_cells": int(local_cells["matched_cells"]),
                    "area": float(local_cells["area"]),
                    "estimated_power_mw": float(local_cells["estimated_power_mw"]),
                    "source_module_count": int(len(source_module_keys)),
                    "source_module_keys": sorted(source_module_keys),
                    "source_module_paths": [
                        self._display_module_path(path, design_name) for path in sorted(source_module_keys)
                    ],
                    "is_fused": False,
                }
            )

        if not rows:
            return pd.DataFrame(
                columns=[
                    "label",
                    "component_type",
                    "module_key",
                    "module_path",
                    "matched_cells",
                    "area",
                    "estimated_power_mw",
                    "source_module_count",
                    "source_module_keys",
                    "source_module_paths",
                    "is_fused",
                ]
            )

        return pd.DataFrame(rows).sort_values(
            by=["area", "label"],
            ascending=[False, True],
        ).reset_index(drop=True)

    @staticmethod
    def _limit_breakdown_rows(
        breakdown: pd.DataFrame,
        top_n: int,
        include_others: bool,
    ) -> pd.DataFrame:
        if breakdown.empty:
            return breakdown.copy()

        limited = breakdown.sort_values("area", ascending=False).reset_index(drop=True)
        if top_n <= 0 or len(limited) <= top_n:
            return limited

        head = limited.head(top_n).copy()
        if include_others:
            remainder = limited.iloc[top_n:]
            head = pd.concat(
                [
                    head,
                    pd.DataFrame(
                        [
                            {
                                "label": "others",
                                "component_type": "aggregate",
                                "module_key": None,
                                "module_path": None,
                                "matched_cells": int(remainder["matched_cells"].sum()),
                                "area": float(remainder["area"].sum()),
                                "estimated_power_mw": float(remainder["estimated_power_mw"].sum()),
                                "source_module_count": int(remainder["source_module_count"].sum()),
                                "source_module_keys": [],
                                "source_module_paths": [],
                                "is_fused": False,
                            }
                        ]
                    ),
                ],
                ignore_index=True,
            )
        return head.reset_index(drop=True)

    @staticmethod
    def _add_breakdown_percentages(breakdown: pd.DataFrame) -> pd.DataFrame:
        if breakdown.empty:
            enriched = breakdown.copy()
            enriched["area_pct"] = []
            enriched["estimated_power_pct"] = []
            return enriched

        enriched = breakdown.copy()
        total_area = float(enriched["area"].sum())
        total_power = float(enriched["estimated_power_mw"].sum())
        enriched["area_pct"] = enriched["area"].apply(
            lambda value: 100.0 * value / total_area if total_area else 0.0
        )
        enriched["estimated_power_pct"] = enriched["estimated_power_mw"].apply(
            lambda value: 100.0 * value / total_power if total_power else 0.0
        )
        return enriched

    @staticmethod
    def _has_child_module_rows(breakdown: pd.DataFrame) -> bool:
        if breakdown.empty:
            return False
        return bool(
            breakdown["component_type"].isin(["module", "fused_module_group"]).any()
        )

    @staticmethod
    def _normalize_generated_module_name(name: str) -> str:
        if "gen_" not in name:
            return name
        return GENERATED_SEGMENT_INDEX_RE.sub("_*", name)

    @staticmethod
    def _join_module_path(parent_path: str, child_name: str) -> str:
        if not parent_path:
            return child_name
        return f"{parent_path}/{child_name}"

    @staticmethod
    def _artifact_stem(scope_key: str, design_name: str) -> str:
        raw_name = design_name if scope_key == "" else scope_key.replace("/", "__")
        raw_name = raw_name.replace("*", "STAR")
        sanitized = SAFE_ARTIFACT_NAME_RE.sub("_", raw_name).strip("._")
        return sanitized or design_name

    @staticmethod
    def _serialize_breakdown_frame(frame: pd.DataFrame) -> pd.DataFrame:
        serialized = frame.copy()
        for column in ["source_module_keys", "source_module_paths"]:
            if column in serialized.columns:
                serialized[column] = serialized[column].apply(
                    lambda value: json.dumps(value) if isinstance(value, list) else value
                )
        return serialized

    def save_pie_chart(
        self,
        df: pd.DataFrame,
        value_column: str,
        label_column: str,
        title: str,
        value_unit: str,
        output_path: str | Path,
    ) -> None:
        chart_df = df[df[value_column] > 0].copy()
        if chart_df.empty:
            return

        main_df, detail_df, detail_label = self._prepare_pie_chart_frames(
            chart_df=chart_df,
            value_column=value_column,
            label_column=label_column,
        )
        display_scale, display_unit = self._select_display_unit(
            values=chart_df[value_column].tolist(),
            value_unit=value_unit,
        )

        def build_autopct(values: list[float]):
            total_value = sum(values)

            def format_pct(percent: float) -> str:
                absolute = percent / 100.0 * total_value
                if percent < 4.0:
                    return f"{percent:.1f}%"
                return (
                    f"{percent:.1f}%\n"
                    f"{self._format_chart_value(absolute, value_unit, display_scale, display_unit)}"
                )

            return format_pct

        has_detail_pie = detail_df is not None and not detail_df.empty
        if has_detail_pie:
            fig = plt.figure(figsize=(14, 8))
            main_ax = fig.add_axes([0.04, 0.08, 0.58, 0.78])
            detail_ax = fig.add_axes([0.70, 0.24, 0.25, 0.50])
            fig.suptitle(self._append_display_unit_to_title(title, display_unit, value_unit))
        else:
            fig, main_ax = plt.subplots(figsize=(11, 8))
            detail_ax = None

        main_values = main_df[value_column].tolist()
        main_labels = main_df[label_column].astype(str).tolist()
        show_main_labels = has_detail_pie or len(main_labels) <= 6

        wedges, _, _ = main_ax.pie(
            main_values,
            labels=main_labels if show_main_labels else None,
            labeldistance=1.08 if show_main_labels else 1.12,
            startangle=140,
            autopct=build_autopct(main_values),
            pctdistance=0.78,
            wedgeprops={"linewidth": 1.0, "edgecolor": "white"},
            textprops={"fontsize": 10},
        )

        if not has_detail_pie and not show_main_labels:
            main_ax.legend(
                wedges,
                [
                    f"{label}: {self._format_chart_value(value, value_unit, display_scale, display_unit)}"
                    for label, value in zip(main_labels, main_values, strict=True)
                ],
                title="Breakdown",
                loc="center left",
                bbox_to_anchor=(1.0, 0.5),
                frameon=False,
            )

        if has_detail_pie and detail_ax is not None:
            detail_bar_df = detail_df.sort_values(value_column, ascending=True).reset_index(drop=True)
            detail_values = detail_bar_df[value_column].tolist()
            detail_labels = detail_bar_df[label_column].astype(str).tolist()
            detail_display_values = [value * display_scale for value in detail_values]
            total_chart_value = float(chart_df[value_column].sum())
            x_limit = max(detail_display_values) if detail_display_values else 0.0
            x_padding = max(x_limit * 0.28, 0.1 if value_unit == "mW" else 0.02)
            y_positions = list(range(len(detail_labels)))
            detail_colors = plt.get_cmap("tab20")(range(len(detail_labels)))

            detail_ax.barh(
                y_positions,
                detail_display_values,
                color=detail_colors,
                edgecolor="white",
                linewidth=0.8,
            )
            detail_ax.set_yticks(y_positions, labels=detail_labels)
            detail_ax.tick_params(axis="y", labelsize=9)
            detail_ax.tick_params(axis="x", labelsize=8)
            detail_ax.set_xlim(0.0, x_limit + x_padding)
            detail_ax.grid(axis="x", linestyle="--", alpha=0.25)
            detail_ax.set_axisbelow(True)
            detail_ax.set_xlabel(
                f"Area ({display_unit})" if value_unit == "area" else f"Power ({display_unit})",
                fontsize=9,
            )
            detail_ax.set_title(f"{detail_label} breakdown")
            for spine_name in ["top", "right"]:
                detail_ax.spines[spine_name].set_visible(False)

            for position, raw_value, display_value in zip(
                y_positions,
                detail_values,
                detail_display_values,
                strict=True,
            ):
                pct_total = 100.0 * raw_value / total_chart_value if total_chart_value else 0.0
                detail_ax.text(
                    display_value + x_padding * 0.08,
                    position,
                    (
                        f"{self._format_chart_value(raw_value, value_unit, display_scale, display_unit)}"
                        f" ({pct_total:.2f}%)"
                    ),
                    va="center",
                    ha="left",
                    fontsize=8,
                )
        else:
            main_ax.set_title(self._append_display_unit_to_title(title, display_unit, value_unit))
            fig.tight_layout()

        fig.savefig(Path(output_path), dpi=200, bbox_inches="tight")
        plt.close(fig)

    @staticmethod
    def _prepare_pie_chart_frames(
        chart_df: pd.DataFrame,
        value_column: str,
        label_column: str,
    ) -> tuple[pd.DataFrame, pd.DataFrame | None, str | None]:
        total_value = float(chart_df[value_column].sum())
        if total_value <= 0.0:
            return chart_df.copy(), None, None

        labels = chart_df[label_column].astype(str)
        small_mask = (
            (100.0 * chart_df[value_column] / total_value) < SMALL_SLICE_PCT_THRESHOLD
        ) & (labels != "others")

        if int(small_mask.sum()) < 2 or int((~small_mask).sum()) == 0:
            return (
                chart_df.sort_values(value_column, ascending=False).reset_index(drop=True),
                None,
                None,
            )

        detail_df = chart_df[small_mask].copy().sort_values(value_column, ascending=False)
        main_df = chart_df[~small_mask].copy()
        detail_label = (
            "others"
            if not (main_df[label_column].astype(str) == "others").any()
            else f"others (<{SMALL_SLICE_PCT_THRESHOLD:.0f}%)"
        )
        main_df = pd.concat(
            [
                main_df,
                pd.DataFrame(
                    [
                        {
                            label_column: detail_label,
                            value_column: float(detail_df[value_column].sum()),
                        }
                    ]
                ),
            ],
            ignore_index=True,
        )
        return (
            main_df.sort_values(value_column, ascending=False).reset_index(drop=True),
            detail_df.reset_index(drop=True),
            detail_label,
        )

    @staticmethod
    def _select_display_unit(values: list[float], value_unit: str) -> tuple[float, str]:
        if value_unit != "area":
            return 1.0, value_unit
        if not values:
            return 1.0, "um^2"
        if max(values) >= AREA_MM2_SWITCH_THRESHOLD_UM2 or sum(values) >= AREA_MM2_SWITCH_THRESHOLD_UM2:
            return 1.0 / UM2_PER_MM2, "mm^2"
        return 1.0, "um^2"

    @staticmethod
    def _format_chart_value(
        value: float,
        value_unit: str,
        display_scale: float,
        display_unit: str,
    ) -> str:
        display_value = value * display_scale
        if value_unit == "mW":
            return f"{display_value:.3f} {display_unit}"
        if display_unit == "mm^2":
            if display_value >= 1.0:
                return f"{display_value:.3f} {display_unit}"
            return f"{display_value:.4f} {display_unit}"
        if display_value >= 100.0:
            return f"{display_value:.0f} {display_unit}"
        if display_value >= 10.0:
            return f"{display_value:.1f} {display_unit}"
        return f"{display_value:.2f} {display_unit}"

    @staticmethod
    def _append_display_unit_to_title(title: str, display_unit: str, value_unit: str) -> str:
        if value_unit != "area":
            return title
        return f"{title} ({display_unit})"

    def _annotate_power_groups(
        self,
        cell_df: pd.DataFrame,
        power_groups: dict[str, Any],
    ) -> pd.DataFrame:
        annotated = cell_df.copy()
        annotated["power_group"] = annotated.apply(self._classify_power_group, axis=1)
        power_density_mw_per_area: dict[str, float] = {}

        for group_name, subset in annotated.groupby("power_group", dropna=False):
            area_total = float(subset["area"].sum())
            total_power_mw = float(power_groups.get(group_name, {}).get("total_power_mw", 0.0))
            power_density_mw_per_area[group_name] = total_power_mw / area_total if area_total else 0.0

        annotated["estimated_power_mw"] = annotated.apply(
            lambda row: row["area"] * power_density_mw_per_area.get(row["power_group"], 0.0),
            axis=1,
        )
        return annotated

    def _ensure_module_node(
        self,
        nodes: dict[str, ModuleNode],
        path: str,
    ) -> ModuleNode:
        if path in nodes:
            return nodes[path]
        parent_path = self._parent_module_path(path)
        if parent_path is not None:
            parent = self._ensure_module_node(nodes, parent_path)
        else:
            parent = None
        node = ModuleNode(path=path, parent_path=parent_path)
        nodes[path] = node
        if parent is not None:
            parent.children.add(path)
        return node

    def _finalize_module_totals(
        self,
        nodes: dict[str, ModuleNode],
        path: str,
    ) -> None:
        node = nodes[path]
        total_area = node.direct_leaf_area
        total_power = node.direct_leaf_power_mw
        total_leaf_cell_count = node.direct_leaf_cell_count

        for child_path in sorted(node.children):
            self._finalize_module_totals(nodes, child_path)
            child = nodes[child_path]
            total_area += child.total_area
            total_power += child.total_estimated_power_mw
            total_leaf_cell_count += child.total_leaf_cell_count

        if total_area == 0.0 and node.reported_module_area is not None:
            total_area = node.reported_module_area
        if total_power == 0.0 and node.reported_module_power_mw is not None:
            total_power = node.reported_module_power_mw

        node.total_area = total_area
        node.total_estimated_power_mw = total_power
        node.total_leaf_cell_count = total_leaf_cell_count

    def _allocate_unexpanded_module_power(
        self,
        nodes: dict[str, ModuleNode],
        path: str,
    ) -> None:
        node = nodes[path]
        for child_path in sorted(node.children):
            self._allocate_unexpanded_module_power(nodes, child_path)

        for child_path in node.children:
            child = nodes[child_path]
            if child.allocated_total_power_mw == 0.0:
                child.allocated_total_power_mw = child.total_estimated_power_mw

        unexpanded_children = [
            nodes[child_path]
            for child_path in sorted(node.children)
            if not nodes[child_path].children
            and nodes[child_path].total_leaf_cell_count == 0
            and nodes[child_path].reported_module_area is not None
        ]

        shareable_power = node.direct_leaf_power_mw + sum(
            child.total_estimated_power_mw for child in unexpanded_children
        )
        shareable_area = node.direct_leaf_area + sum(child.total_area for child in unexpanded_children)

        if unexpanded_children and shareable_power > 0.0 and shareable_area > 0.0:
            node.allocated_direct_leaf_power_mw = shareable_power * node.direct_leaf_area / shareable_area
            for child in unexpanded_children:
                child.allocated_total_power_mw = shareable_power * child.total_area / shareable_area
        else:
            node.allocated_direct_leaf_power_mw = node.direct_leaf_power_mw

        node.allocated_total_power_mw = node.allocated_direct_leaf_power_mw + sum(
            nodes[child_path].allocated_total_power_mw for child_path in node.children
        )

    def _resolve_focus_module(
        self,
        nodes: dict[str, ModuleNode],
        design_name: str,
        focus_module: str | None,
    ) -> str:
        if focus_module is None or focus_module in {"", "/", ".", design_name}:
            return ""
        normalized = focus_module.strip().strip("/")
        if normalized not in nodes:
            available = ", ".join(
                self._display_module_path(path, design_name)
                for path in sorted(nodes)
                if path
            )
            raise KeyError(
                f"Unknown focus module: {focus_module}. Available module paths: {available}"
            )
        return normalized

    @staticmethod
    def _iter_parent_paths(path: str) -> list[str]:
        parts = [part for part in path.split("/") if part]
        return ["/".join(parts[:index]) for index in range(1, len(parts))]

    @staticmethod
    def _parent_module_path(path: str) -> str | None:
        if not path:
            return None
        if "/" not in path:
            return ""
        return path.rsplit("/", 1)[0]

    @staticmethod
    def _display_module_path(path: str, design_name: str) -> str:
        return design_name if path == "" else path

    @staticmethod
    def _module_name(path: str, design_name: str) -> str:
        if path == "":
            return design_name
        return path.rsplit("/", 1)[-1]

    @staticmethod
    def _classify_power_group(row: pd.Series) -> str:
        cell_name = str(row.get("cell", "") or "").upper()
        reference = str(row.get("reference", "") or "").upper()
        library = str(row.get("library", "") or "").upper()
        attrs = SynReportParser._attribute_tokens(row.get("attributes"))
        if "cg" in attrs or "SNPS_CLOCK_GATE" in cell_name or "SNPS_CLOCK_GATE" in reference:
            return "clock_network"
        if (
            "SRAM" in reference
            or "SRAM" in library
            or "MEMORY" in reference
            or "REGFILE" in reference
            or "DATASRAM" in reference
        ):
            return "memory"
        if "b" in attrs:
            return "black_box"
        if "n" in attrs or re.match(r"^(DF|SDFF|EDFF|LAT|LHQ|LHN)", reference):
            return "register"
        return "combinational"

    def _parse_cell_block(self, block: list[str]) -> dict[str, Any] | None:
        first_line = block[0].rstrip()
        first_match = re.match(r"^(\S+)(?:\s+(.*\S))?\s*$", first_line)
        if first_match is None:
            return None

        cell_name = first_match.group(1)
        fragments: list[str] = []
        first_rest = (first_match.group(2) or "").strip()
        if first_rest:
            fragments.append(first_rest)
        fragments.extend(line.strip() for line in block[1:] if line.strip())

        reference_tokens: list[str] = []
        library_tokens: list[str] = []
        attribute_fragments: list[str] = []
        area: float | None = None

        for fragment in fragments:
            before_tokens, area_value, attr_text = self._split_area_fragment(fragment)
            if area_value is not None:
                self._classify_fragment_tokens(before_tokens, reference_tokens, library_tokens)
                area = area_value
                if attr_text:
                    attribute_fragments.append(attr_text)
                continue
            if self._looks_like_attribute_fragment(fragment):
                attribute_fragments.append(fragment)
                continue
            self._classify_fragment_tokens(fragment.split(), reference_tokens, library_tokens)

        if area is None:
            return None

        reference = " ".join(reference_tokens).strip() or None
        library = " ".join(library_tokens).strip() or None
        attrs = ", ".join(part.strip() for part in attribute_fragments if part.strip()) or None
        return {
            "cell": cell_name,
            "reference": reference,
            "library": library,
            "area": area,
            "attributes": attrs,
        }

    @staticmethod
    def _split_area_fragment(fragment: str) -> tuple[list[str], float | None, str | None]:
        tokens = fragment.split()
        for index, token in enumerate(tokens):
            if NUMERIC_RE.match(token):
                before_tokens = tokens[:index]
                attr_text = " ".join(tokens[index + 1 :]).strip() or None
                return before_tokens, float(token), attr_text
        return tokens, None, None

    @staticmethod
    def _classify_fragment_tokens(
        tokens: list[str],
        reference_tokens: list[str],
        library_tokens: list[str],
    ) -> None:
        for token in tokens:
            if any(token.startswith(prefix) for prefix in LIBRARY_TOKEN_PREFIXES) or token.endswith((".db", ".lib")):
                library_tokens.append(token)
            else:
                reference_tokens.append(token)

    @staticmethod
    def _looks_like_attribute_fragment(fragment: str) -> bool:
        tokens = [token for token in re.split(r"[\s,]+", fragment.strip()) if token]
        return bool(tokens) and all(token.lower() in CELL_ATTR_CODES for token in tokens)

    @staticmethod
    def _attribute_tokens(attributes: str | None) -> set[str]:
        if not attributes:
            return set()
        return {
            token.strip().lower()
            for token in re.split(r"[\s,]+", str(attributes))
            if token.strip()
        }

    @staticmethod
    def _try_build_report_files(candidate: Path) -> ReportFiles | None:
        if not candidate.is_dir():
            return None

        area_files = sorted(candidate.glob("area_rpt_*.txt"))
        power_files = sorted(candidate.glob("power_rpt_*.txt"))
        cell_files = sorted(candidate.glob("cell_rpt_*.txt"))
        if len(area_files) != 1 or len(power_files) != 1 or len(cell_files) != 1:
            return None

        area_match = re.match(r"area_rpt_(.+)\.txt$", area_files[0].name)
        if area_match is None:
            return None
        design = area_match.group(1)

        timing_max = next(iter(sorted(candidate.glob(f"timing_max_rpt_{design}.txt"))), None)
        timing_min = next(iter(sorted(candidate.glob(f"timing_min_rpt_{design}.txt"))), None)
        return ReportFiles(
            report_dir=candidate,
            design=design,
            area=area_files[0],
            power=power_files[0],
            cell=cell_files[0],
            timing_max=timing_max,
            timing_min=timing_min,
        )

    @staticmethod
    def _frame_to_records(df: pd.DataFrame) -> list[dict[str, Any]]:
        records = df.to_dict(orient="records")
        for record in records:
            for key, value in list(record.items()):
                if pd.isna(value):
                    record[key] = None
        return records

    @staticmethod
    def _extract_optional_float(text: str, pattern: str) -> float | None:
        match = re.search(pattern, text, flags=re.MULTILINE)
        if not match:
            return None
        raw = match.group(1).strip()
        if raw == "undefined":
            return None
        return float(raw)

    @staticmethod
    def _extract_power_mw(text: str, pattern: str) -> float | None:
        match = re.search(pattern, text, flags=re.MULTILINE)
        if not match:
            return None
        return SynReportParser._convert_power_to_mw(match.group(1), match.group(2))

    @staticmethod
    def _convert_power_to_mw(value: str, unit: str) -> float:
        if unit not in POWER_TO_MW:
            raise ValueError(f"Unsupported power unit: {unit}")
        return float(value) * POWER_TO_MW[unit]

    @staticmethod
    def _extract_text(text: str, pattern: str) -> str | None:
        match = re.search(pattern, text, flags=re.MULTILINE)
        return match.group(1).strip() if match else None

    @staticmethod
    def _read_text(file_path: str | Path) -> str:
        return Path(file_path).read_text(encoding="utf-8", errors="ignore")