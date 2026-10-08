"""Seeded PLI data must follow cc packing and PE reduction semantics (R-13)."""
import dataclasses
import json
import struct
import sys

import numpy as np
import pytest
import yaml

from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload
from hybridacc_verify.gen import gen_test_dram as gen
from hybridacc_verify.check import compare_golden


def workload(op):
    hw = dict(num_clusters=1, num_pes=12, num_bus=3,
              spm_banks_per_group=3, spm_bank_depth=8192, dram_base=0x80000000)
    if op in ('gemm', 'split'):
        k = 12 if op == 'gemm' else 192
        return dict(name='bias', hardware=hw, tensors={
            'A': dict(shape=[12, k], dtype='fp16'),
            'B': dict(shape=[k, 8], dtype='fp16'),
            'C': dict(shape=[12, 8], dtype='fp16')},
            ops=[dict(name='g', type='gemm', inputs=['A', 'B'], outputs=['C'])])
    kh = int(op[-1])
    return dict(name='bias', hardware=hw, tensors={
        'X': dict(shape=[1, 4, 8, 12 if kh == 1 else 4], dtype='fp16', layout='NHWC'),
        'W': dict(shape=[16, kh, kh, 12 if kh == 1 else 4], dtype='fp16', layout='OIHW'),
        'Y': dict(shape=[1, 5-kh, 9-kh, 16], dtype='fp16', layout='NHWC')},
        ops=[dict(name='c', type=f'conv2d_{kh}x{kh}', inputs=['X', 'W'], outputs=['Y'])])


def run_gen(tmp_path, monkeypatch, op, mode=None, seed=42):
    tmp_path.mkdir(exist_ok=True)
    wl = workload(op)
    y = tmp_path/'workload.yaml'; y.write_text(yaml.safe_dump(wl, sort_keys=False))
    ir = dataclasses.asdict(lower_workload(parse_workload(y)))
    j = tmp_path/'hardware_ir.json'; j.write_text(json.dumps(ir))
    argv = ['gen', '--ir', str(j), '--workload', str(y), '--output-dir', str(tmp_path), '--seed', str(seed)]
    if mode is not None: argv += ['--bias-mode', mode]
    monkeypatch.setattr(sys, 'argv', argv); gen.main()
    return wl, ir


@pytest.mark.parametrize('op', ['gemm', 'split', 'conv1', 'conv3'])
def test_seeded_cli_bias_is_nonzero_and_default_unchanged(tmp_path, monkeypatch, op):
    a, b, c, z, d = [tmp_path/x for x in ('random', 'repeat', 'other', 'zero', 'default')]
    wl, ir = run_gen(a, monkeypatch, op, 'random')
    run_gen(b, monkeypatch, op, 'random')
    run_gen(c, monkeypatch, op, 'random', seed=43)
    run_gen(z, monkeypatch, op, 'zero'); run_gen(d, monkeypatch, op)
    for name in ('dram_init.bin', 'golden_output.bin', 'golden_meta.txt'):
        assert (a/name).read_bytes() == (b/name).read_bytes()
        assert (z/name).read_bytes() == (d/name).read_bytes()
    bias = np.load(a/'layer0_bias.npy')
    assert np.all(bias != 0)
    assert not np.array_equal(bias, np.load(c/'layer0_bias.npy'))
    raw, zero = (a/'dram_init.bin').read_bytes(), (z/'dram_init.bin').read_bytes()
    t = ir['layers'][0]['tiling_params']; off = t['dram_bias_base']-0x80000000
    size = gen._bias_region_size(t)
    assert raw[:off] == zero[:off]  # Source operands and reserved output unchanged.
    assert any(raw[off:off+size]) and not any(zero[off:off+size])
    if op == 'split':
        assert not (a/'layer1_bias.npy').exists()  # No double bias or overwrite of producer output.
        assert ir['layers'][1]['tiling_params']['dram_bias_base'] == t['dram_output_base']
    if op.startswith('conv'):
        tile = np.frombuffer(raw[off:off+size], dtype=np.float16).reshape(-1, 16)
        np.testing.assert_array_equal(tile, np.broadcast_to(bias[0, 0, 0], tile.shape))
    else:
        # One PE tile: physical C order is [N, M_word, lane], i.e. transpose.
        actual = np.frombuffer(raw[off:off+size], dtype=np.float16).reshape(8, 12).T
        np.testing.assert_array_equal(actual, bias)
        rng = np.random.RandomState(42)
        aa = rng.uniform(-1, 1, wl['tensors']['A']['shape']).astype(np.float16)
        bb = rng.uniform(-1, 1, wl['tensors']['B']['shape']).astype(np.float16)
        ref = np.fromfile(a/'golden_reference_fp32.bin', dtype=np.float32).reshape(12, 8)
        np.testing.assert_array_equal(ref, aa.astype(np.float32) @ bb.astype(np.float32) + bias.astype(np.float32))
    assert (a/'golden_output.bin').read_bytes() != (z/'golden_output.bin').read_bytes()


