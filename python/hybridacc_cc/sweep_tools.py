from __future__ import annotations

import argparse
import base64
import json
import math
from dataclasses import dataclass
from datetime import datetime, timezone
from html import escape
from io import BytesIO
from pathlib import Path
from typing import Any

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
from matplotlib import cm
import pandas as pd
import yaml


_REPO_ROOT = Path(__file__).resolve().parents[2]
_DEFAULT_RESULTS_ROOT = _REPO_ROOT / "output"
_DIMENSION_ALIASES = {
    "h": "h",
    "height": "h",
    "ich": "ic",
    "ic": "ic",
    "input_channels": "ic",
    "och": "oc",
    "oc": "oc",
    "output_channels": "oc",
    "w": "w",
    "width": "w",
}


@dataclass(frozen=True)
class ConvSweepProfile:
    key: str
    op_type: str
    kernel: int
    base_h: int
    base_w: int
    base_ic: int
    base_oc: int
    default_sweeps: dict[str, list[int]]


_PROFILES = {
    "conv3x3": ConvSweepProfile(
        key="conv3x3",
        op_type="conv2d_3x3",
        kernel=3,
        base_h=16,
        base_w=16,
        base_ic=4,
        base_oc=16,
        default_sweeps={
            "h": [4, 6, 8, 10, 12, 14, 16],
            "ic": [4, 8, 16, 64, 256],
            "oc": [8, 16, 32, 48],
            "w": [8, 16, 32, 64, 128, 200],
        },
    ),
    "conv1x1": ConvSweepProfile(
        key="conv1x1",
        op_type="conv2d_1x1",
        kernel=1,
        base_h=16,
        base_w=16,
        base_ic=12,
        base_oc=16,
        default_sweeps={
            "h": [16, 32, 48],
            "ic": [12, 36, 48, 96],
            "oc": [16, 32, 64],
            "w": [16, 64, 192],
        },
    ),
}


def _utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _canonical_dimension(name: str) -> str:
    key = name.strip().lower()
    if key not in _DIMENSION_ALIASES:
        supported = ", ".join(sorted(_DIMENSION_ALIASES))
        raise ValueError(f"unsupported sweep dimension '{name}', expected one of: {supported}")
    return _DIMENSION_ALIASES[key]


def _parse_csv_ints(text: str | None, fallback: list[int]) -> list[int]:
    if text is None or text.strip() == "":
        return list(fallback)
    values = []
    for part in text.split(","):
        piece = part.strip()
        if not piece:
            continue
        values.append(int(piece))
    if not values:
        raise ValueError("expected at least one integer value")
    return sorted(dict.fromkeys(values))


def _parse_dimensions(text: str | None) -> list[str]:
    if text is None or text.strip() == "":
        return ["h", "ic", "oc", "w"]
    dims = [_canonical_dimension(piece) for piece in text.split(",") if piece.strip()]
    if not dims:
        raise ValueError("expected at least one sweep dimension")
    return list(dict.fromkeys(dims))


def _validate_conv_shape(profile: ConvSweepProfile, h: int, w: int, ic: int, oc: int, stride: int, padding: int) -> tuple[int, int]:
    if min(h, w, ic, oc, stride) <= 0:
        raise ValueError("all shape parameters and stride must be positive")
    if padding < 0:
        raise ValueError("padding must be >= 0")
    if profile.op_type == "conv2d_3x3":
        if ic % 4 != 0:
            raise ValueError(f"conv3x3 requires IC divisible by 4, got {ic}")
    elif profile.op_type == "conv2d_1x1":
        if padding != 0:
            raise ValueError("conv1x1 sweeps do not support padding; only conv2d_3x3 may set padding")
        if ic % 12 != 0:
            raise ValueError(f"conv1x1 requires IC divisible by 12, got {ic}")
    if oc % 4 != 0:
        raise ValueError(f"OC must be divisible by 4, got {oc}")
    out_h = (h + 2 * padding - profile.kernel) // stride + 1
    out_w = (w + 2 * padding - profile.kernel) // stride + 1
    if out_h <= 0 or out_w <= 0:
        raise ValueError(
            f"invalid output shape for H={h}, W={w}, kernel={profile.kernel}, stride={stride}, padding={padding}"
        )
    return out_h, out_w


