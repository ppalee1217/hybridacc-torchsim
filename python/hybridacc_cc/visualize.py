"""Static HardwareIR visualization output for hybridacc-cc.

Generates a self-contained HTML page for scan-chain topology,
SPM layout, DMA transport, and 4D AGU iter/stride views.
"""

from __future__ import annotations

import base64
import colorsys
import math
from html import escape
from io import BytesIO
from pathlib import Path

from mpl_toolkits.mplot3d import proj3d

from .ir import AguBankConfig, HardwareDesc, HardwareIR, LayerHwConfig, ScanChainEntry


_ROUTE_MODE_META = {
    0: {
        "symbol": "(IL, OL)",
        "name": "PLI_FROM_LN_PLO_TO_LN",
        "summary": "PLI from local link, PLO to local link",
        "class_name": "mode-ln-ln",
    },
    1: {
        "symbol": "(IB, OL)",
        "name": "PLI_FROM_BUS_PLO_TO_LN",
        "summary": "PLI from bus, PLO to local link",
        "class_name": "mode-bus-ln",
    },
    2: {
        "symbol": "(IL, OB)",
        "name": "PLI_FROM_LN_PLO_TO_BUS",
        "summary": "PLI from local link, PLO to bus",
        "class_name": "mode-ln-bus",
    },
    3: {
        "symbol": "(IB, OB)",
        "name": "PLI_FROM_BUS_PLO_TO_BUS",
        "summary": "PLI from bus, PLO to bus",
        "class_name": "mode-bus-bus",
    },
}

_SPM_GROUP_META = [
    ("ps", "PS", "Weights", "#1f6feb"),
    ("pd", "PD", "Activations", "#2da44e"),
    ("pli", "PLI", "Initial partial sum", "#d29922"),
    ("plo", "PLO", "Output partial sum", "#bf3989"),
]

_AGU_GROUP_META = [
    ("agu_ps", "AGU PS", "Weights"),
    ("agu_pd", "AGU PD", "Activations"),
    ("agu_pli", "AGU PLI", "Initial partial sum"),
    ("agu_plo", "AGU PLO", "Output partial sum"),
]

_BANK_TAG_META = {
    "agu_ps": {"scan_attr": "ps_id", "scan_label": "PS scan-chain id"},
    "agu_pd": {"scan_attr": "pd_id", "scan_label": "PD scan-chain id"},
    "agu_pli": {"scan_attr": "pli_id", "scan_label": "PLI scan-chain id"},
    "agu_plo": {"scan_attr": "plo_id", "scan_label": "PLO scan-chain id"},
}

_TIME_PALETTE = (
        (0.00, "#1f6feb"),
        (0.35, "#2da44e"),
        (0.70, "#d29922"),
        (1.00, "#d73a49"),
)


def dump_hardware_visualization(hw_ir: HardwareIR, path: Path) -> None:
    """Write a self-contained HTML visualization for a HardwareIR dump."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(render_hardware_visualization(hw_ir), encoding="utf-8")


def render_hardware_visualization(hw_ir: HardwareIR) -> str:
    """Render a HardwareIR to HTML."""
    header = _render_header(hw_ir)
    layers = "\n".join(
        _render_layer(layer, hw_ir.hardware, idx)
        for idx, layer in enumerate(hw_ir.layers)
    )
    legend = _render_route_legend()

    return f"""<!DOCTYPE html>
