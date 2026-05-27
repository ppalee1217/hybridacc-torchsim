#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import re
import shutil
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

from jinja2 import Template

import analyze_signoff_reports as analysis


@dataclass
class SynthesisArea:
    total_cell_area: float = 0.0
    combinational_area: float = 0.0
    noncombinational_area: float = 0.0
    macro_area: float = 0.0
    synthetic_area: float = 0.0
    number_of_cells: int = 0
    number_of_comb_cells: int = 0
    number_of_seq_cells: int = 0
    number_of_macros: int = 0
    number_of_buf_inv: int = 0


def parse_float(text: str, pattern: str) -> float:
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        return 0.0
    return float(match.group(1).replace(",", ""))


def parse_int(text: str, pattern: str) -> int:
    match = re.search(pattern, text, re.MULTILINE)
    if not match:
        return 0
    return int(match.group(1).replace(",", ""))


def parse_area_report(path: Path) -> SynthesisArea:
    text = analysis.read_text(path)
    if not text:
        return SynthesisArea()

    return SynthesisArea(
        total_cell_area=parse_float(text, r"^Total cell area:\s+([\d.eE+-]+)"),
        combinational_area=parse_float(text, r"^Combinational area:\s+([\d.eE+-]+)"),
        noncombinational_area=parse_float(text, r"^Noncombinational area:\s+([\d.eE+-]+)"),
        macro_area=parse_float(text, r"^Macro/Black Box area:\s+([\d.eE+-]+)"),
        synthetic_area=parse_float(text, r"^Total synthetic cell area:\s+([\d.eE+-]+)"),
        number_of_cells=parse_int(text, r"^Number of cells:\s+([\d,]+)"),
        number_of_comb_cells=parse_int(text, r"^Number of combinational cells:\s+([\d,]+)"),
        number_of_seq_cells=parse_int(text, r"^Number of sequential cells:\s+([\d,]+)"),
        number_of_macros=parse_int(text, r"^Number of macros/black boxes:\s+([\d,]+)"),
        number_of_buf_inv=parse_int(text, r"^Number of buf/inv:\s+([\d,]+)"),
    )


def fmt_area(value: float) -> str:
    return f"{value:,.3f}"


def fmt_count(value: int) -> str:
    return f"{value:,}"


def fmt_ns(value: float) -> str:
    return f"{value:+.4f} ns"


def report_has_no_negative_paths(path: Path) -> bool:
    return "No paths with slack less than 0.00." in analysis.read_text(path)


def timing_display(path: Path, point: analysis.TimingPoint, clean_label: str) -> str:
    if report_has_no_negative_paths(path):
        return clean_label
    return analysis.fmt_slack(point)


def timing_tone(path: Path, point: analysis.TimingPoint) -> str:
    if report_has_no_negative_paths(path):
        return "good"
    if abs(point.slack) < 0.0001 and "increase significant digits" in point.status:
        return "warn"
    return "good" if point.met else "bad"


def embed_chart_images(chart_paths: list[Path]) -> list[dict[str, str]]:
    figures: list[dict[str, str]] = []
    for chart_path in chart_paths:
        image_bytes = chart_path.read_bytes()
        encoded = base64.b64encode(image_bytes).decode("ascii")
        figures.append(
            {
                "title": chart_path.stem.replace("_", " ").title(),
                "file": chart_path.name,
                "src": f"data:image/png;base64,{encoded}",
            }
        )
    return figures


def reuse_existing_chart(existing_path: Path, output_dir: Path) -> list[Path]:
  if not existing_path.exists():
    return []

  target_path = output_dir / existing_path.name
  if existing_path.resolve() != target_path.resolve():
    shutil.copy2(existing_path, target_path)

  return [target_path]