def _build_conv_workload(
    profile: ConvSweepProfile,
    *,
    name: str,
    h: int,
    w: int,
    ic: int,
    oc: int,
    stride: int,
    padding: int,
    activation: str,
) -> dict[str, Any]:
    out_h, out_w = _validate_conv_shape(profile, h, w, ic, oc, stride, padding)
    attrs: dict[str, Any] = {"stride": stride}
    if profile.op_type == "conv2d_3x3":
        attrs["padding"] = padding
    if activation != "none":
        attrs["activation"] = activation
    return {
        "name": name,
        "hardware": {
            "num_clusters": 1,
            "num_pes": 48,
            "num_bus": 3,
            "spm_banks_per_group": 3,
            "spm_bank_depth": 8192,
            "dram_base": 0x80000000,
        },
        "tensors": {
            "input": {
                "shape": [1, h, w, ic],
                "dtype": "fp16",
                "layout": "NHWC",
            },
            "weight": {
                "shape": [oc, profile.kernel, profile.kernel, ic],
                "dtype": "fp16",
                "layout": "OIHW",
            },
            "output": {
                "shape": [1, out_h, out_w, oc],
                "dtype": "fp16",
                "layout": "NHWC",
            },
        },
        "ops": [
            {
                "name": "conv1",
                "type": profile.op_type,
                "inputs": ["input", "weight"],
                "outputs": ["output"],
                "attrs": attrs,
            }
        ],
    }


def _case_stem(
    profile: ConvSweepProfile,
    sweep_dim: str,
    h: int,
    w: int,
    ic: int,
    oc: int,
    padding: int,
    activation: str,
) -> str:
    parts = [
        profile.key,
        sweep_dim,
        f"h{h}",
        f"w{w}",
        f"ic{ic}",
        f"oc{oc}",
    ]
    if padding > 0:
        parts.append(f"pad{padding}")
    if activation != "none":
        parts.append(activation)
    return "_".join(parts)


