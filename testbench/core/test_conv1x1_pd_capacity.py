"""Parallel conv1x1 PD tiles must fit between their ping and pong bases.

D-40: upstream a090a7b used the linear group half capacity for PD even when
H-stripe execution selected parallel PD addressing. Co=8 no longer had the
larger PLI footprint that accidentally bounded PD, so prefetch overlapped
live input data. The contract is the allocated address interval, independent
of the compiler's width-selection formula.
"""
import pytest
import yaml
from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload


@pytest.mark.parametrize("h,w,ci,co,pes,buses", [
    (31, 63, 100, 8, 48, 2),
    (55, 47, 60, 8, 96, 2),
    (31, 56, 100, 8, 48, 2),
    (31, 57, 100, 8, 48, 2),
    (55, 28, 60, 8, 96, 2),
    (55, 29, 60, 8, 96, 2),
    (31, 63, 100, 8, 96, 2),
    (31, 63, 100, 16, 48, 2),
])
def test_pd_transfer_stays_in_its_ping_pong_allocation(tmp_path, h, w, ci, co, pes, buses):
    workload = {
        "name": "conv1x1_pd_capacity",
        "hardware": {"num_clusters": 1, "num_pes": pes, "num_bus": buses,
                     "spm_banks_per_group": buses, "spm_bank_depth": 8192,
                     "dram_base": 0x80000000},
        "tensors": {
            "input": {"shape": [1, h, w, ci], "dtype": "fp16", "layout": "NHWC"},
            "weight": {"shape": [co, 1, 1, ci], "dtype": "fp16", "layout": "OIHW"},
            "output": {"shape": [1, h, w, co], "dtype": "fp16", "layout": "NHWC"},
        },
        "ops": [{"name": "conv", "type": "conv2d_1x1", "inputs": ["input", "weight"],
                 "outputs": ["output"], "attrs": {"stride": 1}}],
    }
    path = tmp_path / "workload.yaml"
    path.write_text(yaml.safe_dump(workload))
    layer = lower_workload(parse_workload(path)).layers[0]
    tp = layer.tiling_params
    pd = layer.spm_layout.pd
    allocated_bytes = pd.pong_base - pd.ping_base
    assert pd.size <= allocated_bytes, (pd.size, allocated_bytes)
    assert tp.dma_pd_words * 8 <= tp.spm_pong[1] - tp.spm_ping[1]
    assert tp.num_w_tiles * tp.tile_w_out >= w
    assert (tp.num_w_tiles - 1) * tp.tile_w_out + tp.last_w_out == w