def parse_constraint_counts(path: Path) -> tuple[int, int]:
    text = analysis.read_text(path)
    if not text:
        return (0, 0)

    hold_match = re.search(r"min_delay/hold.*?(?=\n\n\s*min_capacitance|\Z)", text, re.DOTALL)
    min_cap_match = re.search(r"min_capacitance.*", text, re.DOTALL)
    hold_count = hold_match.group(0).count("increase significant digits") if hold_match else 0
    min_cap_count = min_cap_match.group(0).count("increase significant digits") if min_cap_match else 0
    return hold_count, min_cap_count


def parse_unique_startpoints(path: Path, limit: int = 4) -> list[str]:
    text = analysis.read_text(path)
    items: list[str] = []
    seen: set[str] = set()
    for match in re.finditer(r"Startpoint:\s*(\S+)", text):
        startpoint = match.group(1)
        if startpoint in seen:
            continue
        seen.add(startpoint)
        items.append(startpoint)
        if len(items) >= limit:
            break
    return items


def summarize_residual_sta(constraint_dir: Path, timing_min_path: Path) -> list[str]:
    hold_count, min_cap_count = parse_constraint_counts(constraint_dir / "constraint_violators.rpt")
    startpoints = parse_unique_startpoints(timing_min_path)
    startpoint_text = ", ".join(startpoints) if startpoints else "top-level synchronous control inputs"

    return [
        f"Waiver candidate: {hold_count} residual hold rows all print as -0.00 with 'increase significant digits' and are boundary min paths from {startpoint_text}. Treat them as standalone top-interface artifacts if chip-level min input delay is not defined here.",
        "Fix candidate for the same hold rows: if this block must be signoff-clean as an SoC boundary, add explicit set_input_delay -min constraints for the host-control ports or register/synchronize that boundary in the integration wrapper instead of waiving them at block level.",
        f"Waiver candidate: {min_cap_count} min_capacitance rows report required=0.00, actual=0.00, slack=0.00 with the same precision warning, which is consistent with report-resolution noise rather than a finite electrical violation.",
        "Fix candidate for min_capacitance only if a higher-precision report shows a real non-zero requirement shortfall; then address it with library-approved buffering or by revisiting the min-cap rule, not by changing the timing clock constraints.",
    ]


def top_hierarchy_rows(entries: list[analysis.HierarchyPower], limit: int = 8) -> list[list[str]]:
    top_entries = sorted(entries[1:] if len(entries) > 1 else entries, key=lambda entry: entry.total_mw, reverse=True)[:limit]
    rows = []
    for entry in top_entries:
        rows.append(
            [
                entry.name,
                analysis.fmt_mw(entry.total_mw),
                f"{entry.percent:.2f}%" if entry.percent is not None else "-",
            ]
        )
    return rows


