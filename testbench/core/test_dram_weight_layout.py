"""Conv tail allocation/packing and DRAM collision regression tests."""
import dataclasses
import itertools
import json
import sys

import numpy as np
import pytest
import yaml

from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload
from hybridacc_verify.gen import gen_test_dram as gen


def conv_workload(kh, ic, oc):
    return {
        "name": "weight_tail", "hardware": {"num_clusters": 1},
        "tensors": {
            "input": {"shape": [1, 4, 8, ic], "dtype": "fp16", "layout": "NHWC"},
            "weight": {"shape": [oc, kh, kh, ic], "dtype": "fp16", "layout": "OIHW"},
            "output": {"shape": [1, 5-kh, 9-kh, oc], "dtype": "fp16", "layout": "NHWC"},
        },
        "ops": [{"name": "conv", "type": f"conv2d_{kh}x{kh}",
                 "inputs": ["input", "weight"], "outputs": ["output"],
                 "attrs": {"stride": 1}}],
    }


def lower(tmp_path, workload):
    path = tmp_path / "workload.yaml"
    path.write_text(yaml.safe_dump(workload, sort_keys=False))
    return dataclasses.asdict(lower_workload(parse_workload(path)))


@pytest.mark.parametrize("kh,ic,oc", [(1, 8, 125), (1, 15, 91), (3, 73, 87),
                                       (1, 12, 8), (1, 12, 32), (3, 4, 16)])
