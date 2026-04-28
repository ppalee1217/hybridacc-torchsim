from __future__ import annotations

import argparse
import json
import math
import re
from dataclasses import dataclass
from datetime import datetime, timezone
from html import escape
from itertools import product
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
_METRIC_LABELS = {
    "sim_time_ns": "Simulation Time (ns)",
    "ebreak_cycle": "Core-level cycles (EBREAK)",
    "cluster_run_cycles": "Cluster Busy Cycles (HDDU/AGU)",
    "dma_active_cycles": "DMA Active Cycles",
    "compute_dma_overlap_cycles": "Compute/DMA Overlap Cycles",
    "compute_dma_overlap_pct_of_compute": "Overlap / Compute Busy (%)",
    "compute_dma_overlap_pct_of_dma": "Overlap / DMA Active (%)",
    "wave_gap_windows": "Completed Wave Gaps",
    "wave_gap_partial_windows_dropped": "Dropped Partial Wave Gaps",
    "wave_gap_cycles_total": "Wave Gap Cycles Total",
    "wave_gap_cycles_avg": "Wave Gap Cycles Avg",
    "wave_gap_cycles_min": "Wave Gap Cycles Min",
    "wave_gap_cycles_max": "Wave Gap Cycles Max",
    "wave_gap_last_cycles": "Wave Gap Last Cycles",
    "wave_gap_instructions_total": "Wave Gap Instructions Total",
    "wave_gap_instructions_avg": "Wave Gap Instructions Avg",
    "wave_gap_instructions_min": "Wave Gap Instructions Min",
    "wave_gap_instructions_max": "Wave Gap Instructions Max",
    "wave_gap_last_instructions": "Wave Gap Last Instructions",
    "wave_gap_mmio_config_instructions_total": "Wave Gap MMIO Configure Instructions Total",
    "wave_gap_mmio_config_instructions_avg": "Wave Gap MMIO Configure Instructions Avg",
    "wave_gap_last_mmio_config_instructions": "Wave Gap Last MMIO Configure Instructions",
    "wave_gap_data_compute_instructions_total": "Wave Gap Data Compute Instructions Total",
    "wave_gap_data_compute_instructions_avg": "Wave Gap Data Compute Instructions Avg",
    "wave_gap_last_data_compute_instructions": "Wave Gap Last Data Compute Instructions",
    "wave_gap_control_instructions_total": "Wave Gap Start/Stop Control Instructions Total",
    "wave_gap_control_instructions_avg": "Wave Gap Start/Stop Control Instructions Avg",
    "wave_gap_last_control_instructions": "Wave Gap Last Start/Stop Control Instructions",
    "boot_up_cycles": "Boot-up Cycles",
    "boot_up_instructions": "Boot-up Instructions",
    "drain_out_cycles": "Drain-out Cycles",
    "drain_out_instructions": "Drain-out Instructions",
    "core_probe_cycles_total": "Core Probe Cycles (Lifecycle Probe)",
    "steady_state_core_cycles": "Steady-state Core Cycles",
    "steady_state_core_utilization_pct": "Steady-state MACs Utilization (%)",
    "ideal_hw_sw_codesign_core_cycles": "Ideal HW/SW Co-design Core Cycles",
    "ideal_hw_sw_codesign_core_utilization_pct": "Ideal HW/SW Co-design MACs Utilization (%)",
    "wave_gap_cycles_pct_of_steady_state": "Wave Gap / Steady-state (%)",
    "wave_gap_data_compute_pct_of_gap_instructions": "Gap Data Compute / Gap Instructions (%)",
    "gfops_per_sec": "GFLOPS/sec",
    "active_pes": "Active PEs",
    "active_pe_ratio": "Active PE Ratio",
    "macs": "MACs",
    "core_level_macs_utilization_pct": "Core-level MACs Utilization (%)",
    "cluster_level_macs_utilization_pct": "Cluster-level MACs Utilization (%)",
    "macs_utilization_pct": "Core-level MACs Utilization (%)",
}
_SIM_LOG_VALUE_PATTERNS = {
    "sim_time_ns": re.compile(r"\[SIM\] Simulation ended at\s+(\d+)\s+ns"),
    "ebreak_cycle": re.compile(r"\[TB\] EBREAK at cycle\s+(\d+)"),
    "cluster_run_cycles": re.compile(r"\[SIM\] Cluster RUN cycles:\s+(\d+)"),
    "dma_active_cycles": re.compile(r"\[SIM\] DMA active cycles:\s+(\d+)"),
    "compute_dma_overlap_cycles": re.compile(r"\[SIM\] Compute/DMA overlap cycles:\s+(\d+)"),
    "wave_gap_windows": re.compile(r"\[SIM\] Cluster wave gap windows:\s+(\d+)"),
    "wave_gap_partial_windows_dropped": re.compile(r"\[SIM\] Cluster wave gap partial windows dropped:\s+(\d+)"),
    "wave_gap_cycles_total": re.compile(r"\[SIM\] Cluster wave gap cycles total:\s+([0-9.]+)"),
    "wave_gap_instructions_total": re.compile(r"\[SIM\] Cluster wave gap instructions total:\s+([0-9.]+)"),
    "wave_gap_mmio_config_instructions_total": re.compile(r"\[SIM\] Cluster wave gap MMIO configure instructions total:\s+([0-9.]+)"),
    "wave_gap_data_compute_instructions_total": re.compile(r"\[SIM\] Cluster wave gap data compute instructions total:\s+([0-9.]+)"),
    "wave_gap_control_instructions_total": re.compile(r"\[SIM\] Cluster wave gap start/stop control instructions total:\s+([0-9.]+)"),
    "wave_gap_cycles_avg": re.compile(r"\[SIM\] Cluster wave gap cycles avg:\s+([0-9.]+)"),
    "wave_gap_instructions_avg": re.compile(r"\[SIM\] Cluster wave gap instructions avg:\s+([0-9.]+)"),
    "wave_gap_mmio_config_instructions_avg": re.compile(r"\[SIM\] Cluster wave gap MMIO configure instructions avg:\s+([0-9.]+)"),
    "wave_gap_data_compute_instructions_avg": re.compile(r"\[SIM\] Cluster wave gap data compute instructions avg:\s+([0-9.]+)"),
    "wave_gap_control_instructions_avg": re.compile(r"\[SIM\] Cluster wave gap start/stop control instructions avg:\s+([0-9.]+)"),
    "wave_gap_cycles_min": re.compile(r"\[SIM\] Cluster wave gap cycles min:\s+([0-9.]+)"),
    "wave_gap_cycles_max": re.compile(r"\[SIM\] Cluster wave gap cycles max:\s+([0-9.]+)"),
    "wave_gap_last_cycles": re.compile(r"\[SIM\] Cluster wave gap last window cycles:\s+([0-9.]+)"),
    "wave_gap_instructions_min": re.compile(r"\[SIM\] Cluster wave gap instructions min:\s+([0-9.]+)"),
    "wave_gap_instructions_max": re.compile(r"\[SIM\] Cluster wave gap instructions max:\s+([0-9.]+)"),
    "wave_gap_last_instructions": re.compile(r"\[SIM\] Cluster wave gap last window instructions:\s+([0-9.]+)"),
    "wave_gap_last_mmio_config_instructions": re.compile(r"\[SIM\] Cluster wave gap last MMIO configure instructions:\s+([0-9.]+)"),
    "wave_gap_last_data_compute_instructions": re.compile(r"\[SIM\] Cluster wave gap last data compute instructions:\s+([0-9.]+)"),
    "wave_gap_last_control_instructions": re.compile(r"\[SIM\] Cluster wave gap last start/stop control instructions:\s+([0-9.]+)"),
    "boot_up_cycles": re.compile(r"\[SIM\] Cluster boot-up cycles:\s+([0-9.]+)"),
    "boot_up_instructions": re.compile(r"\[SIM\] Cluster boot-up instructions:\s+([0-9.]+)"),
    "drain_out_cycles": re.compile(r"\[SIM\] Cluster drain-out cycles:\s+([0-9.]+)"),
    "drain_out_instructions": re.compile(r"\[SIM\] Cluster drain-out instructions:\s+([0-9.]+)"),
}
_BOOT_UP_DETAIL_PATTERN = re.compile(
    r"\[SIM\] Cluster boot-up detail: start_cycle=(\d+) first_start_cycle=(\d+) cycles=(\d+) "
    r"start_instruction=(\d+) first_start_instruction=(\d+) instructions=(\d+)"
)
_DRAIN_OUT_DETAIL_PATTERN = re.compile(
    r"\[SIM\] Cluster drain-out detail: last_stop_cycle=(\d+) end_cycle=(\d+) cycles=(\d+) "
    r"last_stop_instruction=(\d+) end_instruction=(\d+) instructions=(\d+)"
)
_WAVE_GAP_WINDOW_PATTERN = re.compile(
    r"\[SIM\] Cluster wave gap window\[(\d+)\]: stop_cycle=(\d+) next_start_cycle=(\d+) cycles=(\d+) "
    r"stop_instruction=(\d+) next_start_instruction=(\d+) instructions=(\d+) mmio_config=(\d+) "
    r"data_compute=(\d+) start_stop_control=(\d+)"
)
_DIMENSION_ALIASES = {
    "h": "oh",
    "height": "oh",
    "oh": "oh",
    "out_h": "oh",
    "output_height": "oh",
    "m": "m",
    "rows": "m",
    "output_rows": "m",
    "ich": "ic",
    "ic": "ic",
    "input_channels": "ic",
    "k": "k",
    "reduction": "k",
    "inner": "k",
    "shared_dim": "k",
    "n": "n",
    "cols": "n",
    "output_cols": "n",
    "och": "oc",
    "oc": "oc",
    "output_channels": "oc",
    "w": "ow",
    "width": "ow",
    "ow": "ow",
    "out_w": "ow",
    "output_width": "ow",
}
_SCATTER_DIMENSION_ALIASES = {
    **_DIMENSION_ALIASES,
    "oh*oc": "oh_oc_product",
    "oh_oc": "oh_oc_product",
    "ohoc": "oh_oc_product",
    "oh_x_oc": "oh_oc_product",
    "oh_times_oc": "oh_oc_product",
}
_SCATTER_DIMENSION_LABELS = {
    "oh": "OH",
    "ow": "OW",
    "ic": "IC",
    "k": "K",
    "m": "M",
    "n": "N",
    "oc": "OC",
    "oh_oc_product": "OH*OC",
}