def _write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _generate_suite(args: argparse.Namespace) -> int:
    profile = _PROFILES[args.workload]
    activation = args.activation.strip().lower()
    if activation not in {"none", "relu"}:
        raise ValueError("activation must be one of: none, relu")
    if profile.op_type != "conv2d_3x3" and args.padding != 0:
        raise ValueError("only conv2d_3x3 sweeps may set padding")

    dims = _parse_dimensions(args.dimensions)
    h_values = _parse_csv_ints(args.h_values, profile.default_sweeps["h"])
    ic_values = _parse_csv_ints(args.ic_values, profile.default_sweeps["ic"])
    oc_values = _parse_csv_ints(args.oc_values, profile.default_sweeps["oc"])
    w_values = _parse_csv_ints(args.w_values, profile.default_sweeps["w"])

    output_dir = args.output_dir.resolve()
    suite_name = args.suite_name or f"{profile.key}_sweeps"
    yaml_root = output_dir / "yaml"
    list_root = output_dir / "lists"
    output_dir.mkdir(parents=True, exist_ok=True)

    dim_to_values = {
        "h": h_values,
        "ic": ic_values,
        "oc": oc_values,
        "w": w_values,
    }
    base = {
        "h": profile.base_h,
        "w": profile.base_w,
        "ic": profile.base_ic,
        "oc": profile.base_oc,
    }

    manifest_cases: list[dict[str, Any]] = []
    per_dim_paths: dict[str, list[Path]] = {dim: [] for dim in dims}

    for dim in dims:
        for value in dim_to_values[dim]:
            case_dims = dict(base)
            case_dims[dim] = value
            out_h, out_w = _validate_conv_shape(
                profile,
                case_dims["h"],
                case_dims["w"],
                case_dims["ic"],
                case_dims["oc"],
                args.stride,
                args.padding,
            )
            stem = _case_stem(
                profile,
                dim,
                case_dims["h"],
                case_dims["w"],
                case_dims["ic"],
                case_dims["oc"],
                args.padding,
                activation,
            )
            yaml_path = (yaml_root / dim / f"{stem}.yaml").resolve()
            payload = _build_conv_workload(
                profile,
                name=stem,
                h=case_dims["h"],
                w=case_dims["w"],
                ic=case_dims["ic"],
                oc=case_dims["oc"],
                stride=args.stride,
                padding=args.padding,
                activation=activation,
            )
            yaml_path.parent.mkdir(parents=True, exist_ok=True)
            yaml_path.write_text(yaml.safe_dump(payload, sort_keys=False), encoding="utf-8")
            per_dim_paths[dim].append(yaml_path)
            manifest_cases.append(
                {
                    "case_name": stem,
                    "workload_key": profile.key,
                    "op_type": profile.op_type,
                    "sweep_dim": dim,
                    "kernel": profile.kernel,
                    "stride": args.stride,
                    "padding": args.padding,
                    "activation": activation,
                    "h": case_dims["h"],
                    "w": case_dims["w"],
                    "ic": case_dims["ic"],
                    "oc": case_dims["oc"],
                    "out_h": out_h,
                    "out_w": out_w,
                    "yaml_path": str(yaml_path),
                    "yaml_stem": stem,
                    "default_result_dir": str((_DEFAULT_RESULTS_ROOT / f"e2e_{stem}").resolve()),
                }
            )

    for dim, paths in per_dim_paths.items():
        list_path = list_root / f"{profile.key}_{dim}.list"
        _write_text(list_path, "\n".join(str(path) for path in paths) + "\n")

    all_list_path = list_root / f"{profile.key}_all.list"
    _write_text(all_list_path, "\n".join(str(case["yaml_path"]) for case in manifest_cases) + "\n")

    manifest = {
        "suite_name": suite_name,
        "generated_at": _utc_now(),
        "repo_root": str(_REPO_ROOT.resolve()),
        "results_default_root": str(_DEFAULT_RESULTS_ROOT.resolve()),
        "workload": {
            "key": profile.key,
            "op_type": profile.op_type,
            "kernel": profile.kernel,
            "stride": args.stride,
            "padding": args.padding,
            "activation": activation,
            "dimensions": dims,
        },
        "cases": manifest_cases,
        "lists": {
            "all": str(all_list_path.resolve()),
            **{dim: str((list_root / f"{profile.key}_{dim}.list").resolve()) for dim in dims},
        },
    }
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    guide_path = output_dir / "README.txt"
    _write_text(
        guide_path,
        "\n".join(
            [
                f"Suite: {suite_name}",
                f"Manifest: {manifest_path.resolve()}",
                f"All list: {all_list_path.resolve()}",
                "",
                "Run the full sweep with the generated list file:",
                f"  ./scripts/fast_entry/run_e2e.sh $(cat {all_list_path.resolve()}) --output-dir /tmp/hacc-sweep-results --skip-build",
                "",
                "Build the HTML report after the runs finish:",
                f"  uv run hacc-sweep report --manifest {manifest_path.resolve()} --results-root /tmp/hacc-sweep-results --output-dir /tmp/hacc-sweep-report",
            ]
        )
        + "\n",
    )

    print(f"Generated {len(manifest_cases)} sweep cases in {output_dir}")
    print(f"Manifest: {manifest_path.resolve()}")
    print(f"List file: {all_list_path.resolve()}")
    return 0


def _parse_sim_log(path: Path) -> dict[str, Any]:
    metrics: dict[str, Any] = {
        "result_state": "missing",
        "sim_time_ns": math.nan,
        "ebreak_cycle": math.nan,
        "timeout": 0.0,
        "passed": 0.0,
    }
    if not path.exists():
        return metrics

    text = path.read_text(encoding="utf-8", errors="ignore")
    metrics["result_state"] = "failed"

    sim_match = pd.Series(text.splitlines(), dtype="string").str.extract(r"\[SIM\] Simulation ended at\s+(\d+)\s+ns").dropna()
    if not sim_match.empty:
        metrics["sim_time_ns"] = float(sim_match.iloc[-1, 0])

    ebreak_match = pd.Series(text.splitlines(), dtype="string").str.extract(r"\[TB\] EBREAK at cycle\s+(\d+)").dropna()
    if not ebreak_match.empty:
        metrics["ebreak_cycle"] = float(ebreak_match.iloc[-1, 0])

    timeout = "Timeout waiting for EBREAK halt" in text
    passed = "[SIM] ALL TESTS PASSED" in text and "[SIM] SOME TESTS FAILED" not in text and not timeout
    metrics["timeout"] = 1.0 if timeout else 0.0
    metrics["passed"] = 1.0 if passed else 0.0
    metrics["result_state"] = "passed" if passed else "failed"
    return metrics