def render_dashboard(context: dict) -> str:
    template = Template(
        """
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{{ title }}</title>
  <style>
    :root {
      --bg: #f5efe4;
      --bg-accent: #e9dfcf;
      --panel: rgba(255, 252, 246, 0.9);
      --panel-strong: #fffaf2;
      --ink: #1d2733;
      --muted: #556170;
      --line: rgba(114, 97, 74, 0.18);
      --teal: #0f766e;
      --amber: #b45309;
      --red: #b91c1c;
      --shadow: 0 18px 50px rgba(68, 52, 35, 0.12);
    }

    * { box-sizing: border-box; }
    body {
      margin: 0;
      color: var(--ink);
      background:
        radial-gradient(circle at top right, rgba(15, 118, 110, 0.12), transparent 28%),
        radial-gradient(circle at left 20%, rgba(180, 83, 9, 0.12), transparent 24%),
        linear-gradient(180deg, var(--bg) 0%, #f8f4ec 48%, #f2ebdd 100%);
      font-family: "Iosevka Term", "JetBrains Mono", "IBM Plex Mono", "Cascadia Mono", "SFMono-Regular", Consolas, "Liberation Mono", monospace;
      font-variant-numeric: tabular-nums;
      line-height: 1.55;
    }

    .shell {
      max-width: 1040px;
      margin: 0 auto;
      padding: 36px 28px 52px;
    }

    .hero {
      padding: 34px;
      border: 1px solid var(--line);
      border-radius: 28px;
      background: linear-gradient(135deg, rgba(255, 250, 242, 0.94), rgba(250, 244, 233, 0.9));
      box-shadow: var(--shadow);
      overflow: hidden;
      position: relative;
    }

    .hero::after {
      content: "";
      position: absolute;
      inset: auto -80px -120px auto;
      width: 260px;
      height: 260px;
      border-radius: 50%;
      background: radial-gradient(circle, rgba(15, 118, 110, 0.12), transparent 70%);
    }

    .eyebrow {
      display: inline-block;
      margin-bottom: 10px;
      padding: 6px 12px;
      border-radius: 999px;
      background: rgba(15, 118, 110, 0.09);
      color: var(--teal);
      letter-spacing: 0.08em;
      text-transform: uppercase;
      font-size: 12px;
      font-weight: 700;
    }

    h1 {
      margin: 0 0 10px;
      font-size: clamp(32px, 5vw, 54px);
      line-height: 1.03;
    }

    .hero p {
      margin: 0;
      max-width: 920px;
      color: var(--muted);
      font-size: 18px;
    }

    .meta-grid,
    .card-grid,
    .section-grid,
    .figure-grid {
      display: grid;
      gap: 18px;
      grid-template-columns: 1fr;
    }

    .meta-grid {
      margin-top: 22px;
    }

    .card-grid {
      margin-top: 24px;
    }

    .section-grid {
      margin-top: 24px;
      align-items: start;
    }

    .chip,
    .card,
    .panel,
    .figure {
      border: 1px solid var(--line);
      background: var(--panel);
      border-radius: 22px;
      box-shadow: var(--shadow);
      backdrop-filter: blur(10px);
      min-width: 0;
    }

    .chip {
      padding: 16px 18px;
    }

    .chip .label,
    .card .label {
      display: block;
      margin-bottom: 6px;
      color: var(--muted);
      font-size: 12px;
      letter-spacing: 0.06em;
      text-transform: uppercase;
      font-weight: 700;
    }

    .chip .value,
    .card .value {
      font-size: 22px;
      font-weight: 700;
      word-break: break-word;
    }

    .card {
      padding: 22px;
    }

    .card.good .value { color: var(--teal); }
    .card.warn .value { color: var(--amber); }
    .card.bad .value { color: var(--red); }

    .panel {
      padding: 24px;
    }

    h2 {
      margin: 0 0 12px;
      font-size: 26px;
    }

    h3 {
      margin: 0 0 10px;
      font-size: 18px;
    }

    table {
      width: 100%;
      border-collapse: collapse;
      margin-top: 8px;
      font-size: 15px;
      background: var(--panel-strong);
      border-radius: 14px;
      overflow: hidden;
      table-layout: fixed;
    }

    th, td {
      padding: 12px 14px;
      border-bottom: 1px solid var(--line);
      text-align: left;
      vertical-align: top;
      overflow-wrap: anywhere;
      word-break: break-word;
    }

    th {
      color: var(--muted);
      font-size: 12px;
      letter-spacing: 0.06em;
      text-transform: uppercase;
      background: rgba(114, 97, 74, 0.06);
    }

    tr:last-child td { border-bottom: none; }

    ul {
      margin: 10px 0 0;
      padding-left: 18px;
    }

    li + li {
      margin-top: 8px;
    }

    .figure {
      padding: 16px;
    }

    .figure img {
      display: block;
      width: 100%;
      border-radius: 14px;
      border: 1px solid var(--line);
      background: white;
    }

    .figure-title {
      margin: 0 0 10px;
      font-size: 16px;
      font-weight: 700;
    }

    .mono {
      font-family: "SFMono-Regular", Consolas, "Liberation Mono", monospace;
      font-size: 13px;
      overflow-wrap: anywhere;
      word-break: break-all;
    }

    @media (max-width: 760px) {
      .shell {
        padding: 18px 14px 32px;
      }

      .hero,
      .panel,
      .card,
      .chip,
      .figure {
        border-radius: 18px;
      }

    }
  </style>
</head>
<body>
  <div class="shell">
    <section class="hero">
      <span class="eyebrow">HybridAcc Signoff</span>
      <h1>{{ title }}</h1>
      <p>{{ subtitle }}</p>
      <div class="meta-grid">
        {% for item in meta_rows %}
        <div class="chip">
          <span class="label">{{ item.label }}</span>
          <span class="value">{{ item.value }}</span>
        </div>
        {% endfor %}
      </div>
    </section>

    <section class="card-grid">
      {% for card in overview_cards %}
      <article class="card {{ card.tone }}">
        <span class="label">{{ card.label }}</span>
        <div class="value">{{ card.value }}</div>
      </article>
      {% endfor %}
    </section>

    <section class="section-grid">
      <article class="panel">
        <h2>Synthesis Snapshot</h2>
        <table>
          <thead>
            <tr><th>Metric</th><th>Value</th></tr>
          </thead>
          <tbody>
            {% for row in synthesis_rows %}
            <tr><td>{{ row[0] }}</td><td>{{ row[1] }}</td></tr>
            {% endfor %}
          </tbody>
        </table>
      </article>

      <article class="panel">
        <h2>PrimeTime Snapshot</h2>
        <table>
          <thead>
            <tr><th>Metric</th><th>Value</th></tr>
          </thead>
          <tbody>
            {% for row in primetime_rows %}
            <tr><td>{{ row[0] }}</td><td>{{ row[1] }}</td></tr>
            {% endfor %}
          </tbody>
        </table>
      </article>

      <article class="panel">
        <h2>PrimePower Snapshot</h2>
        <table>
          <thead>
            <tr><th>Metric</th><th>Value</th></tr>
          </thead>
          <tbody>
            {% for row in primepower_rows %}
            <tr><td>{{ row[0] }}</td><td>{{ row[1] }}</td></tr>
            {% endfor %}
          </tbody>
        </table>
      </article>

      <article class="panel">
        <h2>Top PrimePower Hierarchy</h2>
        <table>
          <thead>
            <tr><th>Hierarchy</th><th>Total</th><th>Share</th></tr>
          </thead>
          <tbody>
            {% for row in hierarchy_rows %}
            <tr><td>{{ row[0] }}</td><td>{{ row[1] }}</td><td>{{ row[2] }}</td></tr>
            {% endfor %}
          </tbody>
        </table>
      </article>
    </section>

    <section class="section-grid">
      <article class="panel">
        <h2>Residual STA Disposition</h2>
        <ul>
          {% for item in residual_rows %}
          <li>{{ item }}</li>
          {% endfor %}
        </ul>
      </article>

      <article class="panel">
        <h2>Evidence Excerpts</h2>
        <h3>Analysis Coverage</h3>
        <ul>
          {% for item in coverage_excerpt %}
          <li>{{ item }}</li>
          {% endfor %}
        </ul>
        <h3>Constraint Violators</h3>
        <ul>
          {% for item in constraint_excerpt %}
          <li>{{ item }}</li>
          {% endfor %}
        </ul>
        <h3>Unannotated Activity</h3>
        <ul>
          {% for item in unannotated_excerpt %}
          <li>{{ item }}</li>
          {% endfor %}
        </ul>
      </article>
    </section>

    <section class="panel" style="margin-top: 24px;">
      <h2>Raw Report Anchors</h2>
      <table>
        <thead>
          <tr><th>Item</th><th>Path</th></tr>
        </thead>
        <tbody>
          {% for row in path_rows %}
          <tr><td>{{ row[0] }}</td><td class="mono">{{ row[1] }}</td></tr>
          {% endfor %}
        </tbody>
      </table>
    </section>

    <section class="panel" style="margin-top: 24px;">
      <h2>Figures</h2>
      <div class="figure-grid">
        {% for figure in figures %}
        <figure class="figure">
          <div class="figure-title">{{ figure.title }}</div>
          <img src="{{ figure.src }}" alt="{{ figure.title }}">
        </figure>
        {% endfor %}
      </div>
    </section>
  </div>
</body>
</html>
        """
    )
    return template.render(**context)


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a static signoff dashboard from synthesis, PrimeTime, and PrimePower reports.")
    parser.add_argument("--title", default="HybridAcc Static Signoff Dashboard")
    parser.add_argument("--clock-period", required=True, help="Clock period label shown in the dashboard, for example 2.00 ns.")
    parser.add_argument("--synthesis-report-dir", required=True)
    parser.add_argument("--primetime-report-dir", required=True)
    parser.add_argument("--primepower-report-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    args = parser.parse_args()

    synth_dir = Path(args.synthesis_report_dir).resolve()
    primetime_dir = Path(args.primetime_report_dir).resolve()
    primepower_dir = Path(args.primepower_report_dir).resolve()
    output_dir = Path(args.output_dir).resolve()
    analysis.ensure_dir(output_dir)

    synth_power = analysis.parse_power_report(synth_dir / "power_rpt_HybridAcc.txt")
    synth_area = parse_area_report(synth_dir / "area_rpt_HybridAcc.txt")
    synth_max = analysis.parse_timing_report(synth_dir / "timing_max_rpt_HybridAcc.txt")
    synth_min = analysis.parse_timing_report(synth_dir / "timing_min_rpt_HybridAcc.txt")

    pt_max = analysis.parse_timing_report(primetime_dir / "timing_max.rpt")
    pt_min = analysis.parse_timing_report(primetime_dir / "timing_min.rpt")
    pt_max_path = primetime_dir / "timing_max.rpt"
    pt_min_path = primetime_dir / "timing_min.rpt"
    pt_charts = analysis.create_primetime_delay_chart(output_dir, pt_max)
    if not pt_charts:
      pt_charts = reuse_existing_chart(primetime_dir / "analysis" / "primetime_sta_delay_profile.png", output_dir)

    pp_power = analysis.parse_power_report(primepower_dir / "power_summary.rpt")
    hierarchy_entries = analysis.parse_hierarchy_report(primepower_dir / "power_hierarchy.rpt")
    pp_charts = analysis.create_primepower_charts(output_dir, synth_power, pp_power, hierarchy_entries)

    hold_count, min_cap_count = parse_constraint_counts(primetime_dir / "constraint_violators.rpt")
    figures = embed_chart_images(pt_charts + pp_charts)

    context = {
        "title": args.title,
        "subtitle": "One static view that rolls synthesis baseline, PrimeTime STA, PrimePower activity-based power, and residual signoff disposition into a single review surface.",
        "meta_rows": [
            {"label": "Generated", "value": datetime.now().strftime("%Y-%m-%d %H:%M:%S")},
            {"label": "Clock Period", "value": args.clock_period},
            {"label": "PrimeTime Tag", "value": primetime_dir.name},
            {"label": "PrimePower Tag", "value": primepower_dir.name},
        ],
        "overview_cards": [
            {"label": "PrimeTime Max Slack", "value": timing_display(pt_max_path, pt_max, "No paths < 0.00 ns"), "tone": timing_tone(pt_max_path, pt_max)},
            {"label": "PrimeTime Min Slack", "value": timing_display(pt_min_path, pt_min, "No paths < 0.00 ns"), "tone": timing_tone(pt_min_path, pt_min)},
            {"label": "PrimePower Total", "value": analysis.fmt_mw(pp_power.total_mw), "tone": "warn"},
            {"label": "Synthesis Cell Area", "value": fmt_area(synth_area.total_cell_area), "tone": "warn"},
            {"label": "Residual Hold Rows", "value": str(hold_count), "tone": "warn"},
            {"label": "Residual Min-Cap Rows", "value": str(min_cap_count), "tone": "warn"},
        ],
        "synthesis_rows": [
            ["Total cell area", fmt_area(synth_area.total_cell_area)],
            ["Combinational area", fmt_area(synth_area.combinational_area)],
            ["Noncombinational area", fmt_area(synth_area.noncombinational_area)],
            ["Macro / black-box area", fmt_area(synth_area.macro_area)],
            ["Synthetic cell area", fmt_area(synth_area.synthetic_area)],
            ["Number of cells", fmt_count(synth_area.number_of_cells)],
            ["Sequential cells", fmt_count(synth_area.number_of_seq_cells)],
            ["Buf / inv cells", fmt_count(synth_area.number_of_buf_inv)],
            ["Synthesis total power", analysis.fmt_mw(synth_power.total_mw)],
        ],
        "primetime_rows": [
            ["Worst max slack", timing_display(pt_max_path, pt_max, "No paths < 0.00 ns")],
            ["Worst min slack", timing_display(pt_min_path, pt_min, "No paths < 0.00 ns")],
            ["Setup delta vs synthesis", fmt_ns(pt_max.slack - synth_max.slack)],
            ["Hold delta vs synthesis", fmt_ns(pt_min.slack - synth_min.slack)],
            ["Worst max path group", pt_max.path_group or "n/a"],
            ["Worst max path", f"{pt_max.startpoint} -> {pt_max.endpoint}"],
            ["Worst min path", f"{pt_min.startpoint} -> {pt_min.endpoint}"],
            ["Max arrival / required", f"{pt_max.arrival_time:.4f} ns / {pt_max.required_time:.4f} ns"],
        ],
        "primepower_rows": [
            ["PrimePower total", analysis.fmt_mw(pp_power.total_mw)],
            ["Synthesis baseline total", analysis.fmt_mw(synth_power.total_mw)],
            ["Total power delta", analysis.fmt_mw(pp_power.total_mw - synth_power.total_mw)],
            ["Internal delta", analysis.fmt_mw(pp_power.internal_mw - synth_power.internal_mw)],
            ["Switching delta", analysis.fmt_mw(pp_power.switching_mw - synth_power.switching_mw)],
            ["Leakage delta", analysis.fmt_mw(pp_power.leakage_mw - synth_power.leakage_mw)],
            ["Unannotated activity lines", str(analysis.count_relevant_lines(primepower_dir / "unannotated_activity.rpt"))],
        ],
        "hierarchy_rows": top_hierarchy_rows(hierarchy_entries),
        "residual_rows": summarize_residual_sta(primetime_dir.parent, primetime_dir / "timing_min.rpt") if primetime_dir.name == "analysis" else summarize_residual_sta(primetime_dir, primetime_dir / "timing_min.rpt"),
        "coverage_excerpt": analysis.excerpt_lines(primetime_dir / "analysis_coverage.rpt") or ["No analysis_coverage.rpt content found."],
        "constraint_excerpt": analysis.excerpt_lines(primetime_dir / "constraint_violators.rpt") or ["No constraint_violators.rpt content found."],
        "unannotated_excerpt": analysis.excerpt_lines(primepower_dir / "unannotated_activity.rpt") or ["No unannotated_activity.rpt content found."],
        "path_rows": [
            ["Synthesis report dir", str(synth_dir)],
            ["PrimeTime report dir", str(primetime_dir)],
            ["PrimePower report dir", str(primepower_dir)],
            ["Dashboard output dir", str(output_dir)],
        ],
        "figures": figures,
    }

    (output_dir / "index.html").write_text(render_dashboard(context), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())