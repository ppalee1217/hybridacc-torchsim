"""An ultra PLO read needs a response from every NoC port (E010)."""

import dataclasses

import pytest
import yaml

from hybridacc_cc.frontend import CompilationError, parse_workload
from hybridacc_cc.lowering import _assert_ultra_plo_all_ports, lower_workload


def _conv1x1(tmp_path, h, w, ic, oc, num_pes, num_bus):
    workload = tmp_path / f"conv1x1_{h}_{w}_{ic}_{oc}_pe{num_pes}_b{num_bus}.yaml"
    workload.write_text(yaml.safe_dump({
        "name": "conv1x1_ultra_ports",
        "hardware": {
            "num_clusters": 1,
            "num_pes": num_pes,
            "num_bus": num_bus,
            "spm_banks_per_group": num_bus,
            "spm_bank_depth": 1024,
            "dram_base": 0x80000000,
        },
        "tensors": {
            "input": {"shape": [1, h, w, ic], "dtype": "fp16", "layout": "NHWC"},
            "weight": {"shape": [oc, 1, 1, ic], "dtype": "fp16", "layout": "OIHW"},
            "output": {"shape": [1, h, w, oc], "dtype": "fp16", "layout": "NHWC"},
        },
        "ops": [{
            "name": "conv1",
            "type": "conv2d_1x1",
            "inputs": ["input", "weight"],
            "outputs": ["output"],
            "attrs": {"stride": 1},
        }],
    }))
    ir = lower_workload(parse_workload(workload))
    return ir, ir.layers[0]


def _enabled_buses(layer, num_bus):
    per_bus = len(layer.scan_chain) // num_bus
    return {i // per_bus for i, e in enumerate(layer.scan_chain) if e.enable}


# Both shapes come from the M16 kill-test cells that hung waiting for a third port:
# H=16 over 8-PE buses fills only two H stripes; 8 OC tiles over 3 buses keep
# only two resident OC tiles.
@pytest.mark.parametrize("h,w,ic,oc,num_pes", [(16, 16, 36, 16, 24), (18, 53, 8, 125, 12)])
def test_partial_port_mapping_falls_back_to_normal(tmp_path, h, w, ic, oc, num_pes):
    _, layer = _conv1x1(tmp_path, h, w, ic, oc, num_pes, 3)
    assert not layer.agu_plo.ctrl & 0x8
    assert _enabled_buses(layer, 3) == {0}


@pytest.mark.parametrize("h,oc", [(24, 16), (8, 48)])
def test_mappings_covering_every_port_stay_ultra(tmp_path, h, oc):
    # H=24 fills three 8-PE H stripes; OC=48 is three resident OC tiles.
    _, layer = _conv1x1(tmp_path, h, 8, 12, oc, 24, 3)
    assert layer.agu_plo.ctrl & 0x8
    assert _enabled_buses(layer, 3) == {0, 1, 2}


def test_ultra_layer_with_a_silent_port_is_rejected(tmp_path):
    _, layer = _conv1x1(tmp_path, 24, 8, 12, 16, 24, 3)
    _assert_ultra_plo_all_ports(layer, 3)
    per_bus = len(layer.scan_chain) // 3
    silenced = [dataclasses.replace(e, enable=False) if i >= 2 * per_bus else e
                for i, e in enumerate(layer.scan_chain)]
    with pytest.raises(CompilationError, match="E010"):
        _assert_ultra_plo_all_ports(dataclasses.replace(layer, scan_chain=silenced), 3)
