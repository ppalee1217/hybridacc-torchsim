"""Single-K generic PLI DMA must fill the half selected by the AGU.

The shared runtime selects PLI groups by IC parity for conv's reduction ring.
Single-K generic GEMM instead alternates halves of group 2 by global wave.
Record actual firmware DMA submissions, including mixed dispatch workloads;
the instant-completion host stub tests addresses, not physical timing.
"""
from dataclasses import replace

import pytest
import yaml

from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload
from test_gemm_multik_ring_bias import _gemm_yaml, _host_firmware_events, _require_gcc


def _check_singlek_pli(ir, events):
    t = ir.layers[0].tiling_params
    assert t.num_ic_tiles == 1 and t.gemm_resident_m_tiles == 0
    loads = [e for e in events if e[0] == 'dma' and e[1:3] == (0, 1)
             and t.dram_bias_base <= e[3] < t.dram_bias_base + t.num_h_tiles * t.dram_bias_h_stride]
    assert len(loads) == t.num_h_tiles > 1
    for wave, e in enumerate(loads):
        agu = t.agu_pong[2] if wave % 2 else t.agu_ping[2]
        assert e[4] == t.spm_ping[2] + agu * 8, (wave, e[4], agu)
        if wave:
            assert abs(e[4] - loads[wave - 1][4]) >= t.dma_pli_words * 8


@pytest.mark.parametrize('pe,bus', [(p, b) for p in (12, 24, 48, 96) for b in (2, 3)])
def test_singlek_bias_dma_matches_agu(tmp_path, pe, bus):
    _require_gcc()
    ir = lower_workload(parse_workload(_gemm_yaml(tmp_path, 16, 60, 1024, pe, bus)))
    _check_singlek_pli(ir, _host_firmware_events(tmp_path, ir))


def test_mixed_generic_resident_chunked_dispatch(tmp_path):
    """Enabling the generic template block must not change specialized layers.

    Compile one real three-op workload. Specialized layers return before the
    generic runtime builder. Their DMA/start traces must equal those rendered
    alone, where the generic template block is absent.
    """
    _require_gcc()
    workload = None
    for i, (m, k, n) in enumerate(((16, 60, 1024), (16, 60, 256), (247, 81, 225))):
        one = yaml.safe_load(_gemm_yaml(tmp_path, m, k, n, 96, 3).read_text())
        if workload is None:
            workload = dict(name='mixed_singlek_dispatch', hardware=one['hardware'], tensors={}, ops=[])
        workload['tensors'].update({f'{name}{i}': t for name, t in one['tensors'].items()})
        workload['ops'].append(dict(name=f'gemm{i}', type='gemm', inputs=[f'A{i}', f'B{i}'], outputs=[f'C{i}']))
    path = tmp_path / 'mixed.yaml'
    path.write_text(yaml.safe_dump(workload))
    ir = lower_workload(parse_workload(path))
    generic, resident, chunked = [l.tiling_params for l in ir.layers]
    assert all(t.num_ic_tiles == 1 for t in (generic, resident, chunked))
    assert generic.gemm_resident_m_tiles == 0 and generic.num_h_tiles > 1
    assert resident.gemm_resident_m_tiles == resident.num_oc_tiles
    assert resident.gemm_resident_n_tiles == resident.num_h_tiles
    assert chunked.num_h_tiles == 1 and chunked.gemm_resident_n_tiles == 0
    assert 1 < chunked.gemm_resident_m_tiles < chunked.num_oc_tiles
    standalone_events = []
    for i, layer in enumerate(ir.layers):
        single = replace(ir, layers=[layer])
        events = _host_firmware_events(tmp_path / f'layer{i}', single)
        if i == 0:
            _check_singlek_pli(single, events)
        standalone_events.extend(events)
    assert _host_firmware_events(tmp_path / 'mixed', ir) == standalone_events
