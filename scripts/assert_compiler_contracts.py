"""Assert compiler contracts on dumped hardware_ir.json.

Used by the conv1x1 / GEMM lowering modification plan (see
output/compiler_modification_plan.md) for per-phase artifact-level
verification.

Usage:
    python scripts/assert_compiler_contracts.py <out_root>

Where <out_root> contains subdirectories named after the testcase
yaml stem (e.g. test_conv1x1_s1_ic12oc16h16) each holding a
hardware_ir.json file produced by `hybridacc_cc.cli ... --dump-ir`.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any, Dict, List


# ---------------------------------------------------------------------------
# Expected contracts per testcase.
# Each contract is a dict with keys interpreted by the asserts below:
#   tiling: dict of expected tiling_params subset
#   pe_params: dict of expected pe_program.params subset
#   active_pe: int (total enabled scan-chain entries)
#   buses: dict {bus_idx: {"enabled": int, "modes": set[int]}}
# ---------------------------------------------------------------------------

CONTRACTS: Dict[str, Dict[str, Any]] = {
    "test_conv1x1_s1_ic12oc16h16": {
        "tiling": {
            "num_oc_tiles": 1, "num_h_tiles": 1, "num_w_tiles": 1,
            "num_ic_tiles": 1, "tile_h_out": 16, "tile_w_out": 10,
            "last_h_out": 16,
        },
        "pe_params": {
            "OUTPUT_WINDOW_CNT_MINUS_ONE": 9,
            "KERNEL_LOOP_OUTER": 1, "KERNEL_LOOP_INNER": 1,
        },
        "active_pe": 16,
        "buses": {
            0: {"enabled": 16, "modes": {3}},
            1: {"enabled": 0,  "modes": set()},
            2: {"enabled": 0,  "modes": set()},
        },
    },
    "test_conv1x1_s1_ic12oc16h48": {
        "tiling": {
            "num_oc_tiles": 1, "num_h_tiles": 1, "num_w_tiles": 1,
            "num_ic_tiles": 1, "tile_h_out": 48, "tile_w_out": 10,
            "last_h_out": 48,
        },
        "pe_params": {
            "OUTPUT_WINDOW_CNT_MINUS_ONE": 9,
            "KERNEL_LOOP_OUTER": 1, "KERNEL_LOOP_INNER": 1,
        },
        "active_pe": 48,
        "buses": {
            0: {"enabled": 16, "modes": {3}},
            1: {"enabled": 16, "modes": {3}},
            2: {"enabled": 16, "modes": {3}},
        },
    },
    "test_conv1x1_s1_ic12oc16": {
        "tiling": {"num_w_tiles": 1, "tile_h_out": 4, "tile_w_out": 4},
        "pe_params": {"OUTPUT_WINDOW_CNT_MINUS_ONE": 3},
        "active_pe": 4,
        "buses": {
            0: {"enabled": 4, "modes": {3}},
            1: {"enabled": 0, "modes": set()},
            2: {"enabled": 0, "modes": set()},
        },
    },
    "test_gemm_s1_m8k16n16": {
        "pe_params": {
            "GRID_M": 1, "GRID_N": 2, "GRID_K": 1,
            "GRID_M_PER_WAVE": 1, "GRID_N_PER_WAVE": 2, "GRID_K_PER_WAVE": 1,
        },
        "active_pe": 2,
        "buses": {
            0: {"enabled": 2, "modes": {3}},   # single K-stage → (IB, OB)
            1: {"enabled": 0, "modes": set()},
            2: {"enabled": 0, "modes": set()},
        },
        "gemm_planes": True,
    },
    "test_gemm_s2_m16k64n16": {
        "pe_params": {
            "GRID_M": 2, "GRID_N": 2, "GRID_K": 2,
            "GRID_K_PER_WAVE": 2,
        },
        "active_pe": 8,
        "buses": {
            0: {"enabled": 4, "modes": {1}},   # first K-stage  (IB, OL)
            1: {"enabled": 4, "modes": {2}},   # last K-stage   (IL, OB)
            2: {"enabled": 0, "modes": set()},
        },
        "gemm_planes": True,
    },
    "test_gemm_s3_m32k64n48": {
        "pe_params": {
            "GRID_M": 3, "GRID_N": 6, "GRID_K": 2,
            "GRID_K_PER_WAVE": 2,
        },
        "buses": {
            0: {"modes": {1}},
            1: {"modes": {2}},
            2: {"enabled": 0, "modes": set()},
        },
        "gemm_planes": True,
    },
    "test_gemm_s4_m60k128n80": {
        "pe_params": {
            "GRID_M": 5, "GRID_N": 10, "GRID_K": 4,
            "GRID_M_PER_WAVE": 3, "GRID_N_PER_WAVE": 5, "GRID_K_PER_WAVE": 3,
        },
        "active_pe": 45,
        "buses": {
            0: {"enabled": 15, "modes": {1}},
            1: {"enabled": 15, "modes": {0}},
            2: {"enabled": 15, "modes": {2}},
        },
        "gemm_planes": True,
    },
}


def _load_ir(out_root: Path, name: str) -> Dict[str, Any]:
    p = out_root / name / "hardware_ir.json"
    if not p.exists():
        raise FileNotFoundError(p)
    return json.loads(p.read_text())


def _bus_summary(scan_chain: List[Dict[str, Any]]) -> Dict[int, Dict[str, Any]]:
    by_bus: Dict[int, Dict[str, Any]] = {}
    for i, e in enumerate(scan_chain):
        bus = i // 16
        d = by_bus.setdefault(bus, {"enabled": 0, "modes": set()})
        if e["enable"]:
            d["enabled"] += 1
            d["modes"].add(int(e["route_mode"]))
    return by_bus


def _check_subset(label: str, want: Dict[str, Any], got: Dict[str, Any],
                  fails: List[str]) -> None:
    for k, v in want.items():
        if got.get(k) != v:
            fails.append(f"  {label}.{k}: want={v!r} got={got.get(k)!r}")


def _check_buses(want: Dict[int, Dict[str, Any]],
                 got: Dict[int, Dict[str, Any]],
                 fails: List[str]) -> None:
    for bus_idx, want_d in want.items():
        got_d = got.get(bus_idx, {"enabled": 0, "modes": set()})
        if "enabled" in want_d and got_d["enabled"] != want_d["enabled"]:
            fails.append(
                f"  bus{bus_idx}.enabled: want={want_d['enabled']} got={got_d['enabled']}"
            )
        if "modes" in want_d and got_d["modes"] != want_d["modes"]:
            fails.append(
                f"  bus{bus_idx}.modes: want={want_d['modes']} got={got_d['modes']}"
            )


def _check_gemm_planes(layer: Dict[str, Any], fails: List[str]) -> None:
    tp = layer["tiling_params"]
    spm = layer["spm_layout"]
    agu_pli = layer["agu_pli"]
    agu_pd = layer["agu_pd"]
    agu_plo = layer["agu_plo"]
    use_k_chain = layer["pe_program"]["params"].get("GRID_K_PER_WAVE", 1) > 1
    # PS = B should sit AFTER A in DRAM; PD = A should be at tensor_base.
    if tp["dram_input_base"] >= tp["dram_weight_base"]:
        fails.append(
            "  gemm planes: dram_input_base (A) must be < dram_weight_base (B), got "
            f"A={hex(tp['dram_input_base'])} B={hex(tp['dram_weight_base'])}"
        )
    # B not dependent on M
    if tp["dram_ps_oc_stride"] != 0:
        fails.append(
            f"  gemm planes: dram_ps_oc_stride should be 0 (B not dep on M), "
            f"got {tp['dram_ps_oc_stride']}"
        )
    # A not dependent on N
    if tp["dram_pd_h_stride"] != 0:
        fails.append(
            f"  gemm planes: dram_pd_h_stride should be 0 (A not dep on N), "
            f"got {tp['dram_pd_h_stride']}"
        )
    want_pd_mode = "parallel" if use_k_chain else "linear"
    if spm["pd"]["spm_mode"] != want_pd_mode:
        fails.append(
            f"  gemm planes: PD spm_mode should be {want_pd_mode}, got {spm['pd']['spm_mode']}"
        )
    if bool(agu_pd["ctrl"] & 0x8) != use_k_chain:
        fails.append(
            f"  gemm planes: PD AGU ultra should be {use_k_chain}, got ctrl=0x{agu_pd['ctrl']:X}"
        )
    if spm["pli"]["spm_mode"] != "linear":
        fails.append(
            f"  gemm planes: PLI spm_mode should stay linear, got {spm['pli']['spm_mode']}"
        )
    if bool(agu_pli["ctrl"] & 0x8):
        fails.append(
            f"  gemm planes: PLI AGU must be non-ultra, got ctrl=0x{agu_pli['ctrl']:X}"
        )
    if spm["plo"]["spm_mode"] != "linear":
        fails.append(
            f"  gemm planes: PLO spm_mode should stay linear, got {spm['plo']['spm_mode']}"
        )
    if bool(agu_plo["ctrl"] & 0x8):
        fails.append(
            f"  gemm planes: PLO AGU must be non-ultra, got ctrl=0x{agu_plo['ctrl']:X}"
        )
    if use_k_chain and (tp.get("parallel_groups", 0) & 0x2) == 0:
        fails.append("  gemm planes: PD must be included in parallel_groups for K-chain GEMM")
    if use_k_chain and tp.get("dma_ps_words_per_bank", 0) == 0:
        fails.append("  gemm planes: PS must expose dma_ps_words_per_bank for K-chain GEMM")
    if use_k_chain and tp.get("dma_pd_words_per_bank", 0) == 0:
        fails.append("  gemm planes: PD must expose dma_pd_words_per_bank for K-chain GEMM")
    if tp.get("num_h_tiles", 1) > 1 and tp.get("dram_ps_h_stride", 0) == 0:
        fails.append("  gemm planes: PS must expose N-wave stride when num_h_tiles > 1")
    if tp.get("num_oc_tiles", 1) > 1 and tp.get("dram_pd_oc_stride", 0) == 0:
        fails.append("  gemm planes: PD must expose M-wave stride when num_oc_tiles > 1")
    if tp.get("num_oc_tiles", 1) > 1 and tp.get("dram_bias_oc_stride", 0) == 0:
        fails.append("  gemm planes: PLI must expose M-wave stride when num_oc_tiles > 1")
    if tp.get("num_h_tiles", 1) > 1 and tp.get("dram_bias_h_stride", 0) == 0:
        fails.append("  gemm planes: PLI must expose N-wave stride when num_h_tiles > 1")


def main(out_root: Path) -> int:
    total_fails = 0
    for name, contract in CONTRACTS.items():
        try:
            ir = _load_ir(out_root, name)
        except FileNotFoundError as exc:
            print(f"[MISS] {name}: {exc}")
            total_fails += 1
            continue

        layer = ir["layers"][0]
        fails: List[str] = []

        if "tiling" in contract:
            _check_subset("tiling", contract["tiling"],
                          layer["tiling_params"], fails)
        if "pe_params" in contract:
            _check_subset("pe_params", contract["pe_params"],
                          layer["pe_program"]["params"], fails)
        if "active_pe" in contract:
            enabled = sum(1 for e in layer["scan_chain"] if e["enable"])
            if enabled != contract["active_pe"]:
                fails.append(
                    f"  active_pe: want={contract['active_pe']} got={enabled}"
                )
        if "buses" in contract:
            _check_buses(contract["buses"],
                         _bus_summary(layer["scan_chain"]),
                         fails)
        if contract.get("gemm_planes"):
            _check_gemm_planes(layer, fails)

        if fails:
            print(f"[FAIL] {name}")
            for line in fails:
                print(line)
            total_fails += len(fails)
        else:
            print(f"[ OK ] {name}")

    if total_fails:
        print(f"\n{total_fails} contract violation(s).")
        return 1
    print("\nAll contracts satisfied.")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(Path(sys.argv[1]).resolve()))
