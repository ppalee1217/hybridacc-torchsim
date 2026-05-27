#!/usr/bin/env python3
"""Parse DC synthesis reports and generate a Markdown summary.

Reads report files from the report/ directory structure produced by
synthesis_pe_units.tcl and outputs a consolidated Markdown table.

Usage:
    python3 syn_report.py [--report-dir REPORT_DIR] [--output OUTPUT_FILE]
                          [--modules MOD1 MOD2 ...] [--all]

Examples:
    python3 syn_report.py --all
    python3 syn_report.py --modules VADDU VMULU LoopController
    python3 syn_report.py --report-dir ./report --output syn_summary.md
"""
import argparse
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class SynResult:
    module: str = ""
    # Timing
    clk_period: float = 0.0
    slack_max: float = 0.0       # setup slack (delay max)
    slack_min: float = 0.0       # hold slack  (delay min)
    slack_max_met: bool = False
    slack_min_met: bool = False
    critical_path_start: str = ""
    critical_path_end: str = ""
    # Area
    comb_area: float = 0.0
    noncomb_area: float = 0.0
    total_cell_area: float = 0.0
    num_cells: int = 0
    num_comb_cells: int = 0
    num_seq_cells: int = 0
    num_ports: int = 0
    # Power
    internal_power_mw: float = 0.0
    switching_power_mw: float = 0.0
    leakage_power_nw: float = 0.0
    total_dynamic_power_mw: float = 0.0
    # Status
    errors: list = field(default_factory=list)


def parse_timing_report(filepath: str, result: SynResult, is_max: bool = True):
    """Parse a DC timing report (max or min delay)."""
    if not os.path.isfile(filepath):
        result.errors.append(f"Missing: {os.path.basename(filepath)}")
        return

    text = Path(filepath).read_text()

    # Extract slack value and MET/VIOLATED
    m = re.search(r'slack\s+\((\w+)\)\s+([-\d.]+)', text)
    if m:
        met = m.group(1) == "MET"
        slack = float(m.group(2))
        if is_max:
            result.slack_max = slack
            result.slack_max_met = met
        else:
            result.slack_min = slack
            result.slack_min_met = met

    if is_max:
        # Extract critical path endpoints
        m_start = re.search(r'Startpoint:\s*(\S+)', text)
        m_end = re.search(r'Endpoint:\s*(\S+)', text)
        if m_start:
            result.critical_path_start = m_start.group(1)
        if m_end:
            result.critical_path_end = m_end.group(1)


def parse_area_report(filepath: str, result: SynResult):
    """Parse a DC area report."""
    if not os.path.isfile(filepath):
        result.errors.append(f"Missing: {os.path.basename(filepath)}")
        return

    text = Path(filepath).read_text()

    patterns = {
        'num_ports': r'Number of ports:\s+(\d+)',
        'num_cells': r'Number of cells:\s+(\d+)',
        'num_comb_cells': r'Number of combinational cells:\s+(\d+)',
        'num_seq_cells': r'Number of sequential cells:\s+(\d+)',
        'comb_area': r'Combinational area:\s+([\d.]+)',
        'noncomb_area': r'Noncombinational area:\s+([\d.]+)',
        'total_cell_area': r'Total cell area:\s+([\d.]+)',
    }

    for attr, pat in patterns.items():
        m = re.search(pat, text)
        if m:
            val = m.group(1)
            if attr.startswith('num_'):
                setattr(result, attr, int(val))
            else:
                setattr(result, attr, float(val))