def test_gemm_bias_is_added_after_stage_not_before_macs():
    a = np.ones((1, 2), dtype=np.float16)
    b = np.array([[1], [-1]], dtype=np.float16)
    bias = np.array([[2048]], dtype=np.float16)
    got = gen._gemm_pli_golden(a, b, bias, 2)
    assert got.item() == 2048  # Bias-before-MAC rounds to 2047.
    assert np.float16(np.float16(2048) + np.float16(1)) - np.float16(1) == 2047


def test_gemm_stage_rounding_and_bias_from():
    a = np.ones((1, 4), dtype=np.float16)
    b = np.array([[2048], [1], [-2048], [1]], dtype=np.float16)
    bias = np.array([[1]], dtype=np.float16)
    assert gen._gemm_pli_golden(a, b, bias, 2).item() == 1
    assert np.float16(gen.fp16_gemm_golden(a, b).item() + 1) == 2


def test_conv_vmac_tree_then_pli():
    x = np.ones((1, 1, 1, 4), dtype=np.float16)
    w = np.array([2048, 1, -2048, 1], dtype=np.float16).reshape(1, 1, 1, 4)
    bias = np.ones((1, 1, 1, 1), dtype=np.float16)
    # ((2048+1)+(-2048+1)) + PLI = 1 + 1 = 2.
    assert gen._conv_pli_golden(x, w, bias, 4).item() == 2


def test_compare_consumes_bias_in_golden(tmp_path, monkeypatch):
    _, ir = run_gen(tmp_path, monkeypatch, 'gemm', 'random')
    data = (tmp_path/'golden_output.bin').read_bytes()
    base = ir['layers'][0]['tiling_params']['dram_output_base']
    def compare(blob):
        (tmp_path/'dram_init.bin.out').write_bytes(struct.pack('<IIII', 0x53505253, 1, base, len(blob)) + blob + bytes(8))
        with pytest.raises(SystemExit) as exc:
            compare_golden.main([str(tmp_path), '--tolerance', '0.9999'])
        return exc.value.code
    assert compare(data) == 0
    assert compare(bytes(len(data))) == 1


def test_parallel_conv_bias_covers_all_spatial_rows(tmp_path):
    # Already exposed A1 / D-40 shape: rows 24..30 reside on the second bank.
    wl = workload("conv1")
    wl["hardware"].update(num_pes=48, num_bus=2, spm_banks_per_group=2)
    wl["tensors"]["X"]["shape"] = [1, 31, 63, 100]
    wl["tensors"]["W"]["shape"] = [8, 1, 1, 100]
    wl["tensors"]["Y"]["shape"] = [1, 31, 63, 8]
    path = tmp_path / "workload.yaml"
    path.write_text(yaml.safe_dump(wl, sort_keys=False))
    layer = dataclasses.asdict(lower_workload(parse_workload(path)))["layers"][0]
    tp = layer["tiling_params"]
    assert tp["dma_pli_rows_per_bank"] == 24
    bias = np.broadcast_to(np.arange(1, 9, dtype=np.float16), (1, 31, 63, 8))
    packed = gen._pack_test_bias(bias, layer)
    assert len(packed) == 48 * 56 * 8 * 2
    physical = np.frombuffer(packed, dtype=np.float16).reshape(48, 56, 8)
    np.testing.assert_array_equal(physical, np.broadcast_to(bias[0, 0, 0], physical.shape))


def test_nonzero_bias_rejects_overlapping_oc_tiles():
    # A small synthetic IR isolates the allocation contract, not a new workload.
    layer = {"op_type": "conv2d_1x1", "pe_program": {"params": {"KERNEL_COUNT": 4}},
             "tiling_params": {"num_oc_tiles": 2, "tile_h_out": 2, "tile_w_out": 1,
                               "dram_bias_h_stride": 0, "dram_bias_oc_stride": 8,
                               "dma_pli_words": 1}}
    with pytest.raises(ValueError, match="overlaps full spatial PLI"):
        gen._pack_test_bias(np.ones((1, 2, 1, 8), dtype=np.float16), layer)
