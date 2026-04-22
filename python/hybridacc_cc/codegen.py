"""Stage 2: Code Generation — HardwareIR → C firmware source files.

Uses Jinja2 templates to render firmware_hw.h, firmware_payload.h,
firmware_data.c, firmware_ops.c, firmware_main.c, and linker.ld.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any, Dict, List

from .ir import HardwareIR, LayerHwConfig
from .pe_payload import collect_payload_context

_TEMPLATE_DIR = Path(__file__).resolve().parent / "templates"


def _has_hw_multiply(march: str) -> bool:
    """Check if the march string includes hardware multiply support."""
    march_l = march.lower()
    if 'zmmul' in march_l:
        return True
    # Full M extension: 'm' in base ISA chars (e.g. rv32im)
    base = march_l.split('_')[0]
    isa_chars = base.replace('rv32', '').replace('rv64', '')
    return 'm' in isa_chars


def _pack_agu_regs(agu) -> List[int]:
    """Convert an AguBankConfig to a 15-element uint32 register array."""
    return agu.to_regs()


def _format_hex(val: int, width: int = 8) -> str:
    """Format an integer as a C hex literal."""
    return f"0x{val & 0xFFFFFFFF:0{width}X}u"


def _layer_runner_name(op_type: str) -> str:
    """Return the generated firmware runner symbol for an op type."""
    if op_type == "conv2d_3x3":
        return "run_layer_conv2d_3x3"
    if op_type == "conv2d_1x1":
        return "run_layer_conv2d_1x1"
    if op_type == "gemm":
        return "run_layer_gemm"
    raise ValueError(f"Unsupported op type for firmware runner: {op_type}")


def prepare_template_context(hw_ir: HardwareIR,
                             kernel_json_dir: Path | None = None,
                             stack_size: int = 4096,
                             march: str = "rv32i_zmmul_zicsr") -> Dict[str, Any]:
    """Build the complete Jinja2 template context from HardwareIR."""

    payload = collect_payload_context(hw_ir.layers, kernel_json_dir)

    layers_ctx: List[Dict[str, Any]] = []
    used_runners: List[str] = []
    for i, layer in enumerate(hw_ir.layers):
        lp = payload["layer_payloads"][i]
        tp = layer.tiling_params
        runner = _layer_runner_name(layer.op_type)
        if runner not in used_runners:
            used_runners.append(runner)

        layers_ctx.append({
            "name": layer.name,
            "index": i,
            "op_type": layer.op_type,
            "runner": runner,
            "cluster_mask_lo": layer.target_cluster_mask & 0xFFFFFFFF,
            "cluster_mask_hi": (layer.target_cluster_mask >> 32) & 0xFFFFFFFF,
            "num_clusters": layer.cluster_mapping.active_clusters,
            "hddu_plane_en": layer.hddu.plane_en,
            "hddu_plane_mode": layer.hddu.plane_mode,
            "agu_ps": _pack_agu_regs(layer.agu_ps),
            "agu_pd": _pack_agu_regs(layer.agu_pd),
            "agu_pli": _pack_agu_regs(layer.agu_pli),
            "agu_plo": _pack_agu_regs(layer.agu_plo),
            "scan_chain_symbol": lp["scan_chain_symbol"],
            "scan_chain_len": lp["scan_chain_len"],
            "pe_template_symbol": lp["template_symbol"],
            "pe_template_len": lp["template_len"],
            "patch_symbol": lp["patch_symbol"],
            "patch_count": lp["patch_count"],
            "patch_entries": lp["patch_entries"],
            "tiling": {
                "num_oc_tiles": tp.num_oc_tiles,
                "num_h_tiles": tp.num_h_tiles,
                "num_w_tiles": tp.num_w_tiles,
                "num_ic_tiles": tp.num_ic_tiles,
                "tile_h_out": tp.tile_h_out,
                "tile_w_out": tp.tile_w_out,
                "tile_h_in": tp.tile_h_in,
                "tile_w_in": tp.tile_w_in,
                "last_h_out": tp.last_h_out,
                "last_w_out": tp.last_w_out,
                "spm_ping": tp.spm_ping,
                "spm_pong": tp.spm_pong,
                "agu_ping": tp.agu_ping,
                "agu_pong": tp.agu_pong,
                "spm_map_even": tp.spm_map_even,
                "spm_map_odd": tp.spm_map_odd,
                "dram_weight_base": tp.dram_weight_base,
                "dram_input_base": tp.dram_input_base,
                "dram_output_base": tp.dram_output_base,
                "dram_ps_oc_stride": tp.dram_ps_oc_stride,
                "dram_ps_h_stride": tp.dram_ps_h_stride,
                "dram_ps_ic_stride": tp.dram_ps_ic_stride,
                "dram_pd_oc_stride": tp.dram_pd_oc_stride,
                "dram_pd_h_stride": tp.dram_pd_h_stride,
                "dram_pd_w_stride": tp.dram_pd_w_stride,
                "dram_pd_ic_stride": tp.dram_pd_ic_stride,
                "dram_out_oc_stride": tp.dram_out_oc_stride,
                "dram_out_h_stride": tp.dram_out_h_stride,
                "dram_out_w_stride": tp.dram_out_w_stride,
                "dma_ps_words": tp.dma_ps_words,
                "dma_pd_words": tp.dma_pd_words,
                "dma_plo_words": tp.dma_plo_words,
                "dram_bias_base": tp.dram_bias_base,
                "dram_bias_oc_stride": tp.dram_bias_oc_stride,
                "dram_bias_h_stride": tp.dram_bias_h_stride,
                "dma_pli_words": tp.dma_pli_words,
                "ps_reuse_across_spatial": 1 if tp.ps_reuse_across_spatial else 0,
                "spatial_2d_dma": 1 if tp.spatial_2d_dma else 0,
                "pd_ic_agu_offset": tp.pd_ic_agu_offset,
                "bank_depth_bytes": tp.bank_depth_bytes,
                "parallel_groups": tp.parallel_groups,
                "dma_pd_rows_per_bank": tp.dma_pd_rows_per_bank,
                "dma_pli_rows_per_bank": tp.dma_pli_rows_per_bank,
                "dma_plo_rows_per_bank": tp.dma_plo_rows_per_bank,
                "dma_ps_words_per_bank": tp.dma_ps_words_per_bank,
                "dma_pd_words_per_bank": tp.dma_pd_words_per_bank,
                "dma_plo_words_per_bank": tp.dma_plo_words_per_bank,
                "gemm_resident_m_tiles": tp.gemm_resident_m_tiles,
                "gemm_resident_n_tiles": tp.gemm_resident_n_tiles,
            },
        })

    return {
        "workload_name": hw_ir.workload_name,
        "isram_size": "16K",
        "dsram_size": "64K",
        "stack_size": stack_size,
        "has_hw_mul": _has_hw_multiply(march),
        "num_layers": len(hw_ir.layers),
        "templates": payload["templates"],
        "scan_chains": payload["scan_chains"],
        "layers": layers_ctx,
        "used_runners": used_runners,
        "hex": _format_hex,
    }


def generate_firmware(hw_ir: HardwareIR,
                      output_dir: Path,
                      kernel_json_dir: Path | None = None,
                      template_dir: Path | None = None,
                      stack_size: int = 4096,
                      march: str = "rv32i_zmmul_zicsr") -> List[Path]:
    """Render all firmware source files to output_dir.

    Returns list of generated file paths.
    """
    tpl_dir = template_dir or _TEMPLATE_DIR
    from jinja2 import Environment, FileSystemLoader
    env = Environment(
        loader=FileSystemLoader(str(tpl_dir)),
        keep_trailing_newline=True,
        trim_blocks=True,
        lstrip_blocks=True,
    )
    env.filters["hex"] = _format_hex

    ctx = prepare_template_context(hw_ir, kernel_json_dir, stack_size=stack_size, march=march)
    output_dir.mkdir(parents=True, exist_ok=True)

    generated: List[Path] = []
    file_map = {
        "firmware_hw.h.j2": "firmware_hw.h",
        "firmware_payload.h.j2": "firmware_payload.h",
        "firmware_data.c.j2": "firmware_data.c",
        "firmware_ops.c.j2": "firmware_ops.c",
        "firmware_main.c.j2": "firmware_main.c",
        "linker.ld.j2": "linker.ld",
    }

    for tpl_name, out_name in file_map.items():
        template = env.get_template(tpl_name)
        rendered = template.render(ctx)
        out_path = output_dir / out_name
        out_path.write_text(rendered, encoding="utf-8")
        generated.append(out_path)

    return generated