def _parse_active_pe_metrics(path: Path) -> dict[str, Any]:
    metrics = {
        "active_pes": math.nan,
        "total_pes": math.nan,
        "active_pe_ratio": math.nan,
        "layer_count": math.nan,
    }
    if not path.exists():
        return metrics
    data = json.loads(path.read_text(encoding="utf-8"))
    total_pes = float(data.get("hardware", {}).get("num_pes", 0))
    layers = data.get("layers", [])
    active = 0
    for layer in layers:
        enabled = sum(1 for entry in layer.get("scan_chain", []) if entry.get("enable"))
        active = max(active, enabled)
    metrics["active_pes"] = float(active)
    metrics["total_pes"] = total_pes if total_pes > 0 else math.nan
    metrics["active_pe_ratio"] = float(active) / total_pes if total_pes > 0 else math.nan
    metrics["layer_count"] = float(len(layers))
    return metrics


def _case_macs(case: dict[str, Any]) -> float:
    return float(case["out_h"] * case["out_w"] * case["oc"] * case["kernel"] * case["kernel"] * case["ic"])


def _resolve_result_dir(case: dict[str, Any], manifest: dict[str, Any], results_root: Path | None) -> Path:
    if results_root is None:
        return Path(case["default_result_dir"])

    case_dir = results_root / str(case["yaml_stem"])
    if case_dir.exists():
        return case_dir

    if len(manifest.get("cases", [])) == 1 and (results_root / "sim.log").exists():
        return results_root

    return case_dir


def _collect_report_rows(manifest: dict[str, Any], results_root: Path | None) -> pd.DataFrame:
    rows: list[dict[str, Any]] = []
    for case in manifest.get("cases", []):
        result_dir = _resolve_result_dir(case, manifest, results_root)
        sim_metrics = _parse_sim_log(result_dir / "sim.log")
        pe_metrics = _parse_active_pe_metrics(result_dir / "hardware_ir.json")
        row = dict(case)
        row["result_dir"] = str(result_dir)
        row.update(sim_metrics)
        row.update(pe_metrics)
        row["macs"] = _case_macs(case)
        active_pes = row.get("active_pes")
        row["macs_per_active_pe"] = row["macs"] / active_pes if isinstance(active_pes, (int, float)) and active_pes and not math.isnan(active_pes) else math.nan
        rows.append(row)
    if not rows:
        return pd.DataFrame()
    df = pd.DataFrame(rows)
    order = ["h", "ic", "oc", "w"]
    df["sweep_sort_key"] = df["sweep_dim"].map({name: idx for idx, name in enumerate(order)})
    return df.sort_values(["sweep_sort_key", "sweep_dim", "case_name"]).drop(columns=["sweep_sort_key"])


def _figure_data_uri(fig: plt.Figure) -> str:
    buffer = BytesIO()
    fig.tight_layout()
    fig.savefig(buffer, format="png", dpi=180, facecolor=fig.get_facecolor())
    plt.close(fig)
    return "data:image/png;base64," + base64.b64encode(buffer.getvalue()).decode("ascii")


def _build_1d_sweep_plot(frame: pd.DataFrame, sweep_dim: str) -> str | None:
    passed = frame[frame["passed"] == 1.0].copy()
    if passed.empty:
        return None
    passed = passed.sort_values(sweep_dim)
    metrics = [
        ("sim_time_ns", "Simulation Time (ns)"),
        ("active_pes", "Active PEs"),
        ("active_pe_ratio", "Active PE Ratio"),
        ("macs", "MACs"),
        ("macs_per_active_pe", "MACs / Active PE"),
    ]
    fig, axes = plt.subplots(3, 2, figsize=(12, 10), facecolor="#f7f2e8")
    axes_list = list(axes.flat)
    x = passed[sweep_dim]
    for axis, (metric, label) in zip(axes_list, metrics):
        axis.plot(x, passed[metric], marker="o", color="#8b5e34", linewidth=2.0)
        axis.set_title(label, fontsize=11)
        axis.set_xlabel(sweep_dim.upper())
        axis.grid(alpha=0.25, color="#b79b77")
        axis.set_facecolor("#fffdf8")
    for axis in axes_list[len(metrics):]:
        axis.remove()
    fig.suptitle(f"{frame['workload_key'].iloc[0]} {sweep_dim.upper()} sweep", fontsize=14, color="#4f3b27")
    return _figure_data_uri(fig)


