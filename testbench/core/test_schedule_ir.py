"""Common Schedule IR v1 (``hybridacc_cc.schedule_ir``): extraction from a host run of the firmware.

Positive cases render real workloads (GEMM generic multi-K on 2 buses, GEMM resident on
3 buses, conv1x1, conv3x3) and check the IR's structure. Negative cases feed the decoder
recorder lines that it must refuse, one per fail-loud rule.
"""

import copy
import dataclasses
import json
import shutil
from pathlib import Path

import pytest
import yaml

from hybridacc_cc import schedule_ir as si
from hybridacc_cc.codegen import generate_firmware
from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload

KERNEL_DIR = Path(__file__).resolve().parents[2] / "design/hybridacc-cc/kernel/json"
PLATFORM = {"clock_mhz": 500, "dma_beat_bytes": 8, "dma_cmd_fifo_depth": 8}
REF = {"path": "test", "sha256": "0" * 64}


def _require_gcc():
    if shutil.which("gcc") is None:
        pytest.skip("host gcc is required")


def _hardware(num_pes, num_bus):
    return {"num_clusters": 1, "num_pes": num_pes, "num_bus": num_bus, "spm_banks_per_group": num_bus,
            "spm_bank_depth": 8192, "dram_base": 0x80000000}


def _gemm(m, k, n):
    return ({"A": {"shape": [m, k], "dtype": "fp16"}, "B": {"shape": [k, n], "dtype": "fp16"},
             "C": {"shape": [m, n], "dtype": "fp16"}},
            [{"name": "gemm1", "type": "gemm", "inputs": ["A", "B"], "outputs": ["C"]}])


def _conv(h, w, ci, co, k):
    attrs = {"stride": 1} if k == 1 else {"stride": 1, "padding": 0}
    return ({"input": {"shape": [1, h, w, ci], "dtype": "fp16", "layout": "NHWC"},
             "weight": {"shape": [co, k, k, ci], "dtype": "fp16", "layout": "OIHW"},
             "output": {"shape": [1, h - k + 1, w - k + 1, co], "dtype": "fp16", "layout": "NHWC"}},
            [{"name": "conv1", "type": f"conv2d_{k}x{k}", "inputs": ["input", "weight"], "outputs": ["output"],
              "attrs": attrs}])


def _render(tmp_path, name, hardware, tensors_ops):
    tensors, ops = tensors_ops
    path = tmp_path / f"{name}.yaml"
    path.write_text(yaml.safe_dump({"name": name, "hardware": hardware, "tensors": tensors, "ops": ops}))
    hir = lower_workload(parse_workload(path))
    out = tmp_path / name
    generate_firmware(hir, out, kernel_json_dir=KERNEL_DIR)
    (out / "hardware_ir.json").write_text(json.dumps(dataclasses.asdict(hir)))
    return out


CASES = {
    "gemm_generic_multik_2bus": (_hardware(12, 2), _gemm(100, 74, 79)),
    "gemm_resident_3bus": (_hardware(12, 3), _gemm(100, 74, 79)),
    "conv1x1": (_hardware(12, 3), _conv(10, 64, 52, 36, 1)),
    "conv3x3": (_hardware(12, 3), _conv(10, 60, 104, 120, 3)),
}


@pytest.mark.parametrize("name", sorted(CASES))
def test_extracts_a_closed_schedule(tmp_path, name):
    _require_gcc()
    d = _render(tmp_path, name, *CASES[name])
    ir = si.build(d, PLATFORM, REF)
    assert ir["schema"] == si.SCHEMA
    hw = json.loads((d / "hardware_ir.json").read_text())
    for layer, hw_layer in zip(ir["layers"], hw["layers"]):
        t = hw_layer["tiling_params"]
        tiles = t["num_oc_tiles"] * t["num_h_tiles"] * t["num_w_tiles"] * t["num_ic_tiles"]
        assert sum(len(w["covers"]) for w in layer["execution_waves"]) == tiles
        events = layer["events"]
        for wait in layer["waits"]:
            if wait["kind"] == "hddu_done":
                start = layer["execution_waves"][wait["target_wave"]]["event"]
                assert start < wait["id"]
            if wait["kind"] == "dma_tag":
                assert layer["dma"][wait["target_dma"]]["event"] < wait["id"]
        assert [e["id"] for e in events] == list(range(len(events)))
        assert all(c["bytes"] == c["beats"] * PLATFORM["dma_beat_bytes"] for c in layer["dma"])
        assert not layer["dma_fifo_hazard"]
    assert ir["provenance"]["files_sha256"]["hardware_ir.json"] == si.sha256_file(d / "hardware_ir.json")
    t0 = hw["layers"][0]["tiling_params"]
    waves0 = len(ir["layers"][0]["execution_waves"])
    if name == "gemm_generic_multik_2bus":
        assert t0["num_ic_tiles"] > 1 and t0["gemm_resident_m_tiles"] == 0
    if name == "gemm_resident_3bus":
        assert t0["gemm_resident_m_tiles"] > 0 and waves0 < t0["num_oc_tiles"] * t0["num_h_tiles"]


