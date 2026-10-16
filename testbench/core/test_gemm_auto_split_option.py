"""GEMM K auto-split as an explicit cc option (ODR D-16).

``--gemm-auto-split off`` (default since 2026-10-17) keeps a GEMM with K>96 as one layer with
native multi-K waves; ``on`` rewrites it into bias-chained K chunks (replay of earlier artifacts). The data generator must follow what the
hardware IR actually contains instead of re-deciding the split.
"""
import dataclasses
import json
import sys

import pytest
import yaml

from hybridacc_cc import cli
from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload
from hybridacc_verify.gen import gen_test_dram as gen


def _workload(tmp_path, k=192, num_bus=3):
    hw = dict(num_clusters=1, num_pes=12, num_bus=num_bus, spm_banks_per_group=num_bus,
              spm_bank_depth=8192, dram_base=0x80000000)
    wl = dict(name='split', hardware=hw, tensors={
        'A': dict(shape=[12, k], dtype='fp16'),
        'B': dict(shape=[k, 8], dtype='fp16'),
        'C': dict(shape=[12, 8], dtype='fp16')},
        ops=[dict(name='g', type='gemm', inputs=['A', 'B'], outputs=['C'])])
    y = tmp_path / 'workload.yaml'
    y.write_text(yaml.safe_dump(wl, sort_keys=False))
    return y


def _ir(y, **kw):
    return dataclasses.asdict(lower_workload(parse_workload(y, **kw)))


def test_default_keeps_one_native_layer_and_on_splits(tmp_path):
    y = _workload(tmp_path)
    split = _ir(y, gemm_auto_split=True)
    native = _ir(y)
    assert [l['name'] for l in split['layers']] == ['g__kchunk0', 'g__kchunk1']
    assert [l['name'] for l in native['layers']] == ['g']
    assert native['layers'][0]['tiling_params']['num_ic_tiles'] > 1


def test_cli_option(tmp_path):
    y = _workload(tmp_path)
    for flag, layers in (('on', 2), ('off', 1)):
        out = tmp_path / flag
        assert cli.main([str(y), '-o', str(out), '--no-compile', '--dump-ir', '--gemm-auto-split', flag]) == 0
        assert len(json.loads((out / 'hardware_ir.json').read_text())['layers']) == layers


def _gen(tmp_path, monkeypatch, y, ir):
    j = tmp_path / 'hardware_ir.json'
    j.write_text(json.dumps(ir))
    monkeypatch.setattr(sys, 'argv', ['gen', '--ir', str(j), '--workload', str(y),
                                      '--output-dir', str(tmp_path), '--seed', '42', '--bias-mode', 'random'])
    gen.main()


def test_generator_follows_the_ir(tmp_path, monkeypatch):
    y = _workload(tmp_path)
    for name, kw in (('split', {'gemm_auto_split': True}), ('native', {})):
        d = tmp_path / name
        d.mkdir()
        _gen(d, monkeypatch, y, _ir(y, **kw))
        assert (d / 'golden_output.bin').stat().st_size == 12 * 8 * 2
        layer_bias = sorted(p.name for p in d.glob('layer*_bias.npy'))
        assert layer_bias == ['layer0_bias.npy']  # bias only on the first (or only) layer
    # The fp32 reference is schedule-independent: the same seed gives the same reference either way.
    assert (tmp_path / 'split/golden_reference_fp32.bin').read_bytes() == \
        (tmp_path / 'native/golden_reference_fp32.bin').read_bytes()


def test_generator_refuses_an_ir_it_cannot_match(tmp_path, monkeypatch):
    y = _workload(tmp_path)
    native = _ir(y)
    native['layers'][0]['name'] = 'other'
    with pytest.raises(ValueError, match='neither its K-chunk layers nor a native layer'):
        _gen(tmp_path, monkeypatch, y, native)
    split = _ir(y, gemm_auto_split=True)
    split['layers'] = split['layers'][:1]
    with pytest.raises(ValueError, match='K-chunk layers'):
        _gen(tmp_path, monkeypatch, y, split)