def _build_3d_scatter(frame: pd.DataFrame, dims: list[str], color_metric: str) -> str | None:
    if len(dims) != 3:
        raise ValueError("3D scatter expects exactly three dimensions")
    for dim in dims:
        if dim not in frame.columns:
            raise ValueError(f"manifest does not contain dimension '{dim}'")
    passed = frame[(frame["passed"] == 1.0) & frame[color_metric].notna()].copy()
    if len(passed) < 2:
        return None
    fig = plt.figure(figsize=(10, 8), facecolor="#f7f2e8")
    ax = fig.add_subplot(111, projection="3d")
    scatter = ax.scatter(
        passed[dims[0]],
        passed[dims[1]],
        passed[dims[2]],
        c=passed[color_metric],
        cmap=cm.get_cmap("viridis"),
        s=70,
        edgecolors="#3d2d1f",
        linewidths=0.5,
        alpha=0.9,
    )
    ax.set_xlabel(dims[0].upper())
    ax.set_ylabel(dims[1].upper())
    ax.set_zlabel(dims[2].upper())
    ax.set_title(f"3D sweep distribution colored by {color_metric}")
    fig.colorbar(scatter, ax=ax, fraction=0.03, pad=0.08, label=color_metric)
    return _figure_data_uri(fig)


def _summary_cards(frame: pd.DataFrame) -> str:
    total = len(frame)
    passed = int((frame["passed"] == 1.0).sum())
    failed = int((frame["result_state"] == "failed").sum())
    missing = int((frame["result_state"] == "missing").sum())
    avg_time = frame.loc[frame["sim_time_ns"].notna(), "sim_time_ns"].mean()
    avg_active = frame.loc[frame["active_pes"].notna(), "active_pes"].mean()
    cards = [
        ("Cases", str(total), "manifest entries"),
        ("Passed", str(passed), "completed simulations"),
        ("Failed", str(failed), "sim.log present but not passing"),
        ("Missing", str(missing), "results directory not found"),
        ("Avg sim time", f"{avg_time:.0f} ns" if not math.isnan(avg_time) else "-", "passed cases"),
        ("Avg active PEs", f"{avg_active:.1f}" if not math.isnan(avg_active) else "-", "max enabled per workload"),
    ]
    return "".join(
        f'<div class="card"><div class="label">{escape(label)}</div><div class="value">{escape(value)}</div><div class="sub">{escape(sub)}</div></div>'
        for label, value, sub in cards
    )


def _format_table(frame: pd.DataFrame, columns: list[str]) -> str:
    table = frame[columns].copy()
    for column in table.columns:
        if pd.api.types.is_float_dtype(table[column]):
            table[column] = table[column].map(lambda value: "-" if pd.isna(value) else f"{value:.4g}")
    return table.to_html(index=False, classes="report-table", border=0, justify="center")


