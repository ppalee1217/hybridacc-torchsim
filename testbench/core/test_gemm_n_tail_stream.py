"""GEMM PS padding must be consumed before LDMA advances to the next K row.

D-39: logical N<8 used a short VMUL loop with an eight-column PS image.
These tests check the emitted PE loop against the independent AGU/LDMA
and PLI/PLO stream contracts, including auto-split and unsplit workloads.
"""
import pytest
import yaml

from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload
from hybridacc_cc.pe_payload import generate_patch_entries, load_template_json


@pytest.mark.parametrize('m,k,n,pes,bus', [
    (62, 230, 6, 24, 3), (131, 245, 7, 48, 3), (246, 207, 4, 96, 3),
    (12, 32, 4, 12, 2), (12, 64, 6, 12, 2), (12, 96, 7, 12, 3),
    (12, 230, 8, 24, 3), (12, 64, 9, 12, 2),
])
def test_gemm_pe_loop_consumes_complete_ps_rows(tmp_path, m, k, n, pes, bus):
    path = tmp_path / 'case.yaml'
    path.write_text(yaml.safe_dump({
        'name': 'n_tail',
        'hardware': {'num_clusters': 1, 'num_pes': pes, 'num_bus': bus,
                     'spm_banks_per_group': bus, 'spm_bank_depth': 8192},
        'tensors': {'A': {'shape': [m,k], 'dtype': 'fp16'},
                    'B': {'shape': [k,n], 'dtype': 'fp16'},
                    'C': {'shape': [m,n], 'dtype': 'fp16'}},
        'ops': [{'name': 'g', 'type': 'gemm', 'inputs': ['A','B'], 'outputs': ['C']}],
    }))
    ir = lower_workload(parse_workload(path))
    for layer in ir.layers:
        params = layer.pe_program.params
        kernel = load_template_json(layer.pe_program.template_name)
        patches = {p['offset']: p['encoded_val'] for p in generate_patch_entries(kernel, params)}
        # ISA LOOPIN uses N-1 encoding. gemm.asm has the loop plus one final column.
        columns = patches[17] + 1 + 1
        k_steps = patches[13] + 1
        ps_scalar_count = (patches[3] + 1) * 4
        ldma_scalar_count = patches[6] + 1
        plo_words = layer.agu_plo.iter0 * layer.agu_plo.iter1
        assert columns * k_steps == ps_scalar_count == ldma_scalar_count
        assert columns * layer.agu_pd.iter0 == plo_words == patches[24] + 1