def test_conv_allocates_full_ps_tiles(tmp_path, kh, ic, oc):
    wl = conv_workload(kh, ic, oc)
    ir = lower(tmp_path, wl)
    layer = ir["layers"][0]
    tp = layer["tiling_params"]
    padded_oc = ((oc + 15) // 16) * min(oc, 16)
    tile_ic = 12 if kh == 1 else 4
    padded_ic = ((ic + tile_ic - 1) // tile_ic) * tile_ic
    expected_weight_bytes = padded_oc * kh * kh * padded_ic * 2
    assert tp["dram_input_base"] - tp["dram_weight_base"] == expected_weight_bytes
    assert tp["dram_output_base"] - tp["dram_input_base"] == 4 * 8 * padded_ic * 2
    assert tp["dram_bias_base"] == tp["dram_output_base"] + gen._output_region_size(tp)
    gen._validate_dram_regions(gen._dram_layout_regions([(wl["ops"][0], layer)], wl["tensors"]), ir["hardware"]["dram_base"])


@pytest.mark.parametrize("kh,ic,oc,tile_ic,tile_oc", [(1, 8, 125, 12, 16),
    (1, 15, 91, 12, 16), (3, 73, 87, 4, 16), (1, 5, 7, 8, 3)])
def test_weight_bytes_follow_tile_indices_and_zero_tails(kh, ic, oc, tile_ic, tile_oc):
    weight = (np.arange(oc * kh * kh * ic) % 113 + 1).astype(np.float16).reshape(oc, kh, kh, ic)
    ni, no = (ic + tile_ic - 1) // tile_ic, (oc + tile_oc - 1) // tile_oc
    source = weight[:, 0, 0, :] if kh == 1 else weight
    packed = np.frombuffer(gen._tile_weight_ic(source, ni, tile_ic, no, tile_oc), dtype=np.float16)
    # Independent scalar indexing of the physical layout (no inverse transpose).
    expected = []
    for ot, it, o, h, w, c in itertools.product(range(no), range(ni), range(tile_oc), range(kh), range(kh), range(tile_ic)):
        out_c, in_c = ot * tile_oc + o, it * tile_ic + c
        expected.append(weight[out_c, h, w, in_c] if out_c < oc and in_c < ic else 0)
    np.testing.assert_array_equal(packed.view(np.uint16), np.array(expected, dtype=np.float16).view(np.uint16))


@pytest.mark.parametrize("ni,ti,no,to", [(1, 4, 1, 16), (2, 4, 1, 8), (2, 4, 0, 16)])
def test_invalid_weight_extent_is_rejected(ni, ti, no, to):
    with pytest.raises(ValueError):
        gen._tile_weight_ic(np.ones((17, 5), dtype=np.float16), ni, ti, no, to)


@pytest.mark.parametrize("left,right", list(itertools.combinations(range(4), 2)))
def test_all_region_pairs_reject_overlap(left, right):
    roles = ["weight", "input", "output", "PLI"]
    regions = [(f"layer0.{r}", 0x1000+i*16, 0x1010+i*16, r) for i, r in enumerate(roles)]
    name, _, _, tensor = regions[right]
    regions[right] = (name, regions[left][1]+8, regions[left][2]+8, tensor)
    with pytest.raises(ValueError, match="Invalid DRAM layout") as exc:
        gen._validate_dram_regions(regions, 0x1000)
    for r in roles:
        assert f"layer0.{r}=[0x" in str(exc.value)


def test_adjacent_regions_and_named_producer_alias():
    regions = [("layer0.output", 4096, 4112, "mid"),
               ("layer1.PLI", 4096, 4112, "mid"),
               ("layer1.output", 4112, 4128, "final")]
    gen._validate_dram_regions(regions, 4096)
    regions[1] = ("layer1.PLI", 4096, 4113, "mid")
    with pytest.raises(ValueError, match="overlap"):
        gen._validate_dram_regions(regions, 4096)


def test_old_tail_ir_fails_before_writing_dram(tmp_path, monkeypatch):
    wl = conv_workload(1, 15, 91)
    ir = lower(tmp_path, wl)
    tp = ir["layers"][0]["tiling_params"]
    tp["dram_input_base"] = tp["dram_weight_base"] + 91 * 24 * 2
    ir_path = tmp_path / "hardware_ir.json"
    ir_path.write_text(json.dumps(ir))
    out = tmp_path / "generated"
    monkeypatch.setattr(sys, "argv", ["gen", "--ir", str(ir_path), "--workload", str(tmp_path / "workload.yaml"), "--output-dir", str(out)])
    with pytest.raises(ValueError, match="overlap: layer0.weight / layer0.input"):
        gen.main()
    assert not out.exists()


@pytest.mark.parametrize("m,k,n", [(12, 32, 24), (13, 35, 19)])
def test_gemm_allocates_padded_dma_footprint(tmp_path, m, k, n):
    wl = {"name": "gemm_tail", "hardware": {"num_clusters": 1}, "tensors": {
        name: {"shape": shape, "dtype": "fp16"} for name, shape in
        (("A", [m, k]), ("B", [k, n]), ("C", [m, n]))},
        "ops": [{"name": "gemm", "type": "gemm", "inputs": ["A", "B"], "outputs": ["C"]}]}
    ir = lower(tmp_path, wl)
    layer = ir["layers"][0]
    tp = layer["tiling_params"]
    assert tp["dram_weight_base"] - tp["dram_input_base"] == len(gen._pack_gemm_a_pd(np.ones((m, k)), layer))
    assert tp["dram_output_base"] - tp["dram_weight_base"] == len(gen._pack_gemm_b_ps(np.ones((k, n)), layer))
    gen._validate_dram_regions(gen._dram_layout_regions([(wl["ops"][0], layer)], wl["tensors"]), ir["hardware"]["dram_base"])


@pytest.mark.parametrize("kind", ["conv_batch", "gemm_split_k"])
def test_region_guard_preserves_shared_batch_and_split_k(tmp_path, monkeypatch, kind):
    if kind == "conv_batch":
        wl = conv_workload(1, 12, 16)
        wl["tensors"]["input"]["shape"][0] = 2
        wl["tensors"]["output"]["shape"][0] = 2
    else:
        wl = {"name": "split_k", "hardware": {"num_clusters": 1}, "tensors": {
            name: {"shape": shape, "dtype": "fp16"} for name, shape in
            (("A", [12, 128]), ("B", [128, 24]), ("C", [12, 24]))},
            "ops": [{"name": "gemm", "type": "gemm", "inputs": ["A", "B"], "outputs": ["C"]}]}
    ir = lower(tmp_path, wl)
    assert len(ir["layers"]) > 1
    ir_path = tmp_path / "hardware_ir.json"
    ir_path.write_text(json.dumps(ir))
    out = tmp_path / "generated"
    monkeypatch.setattr(sys, "argv", ["gen", "--ir", str(ir_path), "--workload", str(tmp_path / "workload.yaml"), "--output-dir", str(out)])
    gen.main()
    assert (out / "dram_init.bin").stat().st_size > 0
    assert (out / "golden_output.bin").stat().st_size > 0