def _render_report_html(
    manifest: dict[str, Any],
    frame: pd.DataFrame,
    plots_1d: dict[str, str | None],
    scatter_plot: str | None,
    scatter_dims: list[str],
    color_metric: str,
) -> str:
    sweep_sections = []
    for sweep_dim, group in frame.groupby("sweep_dim", sort=False):
        plot_uri = plots_1d.get(sweep_dim)
        image_html = (
            f'<img src="{plot_uri}" alt="{escape(sweep_dim)} sweep plot" class="plot">' if plot_uri is not None else '<div class="empty">No passed runs available for plotting.</div>'
        )
        sweep_sections.append(
            "".join(
                [
                    f'<section class="section"><div class="section-head"><h2>{escape(sweep_dim.upper())} Sweep</h2><span>{escape(group.iloc[0]["workload_key"])} workload</span></div>',
                    image_html,
                    _format_table(
                        group.sort_values(sweep_dim),
                        [sweep_dim, "sim_time_ns", "active_pes", "active_pe_ratio", "macs", "macs_per_active_pe", "result_state"],
                    ),
                    "</section>",
                ]
            )
        )

    scatter_html = '<div class="empty">Not enough completed runs to render the 3D scatter plot.</div>'
    if scatter_plot is not None:
        scatter_html = f'<img src="{scatter_plot}" alt="3D sweep scatter" class="plot plot-wide">'

    return f"""
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{escape(manifest['suite_name'])} sweep report</title>
  <style>
    :root {{
      --bg: #f4ede1;
      --panel: rgba(255, 251, 245, 0.92);
      --panel-strong: rgba(255, 255, 255, 0.96);
      --line: rgba(117, 86, 54, 0.18);
      --text: #2f251b;
      --muted: #6d5c4b;
      --accent: #8b5e34;
      --accent-soft: #d8b991;
      --shadow: 0 18px 48px rgba(86, 62, 38, 0.12);
    }}
    * {{ box-sizing: border-box; }}
    body {{ margin: 0; background: radial-gradient(circle at top left, #fff7ea 0%, var(--bg) 48%, #ebe0cf 100%); color: var(--text); font-family: "IBM Plex Sans", "Segoe UI", sans-serif; }}
    main {{ padding: 28px; max-width: 1440px; margin: 0 auto; }}
    .hero {{ background: linear-gradient(135deg, rgba(139, 94, 52, 0.14), rgba(255,255,255,0.88)); border: 1px solid var(--line); border-radius: 28px; padding: 28px; box-shadow: var(--shadow); }}
    .hero h1 {{ margin: 0 0 10px; font-size: 34px; }}
    .hero p {{ margin: 0; color: var(--muted); line-height: 1.5; }}
    .meta {{ display: flex; flex-wrap: wrap; gap: 10px; margin-top: 18px; }}
    .pill {{ border-radius: 999px; padding: 8px 12px; background: rgba(255,255,255,0.7); border: 1px solid var(--line); color: var(--muted); font-size: 13px; }}
    .card-grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 14px; margin-top: 22px; }}
    .card {{ background: var(--panel); border: 1px solid var(--line); border-radius: 18px; padding: 16px; box-shadow: var(--shadow); }}
    .card .label {{ color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: 0.08em; }}
    .card .value {{ margin-top: 10px; font-size: 28px; font-weight: 700; }}
    .card .sub {{ margin-top: 6px; color: var(--muted); font-size: 13px; }}
    .section {{ margin-top: 24px; background: var(--panel); border: 1px solid var(--line); border-radius: 24px; padding: 22px; box-shadow: var(--shadow); }}
    .section-head {{ display: flex; justify-content: space-between; align-items: baseline; gap: 12px; margin-bottom: 16px; }}
    .section-head h2 {{ margin: 0; font-size: 24px; }}
    .section-head span {{ color: var(--muted); }}
    .plot {{ width: 100%; border-radius: 18px; border: 1px solid rgba(0,0,0,0.05); background: #fffdf8; margin-bottom: 18px; }}
    .plot-wide {{ max-height: 760px; object-fit: contain; }}
    .report-table {{ width: 100%; border-collapse: collapse; background: var(--panel-strong); overflow: hidden; border-radius: 16px; }}
    .report-table th, .report-table td {{ padding: 10px 12px; border-bottom: 1px solid rgba(0,0,0,0.06); text-align: center; font-size: 13px; }}
    .report-table th {{ background: rgba(139, 94, 52, 0.08); color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; font-size: 12px; }}
    .empty {{ border: 1px dashed var(--line); border-radius: 18px; padding: 28px; text-align: center; color: var(--muted); background: rgba(255,255,255,0.56); }}
    @media (max-width: 860px) {{
      main {{ padding: 14px; }}
      .section-head {{ flex-direction: column; align-items: flex-start; }}
      .report-table th, .report-table td {{ padding: 8px; font-size: 12px; }}
    }}
  </style>
</head>
<body>
  <main>
    <section class="hero">
      <h1>{escape(manifest['suite_name'])} sweep report</h1>
      <p>Generated from manifest plus e2e output directories. The report combines simulation runtime, active PE footprint, MAC counts, and MACs per active PE into one visual summary.</p>
      <div class="meta">
        <span class="pill">workload={escape(manifest['workload']['key'])}</span>
        <span class="pill">op={escape(manifest['workload']['op_type'])}</span>
        <span class="pill">padding={escape(str(manifest['workload']['padding']))}</span>
        <span class="pill">activation={escape(manifest['workload']['activation'])}</span>
        <span class="pill">scatter={escape(', '.join(dim.upper() for dim in scatter_dims))}</span>
        <span class="pill">color metric={escape(color_metric)}</span>
      </div>
      <div class="card-grid">{_summary_cards(frame)}</div>
    </section>
    <section class="section">
      <div class="section-head"><h2>3D Scatter</h2><span>Distribution over {escape(', '.join(dim.upper() for dim in scatter_dims))}</span></div>
      {scatter_html}
    </section>
    {''.join(sweep_sections)}
  </main>
</body>
</html>
"""