_DIMENSION_VALUE_ARGUMENTS = {
    "oh": "oh_values",
    "ow": "ow_values",
    "ic": "ic_values",
    "oc": "oc_values",
    "m": "m_values",
    "n": "n_values",
    "k": "k_values",
}


@dataclass(frozen=True)
class SweepProfile:
    key: str
    op_type: str
    axis_order: tuple[str, ...]
    base_dims: dict[str, int]
    default_sweeps: dict[str, list[int]]
    default_scatter_dims: tuple[str, str, str]
    kernel: int | None = None
    supports_activation: bool = False


_PROFILES = {
    "conv3x3": SweepProfile(
        key="conv3x3",
        op_type="conv2d_3x3",
        axis_order=("oh", "ow", "ic", "oc"),
        base_dims={"oh": 14, "ow": 192, "ic": 4, "oc": 16},
        kernel=3,
        default_sweeps={
            "oh": [16, 32, 64, 128],
            "ow": [64, 128, 192, 384],
            "ic": [4, 8, 16, 32, 64, 128],
            "oc": [16, 32, 64, 128],
        },
        default_scatter_dims=("ic", "ow", "oh_oc_product"),
        supports_activation=True,
    ),
    "conv1x1": SweepProfile(
        key="conv1x1",
        op_type="conv2d_1x1",
        axis_order=("oh", "ow", "ic", "oc"),
        base_dims={"oh": 16, "ow": 64, "ic": 12, "oc": 48},
        kernel=1,
        default_sweeps={
            "oh": [16, 48, 96, 144],
            "ow": [16, 32, 64, 128, 192, 384],
            "ic": [12, 24, 48, 96, 192, 384],
            "oc": [16, 48, 96, 144],
        },
        default_scatter_dims=("ic", "ow", "oh_oc_product"),
        supports_activation=True,
    ),
    "gemm": SweepProfile(
        key="gemm",
        op_type="gemm",
        axis_order=("m", "n", "k"),
        base_dims={"m": 48, "n": 32, "k": 96},
        default_sweeps={
            "m": [48, 96, 192, 384, 768],
            "n": [32, 64, 128, 256, 512],
            "k": [32, 64, 96, 192, 384, 768],
        },
        default_scatter_dims=("m", "n", "k"),
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


def _parse_dimensions(text: str | None, default_dims: list[str]) -> list[str]:
    if text is None or text.strip() == "":
        return list(default_dims)
    dims = [_canonical_dimension(piece) for piece in text.split(",") if piece.strip()]
    if not dims:
        raise ValueError("expected at least one sweep dimension")
    return list(dict.fromkeys(dims))


def _canonical_scatter_dimension(name: str) -> str:
    key = name.strip().lower()
    if key not in _SCATTER_DIMENSION_ALIASES:
        supported = ", ".join(sorted(_SCATTER_DIMENSION_ALIASES))
        raise ValueError(f"unsupported scatter dimension '{name}', expected one of: {supported}")
    return _SCATTER_DIMENSION_ALIASES[key]


def _parse_scatter_dimensions(text: str | None, default_dims: list[str]) -> list[str]:
    if text is None or text.strip() == "":
        return list(default_dims)
    dims = [_canonical_scatter_dimension(piece) for piece in text.split(",") if piece.strip()]
    if not dims:
        raise ValueError("expected exactly three scatter dimensions")
    return list(dict.fromkeys(dims))


def _parse_metric_names(text: str | None) -> list[str]:
    if text is None or text.strip() == "":
        return ["core_level_macs_utilization_pct", "cluster_level_macs_utilization_pct"]
    metrics = [piece.strip() for piece in text.split(",") if piece.strip()]
    if not metrics:
        raise ValueError("expected at least one metric name")
    return list(dict.fromkeys(metrics))


def _metric_display_label(metric: str) -> str:
    return _METRIC_LABELS.get(metric, metric.replace("_", " ").title())


def _extract_last_float(text: str, pattern: re.Pattern[str]) -> float:
    matches = list(pattern.finditer(text))
    if not matches:
        return math.nan
    return float(matches[-1].group(1))


def _scatter_dimension_display_label(dim: str) -> str:
    return _SCATTER_DIMENSION_LABELS.get(dim, dim.upper())


def _with_scatter_dimensions(frame: pd.DataFrame, dims: list[str]) -> pd.DataFrame:
    scatter_frame = frame.copy()
    if "oh_oc_product" in dims:
        scatter_frame["oh_oc_product"] = scatter_frame["oh"] * scatter_frame["oc"]
    return scatter_frame


def _input_extent_from_output(output_extent: int, kernel: int, stride: int, padding: int, axis_name: str) -> int:
    if output_extent <= 0:
        raise ValueError(f"{axis_name} must be positive, got {output_extent}")
    input_extent = (output_extent - 1) * stride + kernel - 2 * padding
    if input_extent <= 0:
        raise ValueError(
            f"invalid input {axis_name} derived from output {axis_name}={output_extent}, kernel={kernel}, stride={stride}, padding={padding}"
        )
    return input_extent


def _input_hw_from_output(profile: SweepProfile, oh: int, ow: int, stride: int, padding: int) -> tuple[int, int]:
    h = _input_extent_from_output(oh, profile.kernel, stride, padding, "height")
    w = _input_extent_from_output(ow, profile.kernel, stride, padding, "width")
    return h, w


def _parse_profile_dim_values(args: argparse.Namespace, profile: SweepProfile) -> dict[str, list[int]]:
    return {
        dim: _parse_csv_ints(getattr(args, _DIMENSION_VALUE_ARGUMENTS[dim]), profile.default_sweeps[dim])
        for dim in profile.axis_order
    }


def _sweep_group_name(dims: list[str]) -> str:
    return dims[0] if len(dims) == 1 else "x".join(dims)


def _iter_sweep_cases(
    dims: list[str],
    dim_to_values: dict[str, list[int]],
    base: dict[str, int],
    mode: str,
):
    if mode == "product":
        sweep_group = _sweep_group_name(dims)
        for combo in product(*(dim_to_values[dim] for dim in dims)):
            case_dims = dict(base)
            for dim, value in zip(dims, combo):
                case_dims[dim] = value
            yield sweep_group, list(dims), case_dims
        return

    if mode == "onehot":
        for dim in dims:
            sweep_group = _sweep_group_name([dim])
            for value in dim_to_values[dim]:
                case_dims = dict(base)
                case_dims[dim] = value
                yield sweep_group, [dim], case_dims
        return

    raise ValueError("mode must be one of: product, onehot")


def _validate_conv_shape(profile: SweepProfile, h: int, w: int, ic: int, oc: int, stride: int, padding: int) -> tuple[int, int]:
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


def _validate_gemm_shape(m: int, n: int, k: int) -> None:
    if min(m, n, k) <= 0:
        raise ValueError("all GEMM dimensions must be positive")
    if k % 4 != 0:
        raise ValueError(f"gemm requires K divisible by 4, got {k}")


def _build_conv_workload(
    profile: SweepProfile,
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


def _build_gemm_workload(
    *,
    name: str,
    m: int,
    n: int,
    k: int,
) -> dict[str, Any]:
    _validate_gemm_shape(m, n, k)
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
            "A": {
                "shape": [m, k],
                "dtype": "fp16",
            },
            "B": {
                "shape": [k, n],
                "dtype": "fp16",
            },
            "C": {
                "shape": [m, n],
                "dtype": "fp16",
            },
        },
        "ops": [
            {
                "name": "gemm1",
                "type": "gemm",
                "inputs": ["A", "B"],
                "outputs": ["C"],
            }
        ],
    }


def _case_stem(
    profile: SweepProfile,
    sweep_dim: str,
    case_dims: dict[str, int],
    padding: int,
    activation: str,
) -> str:
    parts = [
        profile.key,
        sweep_dim,
    ]
    parts.extend(f"{dim}{case_dims[dim]}" for dim in profile.axis_order)
    if profile.op_type == "conv2d_3x3" and padding > 0:
        parts.append(f"pad{padding}")
    if profile.supports_activation and activation != "none":
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
    if profile.op_type == "gemm":
        if activation != "none":
            raise ValueError("gemm sweeps do not support activation")
        if args.stride != 1:
            raise ValueError("gemm sweeps do not support stride; leave --stride at 1")
    if profile.op_type != "conv2d_3x3" and args.padding != 0:
        raise ValueError("only conv2d_3x3 sweeps may set padding")
    if args.mode not in {"product", "onehot"}:
        raise ValueError("mode must be one of: product, onehot")

    dims = _parse_dimensions(args.dimensions, list(profile.axis_order))
    unsupported_dims = [dim for dim in dims if dim not in profile.axis_order]
    if unsupported_dims:
        supported = ", ".join(profile.axis_order)
        invalid = ", ".join(unsupported_dims)
        raise ValueError(f"{profile.key} supports sweep dimensions: {supported}; got unsupported dimensions: {invalid}")

    output_dir = args.output_dir.resolve()
    suite_name = args.suite_name or f"{profile.key}_sweeps"
    yaml_root = output_dir / "yaml"
    list_root = output_dir / "lists"
    output_dir.mkdir(parents=True, exist_ok=True)

    dim_to_values = _parse_profile_dim_values(args, profile)
    base = dict(profile.base_dims)

    manifest_cases: list[dict[str, Any]] = []
    grouped_paths: dict[str, list[Path]] = {}

    for sweep_group, sweep_dims, case_dims in _iter_sweep_cases(dims, dim_to_values, base, args.mode):
        stem = _case_stem(
            profile,
            sweep_group,
            case_dims,
            args.padding,
            activation,
        )
        yaml_path = (yaml_root / sweep_group / f"{stem}.yaml").resolve()
        case_record: dict[str, Any] = {
            "case_name": stem,
            "workload_key": profile.key,
            "op_type": profile.op_type,
            "sweep_dim": sweep_group,
            "sweep_dims": sweep_dims,
            **{dim: case_dims[dim] for dim in profile.axis_order},
            "yaml_path": str(yaml_path),
            "yaml_stem": stem,
            "default_result_dir": str((_DEFAULT_RESULTS_ROOT / f"e2e_{stem}").resolve()),
        }

        if profile.op_type == "gemm":
            _validate_gemm_shape(case_dims["m"], case_dims["n"], case_dims["k"])
            payload = _build_gemm_workload(
                name=stem,
                m=case_dims["m"],
                n=case_dims["n"],
                k=case_dims["k"],
            )
        else:
            input_h, input_w = _input_hw_from_output(
                profile,
                case_dims["oh"],
                case_dims["ow"],
                args.stride,
                args.padding,
            )
            out_h, out_w = _validate_conv_shape(
                profile,
                input_h,
                input_w,
                case_dims["ic"],
                case_dims["oc"],
                args.stride,
                args.padding,
            )
            if out_h != case_dims["oh"] or out_w != case_dims["ow"]:
                raise ValueError(
                    f"derived output shape mismatch: expected OH/OW={case_dims['oh']}/{case_dims['ow']}, got {out_h}/{out_w}"
                )
            payload = _build_conv_workload(
                profile,
                name=stem,
                h=input_h,
                w=input_w,
                ic=case_dims["ic"],
                oc=case_dims["oc"],
                stride=args.stride,
                padding=args.padding,
                activation=activation,
            )
            case_record.update(
                {
                    "kernel": profile.kernel,
                    "stride": args.stride,
                    "padding": args.padding,
                    "activation": activation,
                    "h": input_h,
                    "w": input_w,
                    "out_h": out_h,
                    "out_w": out_w,
                }
            )
        yaml_path.parent.mkdir(parents=True, exist_ok=True)
        yaml_path.write_text(yaml.safe_dump(payload, sort_keys=False), encoding="utf-8")
        grouped_paths.setdefault(sweep_group, [])
        grouped_paths[sweep_group].append(yaml_path)
        manifest_cases.append(case_record)

    for group_name, paths in grouped_paths.items():
        list_path = list_root / f"{profile.key}_{group_name}.list"
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
            "mode": args.mode,
            "axis_order": list(profile.axis_order),
            "default_scatter_dims": list(profile.default_scatter_dims),
            "dimensions": dims,
        },
        "cases": manifest_cases,
        "lists": {
            "all": str(all_list_path.resolve()),
            **{group_name: str((list_root / f"{profile.key}_{group_name}.list").resolve()) for group_name in grouped_paths},
        },
    }
    if profile.kernel is not None:
        manifest["workload"]["kernel"] = profile.kernel
        manifest["workload"]["stride"] = args.stride
        manifest["workload"]["padding"] = args.padding
        manifest["workload"]["activation"] = activation
    manifest_path = output_dir / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    guide_path = output_dir / "README.txt"
    _write_text(
        guide_path,
        "\n".join(
            [
                f"Suite: {suite_name}",
                f"Mode: {args.mode}",
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
        "cluster_run_cycles": math.nan,
        "dma_active_cycles": math.nan,
        "compute_dma_overlap_cycles": math.nan,
        "wave_gap_windows": math.nan,
        "wave_gap_partial_windows_dropped": math.nan,
        "wave_gap_cycles_total": math.nan,
        "wave_gap_instructions_total": math.nan,
        "wave_gap_mmio_config_instructions_total": math.nan,
        "wave_gap_data_compute_instructions_total": math.nan,
        "wave_gap_control_instructions_total": math.nan,
        "wave_gap_cycles_avg": math.nan,
        "wave_gap_instructions_avg": math.nan,
        "wave_gap_mmio_config_instructions_avg": math.nan,
        "wave_gap_data_compute_instructions_avg": math.nan,
        "wave_gap_control_instructions_avg": math.nan,
        "wave_gap_cycles_min": math.nan,
        "wave_gap_cycles_max": math.nan,
        "wave_gap_last_cycles": math.nan,
        "wave_gap_instructions_min": math.nan,
        "wave_gap_instructions_max": math.nan,
        "wave_gap_last_instructions": math.nan,
        "wave_gap_last_mmio_config_instructions": math.nan,
        "wave_gap_last_data_compute_instructions": math.nan,
        "wave_gap_last_control_instructions": math.nan,
        "boot_up_cycles": math.nan,
        "boot_up_instructions": math.nan,
        "boot_up_start_cycle": math.nan,
        "boot_up_first_start_cycle": math.nan,
        "boot_up_start_instruction": math.nan,
        "boot_up_first_start_instruction": math.nan,
        "drain_out_cycles": math.nan,
        "drain_out_instructions": math.nan,
        "drain_out_last_stop_cycle": math.nan,
        "drain_out_end_cycle": math.nan,
        "drain_out_last_stop_instruction": math.nan,
        "drain_out_end_instruction": math.nan,
        "wave_gap_window_details": [],
        "timeout": 0.0,
        "passed": 0.0,
    }
    if not path.exists():
        return metrics

    text = path.read_text(encoding="utf-8", errors="ignore")
    metrics["result_state"] = "failed"

    for key, pattern in _SIM_LOG_VALUE_PATTERNS.items():
        metrics[key] = _extract_last_float(text, pattern)

    boot_up_match = _BOOT_UP_DETAIL_PATTERN.search(text)
    if boot_up_match is not None:
        metrics["boot_up_start_cycle"] = float(boot_up_match.group(1))
        metrics["boot_up_first_start_cycle"] = float(boot_up_match.group(2))
        metrics["boot_up_start_instruction"] = float(boot_up_match.group(4))
        metrics["boot_up_first_start_instruction"] = float(boot_up_match.group(5))

    drain_out_match = _DRAIN_OUT_DETAIL_PATTERN.search(text)
    if drain_out_match is not None:
        metrics["drain_out_last_stop_cycle"] = float(drain_out_match.group(1))
        metrics["drain_out_end_cycle"] = float(drain_out_match.group(2))
        metrics["drain_out_last_stop_instruction"] = float(drain_out_match.group(4))
        metrics["drain_out_end_instruction"] = float(drain_out_match.group(5))

    metrics["wave_gap_window_details"] = [
        {
            "window_index": int(match.group(1)),
            "stop_cycle": int(match.group(2)),
            "next_start_cycle": int(match.group(3)),
            "cycles": int(match.group(4)),
            "stop_instruction": int(match.group(5)),
            "next_start_instruction": int(match.group(6)),
            "instructions": int(match.group(7)),
            "mmio_config_instructions": int(match.group(8)),
            "data_compute_instructions": int(match.group(9)),
            "control_instructions": int(match.group(10)),
        }
        for match in _WAVE_GAP_WINDOW_PATTERN.finditer(text)
    ]

    timeout = "Timeout waiting for EBREAK halt" in text
    passed = "[SIM] ALL TESTS PASSED" in text and "[SIM] SOME TESTS FAILED" not in text and not timeout
    metrics["timeout"] = 1.0 if timeout else 0.0
    metrics["passed"] = 1.0 if passed else 0.0
    metrics["result_state"] = "passed" if passed else "failed"
    return metrics


def _parse_e2e_result(path: Path) -> dict[str, Any]:
    metrics: dict[str, Any] = {
        "e2e_status": "MISSING",
        "e2e_reason": "",
    }
    if not path.exists():
        return metrics

    text = path.read_text(encoding="utf-8", errors="ignore").strip()
    if not text:
        metrics["e2e_status"] = "UNKNOWN"
        return metrics

    fields = text.split("\x1f")
    metrics["e2e_status"] = fields[0].strip() or "UNKNOWN"
    if len(fields) >= 3:
        metrics["e2e_reason"] = fields[2].strip()
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
    if case["op_type"] == "gemm":
        return float(case["m"] * case["n"] * case["k"])
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


def _derive_utilization(macs: float, cycles: Any, active_pes: Any) -> float:
    if (
        isinstance(cycles, (int, float))
        and isinstance(active_pes, (int, float))
        and cycles
        and active_pes
        and not math.isnan(cycles)
        and not math.isnan(active_pes)
    ):
        return 100.0 * macs / (cycles * active_pes * 4.0)
    return math.nan


def _collect_report_rows(manifest: dict[str, Any], results_root: Path | None) -> tuple[pd.DataFrame, pd.DataFrame]:
    rows: list[dict[str, Any]] = []
    window_rows: list[dict[str, Any]] = []
    for case in manifest.get("cases", []):
        result_dir = _resolve_result_dir(case, manifest, results_root)
        sim_metrics = _parse_sim_log(result_dir / "sim.log")
        window_details = list(sim_metrics.pop("wave_gap_window_details", []))
        e2e_metrics = _parse_e2e_result(result_dir / ".e2e.result")
        pe_metrics = _parse_active_pe_metrics(result_dir / "hardware_ir.json")
        row = dict(case)
        row["result_dir"] = str(result_dir)
        row.update(sim_metrics)
        row.update(e2e_metrics)
        row.update(pe_metrics)
        if row["e2e_status"] == "PASS":
            row["passed"] = 1.0
            row["result_state"] = "passed"
        elif row["e2e_status"] == "FAIL":
            row["passed"] = 0.0
            row["result_state"] = "failed"
        row["macs"] = _case_macs(case)
        sim_time_ns = row.get("sim_time_ns")
        # 1 MAC is counted as 2 floating-point operations, and dividing by ns
        # directly yields GFOPS/sec because the giga and nano scale factors cancel.
        row["gfops_per_sec"] = (
            2.0 * row["macs"] / sim_time_ns
            if isinstance(sim_time_ns, (int, float)) and sim_time_ns and not math.isnan(sim_time_ns)
            else math.nan
        )
        active_pes = row.get("active_pes")
        row["macs_per_active_pe"] = row["macs"] / active_pes if isinstance(active_pes, (int, float)) and active_pes and not math.isnan(active_pes) else math.nan
        core_cycles = row.get("ebreak_cycle")
        cluster_cycles = row.get("cluster_run_cycles")
        dma_cycles = row.get("dma_active_cycles")
        overlap_cycles = row.get("compute_dma_overlap_cycles")
        cluster_cycles_valid = isinstance(cluster_cycles, (int, float)) and not math.isnan(cluster_cycles)
        dma_cycles_valid = isinstance(dma_cycles, (int, float)) and not math.isnan(dma_cycles)
        overlap_cycles_valid = isinstance(overlap_cycles, (int, float)) and not math.isnan(overlap_cycles)
        row["core_level_macs_utilization_pct"] = _derive_utilization(row["macs"], core_cycles, active_pes)
        row["cluster_level_macs_utilization_pct"] = _derive_utilization(row["macs"], cluster_cycles, active_pes)
        row["compute_dma_overlap_pct_of_compute"] = (
            100.0 * overlap_cycles / cluster_cycles
            if overlap_cycles_valid and cluster_cycles_valid and cluster_cycles > 0
            else math.nan
        )
        row["compute_dma_overlap_pct_of_dma"] = (
            100.0 * overlap_cycles / dma_cycles
            if overlap_cycles_valid and dma_cycles_valid and dma_cycles > 0
            else math.nan
        )

        boot_up_cycles = row.get("boot_up_cycles")
        drain_out_cycles = row.get("drain_out_cycles")
        wave_gap_cycles_total = row.get("wave_gap_cycles_total")
        wave_gap_instructions_total = row.get("wave_gap_instructions_total")
        wave_gap_data_compute_total = row.get("wave_gap_data_compute_instructions_total")
        profiled_core_cycles_total = row.get("drain_out_end_cycle")
        if not isinstance(profiled_core_cycles_total, (int, float)) or math.isnan(profiled_core_cycles_total):
            profiled_core_cycles_total = core_cycles
        row["core_probe_cycles_total"] = profiled_core_cycles_total
        steady_state_core_cycles = math.nan
        if (
            isinstance(profiled_core_cycles_total, (int, float))
            and isinstance(boot_up_cycles, (int, float))
            and isinstance(drain_out_cycles, (int, float))
            and not math.isnan(profiled_core_cycles_total)
            and not math.isnan(boot_up_cycles)
            and not math.isnan(drain_out_cycles)
        ):
            steady_state_core_cycles = profiled_core_cycles_total - boot_up_cycles - drain_out_cycles
            if steady_state_core_cycles <= 0:
                steady_state_core_cycles = math.nan
        row["steady_state_core_cycles"] = steady_state_core_cycles
        row["steady_state_core_utilization_pct"] = _derive_utilization(row["macs"], steady_state_core_cycles, active_pes)
        row["wave_gap_cycles_pct_of_steady_state"] = (
            100.0 * wave_gap_cycles_total / steady_state_core_cycles
            if isinstance(wave_gap_cycles_total, (int, float))
            and isinstance(steady_state_core_cycles, (int, float))
            and not math.isnan(wave_gap_cycles_total)
            and not math.isnan(steady_state_core_cycles)
            and steady_state_core_cycles > 0
            else math.nan
        )
        row["wave_gap_data_compute_pct_of_gap_instructions"] = (
            100.0 * wave_gap_data_compute_total / wave_gap_instructions_total
            if isinstance(wave_gap_data_compute_total, (int, float))
            and isinstance(wave_gap_instructions_total, (int, float))
            and not math.isnan(wave_gap_data_compute_total)
            and not math.isnan(wave_gap_instructions_total)
            and wave_gap_instructions_total > 0
            else math.nan
        )
        ideal_hw_sw_codesign_core_cycles = math.nan
        if (
            isinstance(steady_state_core_cycles, (int, float))
            and isinstance(wave_gap_data_compute_total, (int, float))
            and not math.isnan(steady_state_core_cycles)
            and not math.isnan(wave_gap_data_compute_total)
        ):
            ideal_hw_sw_codesign_core_cycles = steady_state_core_cycles - wave_gap_data_compute_total
            if ideal_hw_sw_codesign_core_cycles <= 0:
                ideal_hw_sw_codesign_core_cycles = math.nan
        row["ideal_hw_sw_codesign_core_cycles"] = ideal_hw_sw_codesign_core_cycles
        row["ideal_hw_sw_codesign_core_utilization_pct"] = _derive_utilization(
            row["macs"],
            ideal_hw_sw_codesign_core_cycles,
            active_pes,
        )
        row["macs_utilization_pct"] = row["core_level_macs_utilization_pct"]

        for detail in window_details:
            window_row = dict(case)
            window_row["case_name"] = row["case_name"]
            window_row["sweep_dim"] = row["sweep_dim"]
            window_row["result_dir"] = row["result_dir"]
            window_row.update(detail)
            window_row["mmio_config_pct"] = (
                100.0 * detail["mmio_config_instructions"] / detail["instructions"]
                if detail["instructions"] > 0
                else math.nan
            )
            window_row["data_compute_pct"] = (
                100.0 * detail["data_compute_instructions"] / detail["instructions"]
                if detail["instructions"] > 0
                else math.nan
            )
            window_row["control_pct"] = (
                100.0 * detail["control_instructions"] / detail["instructions"]
                if detail["instructions"] > 0
                else math.nan
            )
            window_rows.append(window_row)

        rows.append(row)
    if not rows:
        return pd.DataFrame(), pd.DataFrame()
    df = pd.DataFrame(rows)
    order = [str(dim) for dim in manifest.get("workload", {}).get("axis_order", ["oh", "ow", "ic", "oc"]) if str(dim) in df.columns]
    df["sweep_sort_key"] = df["sweep_dim"].map({name: idx for idx, name in enumerate(order)}).fillna(len(order))
    window_df = pd.DataFrame(window_rows)
    if not window_df.empty:
        sort_columns = [column for column in ["sweep_dim", *order, "case_name", "window_index"] if column in window_df.columns]
        window_df = window_df.sort_values(sort_columns)
    return df.sort_values(["sweep_sort_key", "sweep_dim", *order, "case_name"]).drop(columns=["sweep_sort_key"]), window_df


def _sanitize_image_stem(stem: str) -> str:
    sanitized = re.sub(r"[^A-Za-z0-9._-]+", "_", stem.strip())
    sanitized = sanitized.strip("._")
    return sanitized or "plot"


def _write_figure_image(fig: plt.Figure, image_dir: Path, stem: str) -> str:
    image_dir.mkdir(parents=True, exist_ok=True)
    filename = f"{_sanitize_image_stem(stem)}.png"
    image_path = image_dir / filename
    fig.tight_layout()
    fig.savefig(image_path, format="png", dpi=180, facecolor=fig.get_facecolor())
    plt.close(fig)
    return (Path("image") / filename).as_posix()


def _build_1d_sweep_plot(frame: pd.DataFrame, sweep_dim: str, image_dir: Path) -> str | None:
    if sweep_dim not in frame.columns:
        return None
    passed = frame[frame["passed"] == 1.0].copy()
    if passed.empty:
        return None
    passed = passed.sort_values(sweep_dim)
    metrics = [
        ("sim_time_ns", _metric_display_label("sim_time_ns")),
        ("ebreak_cycle", _metric_display_label("ebreak_cycle")),
        ("cluster_run_cycles", _metric_display_label("cluster_run_cycles")),
        ("dma_active_cycles", _metric_display_label("dma_active_cycles")),
        ("compute_dma_overlap_cycles", _metric_display_label("compute_dma_overlap_cycles")),
        ("compute_dma_overlap_pct_of_compute", _metric_display_label("compute_dma_overlap_pct_of_compute")),
        ("compute_dma_overlap_pct_of_dma", _metric_display_label("compute_dma_overlap_pct_of_dma")),
        ("active_pes", _metric_display_label("active_pes")),
        ("macs", _metric_display_label("macs")),
        ("gfops_per_sec", _metric_display_label("gfops_per_sec")),
        ("core_level_macs_utilization_pct", _metric_display_label("core_level_macs_utilization_pct")),
        ("cluster_level_macs_utilization_pct", _metric_display_label("cluster_level_macs_utilization_pct")),
    ]
    fig, axes = plt.subplots(6, 2, figsize=(12, 19), facecolor="#f7f2e8")
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
    return _write_figure_image(fig, image_dir, f"sweep_{frame['workload_key'].iloc[0]}_{sweep_dim}")


def _build_1d_profiling_plot(frame: pd.DataFrame, sweep_dim: str, image_dir: Path) -> str | None:
    if sweep_dim not in frame.columns:
        return None
    passed = frame[frame["passed"] == 1.0].copy()
    if passed.empty:
        return None
    metrics = [
        ("wave_gap_cycles_total", _metric_display_label("wave_gap_cycles_total")),
        ("wave_gap_instructions_total", _metric_display_label("wave_gap_instructions_total")),
        ("wave_gap_mmio_config_instructions_total", _metric_display_label("wave_gap_mmio_config_instructions_total")),
        ("wave_gap_data_compute_instructions_total", _metric_display_label("wave_gap_data_compute_instructions_total")),
        ("wave_gap_control_instructions_total", _metric_display_label("wave_gap_control_instructions_total")),
        ("boot_up_cycles", _metric_display_label("boot_up_cycles")),
        ("drain_out_cycles", _metric_display_label("drain_out_cycles")),
        ("steady_state_core_cycles", _metric_display_label("steady_state_core_cycles")),
        ("steady_state_core_utilization_pct", _metric_display_label("steady_state_core_utilization_pct")),
        ("ideal_hw_sw_codesign_core_utilization_pct", _metric_display_label("ideal_hw_sw_codesign_core_utilization_pct")),
    ]
    available_metrics = [
        (metric, label)
        for metric, label in metrics
        if metric in passed.columns and passed[metric].notna().any()
    ]
    if not available_metrics:
        return None
    passed = passed.sort_values(sweep_dim)
    rows = math.ceil(len(available_metrics) / 2)
    fig, axes = plt.subplots(rows, 2, figsize=(12, max(7, rows * 3.1)), facecolor="#f7f2e8")
    axes_list = list(axes.flat) if hasattr(axes, "flat") else [axes]
    x = passed[sweep_dim]
    for axis, (metric, label) in zip(axes_list, available_metrics):
        axis.plot(x, passed[metric], marker="o", color="#355c7d", linewidth=2.0)
        axis.set_title(label, fontsize=11)
        axis.set_xlabel(sweep_dim.upper())
        axis.grid(alpha=0.25, color="#7a8aa0")
        axis.set_facecolor("#fffdf8")
    for axis in axes_list[len(available_metrics):]:
        axis.remove()
    fig.suptitle(f"{frame['workload_key'].iloc[0]} {sweep_dim.upper()} profiling", fontsize=14, color="#28415c")
    return _write_figure_image(fig, image_dir, f"profiling_{frame['workload_key'].iloc[0]}_{sweep_dim}")


def _format_dimension_value(value: float) -> str:
    return str(int(value)) if float(value).is_integer() else f"{value:g}"


def _build_3d_scatter(frame: pd.DataFrame, dims: list[str], color_metric: str, axis_scale: str, image_dir: Path) -> str | None:
    if len(dims) != 3:
        raise ValueError("3D scatter expects exactly three dimensions")
    frame = _with_scatter_dimensions(frame, dims)
    for dim in dims:
        if dim not in frame.columns:
            raise ValueError(f"manifest does not contain dimension '{dim}'")
    passed = frame[(frame["passed"] == 1.0) & frame[color_metric].notna()].copy()
    if len(passed) < 2:
        return None
    if axis_scale not in {"linear", "log"}:
        raise ValueError("axis_scale must be 'linear' or 'log'")

    plot_frame = passed.copy()
    if axis_scale == "log":
        for dim in dims:
            plot_frame = plot_frame[plot_frame[dim] > 0]
        if len(plot_frame) < 2:
            return None

    # Multiple workloads can land on the same 3D coordinate when a non-plotted
    # dimension varies. Color each visible point by the best utilization seen at
    # that coordinate so overdraw does not hide the peak result.
    plot_frame = plot_frame.groupby(dims, as_index=False)[color_metric].max()
    if len(plot_frame) < 2:
        return None

    fig = plt.figure(figsize=(10, 8), facecolor="#f7f2e8")
    ax = fig.add_subplot(111, projection="3d")
    if axis_scale == "log":
        x = plot_frame[dims[0]].map(math.log10)
        y = plot_frame[dims[1]].map(math.log10)
        z = plot_frame[dims[2]].map(math.log10)
    else:
        x = plot_frame[dims[0]]
        y = plot_frame[dims[1]]
        z = plot_frame[dims[2]]

    scatter = ax.scatter(
        x,
        y,
        z,
        c=plot_frame[color_metric],
        cmap=cm.magma,
        s=70,
        edgecolors="#3d2d1f",
        linewidths=0.5,
        alpha=0.9,
    )
    if axis_scale == "log":
        for setter, dim in zip(
            (ax.set_xticks, ax.set_yticks, ax.set_zticks),
            dims,
        ):
            tick_values = sorted(float(value) for value in plot_frame[dim].unique())
            setter([math.log10(value) for value in tick_values])
        ax.set_xticklabels([_format_dimension_value(value) for value in sorted(float(value) for value in plot_frame[dims[0]].unique())])
        ax.set_yticklabels([_format_dimension_value(value) for value in sorted(float(value) for value in plot_frame[dims[1]].unique())])
        ax.set_zticklabels([_format_dimension_value(value) for value in sorted(float(value) for value in plot_frame[dims[2]].unique())])
        ax.set_xlabel(f"{_scatter_dimension_display_label(dims[0])} (log10)")
        ax.set_ylabel(f"{_scatter_dimension_display_label(dims[1])} (log10)")
        ax.set_zlabel(f"{_scatter_dimension_display_label(dims[2])} (log10)")
    else:
        ax.set_xlabel(_scatter_dimension_display_label(dims[0]))
        ax.set_ylabel(_scatter_dimension_display_label(dims[1]))
        ax.set_zlabel(_scatter_dimension_display_label(dims[2]))
    metric_label = _metric_display_label(color_metric)
    ax.set_title(f"3D sweep distribution ({axis_scale} scale) colored by {metric_label}")
    fig.colorbar(scatter, ax=ax, fraction=0.03, pad=0.08, label=metric_label)
    dim_suffix = "_".join(dims)
    return _write_figure_image(fig, image_dir, f"scatter_{color_metric}_{axis_scale}_{dim_suffix}")


def _summary_stats(frame: pd.DataFrame) -> dict[str, float | int]:
    return {
        "total": len(frame),
        "passed": int((frame["passed"] == 1.0).sum()),
        "failed": int((frame["result_state"] == "failed").sum()),
        "missing": int((frame["result_state"] == "missing").sum()),
        "avg_time": frame.loc[frame["sim_time_ns"].notna(), "sim_time_ns"].mean(),
        "avg_dma_active_cycles": frame.loc[frame["dma_active_cycles"].notna(), "dma_active_cycles"].mean(),
        "avg_overlap_cycles": frame.loc[frame["compute_dma_overlap_cycles"].notna(), "compute_dma_overlap_cycles"].mean(),
        "avg_overlap_compute_pct": frame.loc[frame["compute_dma_overlap_pct_of_compute"].notna(), "compute_dma_overlap_pct_of_compute"].mean(),
        "avg_overlap_dma_pct": frame.loc[frame["compute_dma_overlap_pct_of_dma"].notna(), "compute_dma_overlap_pct_of_dma"].mean(),
        "avg_wave_gap_cycles": frame.loc[frame["wave_gap_cycles_total"].notna(), "wave_gap_cycles_total"].mean(),
        "avg_boot_up_cycles": frame.loc[frame["boot_up_cycles"].notna(), "boot_up_cycles"].mean(),
        "avg_drain_out_cycles": frame.loc[frame["drain_out_cycles"].notna(), "drain_out_cycles"].mean(),
        "avg_gfops": frame.loc[frame["gfops_per_sec"].notna(), "gfops_per_sec"].mean(),
        "avg_active": frame.loc[frame["active_pes"].notna(), "active_pes"].mean(),
        "avg_core_util": frame.loc[frame["core_level_macs_utilization_pct"].notna(), "core_level_macs_utilization_pct"].mean(),
        "avg_cluster_util": frame.loc[frame["cluster_level_macs_utilization_pct"].notna(), "cluster_level_macs_utilization_pct"].mean(),
        "avg_steady_state_util": frame.loc[frame["steady_state_core_utilization_pct"].notna(), "steady_state_core_utilization_pct"].mean(),
        "avg_ideal_util": frame.loc[frame["ideal_hw_sw_codesign_core_utilization_pct"].notna(), "ideal_hw_sw_codesign_core_utilization_pct"].mean(),
        "max_gfops": frame.loc[frame["gfops_per_sec"].notna(), "gfops_per_sec"].max(),
        "max_core_util": frame.loc[frame["core_level_macs_utilization_pct"].notna(), "core_level_macs_utilization_pct"].max(),
        "max_cluster_util": frame.loc[frame["cluster_level_macs_utilization_pct"].notna(), "cluster_level_macs_utilization_pct"].max(),
        "max_steady_state_util": frame.loc[frame["steady_state_core_utilization_pct"].notna(), "steady_state_core_utilization_pct"].max(),
        "max_ideal_util": frame.loc[frame["ideal_hw_sw_codesign_core_utilization_pct"].notna(), "ideal_hw_sw_codesign_core_utilization_pct"].max(),
        "max_overlap_compute_pct": frame.loc[frame["compute_dma_overlap_pct_of_compute"].notna(), "compute_dma_overlap_pct_of_compute"].max(),
        "max_overlap_dma_pct": frame.loc[frame["compute_dma_overlap_pct_of_dma"].notna(), "compute_dma_overlap_pct_of_dma"].max(),
    }


def _format_summary_number(value: float | int, precision: str, suffix: str = "") -> str:
    return f"{format(float(value), precision)}{suffix}" if not pd.isna(value) else "-"


def _summary_cards(frame: pd.DataFrame) -> str:
    summary = _summary_stats(frame)
    cards = [
        ("Cases", str(summary["total"]), "manifest entries"),
        ("Passed", str(summary["passed"]), "completed simulations"),
        ("Failed", str(summary["failed"]), "sim.log present but not passing"),
        ("Missing", str(summary["missing"]), "results directory not found"),
        ("Avg sim time", _format_summary_number(summary["avg_time"], ".0f", " ns"), "passed cases"),
        ("Avg DMA active", _format_summary_number(summary["avg_dma_active_cycles"], ".0f"), "live simulator counter"),
        ("Avg overlap cycles", _format_summary_number(summary["avg_overlap_cycles"], ".0f"), "compute and DMA overlap"),
        ("Avg wave-gap cycles", _format_summary_number(summary["avg_wave_gap_cycles"], ".0f"), "sum of completed stop->start windows"),
        ("Avg boot-up", _format_summary_number(summary["avg_boot_up_cycles"], ".0f"), "core enable to first cluster start"),
        ("Avg drain-out", _format_summary_number(summary["avg_drain_out_cycles"], ".0f"), "last cluster stop to MCU end"),
        ("Avg overlap/compute", _format_summary_number(summary["avg_overlap_compute_pct"], ".2f", "%"), "live simulator counter"),
        ("Avg overlap/DMA", _format_summary_number(summary["avg_overlap_dma_pct"], ".2f", "%"), "live simulator counter"),
        ("Avg GFLOPS/sec", _format_summary_number(summary["avg_gfops"], ".3g"), "modeled sim time"),
        ("Max GFLOPS/sec", _format_summary_number(summary["max_gfops"], ".3g"), "modeled sim time"),
        ("Avg active PEs", _format_summary_number(summary["avg_active"], ".1f"), "max enabled per workload"),
        ("Avg core MAC util", _format_summary_number(summary["avg_core_util"], ".2f", "%"), "passed cases"),
        ("Max core MAC util", _format_summary_number(summary["max_core_util"], ".2f", "%"), "available runs"),
        ("Avg cluster MAC util", _format_summary_number(summary["avg_cluster_util"], ".2f", "%"), "passed cases"),
        ("Max cluster MAC util", _format_summary_number(summary["max_cluster_util"], ".2f", "%"), "available runs"),
        ("Avg steady-state util", _format_summary_number(summary["avg_steady_state_util"], ".2f", "%"), "excluding boot-up and drain-out"),
        ("Max steady-state util", _format_summary_number(summary["max_steady_state_util"], ".2f", "%"), "excluding boot-up and drain-out"),
        ("Avg ideal co-design util", _format_summary_number(summary["avg_ideal_util"], ".2f", "%"), "hide gap data-compute instructions"),
        ("Max ideal co-design util", _format_summary_number(summary["max_ideal_util"], ".2f", "%"), "hide gap data-compute instructions"),
        ("Max overlap/compute", _format_summary_number(summary["max_overlap_compute_pct"], ".2f", "%"), "available runs"),
        ("Max overlap/DMA", _format_summary_number(summary["max_overlap_dma_pct"], ".2f", "%"), "available runs"),
    ]
    return "".join(
        f'<div class="card"><div class="label">{escape(label)}</div><div class="value">{escape(value)}</div><div class="sub">{escape(sub)}</div></div>'
        for label, value, sub in cards
    )


def _window_summary_section(window_frame: pd.DataFrame, window_csv_name: str) -> str:
    if window_frame.empty:
        return (
            '<section class="section"><div class="section-head"><h2>Wave-gap Windows</h2><span>No detailed windows found</span></div>'
            '<div class="empty">This report did not find any per-window stop-&gt;start details in sim.log.</div></section>'
        )

    cards = [
        ("Window rows", str(len(window_frame)), "completed stop->start intervals exported"),
        ("Avg cycles/window", _format_summary_number(window_frame["cycles"].mean(), ".2f"), "per-window timing"),
        ("Avg instructions/window", _format_summary_number(window_frame["instructions"].mean(), ".2f"), "retired core instructions"),
        ("Avg MMIO/window", _format_summary_number(window_frame["mmio_config_instructions"].mean(), ".2f"), "MMIO configure instructions"),
        ("Avg data/window", _format_summary_number(window_frame["data_compute_instructions"].mean(), ".2f"), "data-compute instructions"),
        ("Avg control/window", _format_summary_number(window_frame["control_instructions"].mean(), ".2f"), "HDDU stop/start instructions"),
        ("Max cycles/window", _format_summary_number(window_frame["cycles"].max(), ".0f"), "worst observed gap"),
        ("Max instructions/window", _format_summary_number(window_frame["instructions"].max(), ".0f"), "worst observed gap"),
    ]
    preview_columns = [
        column
        for column in [
            "case_name",
            "window_index",
            "stop_cycle",
            "next_start_cycle",
            "cycles",
            "instructions",
            "mmio_config_instructions",
            "data_compute_instructions",
            "control_instructions",
        ]
        if column in window_frame.columns
    ]
    preview_html = _format_table(window_frame.head(40), preview_columns)
    cards_html = "".join(
        f'<div class="card"><div class="label">{escape(label)}</div><div class="value">{escape(value)}</div><div class="sub">{escape(sub)}</div></div>'
        for label, value, sub in cards
    )
    return "".join(
        [
            '<section class="section"><div class="section-head"><h2>Wave-gap Windows</h2>',
            f'<span>Full detail exported to <a class="report-link" href="{escape(window_csv_name)}">{escape(window_csv_name)}</a></span></div>',
            f'<div class="card-grid">{cards_html}</div>',
            '<p class="hero-note">Previewing the first 40 completed stop-&gt;start windows. Use the CSV for the full per-window dataset.</p>',
            preview_html,
            '</section>',
        ]
    )


def _profiling_timeline_section() -> str:
    explainer_cards = [
        (
            "Boot-up",
            "boot_up_cycles / boot_up_instructions",
            "From core enable to the first cluster START. This is setup cost before steady-state wave execution begins.",
        ),
        (
            "Steady-state",
            "core_probe_cycles_total / steady_state_core_cycles",
            "The middle region where waves execute. steady_state_core_cycles removes boot-up and drain-out from the lifecycle probe.",
        ),
        (
            "Wave gaps",
            "wave_gap_windows / wave_gap_cycles_total / wave_gap_partial_windows_dropped",
            "Only completed STOP->next START windows are accumulated. Dropped partial windows mean a STOP did not see a later START before program end.",
        ),
        (
            "Gap instruction mix",
            "wave_gap_instructions_total and breakdown columns",
            "Each completed gap is split into MMIO configure, data compute, and START/STOP control instructions.",
        ),
        (
            "Busy vs DMA",
            "cluster_run_cycles / dma_active_cycles / compute_dma_overlap_cycles",
            "These are overlays, not the same baseline. overlap only counts the intersection between cluster busy time and DMA active time.",
        ),
        (
            "EBREAK vs lifecycle probe",
            "ebreak_cycle vs core_probe_cycles_total",
            "ebreak_cycle is the TB-visible firmware end point. core_probe_cycles_total is the lifecycle basis used for boot-up, steady-state, and drain-out accounting.",
        ),
    ]
    formula_cards = [
        (
            "Steady-state cycles",
            "steady_state_core_cycles = core_probe_cycles_total - boot_up_cycles - drain_out_cycles",
            "Use this when you want the middle part only, without startup or shutdown overhead.",
        ),
        (
            "Steady-state utilization",
            "steady_state_core_utilization_pct = 100 * MACs / (steady_state_core_cycles * active_pes * 4)",
            "This treats only the steady-state middle segment as the time budget.",
        ),
        (
            "Ideal HW/SW co-design",
            "ideal_hw_sw_codesign_core_utilization_pct = 100 * MACs / ((steady_state_core_cycles - wave_gap_data_compute_instructions_total) * active_pes * 4)",
            "Assumption: if gap data-compute instructions can be fully hidden, one retired data-compute instruction saves about one core cycle.",
        ),
    ]
    cards_html = "".join(
        f'<div class="timeline-chip"><div class="chip-title">{escape(title)}</div><div class="chip-metric">{escape(metric)}</div><p>{escape(description)}</p></div>'
        for title, metric, description in explainer_cards
    )
    formulas_html = "".join(
        f'<div class="formula-card"><div class="formula-label">{escape(title)}</div><div class="formula-math">{escape(formula)}</div><div class="formula-note">{escape(note)}</div></div>'
        for title, formula, note in formula_cards
    )
    return "".join(
        [
            '<section class="section">',
            '<div class="section-head timeline-head"><h2>Profiling Timeline</h2><span>How the counters map onto time</span></div>',
            '<p class="timeline-intro">Read the profiling numbers as a single left-to-right firmware timeline. The lifecycle probe covers boot-up, repeated wave execution, and drain-out. Inside the steady-state middle region, the report overlays cluster busy time, DMA activity, and completed STOP-&gt;START wave gaps.</p>',
            '<div class="timeline-svg-wrap">',
            '<svg class="timeline-svg" viewBox="0 0 1380 450" role="img" aria-label="Timeline explaining boot-up, steady-state, wave gaps, DMA overlap, and drain-out metrics">',
            '<defs><marker id="timeline-arrow" markerWidth="10" markerHeight="10" refX="8" refY="3" orient="auto"><path d="M0,0 L0,6 L9,3 z" fill="#8b5e34"></path></marker></defs>',
            '<text x="36" y="40" fill="#2f251b" font-size="24" font-weight="700">How to read the profiling counters</text>',
            '<line x1="250" y1="70" x2="1280" y2="70" stroke="#8b5e34" stroke-width="3" marker-end="url(#timeline-arrow)"></line>',
            '<text x="250" y="54" fill="#6d5c4b" font-size="12">earlier</text>',
            '<text x="1170" y="54" fill="#6d5c4b" font-size="12">later / program end</text>',
            '<text x="36" y="116" fill="#2f251b" font-size="16" font-weight="700">Core lifecycle</text>',
            '<text x="36" y="136" fill="#6d5c4b" font-size="12">core_probe_cycles_total</text>',
            '<rect x="250" y="92" width="180" height="38" rx="14" fill="#c97d60"></rect>',
            '<rect x="438" y="92" width="626" height="38" rx="14" fill="#355c7d"></rect>',
            '<rect x="1072" y="92" width="198" height="38" rx="14" fill="#7b8f6a"></rect>',
            '<text x="306" y="116" fill="#fffaf3" font-size="13" font-weight="700">Boot-up</text>',
            '<text x="668" y="116" fill="#f7fbff" font-size="13" font-weight="700">Steady-state wave execution</text>',
            '<text x="1130" y="116" fill="#f7fbf2" font-size="13" font-weight="700">Drain-out</text>',
            '<text x="438" y="152" fill="#6d5c4b" font-size="12">steady_state_core_cycles = middle section only</text>',
            '<text x="36" y="202" fill="#2f251b" font-size="16" font-weight="700">Inside steady-state</text>',
            '<text x="36" y="222" fill="#6d5c4b" font-size="12">cluster run slices and stop-&gt;start gaps</text>',
            '<rect x="250" y="178" width="180" height="38" rx="14" fill="#e8decd"></rect>',
            '<rect x="438" y="178" width="142" height="38" rx="14" fill="#3f6b8c"></rect>',
            '<rect x="590" y="178" width="108" height="38" rx="14" fill="#d8b991"></rect>',
            '<rect x="708" y="178" width="154" height="38" rx="14" fill="#3f6b8c"></rect>',
            '<rect x="872" y="178" width="108" height="38" rx="14" fill="#d8b991"></rect>',
            '<rect x="990" y="178" width="132" height="38" rx="14" fill="#3f6b8c"></rect>',
            '<rect x="1132" y="178" width="138" height="38" rx="14" fill="#e8decd"></rect>',
            '<text x="474" y="202" fill="#f7fbff" font-size="12" font-weight="700">Cluster run</text>',
            '<text x="621" y="202" fill="#4f3b27" font-size="12" font-weight="700">Wave gap</text>',
            '<text x="755" y="202" fill="#f7fbff" font-size="12" font-weight="700">Cluster run</text>',
            '<text x="903" y="202" fill="#4f3b27" font-size="12" font-weight="700">Wave gap</text>',
            '<text x="1027" y="202" fill="#f7fbff" font-size="12" font-weight="700">Cluster run</text>',
            '<text x="438" y="238" fill="#6d5c4b" font-size="12">wave_gap_cycles_total = sum of every completed gap segment above</text>',
            '<text x="36" y="290" fill="#2f251b" font-size="16" font-weight="700">DMA and overlap overlay</text>',
            '<text x="36" y="310" fill="#6d5c4b" font-size="12">dma_active_cycles and compute_dma_overlap_cycles</text>',
            '<rect x="360" y="264" width="910" height="58" rx="18" fill="#fffaf3" stroke="#d8c4a5"></rect>',
            '<text x="382" y="287" fill="#6d5c4b" font-size="11" font-weight="700">DMA active</text>',
            '<rect x="466" y="274" width="368" height="16" rx="8" fill="#61a5a8"></rect>',
            '<rect x="940" y="274" width="270" height="16" rx="8" fill="#61a5a8"></rect>',
            '<text x="382" y="312" fill="#6d5c4b" font-size="11" font-weight="700">Overlap only</text>',
            '<rect x="580" y="299" width="70" height="16" rx="8" fill="#1f7a8c"></rect>',
            '<rect x="720" y="299" width="90" height="16" rx="8" fill="#1f7a8c"></rect>',
            '<rect x="1030" y="299" width="150" height="16" rx="8" fill="#1f7a8c"></rect>',
            '<text x="548" y="344" fill="#6d5c4b" font-size="12">compute_dma_overlap_cycles = intersection(cluster_run_cycles, dma_active_cycles)</text>',
            '<text x="36" y="390" fill="#2f251b" font-size="16" font-weight="700">Wave-gap instruction mix</text>',
            '<text x="36" y="410" fill="#6d5c4b" font-size="12">wave_gap_instructions_total</text>',
            '<rect x="340" y="374" width="170" height="34" rx="12" fill="#b66b4d"></rect>',
            '<rect x="518" y="374" width="524" height="34" rx="12" fill="#4b8f8c"></rect>',
            '<rect x="1050" y="374" width="160" height="34" rx="12" fill="#a86f3a"></rect>',
            '<text x="376" y="395" fill="#fffaf3" font-size="12" font-weight="700">MMIO configure</text>',
            '<text x="722" y="395" fill="#f4fffe" font-size="12" font-weight="700">Data compute</text>',
            '<text x="1078" y="395" fill="#fff8f0" font-size="12" font-weight="700">START/STOP control</text>',
            '<text x="1050" y="430" fill="#6d5c4b" font-size="12">ideal HW/SW co-design removes only the data-compute slice</text>',
            '</svg>',
            '</div>',
            f'<div class="timeline-chip-grid">{cards_html}</div>',
            f'<div class="formula-grid">{formulas_html}</div>',
            '</section>',
        ]
    )


def _format_table(frame: pd.DataFrame, columns: list[str]) -> str:
    table = frame[columns].copy()
    for column in table.columns:
        if pd.api.types.is_float_dtype(table[column]):
            table[column] = table[column].map(
                lambda value: "-"
                if pd.isna(value)
                else (str(int(value)) if float(value).is_integer() else f"{value:.4g}")
            )
    table.columns = [
        _scatter_dimension_display_label(str(dim)) if str(dim) in _SCATTER_DIMENSION_LABELS else _metric_display_label(str(dim))
        for dim in table.columns
    ]
    return table.to_html(index=False, classes="report-table", border=0, justify="center")


def _render_report_html(
    manifest: dict[str, Any],
    frame: pd.DataFrame,
    plots_1d: dict[str, str | None],
    profiling_plots: dict[str, str | None],
    scatter_plots: dict[str, dict[str, str | None]],
    scatter_dims: list[str],
    scatter_metrics: list[str],
    window_frame: pd.DataFrame,
    window_csv_name: str,
) -> str:
    summary = _summary_stats(frame)
    sweep_sections = []
    for sweep_dim, group in frame.groupby("sweep_dim", sort=False):
        raw_dims = group.iloc[0].get("sweep_dims", [sweep_dim])
        sweep_dims = [str(dim) for dim in raw_dims if str(dim) in group.columns] if isinstance(raw_dims, list) else [sweep_dim] if sweep_dim in group.columns else []
        section_title = " x ".join(_scatter_dimension_display_label(dim) for dim in sweep_dims) if sweep_dims else sweep_dim.upper()
        performance_columns = [
            *sweep_dims,
            "ebreak_cycle",
            "cluster_run_cycles",
            "dma_active_cycles",
            "compute_dma_overlap_cycles",
            "compute_dma_overlap_pct_of_compute",
            "compute_dma_overlap_pct_of_dma",
            "sim_time_ns",
            "gfops_per_sec",
            "active_pes",
            "active_pe_ratio",
            "macs",
            "core_level_macs_utilization_pct",
            "cluster_level_macs_utilization_pct",
            "result_state",
        ]
        profiling_columns = [
            *sweep_dims,
            "wave_gap_windows",
            "wave_gap_partial_windows_dropped",
            "wave_gap_cycles_total",
            "wave_gap_instructions_total",
            "wave_gap_mmio_config_instructions_total",
            "wave_gap_data_compute_instructions_total",
            "wave_gap_control_instructions_total",
            "boot_up_cycles",
            "drain_out_cycles",
            "core_probe_cycles_total",
            "steady_state_core_cycles",
            "steady_state_core_utilization_pct",
            "ideal_hw_sw_codesign_core_utilization_pct",
            "result_state",
        ]
        sort_columns = [*sweep_dims, "case_name"] if sweep_dims else ["case_name"]
        plot_uri = plots_1d.get(sweep_dim)
        profiling_plot_uri = profiling_plots.get(sweep_dim)
        image_html = (
            f'<img src="{plot_uri}" alt="{escape(sweep_dim)} sweep plot" class="plot">'
            if plot_uri is not None
            else '<div class="empty">1D plot is only available when exactly one sweep dimension varies.</div>'
        )
        profiling_image_html = (
            f'<img src="{profiling_plot_uri}" alt="{escape(sweep_dim)} profiling plot" class="plot">'
            if profiling_plot_uri is not None
            else '<div class="empty">Profiling plots require parsed wave-gap, boot-up, or drain-out metrics.</div>'
        )
        sorted_group = group.sort_values(sort_columns)
        sweep_sections.append(
            "".join(
                [
                    f'<section class="section"><div class="section-head"><h2>{escape(section_title)} Sweep</h2><span>{escape(group.iloc[0]["workload_key"])} workload</span></div>',
                    image_html,
                    '<h3>Performance table</h3>',
                    _format_table(
                        sorted_group,
                        performance_columns,
                    ),
                    '<div class="section-head subhead"><h2>Profiling</h2><span>Wave-gap, boot-up, drain-out, and idealized co-design efficiency</span></div>',
                    profiling_image_html,
                    _format_table(
                        sorted_group,
                        profiling_columns,
                    ),
                    "</section>",
                ]
            )
        )

    scatter_parts = []
    for metric in scatter_metrics:
        metric_label = _metric_display_label(metric)
        metric_plots = scatter_plots.get(metric, {})
        metric_sections = []
        for axis_scale, title in (("linear", "Linear scale"), ("log", "Log scale")):
            plot_uri = metric_plots.get(axis_scale)
            if plot_uri is None:
                metric_sections.append(
                    f'<div class="scatter-block"><h3>{escape(title)}</h3><div class="empty">Not enough completed runs to render the {escape(axis_scale)} 3D scatter plot for {escape(metric_label)}.</div></div>'
                )
            else:
                metric_sections.append(
                    f'<div class="scatter-block"><h3>{escape(title)}</h3><img src="{plot_uri}" alt="{escape(axis_scale)} 3D sweep scatter for {escape(metric_label)}" class="plot plot-wide"></div>'
                )
        scatter_parts.append(
            "".join(
                [
                    f'<section class="section"><div class="section-head"><h2>{escape(metric_label)}</h2><span>Distribution over {escape(', '.join(_scatter_dimension_display_label(dim) for dim in scatter_dims))}</span></div>',
                    *metric_sections,
                    "</section>",
                ]
            )
        )
    scatter_html = "".join(scatter_parts)
    timeline_section = _profiling_timeline_section()
    window_section = _window_summary_section(window_frame, window_csv_name)

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
    .hero-note {{ margin-top: 16px; max-width: 960px; }}
    .pill {{ border-radius: 999px; padding: 8px 12px; background: rgba(255,255,255,0.7); border: 1px solid var(--line); color: var(--muted); font-size: 13px; }}
    .report-link {{ color: var(--accent); font-weight: 600; text-decoration: none; }}
    .report-link:hover {{ text-decoration: underline; }}
    .card-grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 14px; margin-top: 22px; }}
    .card {{ background: var(--panel); border: 1px solid var(--line); border-radius: 18px; padding: 16px; box-shadow: var(--shadow); }}
    .card .label {{ color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: 0.08em; }}
    .card .value {{ margin-top: 10px; font-size: 28px; font-weight: 700; }}
    .card .sub {{ margin-top: 6px; color: var(--muted); font-size: 13px; }}
    .section {{ margin-top: 24px; background: var(--panel); border: 1px solid var(--line); border-radius: 24px; padding: 22px; box-shadow: var(--shadow); }}
    .section-head {{ display: flex; justify-content: space-between; align-items: baseline; gap: 12px; margin-bottom: 16px; }}
    .subhead {{ margin-top: 26px; }}
    .section-head h2 {{ margin: 0; font-size: 24px; }}
    .section h3 {{ margin: 0 0 12px; font-size: 18px; }}
    .section-head span {{ color: var(--muted); }}
    .timeline-intro {{ margin: 0; color: var(--muted); line-height: 1.6; max-width: 1080px; }}
    .timeline-head {{ align-items: flex-start; flex-direction: column; gap: 4px; }}
    .timeline-head span {{ font-size: 13px; }}
    .timeline-svg-wrap {{ margin-top: 18px; padding: 18px; border: 1px solid var(--line); border-radius: 22px; background: linear-gradient(180deg, rgba(255,255,255,0.78), rgba(244,237,225,0.9)); overflow-x: auto; }}
    .timeline-svg {{ width: 100%; min-width: 1120px; height: auto; display: block; }}
    .timeline-chip-grid {{ display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 14px; margin-top: 18px; }}
    .timeline-chip {{ background: var(--panel-strong); border: 1px solid var(--line); border-radius: 18px; padding: 16px; height: 100%; }}
    .chip-title {{ color: var(--text); font-size: 14px; font-weight: 700; }}
    .chip-metric {{ margin-top: 6px; color: var(--accent); font-family: "IBM Plex Mono", "SFMono-Regular", monospace; font-size: 11px; font-weight: 600; line-height: 1.45; word-break: break-word; }}
    .timeline-chip p {{ margin: 8px 0 0; color: var(--muted); font-size: 13px; line-height: 1.5; }}
    .formula-grid {{ display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 14px; margin-top: 18px; }}
    .formula-card {{ background: rgba(255,255,255,0.8); border: 1px solid var(--line); border-radius: 18px; padding: 16px; height: 100%; }}
    .formula-label {{ color: var(--text); font-size: 14px; font-weight: 700; }}
    .formula-math {{ margin-top: 10px; color: #28415c; font-family: "IBM Plex Mono", "SFMono-Regular", monospace; font-size: 12px; line-height: 1.6; word-break: break-word; }}
    .formula-note {{ margin-top: 8px; color: var(--muted); font-size: 12px; line-height: 1.5; }}
    .plot {{ width: 100%; border-radius: 18px; border: 1px solid rgba(0,0,0,0.05); background: #fffdf8; margin-bottom: 18px; }}
    .plot-wide {{ max-height: 760px; object-fit: contain; }}
    .scatter-block + .scatter-block {{ margin-top: 24px; }}
    .scatter-block h3 {{ margin: 0 0 12px; font-size: 18px; }}
    .report-table {{ width: 100%; border-collapse: collapse; background: var(--panel-strong); overflow: hidden; border-radius: 16px; }}
    .report-table th, .report-table td {{ padding: 10px 12px; border-bottom: 1px solid rgba(0,0,0,0.06); text-align: center; font-size: 13px; }}
    .report-table th {{ background: rgba(139, 94, 52, 0.08); color: var(--muted); text-transform: uppercase; letter-spacing: 0.05em; font-size: 12px; }}
    .report-table th {{ white-space: normal; overflow-wrap: anywhere; word-break: break-word; }}
    .empty {{ border: 1px dashed var(--line); border-radius: 18px; padding: 28px; text-align: center; color: var(--muted); background: rgba(255,255,255,0.56); }}
        @media (max-width: 1180px) {{
            .timeline-chip-grid, .formula-grid {{ grid-template-columns: repeat(2, minmax(0, 1fr)); }}
        }}
    @media (max-width: 860px) {{
      main {{ padding: 14px; }}
      .section-head {{ flex-direction: column; align-items: flex-start; }}
            .timeline-chip-grid, .formula-grid {{ grid-template-columns: 1fr; }}
      .report-table th, .report-table td {{ padding: 8px; font-size: 12px; }}
    }}
  </style>
</head>
<body>
  <main>
    <section class="hero">
      <h1>{escape(manifest['suite_name'])} sweep report</h1>
                                                <p>Generated from manifest plus e2e output directories. The report summarizes core-level cycles to EBREAK, cluster busy cycles gathered directly from simulator HDDU/AGU activity, DMA active cycles, compute/DMA overlap gathered live during simulation, wave-gap stop-&gt;start profiling, boot-up/drain-out costs, and an idealized HW/SW co-design utilization estimate that hides gap data-compute instructions after removing boot-up and drain-out time.</p>
            <div class="meta">
                <span class="pill">workload={escape(manifest['workload']['key'])}</span>
                <span class="pill">op={escape(manifest['workload']['op_type'])}</span>
                {f'<span class="pill">padding={escape(str(manifest["workload"]["padding"]))}</span>' if 'padding' in manifest['workload'] else ''}
                {f'<span class="pill">activation={escape(manifest["workload"]["activation"])}</span>' if 'activation' in manifest['workload'] else ''}
                <span class="pill">scatter={escape(', '.join(_scatter_dimension_display_label(dim) for dim in scatter_dims))}</span>
                                <span class="pill">scatter metrics={escape(', '.join(_metric_display_label(metric) for metric in scatter_metrics))}</span>
                                <span class="pill">max core util={escape(_format_summary_number(summary["max_core_util"], ".2f", "%"))}</span>
                                <span class="pill">max cluster util={escape(_format_summary_number(summary["max_cluster_util"], ".2f", "%"))}</span>
                                                                <span class="pill">max ideal co-design util={escape(_format_summary_number(summary["max_ideal_util"], ".2f", "%"))}</span>
                                                                <span class="pill">max overlap/compute={escape(_format_summary_number(summary["max_overlap_compute_pct"], ".2f", "%"))}</span>
                                                                <span class="pill">max overlap/DMA={escape(_format_summary_number(summary["max_overlap_dma_pct"], ".2f", "%"))}</span>
                                <span class="pill">max GFLOPS/sec={escape(_format_summary_number(summary["max_gfops"], ".3g"))}</span>
            </div>
                                                                                                                                                                                                <p class="hero-note">Peak observed results: core MAC utilization {escape(_format_summary_number(summary["max_core_util"], ".2f", "%"))}, cluster MAC utilization {escape(_format_summary_number(summary["max_cluster_util"], ".2f", "%"))}, ideal HW/SW co-design utilization {escape(_format_summary_number(summary["max_ideal_util"], ".2f", "%"))}, overlap/compute ratio {escape(_format_summary_number(summary["max_overlap_compute_pct"], ".2f", "%"))}, overlap/DMA ratio {escape(_format_summary_number(summary["max_overlap_dma_pct"], ".2f", "%"))}, and throughput {escape(_format_summary_number(summary["max_gfops"], ".3g"))} GFLOPS/sec.</p>
      <div class="card-grid">{_summary_cards(frame)}</div>
    </section>
        {timeline_section}
        {window_section}
        {scatter_html}
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
    image_dir = output_dir / "image"
    image_dir.mkdir(parents=True, exist_ok=True)

    frame, window_frame = _collect_report_rows(manifest, results_root)
    csv_path = output_dir / "sweep_metrics.csv"
    frame.to_csv(csv_path, index=False)
    window_csv_path = output_dir / "wave_gap_windows.csv"
    window_frame.to_csv(window_csv_path, index=False)

    workload_key = str(manifest.get("workload", {}).get("key", ""))
    profile = _PROFILES.get(workload_key)
    if profile is not None and args.scatter_dims is None:
        default_scatter_dims = list(profile.default_scatter_dims)
    else:
        default_scatter_dims = [
            str(dim)
            for dim in manifest.get("workload", {}).get("default_scatter_dims", ["ic", "ow", "oh_oc_product"])
        ]
    scatter_dims = _parse_scatter_dimensions(args.scatter_dims, default_scatter_dims)
    if len(scatter_dims) != 3:
        raise ValueError("--scatter-dims expects exactly three comma-separated dimensions")
    scatter_metrics = _parse_metric_names(args.color_metric)
    for metric in scatter_metrics:
        if metric not in frame.columns:
            raise ValueError(f"metric column '{metric}' is not available in the report data")

    plots_1d = {
        sweep_dim: _build_1d_sweep_plot(group, sweep_dim, image_dir)
        for sweep_dim, group in frame.groupby("sweep_dim", sort=False)
    }
    profiling_plots = {
        sweep_dim: _build_1d_profiling_plot(group, sweep_dim, image_dir)
        for sweep_dim, group in frame.groupby("sweep_dim", sort=False)
    }
    scatter_plots = {
        metric: {
            "linear": _build_3d_scatter(frame, scatter_dims, metric, "linear", image_dir),
            "log": _build_3d_scatter(frame, scatter_dims, metric, "log", image_dir),
        }
        for metric in scatter_metrics
    }
    html = _render_report_html(
        manifest,
        frame,
        plots_1d,
        profiling_plots,
        scatter_plots,
        scatter_dims,
        scatter_metrics,
        window_frame,
        window_csv_path.name,
    )
    html_path = output_dir / "report.html"
    html_path.write_text(html, encoding="utf-8")

    print(f"CSV: {csv_path}")
    print(f"Wave-gap CSV: {window_csv_path}")
    print(f"Images: {image_dir}")
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
    gen.add_argument("--mode", choices=("product", "onehot"), default="product", help="Sweep expansion mode")
    gen.add_argument("--dimensions", type=str, default=None, help="Comma-separated sweep dimensions; defaults depend on the workload profile")
    gen.add_argument("--oh-values", "--h-values", dest="oh_values", type=str, default=None, help="Comma-separated OH values")
    gen.add_argument("--ic-values", type=str, default=None, help="Comma-separated IC values")
    gen.add_argument("--k-values", type=str, default=None, help="Comma-separated K values for GEMM sweeps")
    gen.add_argument("--m-values", type=str, default=None, help="Comma-separated M values for GEMM sweeps")
    gen.add_argument("--n-values", type=str, default=None, help="Comma-separated N values for GEMM sweeps")
    gen.add_argument("--oc-values", type=str, default=None, help="Comma-separated OC values")
    gen.add_argument("--ow-values", "--w-values", dest="ow_values", type=str, default=None, help="Comma-separated OW values")
    gen.add_argument("--stride", type=int, default=1, help="Stride value written into attrs")
    gen.add_argument("--padding", type=int, default=0, help="Padding value (conv3x3 only)")
    gen.add_argument("--activation", type=str, default="none", help="Output activation: none or relu")
    gen.set_defaults(func=_generate_suite)

    report = subparsers.add_parser("report", help="Parse sweep outputs and build an HTML report")
    report.add_argument("--manifest", type=Path, required=True, help="Manifest JSON emitted by hacc-sweep gen")
    report.add_argument("--results-root", type=Path, default=None, help="Root directory passed to run_e2e.sh --output-dir")
    report.add_argument("--output-dir", type=Path, required=True, help="Directory for CSV, HTML report, and exported plot images")
    report.add_argument("--scatter-dims", type=str, default=None, help="Three comma-separated dimensions for the 3D scatter; defaults depend on the workload profile")
    report.add_argument("--color-metric", type=str, default="core_level_macs_utilization_pct,cluster_level_macs_utilization_pct,ideal_hw_sw_codesign_core_utilization_pct", help="Comma-separated metric columns used for 3D scatter colors")
    report.set_defaults(func=_report_suite)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())