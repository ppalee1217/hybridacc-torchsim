"""M21 WP-I: cc's wave map (operator coordinates of every tile) and its check against the schedule IR."""
import copy
import dataclasses
import json

import pytest
import yaml

from hybridacc_cc import cli
from hybridacc_cc import schedule_ir as si
from hybridacc_cc import wave_map as wm
from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload

from test_schedule_ir import CASES, PLATFORM, REF, KERNEL_DIR, _render as _render_asdict, _require_gcc


def _render(tmp_path, name, hardware, tensors_ops):
    """test_schedule_ir's fixture, with hardware_ir.json written the way the cc CLI writes it."""
    d = _render_asdict(tmp_path, name, hardware, tensors_ops)
    tensors, ops = tensors_ops
    path = tmp_path / f"{name}.yaml"
    wir = parse_workload(path)
    hir = lower_workload(wir)
    text = cli._ir_json(hir)
    (d / "hardware_ir.json").write_text(text)
    _render.workload = dataclasses.asdict(wir)
    return d, hir, text


def test_hardware_ir_text_leaves_the_wave_geometry_out(tmp_path):
    d, hir, text = _render(tmp_path, "gemm", *CASES["gemm_generic_multik_2bus"])
    full = dataclasses.asdict(hir)
    assert all(layer["wave_geometry"] is not None for layer in full["layers"])
    for layer in full["layers"]:
        del layer["wave_geometry"]
    assert text == json.dumps(full, indent=2, default=str)
    assert "wave_geometry" not in text


@pytest.mark.parametrize("name", sorted(CASES))
def test_the_wave_map_checks_against_the_schedule_ir(tmp_path, name):
    _require_gcc()
    d, hir, text = _render(tmp_path, name, *CASES[name])
    ir = si.build(d, PLATFORM, REF)
    m = wm.build(hir, text)
    report = wm.check(m, text.encode(), ir, _render.workload)
    assert report["ok"] and len(report["layers"]) == len(ir["layers"])
    s = report["layers"][0]["strata"]
    if name == "gemm_generic_multik_2bus":  # K = 74 over 64-element K waves; M 100 and N 79 leave tails
        assert s["multi_round_reduction"] and s["reduction_tail"] and s["output_tail"] and not s["resident"]
    if name == "gemm_resident_3bus":
        assert s["resident"]
    if name == "conv3x3_padded":
        assert s["input_padding"]


def _built(tmp_path):
    _require_gcc()
    d, hir, text = _render(tmp_path, "conv3x3", *CASES["conv3x3"])
    return wm.build(hir, text), text.encode(), si.build(d, PLATFORM, REF), _render.workload


def _axis(m, grid):
    return next(a for a in m["layers"][0]["axes"] if a["grid"] == grid)


@pytest.mark.parametrize("mutate, message", [
    (lambda m, ir: _axis(m, "oc")["segments"].pop(), "segments, hardware_ir has"),
    (lambda m, ir: _axis(m, "oc")["segments"][1].__setitem__(0, 15), "starts at 15"),
    (lambda m, ir: _axis(m, "ic")["segments"][-1].__setitem__(1, 103), "tiles end at 103"),
    (lambda m, ir: _axis(m, "h")["segments"][0].__setitem__(2, 1), "allocates 1"),
    (lambda m, ir: m.update(hardware_ir_sha256="0" * 64), "another hardware IR"),
    (lambda m, ir: ir["layers"][0]["execution_waves"][0]["covers"].append([0, 0, 0, 0]), "do not cover"),
    (lambda m, ir: ir["layers"][0]["execution_waves"][0]["covers"].__setitem__(0, [99, 0, 0, 0]), "outside the tile grid"),
    (lambda m, ir: m["layers"][0].update(kernel=[3, 1]), "kernel"),
    # A consistent partition of the wrong extent: only the frontend's shapes catch it.
    (lambda m, ir: (_axis(m, "ic").update(size=102), _axis(m, "ic")["segments"][-1].__setitem__(1, 102)), "axis sizes"),
    (lambda m, ir: m["layers"][0].update(name="other"), "no workload-IR op"),
])
def test_the_check_refuses(tmp_path, mutate, message):
    m, hw, ir, wl = _built(tmp_path)
    m, ir = copy.deepcopy(m), copy.deepcopy(ir)
    mutate(m, ir)
    with pytest.raises(wm.WaveMapError, match=message):
        wm.check(m, hw, ir, wl)


def test_the_cli_writes_a_wave_map_bound_to_hardware_ir(tmp_path):
    tensors, ops = CASES["conv1x1"][1]
    path = tmp_path / "w.yaml"
    path.write_text(yaml.safe_dump({"name": "w", "hardware": CASES["conv1x1"][0], "tensors": tensors, "ops": ops}))
    out = tmp_path / "out"
    assert cli.main([str(path), "-o", str(out), "--dump-ir", "--no-compile", "--kernel-dir", str(KERNEL_DIR)]) in (0, None)
    m = json.loads((out / "wave_map.json").read_text())
    assert m["schema"] == wm.SCHEMA
    assert m["hardware_ir_sha256"] == si.sha256_file(out / "hardware_ir.json")
    assert "wave_geometry" not in (out / "hardware_ir.json").read_text()