def _report_suite(args: argparse.Namespace) -> int:
    manifest_path = args.manifest.resolve()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    results_root = args.results_root.resolve() if args.results_root is not None else None
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    frame = _collect_report_rows(manifest, results_root)
    csv_path = output_dir / "sweep_metrics.csv"
    frame.to_csv(csv_path, index=False)

    scatter_dims = [_canonical_dimension(piece) for piece in args.scatter_dims.split(",") if piece.strip()]
    if len(scatter_dims) != 3:
        raise ValueError("--scatter-dims expects exactly three comma-separated dimensions")

    plots_1d = {
        sweep_dim: _build_1d_sweep_plot(group, sweep_dim)
        for sweep_dim, group in frame.groupby("sweep_dim", sort=False)
    }
    scatter_plot = _build_3d_scatter(frame, scatter_dims, args.color_metric)
    html = _render_report_html(manifest, frame, plots_1d, scatter_plot, scatter_dims, args.color_metric)
    html_path = output_dir / "report.html"
    html_path.write_text(html, encoding="utf-8")

    print(f"CSV: {csv_path}")
    print(f"HTML: {html_path}")
    return 0


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="hacc-sweep",
        description="Generate HybridAcc workload sweeps and summarize e2e results.",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    gen = subparsers.add_parser("gen", help="Generate sweep YAML files plus manifest/list files")
    gen.add_argument("--workload", choices=sorted(_PROFILES), required=True, help="Built-in workload family to sweep")
    gen.add_argument("--output-dir", type=Path, required=True, help="Directory for generated YAML/list/manifest files")
    gen.add_argument("--suite-name", type=str, default=None, help="Optional human-readable suite name")
    gen.add_argument("--dimensions", type=str, default="h,ic,oc,w", help="Comma-separated sweep dimensions")
    gen.add_argument("--h-values", type=str, default=None, help="Comma-separated H values")
    gen.add_argument("--ic-values", type=str, default=None, help="Comma-separated IC values")
    gen.add_argument("--oc-values", type=str, default=None, help="Comma-separated OC values")
    gen.add_argument("--w-values", type=str, default=None, help="Comma-separated W values")
    gen.add_argument("--stride", type=int, default=1, help="Stride value written into attrs")
    gen.add_argument("--padding", type=int, default=0, help="Padding value (conv3x3 only)")
    gen.add_argument("--activation", type=str, default="none", help="Output activation: none or relu")
    gen.set_defaults(func=_generate_suite)

    report = subparsers.add_parser("report", help="Parse sweep outputs and build an HTML report")
    report.add_argument("--manifest", type=Path, required=True, help="Manifest JSON emitted by hacc-sweep gen")
    report.add_argument("--results-root", type=Path, default=None, help="Root directory passed to run_e2e.sh --output-dir")
    report.add_argument("--output-dir", type=Path, required=True, help="Directory for CSV and HTML report")
    report.add_argument("--scatter-dims", type=str, default="h,ic,oc", help="Three comma-separated dimensions for the 3D scatter")
    report.add_argument("--color-metric", type=str, default="sim_time_ns", help="Metric column used for scatter plot color")
    report.set_defaults(func=_report_suite)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())