<html lang=\"en\">
<head>
  <meta charset=\"utf-8\">
  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
  <title>{escape(hw_ir.workload_name)} hardware visualization</title>
  <style>
    :root {{
      --bg: #f4efe6;
      --panel: #fffdf8;
      --panel-strong: #f7f1e6;
      --line: #d8cbb4;
      --text: #231f1a;
      --muted: #62584c;
      --accent: #8b5e34;
      --accent-soft: #e7d7bf;
      --shadow: 0 16px 40px rgba(62, 37, 12, 0.10);
      --radius: 18px;
      --mono: "SFMono-Regular", Consolas, "Liberation Mono", Menlo, monospace;
      --sans: "Segoe UI", "Noto Sans", Ubuntu, sans-serif;
    }}

    * {{ box-sizing: border-box; }}
    html, body {{ margin: 0; padding: 0; background: radial-gradient(circle at top left, #fff8eb 0%, var(--bg) 48%, #efe6d7 100%); color: var(--text); font-family: var(--sans); }}
    body {{ padding: 24px; }}
    main {{ max-width: 1500px; margin: 0 auto; }}
    h1, h2, h3, h4, p {{ margin: 0; }}
    .hero {{ background: linear-gradient(135deg, rgba(139, 94, 52, 0.14), rgba(255,255,255,0.86)); border: 1px solid var(--line); border-radius: 28px; padding: 28px; box-shadow: var(--shadow); margin-bottom: 24px; }}
    .hero-grid {{ display: grid; grid-template-columns: 2fr 1fr; gap: 16px; align-items: end; }}
    .hero h1 {{ font-size: 34px; letter-spacing: -0.04em; margin-bottom: 10px; }}
    .hero p {{ color: var(--muted); line-height: 1.5; max-width: 68ch; }}
    .summary-grid {{ display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 12px; }}
    .summary-card {{ background: rgba(255,255,255,0.65); border: 1px solid var(--line); border-radius: 16px; padding: 14px; min-height: 96px; }}
    .summary-card .label {{ color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: 0.08em; }}
    .summary-card .value {{ margin-top: 10px; font-size: 24px; font-weight: 700; line-height: 1.05; }}
    .summary-card .value.compact {{ font-size: 20px; letter-spacing: -0.02em; }}
    .summary-card .sub {{ margin-top: 6px; color: var(--muted); font-size: 13px; }}
    .legend {{ display: flex; flex-wrap: wrap; gap: 10px; margin-bottom: 20px; }}
    .legend-item {{ display: inline-flex; align-items: center; gap: 8px; background: var(--panel); border: 1px solid var(--line); border-radius: 999px; padding: 8px 12px; font-size: 13px; color: var(--muted); }}
    .swatch {{ width: 14px; height: 14px; border-radius: 4px; border: 1px solid rgba(0,0,0,0.08); }}
    .swatch.mode-ln-ln {{ background: #76d9c8; }}
    .swatch.mode-bus-ln {{ background: #f5c76a; }}
    .swatch.mode-ln-bus {{ background: #f1a28e; }}
    .swatch.mode-bus-bus {{ background: #8bb8f0; }}
    .layer {{ background: rgba(255,255,255,0.82); border: 1px solid var(--line); border-radius: 28px; padding: 24px; box-shadow: var(--shadow); margin-bottom: 24px; }}
    .layer-head {{ display: flex; justify-content: space-between; gap: 12px; align-items: baseline; margin-bottom: 18px; }}
    .layer-title {{ display: flex; align-items: baseline; gap: 12px; flex-wrap: wrap; }}
    .layer-title h2 {{ font-size: 26px; letter-spacing: -0.03em; }}
    .pill-row {{ display: flex; flex-wrap: wrap; gap: 8px; }}
    .pill {{ background: var(--panel-strong); border: 1px solid var(--line); border-radius: 999px; padding: 6px 10px; font-size: 12px; color: var(--muted); }}
    .panel-stack {{ display: grid; gap: 18px; margin-bottom: 18px; }}
    .panel {{ background: var(--panel); border: 1px solid var(--line); border-radius: var(--radius); padding: 18px; }}
    .panel h3 {{ font-size: 18px; margin-bottom: 14px; }}
    .panel-note {{ color: var(--muted); font-size: 13px; margin-top: 6px; line-height: 1.45; }}
    .scan-grid {{ display: grid; gap: 12px; }}
    .scan-scroll {{ overflow-x: auto; padding-bottom: 10px; scrollbar-color: rgba(139, 94, 52, 0.45) rgba(231, 215, 191, 0.60); }}
    .scan-scroll::-webkit-scrollbar {{ height: 10px; }}
    .scan-scroll::-webkit-scrollbar-track {{ background: rgba(231, 215, 191, 0.60); border-radius: 999px; }}
    .scan-scroll::-webkit-scrollbar-thumb {{ background: rgba(139, 94, 52, 0.45); border-radius: 999px; }}
    .scan-scroll-track {{ display: grid; gap: 12px; min-width: max-content; }}
    .scan-row {{ display: grid; gap: 6px; min-width: max-content; padding-bottom: 10px; border-bottom: 1px dashed rgba(139, 94, 52, 0.18); }}
    .scan-row:last-child {{ padding-bottom: 0; border-bottom: none; }}
    .scan-row-label {{ font-size: 11px; color: var(--muted); text-transform: uppercase; letter-spacing: 0.12em; margin-bottom: 2px; }}
    .scan-cells {{ display: grid; grid-auto-flow: column; grid-auto-columns: 84px; gap: 6px; width: max-content; }}
    .scan-cell {{ min-height: 112px; border-radius: 14px; border: 1px solid rgba(0,0,0,0.06); padding: 7px; display: flex; flex-direction: column; gap: 6px; box-shadow: inset 0 1px 0 rgba(255,255,255,0.55); }}
    .scan-cell.disabled {{ opacity: 0.48; filter: saturate(0.4); }}
    .scan-cell.mode-ln-ln {{ background: linear-gradient(180deg, #d4f4ef, #effbf8); }}
    .scan-cell.mode-bus-ln {{ background: linear-gradient(180deg, #ffe7bf, #fff8ea); }}
    .scan-cell.mode-ln-bus {{ background: linear-gradient(180deg, #ffd8cf, #fff4f1); }}
    .scan-cell.mode-bus-bus {{ background: linear-gradient(180deg, #d7e6ff, #f5f9ff); }}
    .scan-top {{ display: flex; justify-content: space-between; gap: 6px; align-items: flex-start; }}
    .scan-index {{ font-family: var(--mono); font-size: 10px; font-weight: 700; color: var(--muted); }}
    .scan-mode {{ font-size: 9px; font-weight: 700; line-height: 1.15; text-align: right; }}
    .field-grid {{ display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 4px; }}
    .field {{ background: rgba(255,255,255,0.72); border: 1px solid rgba(0,0,0,0.05); border-radius: 10px; padding: 4px 4px 5px; }}
    .field.port-ps {{ background: linear-gradient(180deg, rgba(31,111,235,0.11), rgba(255,255,255,0.82)); border-color: rgba(31,111,235,0.18); }}
    .field.port-pd {{ background: linear-gradient(180deg, rgba(45,164,78,0.11), rgba(255,255,255,0.82)); border-color: rgba(45,164,78,0.18); }}
    .field.port-pli {{ background: linear-gradient(180deg, rgba(210,153,34,0.12), rgba(255,255,255,0.82)); border-color: rgba(210,153,34,0.20); }}
    .field.port-plo {{ background: linear-gradient(180deg, rgba(191,57,137,0.11), rgba(255,255,255,0.82)); border-color: rgba(191,57,137,0.18); }}
    .field .k {{ display: block; color: var(--muted); font-size: 8px; text-transform: uppercase; letter-spacing: 0.10em; margin-bottom: 3px; }}
    .field .v-chip {{ display: inline-flex; min-width: 100%; justify-content: center; align-items: center; padding: 2px 4px; border-radius: 999px; font-family: var(--mono); font-size: 10px; font-weight: 700; line-height: 1.1; }}
    .spm-map-grid {{ display: grid; gap: 14px; }}
    .spm-track {{ display: grid; grid-template-columns: 118px 1fr; gap: 12px; align-items: start; }}
    .spm-meta strong {{ display: block; font-size: 15px; }}
    .spm-meta span {{ color: var(--muted); font-size: 12px; }}
    .spm-window-grid {{ display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 10px; }}
    .spm-window {{ border-radius: 16px; padding: 12px; color: #fff; min-height: 102px; display: flex; flex-direction: column; justify-content: space-between; gap: 10px; box-shadow: inset 0 1px 0 rgba(255,255,255,0.20); }}
    .spm-window-head {{ display: flex; align-items: center; justify-content: space-between; gap: 8px; }}
    .spm-window-label {{ font-size: 12px; font-weight: 700; text-transform: uppercase; letter-spacing: 0.08em; }}
    .spm-window-size {{ font-size: 11px; font-weight: 700; text-align: right; }}
    .spm-window-range {{ font-family: var(--mono); font-size: 12px; line-height: 1.35; }}
    .spm-window-meter {{ height: 8px; border-radius: 999px; background: rgba(255,255,255,0.25); overflow: hidden; }}
    .spm-window-meter-fill {{ height: 100%; border-radius: inherit; background: rgba(255,255,255,0.90); }}
    .spm-axis {{ display: flex; justify-content: space-between; gap: 8px; margin-top: 8px; color: var(--muted); font-size: 11px; }}
    .spm-annotations {{ display: flex; flex-wrap: wrap; gap: 8px; margin-top: 12px; }}
    .dma-grid {{ display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 14px; }}
    .dma-card {{ border: 1px solid var(--line); border-radius: 18px; background: linear-gradient(180deg, #fffdf9, #f8f3ea); padding: 16px; }}
    .dma-head {{ display: flex; justify-content: space-between; gap: 12px; align-items: baseline; margin-bottom: 10px; }}
    .dma-head h4 {{ font-size: 17px; margin: 0; }}
    .dma-head span {{ color: var(--muted); font-size: 12px; }}
    .dma-chip-row {{ display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 10px; }}
    .dma-formula {{ font-family: var(--mono); font-size: 12px; color: var(--muted); background: rgba(255,255,255,0.68); border: 1px solid rgba(0,0,0,0.05); border-radius: 12px; padding: 10px; margin-bottom: 10px; line-height: 1.45; }}
    .dma-bank-grid {{ display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }}
    .dma-bank {{ border-radius: 14px; padding: 10px; background: rgba(255,255,255,0.72); border: 1px solid rgba(0,0,0,0.05); min-height: 96px; }}
    .dma-bank.inactive {{ opacity: 0.52; filter: saturate(0.45); }}
    .dma-bank .label {{ display: block; color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: 0.08em; margin-bottom: 6px; }}
    .dma-bank .value {{ display: block; font-family: var(--mono); font-size: 12px; line-height: 1.5; }}
    .dma-footnote {{ color: var(--muted); font-size: 12px; margin-top: 10px; line-height: 1.45; }}
    .agu-grid {{ display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 16px; }}
    .agu-card {{ border: 1px solid var(--line); border-radius: 18px; background: linear-gradient(180deg, #fffdf9, #f8f3ea); padding: 16px; }}
    .agu-head {{ display: flex; justify-content: space-between; gap: 12px; align-items: baseline; margin-bottom: 12px; }}
    .agu-head h4 {{ font-size: 18px; }}
    .agu-head span {{ color: var(--muted); font-size: 12px; }}
    .agu-meta-grid {{ display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px; margin-bottom: 14px; }}
    .meta-card {{ background: rgba(255,255,255,0.7); border: 1px solid rgba(0,0,0,0.05); border-radius: 12px; padding: 10px; }}
    .meta-card .label {{ display: block; color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: 0.08em; margin-bottom: 4px; }}
    .meta-card .value {{ font-family: var(--mono); font-size: 13px; }}
    .dim-flow {{ display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 10px; }}
    .dim-card {{ background: rgba(255,255,255,0.8); border: 1px solid rgba(0,0,0,0.05); border-radius: 14px; padding: 10px; }}
    .dim-card .dim-name {{ font-size: 12px; font-weight: 700; margin-bottom: 8px; letter-spacing: 0.08em; }}
    .metric {{ margin-bottom: 10px; }}
    .metric:last-child {{ margin-bottom: 0; }}
    .metric-head {{ display: flex; justify-content: space-between; gap: 8px; color: var(--muted); font-size: 11px; text-transform: uppercase; letter-spacing: 0.08em; margin-bottom: 5px; }}
    .metric-value {{ font-family: var(--mono); color: var(--text); }}
    .bar-shell {{ height: 9px; border-radius: 999px; background: rgba(99, 86, 70, 0.10); overflow: hidden; }}
    .bar-fill {{ height: 100%; border-radius: inherit; }}
    .bar-fill.iter {{ background: linear-gradient(90deg, #3178c6, #8bb8f0); }}
    .bar-fill.stride {{ background: linear-gradient(90deg, #8b5e34, #d6a15e); }}
    .pattern-section {{ margin-top: 18px; padding-top: 18px; border-top: 1px solid var(--line); }}
    .pattern-section h4 {{ font-size: 17px; margin-bottom: 14px; }}
    .pattern-grid {{ display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 16px; }}
    .pattern-card {{ border: 1px solid var(--line); border-radius: 18px; background: linear-gradient(180deg, #fffdf9, #f8f3ea); padding: 16px; }}
    .pattern-head {{ display: flex; justify-content: space-between; gap: 12px; align-items: baseline; margin-bottom: 10px; }}
    .pattern-head h5 {{ font-size: 16px; margin: 0; }}
    .pattern-head span {{ color: var(--muted); font-size: 12px; }}
    .axis-pills {{ display: flex; flex-wrap: wrap; gap: 8px; margin-bottom: 10px; }}
    .axis-pill {{ background: rgba(255,255,255,0.72); border: 1px solid rgba(0,0,0,0.05); border-radius: 999px; padding: 6px 10px; font-size: 11px; color: var(--muted); }}
    .pattern-formula {{ font-family: var(--mono); font-size: 12px; color: var(--muted); background: rgba(255,255,255,0.65); border: 1px solid rgba(0,0,0,0.05); border-radius: 12px; padding: 10px; margin-bottom: 12px; line-height: 1.4; }}
        .pattern-image-shell {{ border-radius: 16px; padding: 10px; background: linear-gradient(180deg, rgba(255,255,255,0.84), rgba(244, 238, 225, 0.92)); border: 1px solid rgba(0,0,0,0.05); box-shadow: inset 0 1px 0 rgba(255,255,255,0.72); }}
        .pattern-image {{ display: block; width: 100%; height: auto; border-radius: 12px; }}
        .pattern-caption {{ display: flex; justify-content: space-between; gap: 12px; margin-top: 8px; color: var(--muted); font-size: 11px; }}
        .pattern-id-legend {{ display: flex; flex-wrap: wrap; gap: 8px; margin-top: 10px; }}
        .pattern-id-chip {{ display: inline-flex; align-items: center; gap: 6px; padding: 5px 9px; border-radius: 999px; background: rgba(255,255,255,0.78); border: 1px solid rgba(0,0,0,0.05); color: var(--muted); font-size: 11px; }}
        .pattern-id-dot {{ width: 12px; height: 12px; border-radius: 999px; border: 1px solid rgba(0,0,0,0.12); }}
    .pattern-footnote {{ color: var(--muted); font-size: 12px; margin-top: 8px; line-height: 1.45; }}
    .footer {{ color: var(--muted); font-size: 13px; margin-top: 12px; line-height: 1.5; }}
    @media (max-width: 1180px) {{
            .hero-grid, .dma-grid, .agu-grid, .pattern-grid {{ grid-template-columns: 1fr; }}
      .summary-grid {{ grid-template-columns: repeat(2, minmax(0, 1fr)); }}
    }}
    @media (max-width: 820px) {{
      body {{ padding: 12px; }}
      .summary-grid, .agu-meta-grid, .dim-flow {{ grid-template-columns: 1fr; }}
            .dma-bank-grid {{ grid-template-columns: 1fr; }}
      .spm-track {{ grid-template-columns: 1fr; }}
      .spm-window-grid {{ grid-template-columns: 1fr; }}
    }}
  </style>
</head>
<body>
  <main>
    {header}
    {legend}
    {layers}
  </main>
</body>
</html>
"""


def _render_header(hw_ir: HardwareIR) -> str:
    hw = hw_ir.hardware
    layers = len(hw_ir.layers)
    total_scan_entries = sum(len(layer.scan_chain) for layer in hw_ir.layers)
    return f"""
<section class=\"hero\">
  <div class=\"hero-grid\">
    <div>
      <h1>{escape(hw_ir.workload_name)} hardware visualization</h1>
      <p>
        Generated from HardwareIR. This page visualizes scan-chain topology,
        SPM ping/pong mapping, DMA transport, and the four-dimensional iter/stride setup for each AGU bank.
      </p>
    </div>
    <div class=\"summary-grid\">
      {_render_summary_card('Layers', str(layers), 'lowered operators')}
      {_render_summary_card('PEs / cluster', str(hw.num_pes), f'across {hw.num_bus} buses')}
      {_render_summary_card('Clusters', str(hw.num_clusters), 'target hardware')}
      {_render_summary_card('SPM group cap', _fmt_bytes(hw.group_capacity), 'per group', compact=True)}
      {_render_summary_card('Scan entries', str(total_scan_entries), 'all layers')}
    </div>
  </div>
</section>
"""


def _render_route_legend() -> str:
    items = []
    for meta in _ROUTE_MODE_META.values():
        items.append(
            f"<div class=\"legend-item\"><span class=\"swatch {meta['class_name']}\"></span>"
            f"<strong>{escape(meta['symbol'])}</strong><span>{escape(meta['summary'])}</span></div>"
        )
    items.append(
        "<div class=\"legend-item\"><span class=\"swatch\" style=\"background:#d7d3cb\"></span>"
        "<strong>disabled</strong><span>PE disabled in scan-chain topology</span></div>"
    )
    return f"<section class=\"legend\">{''.join(items)}</section>"


def _render_layer(layer: LayerHwConfig, hw: HardwareDesc, idx: int) -> str:
    enabled = sum(1 for entry in layer.scan_chain if entry.enable)
    total = len(layer.scan_chain)
    tp = layer.tiling_params
    return f"""
<section class=\"layer\" id=\"layer-{idx}\">
  <div class=\"layer-head\">
    <div class=\"layer-title\">
      <h2>{escape(layer.name)}</h2>
      <div class=\"pill-row\">
        <span class=\"pill\">{escape(layer.op_type)}</span>
        <span class=\"pill\">cluster_mask=0x{layer.target_cluster_mask:X}</span>
        <span class=\"pill\">active_pe={enabled}/{total}</span>
        <span class=\"pill\">plane_en=0x{layer.hddu.plane_en:X}</span>
        <span class=\"pill\">spm_cfg=0x{layer.spm_config_map:X}</span>
      </div>
    </div>
    <div class=\"pill-row\">
      <span class=\"pill\">tiles oc/h/w/ic = {tp.num_oc_tiles}/{tp.num_h_tiles}/{tp.num_w_tiles}/{tp.num_ic_tiles}</span>
      <span class=\"pill\">tile_out = {tp.tile_h_out}x{tp.tile_w_out}</span>
      <span class=\"pill\">tile_in = {tp.tile_h_in}x{tp.tile_w_in}</span>
    </div>
  </div>
        <div class=\"panel-stack\">
    <div class=\"panel\">
      <h3>Scan-chain topology</h3>
      {_render_scan_chain(layer.scan_chain, hw)}
            <div class="panel-note">Ping/pong windows are shown in local SPM byte address space. Buffer size labels show bytes and word64 side by side; the AGU section below still uses word64 register units.</div>
    </div>
        <div class="panel">
            <h3>DMA transport</h3>
            {_render_dma_panel(layer, hw)}
            <div class="panel-note">parallel_groups marks planes that should be written through bank-split linear DMA sections and then consumed via the parallel AGU alias.</div>
        </div>
    <div class=\"panel\">
      <h3>SPM mapping</h3>
    {_render_spm_mapping(layer, hw)}
      <div class=\"panel-note\">Ping/pong windows are shown in local SPM address space. Runtime map registers are included for cross-checking bank-group swaps.</div>
    </div>
  </div>
  <div class=\"panel\">
    <h3>4D AGU iter/stride</h3>
    {_render_agu_panel(layer)}
    <div class=\"footer\">AGU base/stride values are rendered in word64 units, matching the lowering-side AGU register contract.</div>
  </div>
</section>
"""


def _render_scan_chain(entries: list[ScanChainEntry], hw: HardwareDesc) -> str:
    num_bus = max(1, hw.num_bus)
    columns = max(1, math.ceil(len(entries) / num_bus))
    rows = []
    for bus_idx in range(num_bus):
        start = bus_idx * columns
        end = min(len(entries), start + columns)
        row_entries = entries[start:end]
        cells = "".join(_render_scan_cell(start + offset, entry) for offset, entry in enumerate(row_entries))
        rows.append(
            f"<div class=\"scan-row\">"
            f"<div class=\"scan-row-label\">bus {bus_idx}</div>"
            f"<div class=\"scan-cells\">{cells}</div>"
            f"</div>"
        )
    return (
        f"<div class=\"scan-grid\">"
        f"<div class=\"scan-scroll\">"
        f"<div class=\"scan-scroll-track\">{''.join(rows)}</div>"
        f"</div>"
        f"</div>"
    )


def _render_scan_cell(index: int, entry: ScanChainEntry) -> str:
    meta = _ROUTE_MODE_META.get(entry.route_mode, _ROUTE_MODE_META[3])
    disabled = " disabled" if not entry.enable else ""
    return f"""
<div class=\"scan-cell {meta['class_name']}{disabled}\">
  <div class=\"scan-top\">
    <div class=\"scan-index\">PE {index}</div>
    <div class=\"scan-mode\">{escape(meta['symbol'])}</div>
  </div>
  <div class=\"field-grid\">
    {_render_field('ps', entry.ps_id)}
    {_render_field('pd', entry.pd_id)}
    {_render_field('pli', entry.pli_id)}
    {_render_field('plo', entry.plo_id)}
  </div>
</div>
"""


def _render_field(key: str, value: int) -> str:
    shown = "-" if value == 63 else str(value)
    bg_hex, text_hex, border_hex, _face_rgb, _edge_rgb = _port_id_palette(value)
    return (
        f"<div class=\"field port-{key}\">"
        f"<span class=\"k\">{key}</span>"
        f"<span class=\"v-chip\" style=\"background: {bg_hex}; color: {text_hex}; border: 1px solid {border_hex};\">{shown}</span>"
        f"</div>"
    )


def _render_spm_mapping(layer: LayerHwConfig, hw: HardwareDesc) -> str:
    groups = []
    max_extent = 1
    buffer_extent = max(1, hw.half_group_capacity)
    for attr, title, subtitle, color in _SPM_GROUP_META:
        layout = getattr(layer.spm_layout, attr)
        max_extent = max(max_extent, layout.ping_base + layout.size, layout.pong_base + layout.size)
        groups.append((attr, title, subtitle, color, layout))

    tracks = []
    for _attr, title, subtitle, color, layout in groups:
        ping_end = layout.ping_base + layout.size - 1
        pong_end = layout.pong_base + layout.size - 1
        usage_percent = min(100.0, (layout.size / buffer_extent) * 100.0)
        tracks.append(
            f"<div class=\"spm-track\">"
            f"<div class=\"spm-meta\"><strong>{title}</strong><span>{subtitle}<br>{escape(layout.spm_mode)} mode</span></div>"
            f"<div>"
            f"<div class=\"spm-window-grid\">"
            f"{_render_spm_window('ping', layout.ping_base, ping_end, layout.size, color, usage_percent, buffer_extent)}"
            f"{_render_spm_window('pong', layout.pong_base, pong_end, layout.size, _tint(color, 0.72), usage_percent, buffer_extent)}"
            f"</div>"
            f"<div class=\"spm-axis\"><span>local SPM byte addr 0x0</span><span>buffer = {_fmt_bytes(buffer_extent)} / byte extent 0x{max_extent:X}</span></div>"
            f"</div>"
            f"</div>"
        )

    annotations = (
        f"<div class=\"spm-annotations\">"
        f"<span class=\"pill\">layer spm_config_map = 0x{layer.spm_config_map:X}</span>"
        f"<span class=\"pill\">even map = 0x{layer.tiling_params.spm_map_even:X}</span>"
        f"<span class=\"pill\">odd map = 0x{layer.tiling_params.spm_map_odd:X}</span>"
        f"</div>"
    )
    return f"<div class=\"spm-map-grid\">{''.join(tracks)}{annotations}</div>"


def _render_dma_panel(layer: LayerHwConfig, hw: HardwareDesc) -> str:
    bus_counts = _scan_chain_bus_enabled_counts(layer.scan_chain, hw)
    active_buses = max(1, sum(1 for count in bus_counts if count > 0))
    rows_per_bus = max(bus_counts) if bus_counts else 1
    cards = []
    for attr, title, subtitle, color in _SPM_GROUP_META:
        cards.append(_render_dma_card(layer, hw, attr, title, subtitle, color, active_buses, rows_per_bus))
    return f"<div class=\"dma-grid\">{''.join(cards)}</div>"


def _render_dma_card(
    layer: LayerHwConfig,
    hw: HardwareDesc,
    attr: str,
    title: str,
    subtitle: str,
    color: str,
    active_buses: int,
    rows_per_bus: int,
) -> str:
    tp = layer.tiling_params
    plane_idx = {"ps": 0, "pd": 1, "pli": 2, "plo": 3}[attr]
    layout = getattr(layer.spm_layout, attr)
    parallel = bool(tp.parallel_groups & (1 << plane_idx))
    beat_words = {
        "ps": tp.dma_ps_words,
        "pd": tp.dma_pd_words,
        "pli": tp.dma_pli_words,
        "plo": tp.dma_plo_words,
    }[attr]
    if attr == "plo" and tp.dma_plo_words_per_bank > 0:
        beat_words = tp.dma_plo_words_per_bank

    active_banks = active_buses if parallel else 1
    total_words = beat_words * active_banks if parallel else beat_words
    transport = _dma_transport_label(attr, parallel, active_buses)
    formula = _dma_formula(layer, attr)
    chips = [
        f"<span class=\"pill\">{transport}</span>",
        f"<span class=\"pill\">SPM = {escape(layout.spm_mode)} / AGU {_agu_ctrl_flags(getattr(layer, f'agu_{attr}').ctrl)}</span>",
        f"<span class=\"pill\">wave = {_fmt_words(total_words)} ({_fmt_bytes(total_words * 8)})</span>",
    ]
    if parallel:
        chips.append(f"<span class=\"pill\">per bank = {_fmt_words(beat_words)}</span>")

    banks = []
    if parallel:
        for bank_idx in range(hw.spm_banks_per_group):
            ping = tp.spm_ping[plane_idx] + bank_idx * tp.bank_depth_bytes
            pong = tp.spm_pong[plane_idx] + bank_idx * tp.bank_depth_bytes
            classes = "dma-bank" + (" inactive" if bank_idx >= active_banks else "")
            banks.append(
                f"<div class=\"{classes}\" style=\"border-color: {_tint(color, 0.72)};\">"
                f"<span class=\"label\">bank {bank_idx}</span>"
                f"<span class=\"value\">ping 0x{ping:X}<br>pong 0x{pong:X}<br>{_fmt_words(beat_words)} / {_fmt_bytes(beat_words * 8)}</span>"
                f"</div>"
            )
        footnote = _dma_parallel_note(attr, rows_per_bus, layer)
    else:
        banks.append(
            f"<div class=\"dma-bank\" style=\"border-color: {_tint(color, 0.72)};\">"
            f"<span class=\"label\">group window</span>"
            f"<span class=\"value\">ping 0x{tp.spm_ping[plane_idx]:X}<br>pong 0x{tp.spm_pong[plane_idx]:X}<br>{_fmt_words(beat_words)} / {_fmt_bytes(beat_words * 8)}</span>"
            f"</div>"
        )
        footnote = _dma_linear_note(attr, active_buses)

    return f"""
<section class=\"dma-card\">
  <div class=\"dma-head\">
    <h4>{title} DMA</h4>
    <span>{subtitle}</span>
  </div>
  <div class=\"dma-chip-row\">{''.join(chips)}</div>
  <div class=\"dma-formula\">{formula}</div>
  <div class=\"dma-bank-grid\">{''.join(banks)}</div>
  <div class=\"dma-footnote\">{footnote}</div>
</section>
"""


def _scan_chain_bus_enabled_counts(entries: list[ScanChainEntry], hw: HardwareDesc) -> list[int]:
    num_bus = max(1, hw.num_bus)
    columns = max(1, math.ceil(len(entries) / num_bus))
    counts = []
    for bus_idx in range(num_bus):
        start = bus_idx * columns
        end = min(len(entries), start + columns)
        counts.append(sum(1 for entry in entries[start:end] if entry.enable))
    return counts


def _dma_transport_label(attr: str, parallel: bool, active_buses: int) -> str:
    if parallel:
        return f"bank-split linear DMA -> {active_buses} bus parallel read"
    if attr == "ps" and active_buses > 1:
        return "shared group-linear DMA -> broadcast read"
    return "group-linear DMA"


def _dma_formula(layer: LayerHwConfig, attr: str) -> str:
    tp = layer.tiling_params
    if attr == "ps":
        return (
            f"dram = 0x{tp.dram_weight_base:X} + oc*{tp.dram_ps_oc_stride} + h*{tp.dram_ps_h_stride} + ic*{tp.dram_ps_ic_stride}; "
            f"spm = 0x{tp.spm_ping[0]:X}/0x{tp.spm_pong[0]:X}"
        )
    if attr == "pd":
        return (
            f"dram = 0x{tp.dram_input_base:X} + oc*{tp.dram_pd_oc_stride} + h*{tp.dram_pd_h_stride} + w*{tp.dram_pd_w_stride} + ic*{tp.dram_pd_ic_stride}; "
            f"spm(bank0) = 0x{tp.spm_ping[1]:X}/0x{tp.spm_pong[1]:X}"
        )
    if attr == "pli":
        return (
            f"dram = 0x{tp.dram_bias_base:X} + oc*{tp.dram_bias_oc_stride} + h*{tp.dram_bias_h_stride}; "
            f"spm(bank0) = 0x{tp.spm_ping[2]:X}/0x{tp.spm_pong[2]:X}"
        )
    return (
        f"dram = 0x{tp.dram_output_base:X} + oc*{tp.dram_out_oc_stride} + h*{tp.dram_out_h_stride} + w*{tp.dram_out_w_stride}; "
        f"spm(bank0) = 0x{tp.spm_ping[3]:X}/0x{tp.spm_pong[3]:X}"
    )


def _dma_parallel_note(attr: str, rows_per_bus: int, layer: LayerHwConfig) -> str:
    if attr == "pd":
        row_beats = layer.agu_pd.iter0 * layer.agu_pd.iter2
        return f"Expected transfer shape: {rows_per_bus} rows/bank, {row_beats} word64 per row. DMA writes each active bank through linear sections, then AGU reads the parallel alias."
    if attr == "pli":
        row_beats = layer.agu_pli.iter0 * layer.agu_pli.iter2
        return f"PLI is initialized per active bank. Each bank covers {rows_per_bus} output rows and {row_beats} word64 per row before the parallel AGU starts reading."
    row_beats = layer.agu_plo.iter0 * layer.agu_plo.iter2
    return f"Writeback is bank-split: each active bank covers {rows_per_bus} output rows and {row_beats} word64 per row, then is expected to be drained back to DRAM as a separate linear section."


def _dma_linear_note(attr: str, active_buses: int) -> str:
    if attr == "ps" and active_buses > 1:
        return "One shared weight chunk is loaded once and broadcast to every active bus; no bank partition is required for PS."
    if attr == "pli":
        return "PLI uses a single group-linear initialization chunk. This typically represents bias or zero-fill for the first reduction wave."
    return "The entire plane uses one group-linear ping/pong window per wave."


def _render_agu_panel(layer: LayerHwConfig) -> str:
    cards = []
    patterns = []
    for attr, title, subtitle in _AGU_GROUP_META:
        agu = getattr(layer, attr)
        cards.append(_render_agu_card(title, subtitle, agu))
        patterns.append(_render_access_pattern_card(layer, attr, title, subtitle, agu))
    return (
        f"<div class=\"agu-grid\">{''.join(cards)}</div>"
        f"<section class=\"pattern-section\">"
        f"<h4>3D spatial + temporal color</h4>"
        f"<div class=\"pattern-grid\">{''.join(patterns)}</div>"
        f"</section>"
    )


def _render_agu_card(title: str, subtitle: str, agu: AguBankConfig) -> str:
    dims = _agu_dimension_records(agu)
    max_iter = max(iter_value for _name, iter_value, _stride in dims)
    max_stride = max(stride_value for _name, _iter_value, stride_value in dims)
    dim_cards = "".join(
        _render_dim_card(name, iter_value, stride_value, max_iter, max_stride)
        for name, iter_value, stride_value in dims
    )

    flags = _agu_ctrl_flags(agu.ctrl)
    meta = [
        ("base", f"0x{agu.base_addr:X}"),
        ("ctrl", flags),
        ("mask", f"0x{agu.mask_cfg:X}"),
        ("tag base", str(agu.tag_base)),
        ("tag stride", f"{agu.tag_stride0}, {agu.tag_stride1}"),
        ("tag ctrl", f"0x{agu.tag_ctrl:X}"),
    ]
    meta_cards = "".join(
        f"<div class=\"meta-card\"><span class=\"label\">{label}</span><span class=\"value\">{value}</span></div>"
        for label, value in meta
    )

    return f"""
<section class=\"agu-card\">
  <div class=\"agu-head\">
    <h4>{title}</h4>
    <span>{subtitle}</span>
  </div>
  <div class=\"agu-meta-grid\">{meta_cards}</div>
  <div class=\"dim-flow\">{dim_cards}</div>
</section>
"""


def _render_dim_card(name: str, iter_value: int, stride_value: int, max_iter: int, max_stride: int) -> str:
    iter_width = _metric_width(iter_value, max_iter)
    stride_width = _metric_width(stride_value, max_stride)
    return f"""
<div class=\"dim-card\">
  <div class=\"dim-name\">{name}</div>
  <div class=\"metric\">
    <div class=\"metric-head\"><span>iter</span><span class=\"metric-value\">{iter_value}</span></div>
    <div class=\"bar-shell\"><div class=\"bar-fill iter\" style=\"width:{iter_width:.2f}%\"></div></div>
  </div>
  <div class=\"metric\">
    <div class=\"metric-head\"><span>stride</span><span class=\"metric-value\">{stride_value}</span></div>
    <div class=\"bar-shell\"><div class=\"bar-fill stride\" style=\"width:{stride_width:.2f}%\"></div></div>
  </div>
</div>
"""


def _metric_width(value: int, max_value: int) -> float:
    if value <= 0 or max_value <= 0:
        return 0.0
    denom = math.log2(max_value + 1)
    if denom <= 0:
        return 100.0
    return 18.0 + 82.0 * (math.log2(value + 1) / denom)


def _render_summary_card(label: str, value: str, sub: str, compact: bool = False) -> str:
    value_class = "value compact" if compact else "value"
    return (
        f"<div class=\"summary-card\">"
        f"<div class=\"label\">{escape(label)}</div>"
        f"<div class=\"{value_class}\">{escape(value)}</div>"
        f"<div class=\"sub\">{escape(sub)}</div>"
        f"</div>"
    )


def _render_spm_window(
    label: str,
    base: int,
    end: int,
    size: int,
    color: str,
    usage_percent: float,
    buffer_extent: int,
) -> str:
    return (
        f"<div class=\"spm-window\" style=\"background: linear-gradient(180deg, {color}, {_tint(color, 0.82)});\">"
        f"<div class=\"spm-window-head\">"
        f"<span class=\"spm-window-label\">{label}</span>"
        f"<span class=\"spm-window-size\">{_fmt_spm_size(size)}<br>{usage_percent:.2f}% of {_fmt_bytes(buffer_extent)}</span>"
        f"</div>"
        f"<div class=\"spm-window-range\">byte addr 0x{base:X} - 0x{end:X}</div>"
        f"<div class=\"spm-window-meter\"><div class=\"spm-window-meter-fill\" style=\"width:{usage_percent:.2f}%\"></div></div>"
        f"</div>"
    )


def _port_id_style(value: int) -> str:
    if value == 63:
        return (
            "background: rgba(238, 233, 225, 0.92);"
            "color: #8a7f72;"
            "border: 1px dashed rgba(138, 127, 114, 0.45);"
        )

    hue = (value * 47) % 360
    return (
        f"background: hsl({hue} 78% 88%);"
        f"color: hsl({hue} 52% 24%);"
        f"border: 1px solid hsl({hue} 54% 68%);"
    )


def _fmt_spm_size(value: int) -> str:
    word64 = value // 8
    return f"{_fmt_bytes(value)} / {word64} w64"


def _fmt_words(value: int) -> str:
    return f"{value} w64"


def _render_access_pattern_card(
    layer: LayerHwConfig,
    bank_attr: str,
    title: str,
    subtitle: str,
    agu: AguBankConfig,
) -> str:
    dims = _agu_dimension_info(agu)
    spec = _build_tensor_tile_spec(layer, bank_attr, agu, dims)
    image_data = _render_access_pattern_image(spec)
    tag_note = str(
        spec.get(
            "tag_formula",
            f"tag = ({spec['tag_base']} + local_{spec['tag_axis_label']} × {spec['tag_stride']}) & 0x3F",
        )
    )

    axis_pills = [
        f"<span class=\"axis-pill\">X = {spec['axis_labels'][0]} / full {spec['full_shape'][0]}</span>",
        f"<span class=\"axis-pill\">Y = {spec['axis_labels'][1]} / full {spec['full_shape'][1]}</span>",
        f"<span class=\"axis-pill\">Z = {spec['axis_labels'][2]} / full {spec['full_shape'][2]}</span>",
        f"<span class=\"axis-pill\">TAG_CTRL = D{spec['tag_level']} / {spec['tag_axis_label']}</span>",
        f"<span class=\"axis-pill\">Color = {spec['scan_id_label']}</span>",
    ]

    sample_note = (
        f"tensor = {spec['tensor_name']}; "
        f"full shape = {spec['full_shape'][0]}×{spec['full_shape'][1]}×{spec['full_shape'][2]}; "
        f"compiler tiles = {spec['base_tile_count']}; "
        f"tag-sliced tiles = {len(spec['tiles'])}; "
        f"{tag_note}; "
        f"wave order = {spec['temporal_order_desc']}; "
        f"addr = {_agu_address_expression(dims)}"
    )

    legend_html = _render_pattern_id_legend(spec['legend_ids'], spec['scan_id_label'])

    return f"""
<section class=\"pattern-card\">
  <div class=\"pattern-head\">
    <h5>{title} pattern</h5>
    <span>{subtitle}</span>
  </div>
  <div class=\"axis-pills\">{''.join(axis_pills)}</div>
  <div class=\"pattern-formula\">{sample_note}</div>
  <div class=\"pattern-image-shell\">
    <img class=\"pattern-image\" src=\"data:image/png;base64,{image_data}\" alt=\"{escape(title)} tensor tile pattern\">
  </div>
    <div class=\"pattern-caption\"><span>transparent box = full tensor / solid boxes = TAG_CTRL slices</span><span>{spec['scan_id_label']} = tag low 6-bit</span></div>
    {legend_html}
  <div class=\"pattern-footnote\">{spec['footnote']}</div>
</section>
"""


def _render_access_pattern_image(
    spec: dict[str, object],
) -> str:
    from matplotlib.backends.backend_agg import FigureCanvasAgg
    from matplotlib.figure import Figure
    from mpl_toolkits.mplot3d import proj3d
    from mpl_toolkits.mplot3d.art3d import Poly3DCollection

    full_x, full_y, full_z = spec["full_shape"]
    tile_specs = spec["tiles"]
    fig = Figure(figsize=(7.2, 5.8), dpi=180, facecolor="#fffdf9")
    canvas = FigureCanvasAgg(fig)
    ax = fig.add_subplot(111, projection="3d")
    label_specs: list[tuple[float, float, float, str, float, str, tuple[float, float, float]]] = []

    faces = _box_faces(0.0, 0.0, 0.0, float(full_x), float(full_y), float(full_z))
    ax.add_collection3d(
        Poly3DCollection(
            faces,
            facecolors=(0.86, 0.82, 0.75, 0.05),
            edgecolors="none",
        )
    )
    _draw_wireframe_box(ax, float(full_x), float(full_y), float(full_z))

    for tile in tile_specs:
        x_start, y_start, z_start = tile["start"]
        x_size, y_size, z_size = tile["size"]
        tag_value = int(tile["tag_value"])
        gap_x = min(0.16 * x_size, 0.24)
        gap_y = min(0.16 * y_size, 0.24)
        gap_z = min(0.16 * z_size, 0.24)
        x_pos = x_start + gap_x / 2.0
        y_pos = y_start + gap_y / 2.0
        z_pos = z_start + gap_z / 2.0
        dx = max(0.36, x_size - gap_x)
        dy = max(0.36, y_size - gap_y)
        dz = max(0.36, z_size - gap_z)
        _bg_hex, _text_hex, _border_hex, face_rgb, edge_rgb = _port_id_palette(tag_value)
        ax.bar3d(
            [x_pos],
            [y_pos],
            [z_pos],
            [dx],
            [dy],
            [dz],
            color=[(*face_rgb, 0.92)],
            shade=True,
            alpha=0.92,
            edgecolor=[(*edge_rgb, 0.42)],
            linewidth=0.45,
        )
        label_font = max(5.6, min(7.6, 4.9 + 0.40 * min(dx, dy)))
        label_y = y_pos + dy * 0.12
        label_z = min(float(full_z) + 0.42, z_pos + dz + 0.16)
        label_specs.append((
            x_pos + dx / 2.0,
            label_y,
            label_z,
            str(tag_value),
            label_font,
            _text_hex,
            edge_rgb,
        ))

    ax.view_init(elev=23, azim=-54)
    ax.set_proj_type("persp")
    ax.set_box_aspect(_display_box_aspect(float(full_x), float(full_y), float(full_z)))
    ax.set_xlim(0.0, float(full_x))
    ax.set_ylim(0.0, float(full_y))
    ax.set_zlim(0.0, float(full_z))
    ax.set_xticks(_axis_tick_values(spec["x_ticks"], full_x), [str(value) for value in _axis_tick_values(spec["x_ticks"], full_x)])
    ax.set_yticks(_axis_tick_values(spec["y_ticks"], full_y), [str(value) for value in _axis_tick_values(spec["y_ticks"], full_y)])
    ax.set_zticks(_axis_tick_values(spec["z_ticks"], full_z), [str(value) for value in _axis_tick_values(spec["z_ticks"], full_z)])
    ax.set_xlabel(str(spec["axis_labels"][0]), labelpad=7, fontsize=9, color="#62584c")
    ax.set_ylabel(str(spec["axis_labels"][1]), labelpad=8, fontsize=9, color="#62584c")
    ax.set_zlabel(str(spec["axis_labels"][2]), labelpad=6, fontsize=9, color="#62584c")
    ax.set_title("Tensor tile cuboids", fontsize=10, color="#62584c", pad=8)
    ax.tick_params(axis="x", labelsize=8, colors="#62584c", pad=1)
    ax.tick_params(axis="y", labelsize=8, colors="#62584c", pad=1)
    ax.tick_params(axis="z", labelsize=8, colors="#62584c", pad=1)

    for axis in (ax.xaxis, ax.yaxis, ax.zaxis):
        axis.pane.set_facecolor((0.984, 0.968, 0.933, 1.0))
        axis.pane.set_edgecolor((0.847, 0.800, 0.706, 1.0))

    grid_color = (0.847, 0.800, 0.706, 0.45)
    ax.xaxis._axinfo["grid"]["color"] = grid_color
    ax.yaxis._axinfo["grid"]["color"] = grid_color
    ax.zaxis._axinfo["grid"]["color"] = grid_color

    canvas.draw()
    _layout_pattern_labels(ax, canvas, label_specs)

    fig.subplots_adjust(left=0.06, right=0.98, bottom=0.10, top=0.92)
    buffer = BytesIO()
    fig.savefig(buffer, format="png", dpi=180, facecolor=fig.get_facecolor())
    fig.clear()
    return base64.b64encode(buffer.getvalue()).decode("ascii")


def _build_tensor_tile_spec(
    layer: LayerHwConfig,
    bank_attr: str,
    agu: AguBankConfig,
    dims: list[dict[str, int | str]],
) -> dict[str, object]:
    scan_meta = _bank_scan_meta(layer.scan_chain, bank_attr)
    if layer.op_type in {"conv2d_1x1", "conv2d_3x3"}:
        spec = _build_conv_tensor_tile_spec(layer, bank_attr, agu)
    elif layer.op_type == "gemm":
        spec = _build_gemm_tensor_tile_spec(layer, bank_attr, agu, dims)
    else:
        spec = _build_fallback_tensor_tile_spec(layer, bank_attr, agu, dims)

    spec["scan_id_label"] = scan_meta["scan_label"]
    return spec


def _build_conv_tensor_tile_spec(layer: LayerHwConfig, bank_attr: str, agu: AguBankConfig) -> dict[str, object]:
    tp = layer.tiling_params
    tile_oc = max(1, layer.agu_ps.iter3)
    tile_ic = 12 if layer.op_type == "conv2d_1x1" else 4
    kernel_width = 1 if layer.op_type == "conv2d_1x1" else 3
    kernel_extent = 1 if layer.op_type == "conv2d_1x1" else 9
    halo = 0 if layer.op_type == "conv2d_1x1" else 2

    full_h_out = max(1, (tp.num_h_tiles - 1) * tp.tile_h_out + tp.last_h_out)
    full_w_out = max(1, (tp.num_w_tiles - 1) * tp.tile_w_out + tp.last_w_out)
    full_oc = max(1, tp.num_oc_tiles * tile_oc)
    full_ic = max(1, tp.num_ic_tiles * tile_ic)

    h_out_spans = _actual_tile_spans(tp.num_h_tiles, tp.tile_h_out, tp.last_h_out)
    w_out_spans = _actual_tile_spans(tp.num_w_tiles, tp.tile_w_out, tp.last_w_out)
    oc_spans = _actual_tile_spans(tp.num_oc_tiles, tile_oc, tile_oc)
    ic_spans = _actual_tile_spans(tp.num_ic_tiles, tile_ic, tile_ic)

    if bank_attr == "agu_ps":
        base_tiles = []
        for oc_idx, (oc_start, oc_size) in enumerate(oc_spans):
            for ic_idx, (ic_start, ic_size) in enumerate(ic_spans):
                order = oc_idx * tp.num_ic_tiles + ic_idx
                base_tiles.append(_make_tensor_tile(ic_start, ic_size, 0, kernel_extent, oc_start, oc_size, order))
        tag_split = _build_conv_tag_split_meta(bank_attr, agu.tag_ctrl & 0x3, kernel_width, layer.op_type, tile_ic)
        tiles = _split_tiles_by_tag(base_tiles, tag_split, agu.tag_base, _agu_tag_stride(agu))
        return {
            "tensor_name": "weight tensor",
            "axis_labels": ("IC", "Kernel", "OC"),
            "full_shape": (full_ic, kernel_extent, full_oc),
            "tiles": tiles,
            "base_tile_count": len(base_tiles),
            "temporal_order_desc": "oc_tile -> ic_tile",
            "footnote": f"The transparent cuboid is the full weight tensor envelope. Each compiler tile is additionally sliced along {tag_split['label']} because TAG_CTRL selects that loop as the PE-group tag source.",
            "x_ticks": ic_spans,
            "y_ticks": [(0, kernel_extent)],
            "z_ticks": oc_spans,
            "tag_level": agu.tag_ctrl & 0x3,
            "tag_axis_label": tag_split["label"],
            "tag_base": agu.tag_base & 0x3F,
            "tag_stride": _agu_tag_stride(agu),
            "legend_ids": _legend_ids_from_tiles(tiles),
        }

    if bank_attr == "agu_pd":
        h_in_spans = [(start, size + halo) for start, size in h_out_spans]
        w_in_spans = [(start, size + halo) for start, size in w_out_spans]
        full_h_in = max(1, full_h_out + halo)
        full_w_in = max(1, full_w_out + halo)
        base_tiles = []
        for h_idx, (h_start, h_size) in enumerate(h_in_spans):
            for w_idx, (w_start, w_size) in enumerate(w_in_spans):
                for ic_idx, (ic_start, ic_size) in enumerate(ic_spans):
                    flat_idx = h_idx * tp.num_w_tiles * tp.num_ic_tiles + w_idx * tp.num_ic_tiles + ic_idx
                    base_tiles.append(_make_tensor_tile(w_start, w_size, h_start, h_size, ic_start, ic_size, flat_idx))
        tag_split = _build_conv_tag_split_meta(bank_attr, agu.tag_ctrl & 0x3, kernel_width, layer.op_type, tile_ic)
        tiles = _split_tiles_by_tag(base_tiles, tag_split, agu.tag_base, _agu_tag_stride(agu))
        return {
            "tensor_name": "input activation tensor",
            "axis_labels": ("W_in", "H_in", "IC"),
            "full_shape": (full_w_in, full_h_in, full_ic),
            "tiles": tiles,
            "base_tile_count": len(base_tiles),
            "temporal_order_desc": "h_tile -> w_tile -> ic_tile",
            "footnote": f"The transparent cuboid is the full input activation envelope. Solid blocks are real input tiles; for 3x3 layers the blocks include halo overlap. Each tile is further sliced along {tag_split['label']} because TAG_CTRL routes that row/column group to different PEs.",
            "x_ticks": w_in_spans,
            "y_ticks": h_in_spans,
            "z_ticks": ic_spans,
            "tag_level": agu.tag_ctrl & 0x3,
            "tag_axis_label": tag_split["label"],
            "tag_base": agu.tag_base & 0x3F,
            "tag_stride": _agu_tag_stride(agu),
            "legend_ids": _legend_ids_from_tiles(tiles),
        }

    base_tiles = []
    for oc_idx, (oc_start, oc_size) in enumerate(oc_spans):
        for h_idx, (h_start, h_size) in enumerate(h_out_spans):
            for w_idx, (w_start, w_size) in enumerate(w_out_spans):
                flat_idx = oc_idx * tp.num_h_tiles * tp.num_w_tiles + h_idx * tp.num_w_tiles + w_idx
                base_tiles.append(_make_tensor_tile(w_start, w_size, h_start, h_size, oc_start, oc_size, flat_idx))
    tag_split = _build_conv_tag_split_meta(bank_attr, agu.tag_ctrl & 0x3, kernel_width, layer.op_type, tile_ic)
    tiles = _split_tiles_by_tag(base_tiles, tag_split, agu.tag_base, _agu_tag_stride(agu))
    tensor_name = "partial/output tensor" if bank_attr == "agu_plo" else "initial partial-sum tensor"
    return {
        "tensor_name": tensor_name,
        "axis_labels": ("W_out", "H_out", "OC"),
        "full_shape": (full_w_out, full_h_out, full_oc),
        "tiles": tiles,
        "base_tile_count": len(base_tiles),
        "temporal_order_desc": "oc_tile -> h_tile -> w_tile",
        "footnote": f"The transparent cuboid is the full output tensor envelope. Each compiler tile is further sliced along {tag_split['label']} because TAG_CTRL selects that loop to form PE-group tags.",
        "x_ticks": w_out_spans,
        "y_ticks": h_out_spans,
        "z_ticks": oc_spans,
        "tag_level": agu.tag_ctrl & 0x3,
        "tag_axis_label": tag_split["label"],
        "tag_base": agu.tag_base & 0x3F,
        "tag_stride": _agu_tag_stride(agu),
        "legend_ids": _legend_ids_from_tiles(tiles),
    }


def _build_fallback_tensor_tile_spec(
    layer: LayerHwConfig,
    bank_attr: str,
    agu: AguBankConfig,
    dims: list[dict[str, int | str]],
) -> dict[str, object]:
    tag_level = agu.tag_ctrl & 0x3
    spatial_dims, _trailing_dims = _select_spatial_dims(dims, preferred_idx=tag_level)
    x_dim, y_dim, z_dim = spatial_dims
    full_shape = (max(1, int(x_dim["iter"])), max(1, int(y_dim["iter"])), max(1, int(z_dim["iter"])))
    tile_counts = _tile_grid_counts(full_shape)
    x_tiles = _partition_axis(full_shape[0], tile_counts[0])
    y_tiles = _partition_axis(full_shape[1], tile_counts[1])
    z_tiles = _partition_axis(full_shape[2], tile_counts[2])
    base_tiles = []
    for z_idx, (z_start, z_size) in enumerate(z_tiles):
        for y_idx, (y_start, y_size) in enumerate(y_tiles):
            for x_idx, (x_start, x_size) in enumerate(x_tiles):
                flat_idx = z_idx * len(y_tiles) * len(x_tiles) + y_idx * len(x_tiles) + x_idx
                base_tiles.append(_make_tensor_tile(x_start, x_size, y_start, y_size, z_start, z_size, flat_idx))

    split_meta = _build_fallback_tag_split_meta(spatial_dims, tag_level)
    tiles = _split_tiles_by_tag(base_tiles, split_meta, agu.tag_base, _agu_tag_stride(agu))
    return {
        "tensor_name": f"{bank_attr} tensor",
        "axis_labels": (str(x_dim["name"]), str(y_dim["name"]), str(z_dim["name"])),
        "full_shape": full_shape,
        "tiles": tiles,
        "base_tile_count": len(base_tiles),
        "temporal_order_desc": "derived AGU order",
        "footnote": f"Fallback mode: this bank uses the AGU iter dimensions directly. The compiler tile is additionally sliced along {split_meta['label']} when that dimension is spatialized in the view.",
        "x_ticks": x_tiles,
        "y_ticks": y_tiles,
        "z_ticks": z_tiles,
        "tag_level": tag_level,
        "tag_axis_label": split_meta["label"],
        "tag_base": agu.tag_base & 0x3F,
        "tag_stride": _agu_tag_stride(agu),
        "legend_ids": _legend_ids_from_tiles(tiles),
    }


def _build_gemm_tensor_tile_spec(
    layer: LayerHwConfig,
    bank_attr: str,
    agu: AguBankConfig,
    dims: list[dict[str, int | str]],
) -> dict[str, object]:
    meta = _gemm_pattern_meta(layer)
    if bank_attr == "agu_ps":
        return _build_gemm_ps_tensor_tile_spec(agu, meta)
    if bank_attr == "agu_pd":
        return _build_gemm_pd_tensor_tile_spec(agu, meta)
    if bank_attr in {"agu_pli", "agu_plo"}:
        return _build_gemm_partial_tensor_tile_spec(bank_attr, agu, meta)
    return _build_fallback_tensor_tile_spec(layer, bank_attr, agu, dims)


def _build_gemm_ps_tensor_tile_spec(
    agu: AguBankConfig,
    meta: dict[str, int | bool],
) -> dict[str, object]:
    total_n_tiles = int(meta["grid_n"])
    total_k_tiles = int(meta["grid_k"])
    grid_n_per_wave = int(meta["grid_n_per_wave"])
    grid_k_per_wave = int(meta["grid_k_per_wave"])
    pe_n = int(meta["pe_n"])
    pe_k = int(meta["pe_k"])
    ultra_scan = bool(meta["ultra_scan"])
    n_spans = _actual_tile_spans(total_n_tiles, pe_n, pe_n)
    k_spans = _actual_tile_spans(total_k_tiles, 1, 1)
    tiles = []
    for k_idx, (k_start, k_size) in enumerate(k_spans):
        local_k = k_idx % max(1, grid_k_per_wave)
        for n_idx, (n_start, n_size) in enumerate(n_spans):
            local_n = n_idx % max(1, grid_n_per_wave)
            tag_value = local_n if ultra_scan else (local_k * grid_n_per_wave + local_n)
            tiles.append(
                {
                    "start": (n_start, 0, k_start),
                    "size": (n_size, pe_k, k_size),
                    "order": k_idx * total_n_tiles + n_idx,
                    "tag_value": tag_value & 0x3F,
                }
            )

    return {
        "tensor_name": "weight tensor B",
        "axis_labels": ("N", "K_in_stage", "K_stage"),
        "full_shape": (max(1, total_n_tiles * pe_n), pe_k, total_k_tiles),
        "tiles": tiles,
        "base_tile_count": len(tiles),
        "temporal_order_desc": "k_tile -> n_tile; PS ids reset per N-wave",
        "footnote": "GEMM PS is the B[K, N] tensor. The Z axis enumerates K-chain stages, and tile colors follow the actual GEMM ps_id scan-chain rule instead of a fallback AGU D-axis slice.",
        "x_ticks": n_spans,
        "y_ticks": [(0, pe_k)],
        "z_ticks": k_spans,
        "tag_level": agu.tag_ctrl & 0x3,
        "tag_axis_label": "wave-local n_tile",
        "tag_base": agu.tag_base & 0x3F,
        "tag_stride": _agu_tag_stride(agu),
        "tag_formula": (
            f"tag = local_n_tile"
            if ultra_scan
            else f"tag = local_k_stage × {grid_n_per_wave} + local_n_tile"
        ),
        "legend_ids": _legend_ids_from_tiles(tiles),
    }


def _build_gemm_pd_tensor_tile_spec(
    agu: AguBankConfig,
    meta: dict[str, int | bool],
) -> dict[str, object]:
    total_m_tiles = int(meta["grid_m"])
    total_k_tiles = int(meta["grid_k"])
    grid_m_per_wave = int(meta["grid_m_per_wave"])
    grid_k_per_wave = int(meta["grid_k_per_wave"])
    pe_m = int(meta["pe_m"])
    pe_k = int(meta["pe_k"])
    ultra_scan = bool(meta["ultra_scan"])
    m_tile_spans = _actual_tile_spans(total_m_tiles, 1, 1)
    k_spans = _actual_tile_spans(total_k_tiles, pe_k, pe_k)
    tiles = []
    for m_idx, (m_start, m_size) in enumerate(m_tile_spans):
        local_m = m_idx % max(1, grid_m_per_wave)
        for k_idx, (k_start, k_size) in enumerate(k_spans):
            local_k = k_idx % max(1, grid_k_per_wave)
            tag_value = local_m if ultra_scan else (local_k * grid_m_per_wave + local_m)
            tiles.append(
                {
                    "start": (k_start, 0, m_start),
                    "size": (k_size, pe_m, m_size),
                    "order": m_idx * total_k_tiles + k_idx,
                    "tag_value": tag_value & 0x3F,
                }
            )

    return {
        "tensor_name": "activation tensor A",
        "axis_labels": ("K", "M_in_tile", "M_tile"),
        "full_shape": (max(1, total_k_tiles * pe_k), pe_m, total_m_tiles),
        "tiles": tiles,
        "base_tile_count": len(tiles),
        "temporal_order_desc": "m_tile -> k_tile; PD ids reset per M-wave",
        "footnote": "GEMM PD is the A[M, K] tensor. The Z axis enumerates output M tiles, and tile colors follow the actual GEMM pd_id scan-chain rule instead of the fallback local_D1 slice.",
        "x_ticks": k_spans,
        "y_ticks": [(0, pe_m)],
        "z_ticks": m_tile_spans,
        "tag_level": agu.tag_ctrl & 0x3,
        "tag_axis_label": "wave-local m_tile",
        "tag_base": agu.tag_base & 0x3F,
        "tag_stride": _agu_tag_stride(agu),
        "tag_formula": (
            f"tag = local_m_tile"
            if ultra_scan
            else f"tag = local_k_stage × {grid_m_per_wave} + local_m_tile"
        ),
        "legend_ids": _legend_ids_from_tiles(tiles),
    }


def _build_gemm_partial_tensor_tile_spec(
    bank_attr: str,
    agu: AguBankConfig,
    meta: dict[str, int | bool],
) -> dict[str, object]:
    total_m_tiles = int(meta["grid_m"])
    total_n_tiles = int(meta["grid_n"])
    grid_m_per_wave = int(meta["grid_m_per_wave"])
    grid_n_per_wave = int(meta["grid_n_per_wave"])
    pe_m = int(meta["pe_m"])
    pe_n = int(meta["pe_n"])
    n_spans = _actual_tile_spans(total_n_tiles, pe_n, pe_n)
    m_tile_spans = _actual_tile_spans(total_m_tiles, 1, 1)
    tiles = []
    for m_idx, (m_start, m_size) in enumerate(m_tile_spans):
        local_m = m_idx % max(1, grid_m_per_wave)
        for n_idx, (n_start, n_size) in enumerate(n_spans):
            local_n = n_idx % max(1, grid_n_per_wave)
            pe_tile = local_m * grid_n_per_wave + local_n
            tiles.append(
                {
                    "start": (n_start, 0, m_start),
                    "size": (n_size, pe_m, m_size),
                    "order": m_idx * total_n_tiles + n_idx,
                    "tag_value": pe_tile & 0x3F,
                }
            )

    tensor_name = "output partial-sum tensor" if bank_attr == "agu_plo" else "initial partial-sum tensor"
    return {
        "tensor_name": tensor_name,
        "axis_labels": ("N", "M_in_tile", "M_tile"),
        "full_shape": (max(1, total_n_tiles * pe_n), pe_m, total_m_tiles),
        "tiles": tiles,
        "base_tile_count": len(tiles),
        "temporal_order_desc": "m_tile -> n_tile; ids reset per (M-wave, N-wave)",
        "footnote": "GEMM PLI/PLO represent the C tensor. D2 is the wave-local PE tile id (m_idx * grid_n_per_wave + n_idx), so the view shows one cuboid per C tile and colors it with the real scan-chain id set.",
        "x_ticks": n_spans,
        "y_ticks": [(0, pe_m)],
        "z_ticks": m_tile_spans,
        "tag_level": agu.tag_ctrl & 0x3,
        "tag_axis_label": "wave-local pe_tile",
        "tag_base": agu.tag_base & 0x3F,
        "tag_stride": _agu_tag_stride(agu),
        "tag_formula": f"tag = local_m_tile × {grid_n_per_wave} + local_n_tile",
        "legend_ids": _legend_ids_from_tiles(tiles),
    }


def _gemm_pattern_meta(layer: LayerHwConfig) -> dict[str, int | bool]:
    pe_params = layer.pe_program.params
    rows_per_word = 4
    grid_m = max(1, int(pe_params.get("GRID_M", 1)))
    grid_n = max(1, int(pe_params.get("GRID_N", 1)))
    grid_k = max(1, int(pe_params.get("GRID_K", 1)))
    grid_m_per_wave = max(1, int(pe_params.get("GRID_M_PER_WAVE", grid_m)))
    grid_n_per_wave = max(1, int(pe_params.get("GRID_N_PER_WAVE", grid_n)))
    grid_k_per_wave = max(1, int(pe_params.get("GRID_K_PER_WAVE", grid_k)))
    pe_n = max(1, int(pe_params.get("OUTPUT_DIM", 1)))
    pe_m = max(rows_per_word, int(layer.agu_pli.iter0) * rows_per_word)
    pe_k = max(1, int(pe_params.get("K_TILE_DIM", pe_params.get("INPUT_DIM", 1))))
    return {
        "grid_m": grid_m,
        "grid_n": grid_n,
        "grid_k": grid_k,
        "grid_m_per_wave": grid_m_per_wave,
        "grid_n_per_wave": grid_n_per_wave,
        "grid_k_per_wave": grid_k_per_wave,
        "pe_m": pe_m,
        "pe_n": pe_n,
        "pe_k": pe_k,
        "ultra_scan": grid_k_per_wave > 1,
    }


def _actual_tile_spans(tile_count: int, tile_size: int, last_size: int) -> list[tuple[int, int]]:
    spans = []
    for idx in range(tile_count):
        size = last_size if idx == tile_count - 1 else tile_size
        spans.append((idx * tile_size, max(1, size)))
    return spans


def _temporal_bucket_labels_for_orders(order_values: list[int], bucket_count: int) -> list[str]:
    if not order_values:
        return ["t0"]

    total_steps = max(order_values) + 1
    buckets = _partition_axis(max(1, total_steps), bucket_count)
    labels = []
    for start, size in buckets:
        end = start + size - 1
        labels.append(f"t{start}" if start == end else f"t{start}-{end}")
    return labels


def _tile_grid_counts(full_shape: tuple[int, int, int], max_total_tiles: int = 18) -> tuple[int, int, int]:
    counts = [min(3, max(1, extent)) for extent in full_shape]
    if math.prod(counts) == 1:
        major_axis = max(range(3), key=lambda idx: full_shape[idx])
        counts[major_axis] = min(4, max(1, full_shape[major_axis]))
    while math.prod(counts) > max_total_tiles:
        axis = max(range(3), key=lambda idx: counts[idx])
        if counts[axis] == 1:
            break
        counts[axis] -= 1
    return counts[0], counts[1], counts[2]


def _partition_axis(extent: int, tile_count: int) -> list[tuple[int, int]]:
    tile_count = max(1, min(extent, tile_count))
    base = extent // tile_count
    remainder = extent % tile_count
    start = 0
    parts = []
    for idx in range(tile_count):
        size = base + (1 if idx < remainder else 0)
        parts.append((start, max(1, size)))
        start += size
    return parts


def _axis_tick_values(parts: list[tuple[int, int]], extent: int) -> list[int]:
    ticks = [0]
    for start, size in parts:
        ticks.append(start + size)
    unique = sorted(set(ticks))
    if len(unique) > 5:
        return [0, extent // 2, extent]
    return unique


def _build_conv_tag_split_meta(
    bank_attr: str,
    tag_level: int,
    kernel_width: int,
    op_type: str,
    tile_ic: int,
) -> dict[str, object]:
    if bank_attr == "agu_ps":
        if op_type == "conv2d_1x1" and tag_level == 0:
            return {"axis": 0, "step": tile_ic, "label": f"IC pack ({tile_ic}ch)"}
        mapping = {
            0: {"axis": 0, "step": 1, "label": "IC"},
            1: {"axis": 1, "step": 1, "label": "KW"},
            2: {"axis": 1, "step": kernel_width, "label": "KH"},
            3: {"axis": 2, "step": 1, "label": "OC"},
        }
    elif bank_attr == "agu_pd":
        mapping = {
            0: {"axis": 2, "step": 1, "label": "IC"},
            1: {"axis": 1, "step": 1, "label": "H_in"},
            2: {"axis": 0, "step": 1, "label": "W_in"},
            3: {"axis": 2, "step": 1, "label": "IC"},
        }
    else:
        mapping = {
            0: {"axis": 2, "step": 1, "label": "OC"},
            1: {"axis": 1, "step": 1, "label": "H_out"},
            2: {"axis": 0, "step": 1, "label": "W_out"},
            3: {"axis": 2, "step": 1, "label": "OC"},
        }
    return mapping.get(tag_level, mapping[0])


def _build_fallback_tag_split_meta(
    spatial_dims: list[dict[str, int | str]],
    tag_level: int,
) -> dict[str, object]:
    for axis, dim in enumerate(spatial_dims):
        if int(dim["idx"]) == tag_level:
            return {"axis": axis, "step": 1, "label": str(dim["name"])}
    return {"axis": 0, "step": 1, "label": f"D{tag_level}"}


def _make_tensor_tile(
    x_start: int,
    x_size: int,
    y_start: int,
    y_size: int,
    z_start: int,
    z_size: int,
    order: int,
) -> dict[str, object]:
    return {
        "start": (x_start, y_start, z_start),
        "size": (x_size, y_size, z_size),
        "order": order,
    }


def _split_tiles_by_tag(
    base_tiles: list[dict[str, object]],
    split_meta: dict[str, object],
    tag_base: int,
    tag_stride: int,
) -> list[dict[str, object]]:
    axis = int(split_meta["axis"])
    step = max(1, int(split_meta["step"]))
    split_tiles: list[dict[str, object]] = []

    for tile in base_tiles:
        start = list(tile["start"])
        size = list(tile["size"])
        axis_extent = int(size[axis])
        local_index = 0
        offset = 0
        while offset < axis_extent:
            part = min(step, axis_extent - offset)
            part_start = start[:]
            part_size = size[:]
            part_start[axis] += offset
            part_size[axis] = part
            split_tiles.append(
                {
                    "start": tuple(part_start),
                    "size": tuple(part_size),
                    "order": int(tile["order"]),
                    "tag_value": (int(tag_base) + local_index * int(tag_stride)) & 0x3F,
                }
            )
            offset += part
            local_index += 1

    return split_tiles


def _legend_ids_from_tiles(tiles: list[dict[str, object]]) -> list[int]:
    return sorted({int(tile["tag_value"]) for tile in tiles})


def _bank_scan_meta(scan_chain: list[ScanChainEntry], bank_attr: str) -> dict[str, object]:
    meta = _BANK_TAG_META[bank_attr]
    scan_attr = meta["scan_attr"]
    active_ids = sorted(
        {
            getattr(entry, scan_attr)
            for entry in scan_chain
            if entry.enable and getattr(entry, scan_attr) != 63
        }
    )
    return {
        "scan_attr": scan_attr,
        "scan_label": meta["scan_label"],
        "active_ids": active_ids,
    }


def _agu_tag_stride(agu: AguBankConfig) -> int:
    tag_level = agu.tag_ctrl & 0x3
    return agu.tag_stride0 if tag_level == 0 else agu.tag_stride1


def _render_pattern_id_legend(ids: list[int], label: str) -> str:
    chips = []
    for value in ids:
        bg_hex, _text_hex, border_hex, _face_rgb, _edge_rgb = _port_id_palette(value)
        chips.append(
            f"<span class=\"pattern-id-chip\"><span class=\"pattern-id-dot\" style=\"background:{bg_hex}; border-color:{border_hex};\"></span>{label} {value}</span>"
        )
    return f"<div class=\"pattern-id-legend\">{''.join(chips)}</div>"


def _port_id_palette(value: int) -> tuple[str, str, str, tuple[float, float, float], tuple[float, float, float]]:
    if value == 63:
        face_rgb = _hsl_to_rgb_tuple(35.0, 0.22, 0.90)
        edge_rgb = _hsl_to_rgb_tuple(35.0, 0.16, 0.68)
        text_rgb = _hsl_to_rgb_tuple(35.0, 0.10, 0.49)
    else:
        hue = float((value * 47) % 360)
        face_rgb = _hsl_to_rgb_tuple(hue, 0.78, 0.88)
        edge_rgb = _hsl_to_rgb_tuple(hue, 0.54, 0.68)
        text_rgb = _hsl_to_rgb_tuple(hue, 0.52, 0.24)
    return (
        _rgb_to_hex(face_rgb),
        _rgb_to_hex(text_rgb),
        _rgb_to_hex(edge_rgb),
        face_rgb,
        edge_rgb,
    )


def _hsl_to_rgb_tuple(hue_deg: float, saturation: float, lightness: float) -> tuple[float, float, float]:
    return colorsys.hls_to_rgb(hue_deg / 360.0, lightness, saturation)


def _rgb_to_hex(rgb: tuple[float, float, float]) -> str:
    return "#" + "".join(f"{round(channel * 255):02x}" for channel in rgb)


def _temporal_bucket_labels(bucket_count: int, temporal_dim: dict[str, int | str] | None) -> list[str]:
    if temporal_dim is None:
        return [f"Q{bucket}" for bucket in range(bucket_count)]

    temporal_parts = _partition_axis(int(temporal_dim["iter"]), bucket_count)
    return [
        f"{temporal_dim['name']}[{start}:{start + size}]"
        for start, size in temporal_parts
    ]


def _box_faces(x_extent: float, y_extent: float, z_extent: float, dx: float, dy: float, dz: float) -> list[list[tuple[float, float, float]]]:
    x0, y0, z0 = x_extent, y_extent, z_extent
    x1, y1, z1 = x0 + dx, y0 + dy, z0 + dz
    return [
        [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)],
        [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],
        [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)],
        [(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)],
        [(x0, y0, z0), (x0, y1, z0), (x0, y1, z1), (x0, y0, z1)],
        [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)],
    ]


def _draw_wireframe_box(ax: object, x_extent: float, y_extent: float, z_extent: float) -> None:
    edges = [
        ((0.0, 0.0, 0.0), (x_extent, 0.0, 0.0)),
        ((0.0, 0.0, 0.0), (0.0, y_extent, 0.0)),
        ((0.0, 0.0, 0.0), (0.0, 0.0, z_extent)),
        ((x_extent, 0.0, 0.0), (x_extent, y_extent, 0.0)),
        ((x_extent, 0.0, 0.0), (x_extent, 0.0, z_extent)),
        ((0.0, y_extent, 0.0), (x_extent, y_extent, 0.0)),
        ((0.0, y_extent, 0.0), (0.0, y_extent, z_extent)),
        ((0.0, 0.0, z_extent), (x_extent, 0.0, z_extent)),
        ((0.0, 0.0, z_extent), (0.0, y_extent, z_extent)),
        ((x_extent, y_extent, 0.0), (x_extent, y_extent, z_extent)),
        ((x_extent, 0.0, z_extent), (x_extent, y_extent, z_extent)),
        ((0.0, y_extent, z_extent), (x_extent, y_extent, z_extent)),
    ]
    for start, end in edges:
        ax.plot(
            [start[0], end[0]],
            [start[1], end[1]],
            [start[2], end[2]],
            color="#ad9d8a",
            linewidth=1.0,
            linestyle=(0, (4, 4)),
            alpha=0.72,
        )


def _display_box_aspect(full_x: float, full_y: float, full_z: float) -> tuple[float, float, float]:
    extents = [max(1.0, full_x), max(1.0, full_y), max(1.0, full_z)]
    compressed = [math.pow(extent, 0.58) for extent in extents]
    max_extent = max(compressed)
    if max_extent <= 0.0:
        return (1.0, 1.0, 1.0)

    normalized = [extent / max_extent for extent in compressed]
    return tuple(0.80 + 1.40 * value for value in normalized)


def _layout_pattern_labels(
    ax: object,
    canvas: object,
    label_specs: list[tuple[float, float, float, str, float, str, tuple[float, float, float]]],
) -> None:
    renderer = canvas.get_renderer()
    axis_bbox = ax.get_window_extent(renderer=renderer)
    axes_inverse = ax.transAxes.inverted()
    placed_boxes: list[tuple[float, float, float, float]] = []

    ordered_specs = sorted(label_specs, key=lambda item: (item[2], item[4]), reverse=True)
    for label_x, label_y, label_z, label_text, label_font, text_hex, edge_rgb in ordered_specs:
        proj_x, proj_y, _ = proj3d.proj_transform(label_x, label_y, label_z, ax.get_proj())
        disp_x, disp_y = ax.transData.transform((proj_x, proj_y))

        font_px = max(10.0, label_font * ax.figure.dpi / 72.0)
        box_width = max(18.0, len(label_text) * font_px * 0.72 + 12.0)
        box_height = max(16.0, font_px * 1.25 + 8.0)
        candidate_offsets = [
            (0.0, font_px * 0.95),
            (-box_width * 0.62, font_px * 1.18),
            (box_width * 0.62, font_px * 1.18),
            (0.0, font_px * 1.72),
            (-box_width * 0.80, font_px * 1.72),
            (box_width * 0.80, font_px * 1.72),
            (0.0, -box_height * 0.90),
        ]

        chosen = None
        fallback = None
        min_margin = 6.0
        for offset_x, offset_y in candidate_offsets:
            center_x = disp_x + offset_x
            center_y = disp_y + offset_y
            left = center_x - box_width / 2.0
            right = center_x + box_width / 2.0
            bottom = center_y - box_height / 2.0
            top = center_y + box_height / 2.0

            if fallback is None:
                fallback = (center_x, center_y, left, right, bottom, top)

            if left < axis_bbox.x0 + min_margin or right > axis_bbox.x1 - min_margin:
                continue
            if bottom < axis_bbox.y0 + min_margin or top > axis_bbox.y1 - min_margin:
                continue
            if any(_pattern_boxes_overlap((left, right, bottom, top), existing) for existing in placed_boxes):
                continue
            chosen = (center_x, center_y, left, right, bottom, top)
            break

        if chosen is None and fallback is not None:
            center_x, center_y, left, right, bottom, top = fallback
            center_x = min(axis_bbox.x1 - min_margin - box_width / 2.0,
                           max(axis_bbox.x0 + min_margin + box_width / 2.0, center_x))
            center_y = min(axis_bbox.y1 - min_margin - box_height / 2.0,
                           max(axis_bbox.y0 + min_margin + box_height / 2.0, center_y))
            left = center_x - box_width / 2.0
            right = center_x + box_width / 2.0
            bottom = center_y - box_height / 2.0
            top = center_y + box_height / 2.0
            if any(_pattern_boxes_overlap((left, right, bottom, top), existing, margin=1.5) for existing in placed_boxes):
                continue
            chosen = (center_x, center_y, left, right, bottom, top)

        if chosen is None:
            continue

        center_x, center_y, left, right, bottom, top = chosen
        placed_boxes.append((left, right, bottom, top))
        axes_x, axes_y = axes_inverse.transform((center_x, center_y))
        ax.text2D(
            axes_x,
            axes_y,
            label_text,
            transform=ax.transAxes,
            color=text_hex,
            fontsize=label_font,
            fontweight="bold",
            ha="center",
            va="center",
            bbox={
                "boxstyle": "round,pad=0.16",
                "fc": (1.0, 0.99, 0.96, 0.84),
                "ec": (edge_rgb[0], edge_rgb[1], edge_rgb[2], 0.55),
                "lw": 0.55,
            },
            clip_on=False,
            zorder=50,
        )


def _pattern_boxes_overlap(
    box_a: tuple[float, float, float, float],
    box_b: tuple[float, float, float, float],
    margin: float = 3.0,
) -> bool:
    left_a, right_a, bottom_a, top_a = box_a
    left_b, right_b, bottom_b, top_b = box_b
    return not (
        right_a + margin < left_b
        or right_b + margin < left_a
        or top_a + margin < bottom_b
        or top_b + margin < bottom_a
    )


def _folded_progress(
    coord_map: dict[int, int],
    dims: list[dict[str, int | str]],
    temporal_dim: dict[str, int | str] | None,
) -> float:
    if temporal_dim is None:
        return _access_progress(coord_map, dims)

    sampled = _sample_axis_indices(int(temporal_dim["iter"]), max_samples=6)
    temporal_idx = int(temporal_dim["idx"])
    progress_values = []
    for temporal_value in sampled:
        sample_coord = coord_map.copy()
        sample_coord[temporal_idx] = temporal_value
        progress_values.append(_access_progress(sample_coord, dims))
    return sum(progress_values) / len(progress_values)


def _agu_dimension_records(agu: AguBankConfig) -> list[tuple[str, int, int]]:
    return [
        ("D0", agu.iter0, agu.stride0),
        ("D1", agu.iter1, agu.stride1),
        ("D2", agu.iter2, agu.stride2),
        ("D3", agu.iter3, agu.stride3),
    ]


def _agu_dimension_info(agu: AguBankConfig) -> list[dict[str, int | str]]:
    return [
        {"idx": 0, "name": "D0", "iter": max(1, agu.iter0), "stride": agu.stride0},
        {"idx": 1, "name": "D1", "iter": max(1, agu.iter1), "stride": agu.stride1},
        {"idx": 2, "name": "D2", "iter": max(1, agu.iter2), "stride": agu.stride2},
        {"idx": 3, "name": "D3", "iter": max(1, agu.iter3), "stride": agu.stride3},
    ]


def _select_spatial_dims(
    dims: list[dict[str, int | str]],
    preferred_idx: int | None = None,
) -> tuple[list[dict[str, int | str]], list[dict[str, int | str]]]:
    iter_dims = [dim for dim in dims if int(dim["iter"]) > 1]
    stride_dims = [dim for dim in dims if int(dim["iter"]) <= 1 and int(dim["stride"]) != 0]
    rest_dims = [dim for dim in dims if dim not in iter_dims and dim not in stride_dims]
    ordered = iter_dims + stride_dims + rest_dims
    if preferred_idx is not None:
        preferred = next((dim for dim in dims if int(dim["idx"]) == preferred_idx), None)
        if preferred is not None and preferred not in ordered[:3]:
            leading = [preferred]
            for dim in ordered:
                if dim is preferred:
                    continue
                if len(leading) < 3:
                    leading.append(dim)
            ordered = leading + [dim for dim in ordered if dim not in leading]
    return ordered[:3], ordered[3:]


def _sample_axis_indices(iter_count: int, max_samples: int = 4) -> list[int]:
    if iter_count <= 1:
        return [0]
    if iter_count <= max_samples:
        return list(range(iter_count))
    positions = {
        round(step * (iter_count - 1) / (max_samples - 1))
        for step in range(max_samples)
    }
    return sorted(positions)


def _access_progress(coord_map: dict[int, int], dims: list[dict[str, int | str]]) -> float:
    total = 1
    for dim in dims:
        total *= int(dim["iter"])

    linear = 0
    multiplier = 1
    for dim in dims:
        linear += coord_map.get(int(dim["idx"]), 0) * multiplier
        multiplier *= int(dim["iter"])

    if total <= 1:
        return 0.0
    return linear / (total - 1)


def _agu_address_expression(dims: list[dict[str, int | str]]) -> str:
    terms = []
    for dim in dims:
        stride = int(dim["stride"])
        if stride == 0:
            continue
        terms.append(f"{dim['name']}×{stride}")
    if not terms:
        return "base"
    return "base + " + " + ".join(terms)


def _agu_ctrl_flags(ctrl: int) -> str:
    flags: list[str] = []
    if ctrl & 0x1:
        flags.append("start")
    if ctrl & 0x8:
        flags.append("ultra")
    remaining = ctrl & ~0x9
    if remaining:
        flags.append(f"raw=0x{ctrl:X}")
    return ", ".join(flags) if flags else f"0x{ctrl:X}"


def _fmt_bytes(value: int) -> str:
    if value >= 1024 * 1024:
        return _fmt_scaled_unit(value / (1024 * 1024), "MiB")
    if value >= 1024:
        return _fmt_scaled_unit(value / 1024, "KiB")
    return f"{value} B"


def _fmt_scaled_unit(value: float, unit: str) -> str:
    if float(value).is_integer():
        return f"{int(value)} {unit}"
    return f"{value:.2f}".rstrip("0").rstrip(".") + f" {unit}"


def _tint(hex_color: str, factor: float) -> str:
    color = hex_color.lstrip("#")
    if len(color) != 6:
        return hex_color
    channels = [int(color[i:i + 2], 16) for i in range(0, 6, 2)]
    tinted = [min(255, max(0, round(channel * factor + 255 * (1.0 - factor)))) for channel in channels]
    return "#" + "".join(f"{channel:02x}" for channel in tinted)