def parse_power_report(filepath: str, result: SynResult):
    """Parse a DC power report."""
    if not os.path.isfile(filepath):
        result.errors.append(f"Missing: {os.path.basename(filepath)}")
        return

    text = Path(filepath).read_text()

    # Total Dynamic Power
    m = re.search(r'Total Dynamic Power\s+=\s+([\d.]+)\s+(\w+)', text)
    if m:
        val = float(m.group(1))
        unit = m.group(2)
        if unit == 'uW':
            val /= 1000.0
        elif unit == 'nW':
            val /= 1e6
        result.total_dynamic_power_mw = val

    # Cell Internal Power
    m = re.search(r'Cell Internal Power\s+=\s+([\d.]+)\s+(\w+)', text)
    if m:
        val = float(m.group(1))
        unit = m.group(2)
        if unit == 'uW':
            val /= 1000.0
        elif unit == 'nW':
            val /= 1e6
        result.internal_power_mw = val

    # Net Switching Power
    m = re.search(r'Net Switching Power\s+=\s+([\d.]+)\s+(\w+)', text)
    if m:
        val = float(m.group(1))
        unit = m.group(2)
        if unit == 'uW':
            val /= 1000.0
        elif unit == 'nW':
            val /= 1e6
        result.switching_power_mw = val

    # Cell Leakage Power (always nW)
    m = re.search(r'Cell Leakage Power\s+=\s+([\d.]+)\s+(\w+)', text)
    if m:
        result.leakage_power_nw = float(m.group(1))

    # Total line at bottom: Internal  Switching  Leakage  Total
    m = re.search(
        r'^Total\s+([\d.e+-]+)\s+mW\s+([\d.e+-]+)\s+mW\s+([\d.e+-]+)\s+nW\s+([\d.e+-]+)\s+mW',
        text, re.MULTILINE
    )
    if m:
        result.internal_power_mw = float(m.group(1))
        result.switching_power_mw = float(m.group(2))
        result.leakage_power_nw = float(m.group(3))
        result.total_dynamic_power_mw = float(m.group(4))


def parse_compile_log(filepath: str, result: SynResult):
    """Extract clock period from synthesis compile log."""
    if not os.path.isfile(filepath):
        return

    text = Path(filepath).read_text()

    # Look for create_clock in the log
    m = re.search(r'create_clock.*-period\s+([\d.]+)', text)
    if m:
        result.clk_period = float(m.group(1))
        return

    # Fallback: set clk_period
    m = re.search(r'set clk_period\s+([\d.]+)', text)
    if m:
        result.clk_period = float(m.group(1))


def parse_sdc(sdc_path: str) -> float:
    """Extract clock period from SDC file."""
    if not os.path.isfile(sdc_path):
        return 0.0
    text = Path(sdc_path).read_text()
    m = re.search(r'set\s+clk_period\s+([\d.]+)', text)
    if m:
        return float(m.group(1))
    m = re.search(r'create_clock.*-period\s+([\d.]+)', text)
    if m:
        return float(m.group(1))
    return 0.0


def parse_module(report_dir: str, mod: str, sdc_clk: float = 0.0) -> SynResult:
    """Parse all reports for a given module."""
    result = SynResult(module=mod)
    mod_dir = os.path.join(report_dir, mod)

    if not os.path.isdir(mod_dir):
        result.errors.append(f"Report directory not found: {mod_dir}")
        return result

    parse_timing_report(
        os.path.join(mod_dir, f"timing_max_rpt_{mod}.txt"), result, is_max=True
    )
    parse_timing_report(
        os.path.join(mod_dir, f"timing_min_rpt_{mod}.txt"), result, is_max=False
    )
    parse_area_report(os.path.join(mod_dir, f"area_rpt_{mod}.txt"), result)
    parse_power_report(os.path.join(mod_dir, f"power_rpt_{mod}.txt"), result)
    parse_compile_log(os.path.join(mod_dir, f"syn_compile_{mod}.log"), result)

    # Fallback to SDC clock period
    if result.clk_period == 0.0 and sdc_clk > 0:
        result.clk_period = sdc_clk

    return result


def format_slack(val: float, met: bool) -> str:
    """Format slack value with pass/fail indicator."""
    icon = "✅" if met else "❌"
    return f"{val:+.4f} {icon}"


