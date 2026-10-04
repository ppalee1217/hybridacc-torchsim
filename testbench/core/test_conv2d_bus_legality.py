"""Conv2D 3x3 needs at least one bus per kernel row before lowering succeeds."""

import pytest
import yaml

from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import TilingFailed, lower_workload


def _conv3x3_workload(tmp_path, num_pes, num_bus):
    workload = tmp_path / f"conv3x3_pe{num_pes}_b{num_bus}.yaml"
    workload.write_text(yaml.safe_dump({
        "name": "conv3x3_bus_legality",
        "hardware": {
            "num_clusters": 1,
            "num_pes": num_pes,
            "num_bus": num_bus,
            "spm_banks_per_group": num_bus,
            "spm_bank_depth": 8192,
            "dram_base": 0x80000000,
        },
        "tensors": {
            "input": {"shape": [1, 8, 16, 4], "dtype": "fp16", "layout": "NHWC"},
            "weight": {"shape": [16, 3, 3, 4], "dtype": "fp16", "layout": "OIHW"},
            "bias": {"shape": [16], "dtype": "fp16"},
            "output": {"shape": [1, 6, 14, 16], "dtype": "fp16", "layout": "NHWC"},
        },
        "ops": [{
            "name": "conv1",
            "type": "conv2d_3x3",
            "inputs": ["input", "weight", "bias"],
            "outputs": ["output"],
            "attrs": {"stride": 1, "padding": 0},
        }],
    }))
    return parse_workload(workload)


@pytest.mark.parametrize("num_pes,num_bus", [(12, 2), (24, 2), (48, 2), (96, 2), (16, 1)])
def test_conv3x3_with_fewer_buses_than_kernel_rows_is_rejected(tmp_path, num_pes, num_bus):
    with pytest.raises(TilingFailed, match=rf"needs num_bus >= KH .* got num_bus={num_bus}, KH=3"):
        lower_workload(_conv3x3_workload(tmp_path, num_pes, num_bus))


@pytest.mark.parametrize("num_pes,num_bus", [(12, 3), (48, 3), (64, 4)])
def test_conv3x3_with_one_bus_per_kernel_row_still_lowers(tmp_path, num_pes, num_bus):
    hw_ir = lower_workload(_conv3x3_workload(tmp_path, num_pes, num_bus))
    assert len(hw_ir.layers) == 1