@pytest.fixture(scope="module")
def regmap(tmp_path_factory):
    d = _render(tmp_path_factory.mktemp("rm"), "gemm", _hardware(12, 3), _gemm(48, 64, 24))
    return si.RegisterMap((d / "firmware_hw.h").read_text())


def _lines(regmap, *body):
    return ["WAITKINDS 1 2 3 4 5 6 7 8", "LAYER 0", *body]


def _w(regmap, name, value):
    return f"W {regmap.d[name]} {value}"


def _start(regmap):
    d = regmap.d
    return [f"W {d['CLUSTER_BCAST_BASE'] + d['HDDU_BASE'] + d['HDDU_PLANE_EN']} 15",
            f"W {d['CLUSTER_BCAST_BASE'] + d['HDDU_BASE'] + d['HDDU_PLANE_MODE']} 2",
            f"W {d['CLUSTER_BCAST_BASE'] + d['SPM_CONFIG_MAP']} 228",
            "WAVE 0 1 0 0 0 1",
            f"W {d['CLUSTER_BCAST_BASE'] + d['HDDU_BASE'] + d['HDDU_CTRL']} {d['HDDU_CTRL_START']}"]


def test_decoder_accepts_a_minimal_record(regmap):
    d = regmap.d
    lines = _lines(regmap, _w(regmap, "DMA_CMD_TAG", 1), _w(regmap, "DMA_COUNT_D0", 4),
                   f"W {d['DMA_CTRL']} {d['DMA_CTRL_SUBMIT']}", "WAIT 2 1", *_start(regmap), "WAIT 3 1")
    out = si.decode(lines, regmap, PLATFORM)
    layer = out["layers"][0]
    assert [e["type"] for e in layer["events"]] == ["dma_submit", "wait", "hddu_start", "wait"]
    assert layer["dma"][0]["bytes"] == 32
    assert layer["waits"][0]["target_dma"] == 0 and layer["waits"][1]["target_wave"] == 0


@pytest.mark.parametrize("case,message", [
    ("read", "outside a wait helper"),
    ("unknown_address", "unknown register address"),
    ("marker_not_before_start", "not immediately before an HDDU start"),
    ("start_without_marker", "without a wave marker"),
    ("tag_never_issued", "never issued"),
    ("read_only_dma", "read-only DMA register"),
    ("unknown_wait", "unknown wait kind"),
    ("tags_backwards", "tags go backwards"),
    ("hddu_wait_before_start", "never started"),
    ("unknown_ctrl_bits", "unknown DMA_CTRL bits"),
])
def test_decoder_refuses(regmap, case, message):
    d = regmap.d
    submit = f"W {d['DMA_CTRL']} {d['DMA_CTRL_SUBMIT']}"
    body = {
        "read": [f"R {d['DMA_STATUS']}"],
        "unknown_address": ["W 4 0"],
        "marker_not_before_start": ["WAVE 0 1 0 0 0 1", _w(regmap, "DMA_CMD_TAG", 1)],
        "start_without_marker": [x for x in _start(regmap) if not x.startswith("WAVE")],
        "tag_never_issued": ["WAIT 2 5"],
        "read_only_dma": [_w(regmap, "DMA_DONE_TAG", 1)],
        "unknown_wait": ["WAIT 99 0"],
        "tags_backwards": [_w(regmap, "DMA_CMD_TAG", 5), submit, _w(regmap, "DMA_CMD_TAG", 4), submit],
        "hddu_wait_before_start": ["WAIT 3 1"],
        "unknown_ctrl_bits": [f"W {d['DMA_CTRL']} 2"],
    }[case]
    with pytest.raises(si.ScheduleIRError, match=message):
        si.decode(_lines(regmap, *body), regmap, PLATFORM)


def test_build_refuses_missing_platform_field_and_wrong_grid(tmp_path):
    _require_gcc()
    d = _render(tmp_path, "g", _hardware(12, 3), _gemm(48, 64, 24))
    with pytest.raises(si.ScheduleIRError, match="platform constants"):
        si.build(d, {k: v for k, v in PLATFORM.items() if k != "dma_cmd_fifo_depth"}, REF)
    hw = json.loads((d / "hardware_ir.json").read_text())
    bad = copy.deepcopy(hw)
    bad["layers"][0]["tiling_params"]["num_oc_tiles"] += 1
    (d / "hardware_ir.json").write_text(json.dumps(bad))
    with pytest.raises(si.ScheduleIRError, match="exactly once"):
        si.build(d, PLATFORM, REF)
