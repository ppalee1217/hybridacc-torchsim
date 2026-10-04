"""Conv PE templates emit one VPSUM per output pack (tile_oc / 4) per window."""

import dataclasses
import re

import pytest
import yaml

from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import TilingFailed, lower_workload
from hybridacc_cc.pe_payload import (
    collect_payload_context,
    conv_window_output_packs,
    load_template_json,
)

BASES = ("conv1d_k1c12s1", "conv1d_k3c4s1")


def _template(base, oc):
    name = f"{base}_template" if oc == 16 else f"{base}_oc{oc}_template"
    return load_template_json(name)


def _ordinary_data_moves(jdata):
    """VTSTORE / TSHIFT effects of the ordinary-window epilogue, in order."""
    disasm = [i["disasm"] for i in jdata["instructions"]]
    window = next(i for i, d in enumerate(disasm) if d.startswith("LOOPIN 799"))
    moves = []
    for d in disasm[window + 1:]:
        if d.startswith("SYSCTRL") and moves:
            break
        moves += re.findall(r"VT\d|K\d", d.split(";")[0].replace("VMACRN 0, VT1", "")
                            .replace("VMACRN 1, VTRST", ""))
    return moves


@pytest.mark.parametrize("base", BASES)
@pytest.mark.parametrize("oc", [4, 8, 12, 16])
def test_template_issues_one_vpsum_per_pack(base, oc):
    words = [i["dec"] for i in _template(base, oc)["instructions"]]
    assert conv_window_output_packs(words) == (oc // 4, oc // 4)


@pytest.mark.parametrize("base", BASES)
@pytest.mark.parametrize("oc", [4, 8, 12])
def test_compact_template_keeps_the_input_moves(base, oc):
    # The fused VPSUMs also load the next input (VTSTORE) or shift the window
    # (TSHIFT); dropping a VPSUM must not drop those.
    assert _ordinary_data_moves(_template(base, oc)) == _ordinary_data_moves(_template(base, 16))


def _workload(tmp_path, kind, oc):
    kh, ic = (1, 12) if kind == "conv2d_1x1" else (3, 4)
    path = tmp_path / f"{kind}_{oc}.yaml"
    path.write_text(yaml.safe_dump({
        "name": "conv_templates",
        "hardware": {"num_clusters": 1, "num_pes": 48, "num_bus": 3,
                     "spm_banks_per_group": 3, "spm_bank_depth": 8192,
                     "dram_base": 0x80000000},
        "tensors": {
            "input": {"shape": [1, 6, 8, ic], "dtype": "fp16", "layout": "NHWC"},
            "weight": {"shape": [oc, kh, kh, ic], "dtype": "fp16", "layout": "OIHW"},
            "output": {"shape": [1, 7 - kh, 9 - kh, oc], "dtype": "fp16", "layout": "NHWC"},
        },
        "ops": [{"name": "conv1", "type": kind, "inputs": ["input", "weight"],
                 "outputs": ["output"], "attrs": {"stride": 1}}],
    }))
    return parse_workload(path)


@pytest.mark.parametrize("kind,base", [("conv2d_1x1", "conv1d_k1c12s1"),
                                       ("conv2d_3x3", "conv1d_k3c4s1")])
@pytest.mark.parametrize("oc", [4, 8, 12, 16])
def test_lowering_picks_the_template_for_tile_oc(tmp_path, kind, base, oc):
    ir = lower_workload(_workload(tmp_path, kind, oc))
    expected = f"{base}_template" if oc == 16 else f"{base}_oc{oc}_template"
    assert ir.layers[0].pe_program.template_name == expected
    collect_payload_context(ir.layers)


def test_stream_check_rejects_the_four_pack_template_for_oc12(tmp_path):
    # The 2026-10 D-31 failure: four VPSUMs per window against three PLI packs.
    ir = lower_workload(_workload(tmp_path, "conv2d_1x1", 12))
    layer = ir.layers[0]
    wrong = dataclasses.replace(
        layer, pe_program=dataclasses.replace(layer.pe_program,
                                              template_name="conv1d_k1c12s1_template"))
    with pytest.raises(ValueError, match="E_PE_STREAM"):
        collect_payload_context([wrong])


def test_output_tile_not_a_multiple_of_four_is_rejected(tmp_path):
    with pytest.raises(TilingFailed, match="tile_oc=6"):
        lower_workload(_workload(tmp_path, "conv2d_1x1", 6))
