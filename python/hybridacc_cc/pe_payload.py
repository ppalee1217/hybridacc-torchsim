"""Stage 3: PE Payload Preparation.

Loads kernel JSON metadata, computes N-1 patch encoding, emits C arrays
for PE templates, scan chains, and patch descriptors.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, List, Tuple

from .ir import LayerHwConfig, ScanChainEntry

# NOC command IDs
NOC_CMD_SCAN_CHAIN = 8

# ── Default kernel JSON search path ──
_KERNEL_JSON_DIR = (
    Path(__file__).resolve().parents[2]
    / "design" / "hybridacc-cc" / "kernel" / "json"
)


def _template_name_to_json_stem(template_name: str) -> str:
    """conv1d_k3c4s1_template → conv1d_k3c4s1"""
    return template_name.removesuffix("_template")


def load_template_json(template_name: str,
                       search_dir: Path | None = None) -> Dict[str, Any]:
    """Load and return parsed kernel JSON for a given template name."""
    d = Path(search_dir) if search_dir else _KERNEL_JSON_DIR
    stem = _template_name_to_json_stem(template_name)
    path = d / f"{stem}.json"
    if not path.exists():
        raise FileNotFoundError(f"Kernel JSON not found: {path}")
    with open(path, "r") as f:
        return json.load(f)


# ===================================================================
# Scan-chain pre-encoding
# ===================================================================

def encode_scan_chain(scan_chain: List[ScanChainEntry]) -> List[int]:
    """Pre-encode scan chain entries to uint32 NOC command words (reversed)."""
    words: List[int] = []
    for entry in reversed(scan_chain):
        v = 0
        v |= (entry.ps_id & 0x3F) << 4
        v |= (entry.pd_id & 0x3F) << 10
        v |= (entry.pli_id & 0x3F) << 16
        v |= (entry.plo_id & 0x3F) << 22
        v |= (entry.route_mode & 0x03) << 28
        v |= (1 if entry.enable else 0) << 30
        words.append((v & 0xFFFFFFF0) | (NOC_CMD_SCAN_CHAIN & 0x0F))
    return words


def hash_scan_chain(scan_chain: List[ScanChainEntry]) -> str:
    """Produce a hashable key for scan-chain topology dedup."""
    parts = []
    for e in scan_chain:
        parts.append(f"{e.ps_id},{e.pd_id},{e.pli_id},{e.plo_id},"
                     f"{e.route_mode},{e.enable}")
    return "|".join(parts)


def build_gemm_tail_scan_chain(layer: LayerHwConfig) -> List[ScanChainEntry]:
    """Derive the final uneven-K topology from a GEMM full-wave chain.

    The first active bus keeps PLI-from-bus, the final active bus becomes the
    PLO-to-bus producer, and later buses are disabled.  PS/PD IDs are retained
    from each active bus of the full chain; PLI/PLO IDs are copied from the
    original first/final buses so the output-tag geometry remains unchanged.
    """
    meta = layer.gemm_k_wave
    if meta is None or not meta.tail_reconfigure:
        return []
    if meta.full_stages <= 0 or meta.tail_stages <= 0:
        raise ValueError(f"Invalid GEMM K-wave stage metadata for {layer.name}")
    if meta.tail_stages >= meta.full_stages:
        raise ValueError(f"GEMM tail is not shorter than full wave for {layer.name}")
    if len(layer.scan_chain) % meta.full_stages != 0:
        raise ValueError(f"GEMM scan chain cannot be split by K stages for {layer.name}")

    pes_per_bus = len(layer.scan_chain) // meta.full_stages
    tail_chain: List[ScanChainEntry] = []

    def route_mode(stage: int) -> int:
        if meta.tail_stages == 1:
            return 3  # PLI_FROM_BUS_PLO_TO_BUS
        if stage == 0:
            return 1  # PLI_FROM_BUS_PLO_TO_LN
        if stage + 1 == meta.tail_stages:
            return 2  # PLI_FROM_LN_PLO_TO_BUS
        return 0      # PLI_FROM_LN_PLO_TO_LN

    for stage in range(meta.full_stages):
        for pe_local in range(pes_per_bus):
            if stage >= meta.tail_stages:
                tail_chain.append(ScanChainEntry(
                    ps_id=63, pd_id=63, pli_id=63, plo_id=63,
                    route_mode=3, enable=False,
                ))
                continue

            source = layer.scan_chain[stage * pes_per_bus + pe_local]
            first = layer.scan_chain[pe_local]
            final = layer.scan_chain[
                (meta.full_stages - 1) * pes_per_bus + pe_local
            ]
            tail_chain.append(ScanChainEntry(
                ps_id=source.ps_id,
                pd_id=source.pd_id,
                pli_id=first.pli_id if stage == 0 else 63,
                plo_id=(final.plo_id
                        if stage + 1 == meta.tail_stages else 63),
                route_mode=route_mode(stage),
                enable=source.enable,
            ))

    return tail_chain


# ===================================================================
# PE patch N-1 encoding
# ===================================================================

def generate_patch_entries(json_data: Dict[str, Any],
                           params: Dict[str, int]) -> List[Dict[str, int]]:
    """Generate PePatchEntry descriptors for runtime patching.

    Performs N-1 encoding for LOOPIN, xDMA.LEN, xDMA.LOOP opcodes.
    Returns list of dicts: [{"offset": int, "encoded_val": int}, ...]
    """
    param_defs = json_data["parameters"]
    param_values = {p["name"]: params.get(p["name"], p["default"])
                    for p in param_defs}

    entries: List[Dict[str, int]] = []
    for patch in json_data["patches"]:
        offset = patch["offset"]
        param_idx = patch["param_index"]
        pname = param_defs[param_idx]["name"]
        value = param_values[pname]

        # Decode instruction to detect N-1 encoding requirement
        word = json_data["instructions"][offset]["dec"]
        opcode = (word >> 1) & 0x3
        func2 = (word >> 3) & 0x3

        if opcode == 0b10 and func2 == 0b00:           # LOOPIN
            encoded = value - 1
        elif opcode == 0b00 and func2 in (0b01, 0b10):  # xDMA.LEN / xDMA.LOOP
            encoded = value - 1
        else:
            encoded = value

        if encoded < 0 or encoded > 1023:
            raise ValueError(
                f"E_PATCH_OVERFLOW: param={pname}, value={value}, "
                f"encoded={encoded} at instruction offset {offset}"
            )
        entries.append({"offset": offset, "encoded_val": encoded & 0x3FF})

    return entries


def find_patch_offset(json_data: Dict[str, Any], param_name: str) -> int | None:
    """Return the instruction offset patched by the given template param."""
    param_defs = json_data["parameters"]
    for patch in json_data["patches"]:
        pname = param_defs[patch["param_index"]]["name"]
        if pname == param_name:
            return int(patch["offset"])
    return None


# ===================================================================
# Template context assembly for Jinja2
# ===================================================================

def collect_payload_context(
    layers: List[LayerHwConfig],
    kernel_json_dir: Path | None = None,
) -> Dict[str, Any]:
    """Collect all PE payload data for Jinja2 rendering.

    Returns:
        {
            "templates": {name: {"symbol": str, "instructions": [int], "len": int}},
            "scan_chains": {key: {"symbol": str, "words": [int], "len": int}},
            "layer_payloads": [
                {
                    "name": str,
                    "template_symbol": str,
                    "template_len": int,
                    "scan_chain_symbol": str,
                    "scan_chain_len": int,
                    "tail_scan_chain_symbol": str,
                    "tail_scan_chain_len": int,
                    "patch_symbol": str,
                    "patch_entries": [{"offset": int, "encoded_val": int}],
                    "patch_count": int,
                },
                ...
            ]
        }
    """
    templates: Dict[str, Dict] = {}
    scan_chains: Dict[str, Dict] = {}
    layer_payloads: List[Dict] = []

    for layer in layers:
        tmpl_name = layer.pe_program.template_name

        # Template dedup
        if tmpl_name not in templates:
            jdata = load_template_json(tmpl_name, kernel_json_dir)
            symbol = f"pe_tmpl_{_template_name_to_json_stem(tmpl_name)}"
            instructions = [e["dec"] for e in jdata["instructions"]]
            templates[tmpl_name] = {
                "symbol": symbol,
                "instructions": instructions,
                "len": len(instructions),
            }

        # Full-wave scan chain dedup
        topo_key = hash_scan_chain(layer.scan_chain)
        if topo_key not in scan_chains:
            encoded = encode_scan_chain(layer.scan_chain)
            num_pes = len(layer.scan_chain)
            idx = len(scan_chains)
            suffix = "" if idx == 0 else f"_{idx}"
            scan_chains[topo_key] = {
                "symbol": f"noc_scan_chain_{num_pes}pe{suffix}",
                "words": encoded,
                "len": len(encoded),
            }

        # Uneven final K-wave scan chain.  This is codegen-derived from the
        # lowering metadata so HardwareIR carries the contract rather than a
        # firmware-only shape heuristic.
        tail_scan_chain = build_gemm_tail_scan_chain(layer)
        tail_topo_key = hash_scan_chain(tail_scan_chain) if tail_scan_chain else ""
        if tail_scan_chain and tail_topo_key not in scan_chains:
            encoded = encode_scan_chain(tail_scan_chain)
            num_pes = len(tail_scan_chain)
            idx = len(scan_chains)
            suffix = "" if idx == 0 else f"_{idx}"
            scan_chains[tail_topo_key] = {
                "symbol": f"noc_scan_chain_{num_pes}pe{suffix}",
                "words": encoded,
                "len": len(encoded),
            }

        # Per-layer patch
        jdata = load_template_json(tmpl_name, kernel_json_dir)
        patch_entries = generate_patch_entries(jdata, layer.pe_program.params)
        patch_sym = f"patch_{layer.name}"
        gemm_kernel_prefetch_offset = find_patch_offset(jdata, "NUM_OF_KERNEL_PREFETCH_SETS")
        gemm_kernel_load_offset = find_patch_offset(jdata, "NUM_OF_KERNEL_LOAD_LOOP")
        gemm_kernel_reuse_offset = find_patch_offset(jdata, "NUM_OF_KERNEL_REUSE_LOOP")

        layer_payloads.append({
            "name": layer.name,
            "template_symbol": templates[tmpl_name]["symbol"],
            "template_len": templates[tmpl_name]["len"],
            "scan_chain_symbol": scan_chains[topo_key]["symbol"],
            "scan_chain_len": scan_chains[topo_key]["len"],
            "tail_scan_chain_symbol": (
                scan_chains[tail_topo_key]["symbol"] if tail_scan_chain else "0"
            ),
            "tail_scan_chain_len": (
                scan_chains[tail_topo_key]["len"] if tail_scan_chain else 0
            ),
            "patch_symbol": patch_sym,
            "patch_entries": patch_entries,
            "patch_count": len(patch_entries),
            "gemm_kernel_prefetch_offset": gemm_kernel_prefetch_offset,
            "gemm_kernel_load_offset": gemm_kernel_load_offset,
            "gemm_kernel_reuse_offset": gemm_kernel_reuse_offset,
        })

    return {
        "templates": templates,
        "scan_chains": scan_chains,
        "layer_payloads": layer_payloads,
    }