def generate_markdown(results: list, output_path: str = None) -> str:
    """Generate consolidated Markdown report organized by hierarchy level."""
    lines = []
    lines.append("# Synthesis Report")
    lines.append("")
    lines.append(f"**Generated**: {__import__('datetime').datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append(f"**Technology**: N16ADFP (16nm FinFET)")
    lines.append(f"**Tool**: Synopsys Design Compiler W-2024.09-SP2")
    if results and results[0].clk_period > 0:
        freq = 1000.0 / results[0].clk_period
        lines.append(f"**Clock Period**: {results[0].clk_period} ns ({freq:.0f} MHz)")
    lines.append("")

    # === Overall Summary ===
    valid = [r for r in results if not r.errors]
    total_area = sum(r.total_cell_area for r in valid)
    total_power = sum(r.total_dynamic_power_mw for r in valid)
    all_met = all(r.slack_max_met and r.slack_min_met for r in valid)

    lines.append("## Overall Summary")
    lines.append("")
    lines.append(f"- **Modules Synthesized**: {len(valid)} / {len(results)}")
    lines.append(f"- **Total Cell Area**: {total_area:.2f}")
    lines.append(f"- **Total Dynamic Power**: {total_power:.4f} mW")
    lines.append(f"- **All Timing Met**: {'✅ Yes' if all_met else '❌ No'}")
    lines.append("")

    # Group by hierarchy
    grouped = {}
    for r in results:
        level = classify_module(r.module)
        grouped.setdefault(level, []).append(r)

    level_order = ["PE Unit", "PE Integration", "NoC Unit", "NoC Integration",
                   "Cluster", "Core", "Other"]

    # === Per-level summary tables ===
    global_idx = 0
    for level in level_order:
        level_results = grouped.get(level, [])
        if not level_results:
            continue

        level_valid = [r for r in level_results if not r.errors]
        level_met = all(r.slack_max_met and r.slack_min_met for r in level_valid) if level_valid else False
        level_icon = "✅" if level_met else "❌"
        level_area = sum(r.total_cell_area for r in level_valid)
        level_power = sum(r.total_dynamic_power_mw for r in level_valid)

        lines.append(f"## {level} {level_icon}")
        lines.append("")
        lines.append(f"**Area**: {level_area:.2f} | **Power**: {level_power:.4f} mW")
        lines.append("")

        lines.append(
            "| # | Module | Type | Total Area | Comb Area | Seq Area | "
            "Setup Slack | Hold Slack | Power (mW) | Leakage (nW) |"
        )
        lines.append(
            "|---|--------|------|----------:|---------:|--------:|"
            "-----------:|----------:|----------:|-------------:|"
        )

        for r in level_results:
            global_idx += 1
            mod_type = "Comb" if r.module in COMBINATIONAL_MODULES else "Seq"
            if r.errors:
                lines.append(
                    f"| {global_idx} | {r.module} | {mod_type} "
                    f"| — | — | — | — | — | — | — |"
                )
                continue

            setup = format_slack(r.slack_max, r.slack_max_met)
            hold = format_slack(r.slack_min, r.slack_min_met)
            lines.append(
                f"| {global_idx} | {r.module} "
                f"| {mod_type} "
                f"| {r.total_cell_area:.2f} "
                f"| {r.comb_area:.2f} "
                f"| {r.noncomb_area:.2f} "
                f"| {setup} "
                f"| {hold} "
                f"| {r.total_dynamic_power_mw:.4f} "
                f"| {r.leakage_power_nw:.2f} |"
            )

        lines.append("")

    # === Detailed per-module sections (unified at the end) ===
    lines.append("## Detailed Results")
    lines.append("")

    for level in level_order:
        level_results = grouped.get(level, [])
        if not level_results:
            continue

        lines.append(f"### {level}")
        lines.append("")

        for r in level_results:
            is_comb = r.module in COMBINATIONAL_MODULES
            mod_type = "Combinational" if is_comb else "Sequential"
            clk_type = "Virtual (vclk)" if is_comb else "Physical (clk)"
            lines.append(f"#### {r.module}")
            lines.append("")

            if r.errors:
                for e in r.errors:
                    lines.append(f"- ⚠️ {e}")
                lines.append("")
                continue

            lines.append("| Metric | Value |")
            lines.append("|--------|-------|")
            lines.append(f"| Module Type | {mod_type} |")
            lines.append(f"| Clock Type | {clk_type} |")
            lines.append(f"| Clock Period | {r.clk_period} ns |")
            freq_mhz = 1000.0 / r.clk_period if r.clk_period > 0 else 0
            lines.append(f"| Target Frequency | {freq_mhz:.0f} MHz |")
            lines.append(f"| Setup Slack (max delay) | {format_slack(r.slack_max, r.slack_max_met)} |")
            lines.append(f"| Hold Slack (min delay) | {format_slack(r.slack_min, r.slack_min_met)} |")
            lines.append(f"| Critical Path | {r.critical_path_start} → {r.critical_path_end} |")
            lines.append(f"| Total Cell Area | {r.total_cell_area:.2f} |")
            lines.append(f"| Combinational Area | {r.comb_area:.2f} |")
            lines.append(f"| Non-combinational Area | {r.noncomb_area:.2f} |")
            lines.append(f"| # Cells | {r.num_cells} |")
            lines.append(f"| # Combinational | {r.num_comb_cells} |")
            lines.append(f"| # Sequential | {r.num_seq_cells} |")
            lines.append(f"| # Ports | {r.num_ports} |")
            lines.append(f"| Internal Power | {r.internal_power_mw:.4f} mW |")
            lines.append(f"| Switching Power | {r.switching_power_mw:.4f} mW |")
            lines.append(f"| Total Dynamic Power | {r.total_dynamic_power_mw:.4f} mW |")
            lines.append(f"| Leakage Power | {r.leakage_power_nw:.2f} nW |")
            lines.append("")

    md = "\n".join(lines) + "\n"

    if output_path:
        Path(output_path).parent.mkdir(parents=True, exist_ok=True)
        Path(output_path).write_text(md)
        print(f"Report written to {output_path}")

    return md


# =========================================================================
# Hierarchy classification
# =========================================================================
MODULE_HIERARCHY = {
    "PE Unit": [
        "DataMemory", "InstructionMemory", "LoopController",
        "Decoder", "PsumRegFile", "TransformRegFile",
        "VADDU", "VMULU", "LDMA", "SDMA",
        "IF_ID_Stage", "EXE_M_Stage", "EXE_A_Stage",
        "PErouter",
    ],
    "PE Integration": [
        "ProcessElement",
    ],
    "NoC Unit": [
        "MBUS", "NoCRouter",
    ],
    "NoC Integration": [
        "NetworkOnChip",
    ],
    "Cluster": [],
    "Core": [],
}

_MOD_LEVEL = {}
for _level, _mods in MODULE_HIERARCHY.items():
    for _mod in _mods:
        _MOD_LEVEL[_mod] = _level


def classify_module(mod_name: str) -> str:
    return _MOD_LEVEL.get(mod_name, "Other")


# Default module list (leaf → hierarchical order)
ALL_MODULES = [
    "DataMemory", "InstructionMemory", "LoopController",
    "Decoder", "PsumRegFile", "TransformRegFile",
    "VADDU", "VMULU", "LDMA", "SDMA",
    "IF_ID_Stage", "EXE_M_Stage", "EXE_A_Stage",
    "PErouter", "ProcessElement",
    "MBUS", "NoCRouter", "NetworkOnChip",
]

# Purely combinational modules (no physical clock)
COMBINATIONAL_MODULES = {"Decoder", "VMULU", "VADDU"}


def main():
    parser = argparse.ArgumentParser(
        description="Parse DC synthesis reports and generate Markdown summary."
    )
    parser.add_argument(
        "--report-dir", default="./report",
        help="Path to report directory (default: ./report)"
    )
    parser.add_argument(
        "--output", "-o", default=None,
        help="Output Markdown file (default: report/synthesis_report_<timestamp>.md)"
    )
    parser.add_argument(
        "--modules", nargs="+",
        help="Specific modules to include (default: auto-detect from report-dir)"
    )
    parser.add_argument(
        "--all", action="store_true",
        help="Include all ALL_MODULES even if reports don't exist yet"
    )
    parser.add_argument(
        "--sdc", default=None,
        help="Path to SDC file to extract clock period (default: auto-detect)"
    )
    args = parser.parse_args()

    report_dir = args.report_dir

    # Determine clock period from SDC
    sdc_path = args.sdc
    if sdc_path is None:
        # Try common locations relative to report_dir
        for candidate in [
            os.path.join(os.path.dirname(report_dir), "script", "DC.sdc"),
            os.path.join(report_dir, "..", "script", "DC.sdc"),
        ]:
            if os.path.isfile(candidate):
                sdc_path = candidate
                break
    sdc_clk = parse_sdc(sdc_path) if sdc_path else 0.0

    if args.modules:
        modules = args.modules
    elif args.all:
        modules = ALL_MODULES
    else:
        # Auto-detect: find subdirectories that have timing reports
        modules = []
        if os.path.isdir(report_dir):
            for d in sorted(os.listdir(report_dir)):
                subdir = os.path.join(report_dir, d)
                if os.path.isdir(subdir) and any(
                    f.startswith("timing_max_rpt_") for f in os.listdir(subdir)
                ):
                    modules.append(d)

    if not modules:
        print("No synthesis reports found. Run synthesis first.", file=sys.stderr)
        sys.exit(1)

    print(f"Parsing {len(modules)} module(s): {', '.join(modules)}")
    results = [parse_module(report_dir, mod, sdc_clk) for mod in modules]

    output_path = args.output
    if output_path is None:
        from datetime import datetime
        ts = datetime.now().strftime('%Y_%m_%d_%H_%M_%S')
        output_path = f"report/synthesis_report_{ts}.md"

    md = generate_markdown(results, output_path)
    print(md)


if __name__ == "__main__":
    main()
