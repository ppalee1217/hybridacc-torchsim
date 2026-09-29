"""Hardware legality checks before compiler lowering or firmware generation."""

import pytest
import yaml

from hybridacc_cc.frontend import CompilationError, _parse_hardware, parse_workload


def test_default_hardware_has_matching_payload_widths():
    hw = _parse_hardware({"num_clusters": 1})
    assert hw.num_bus == hw.spm_banks_per_group == 3


@pytest.mark.parametrize("banks_per_group", [1, 2, 3, 4, 8])
def test_matching_payload_widths_are_legal(banks_per_group):
    hw = _parse_hardware({
        "num_clusters": 2,
        "num_pes": 24,
        "num_bus": banks_per_group,
        "spm_banks_per_group": banks_per_group,
    })
    assert hw.num_bus == hw.spm_banks_per_group == banks_per_group


@pytest.mark.parametrize("num_bus,banks_per_group", [(2, 3), (3, 2), (4, 3)])
def test_mismatched_payload_widths_report_both_fields(num_bus, banks_per_group):
    with pytest.raises(CompilationError) as exc:
        _parse_hardware({
            "num_clusters": 1,
            "num_pes": 24,
            "num_bus": num_bus,
            "spm_banks_per_group": banks_per_group,
        })
    assert exc.value.category == "schema"
    assert exc.value.path == "hardware"
    message = str(exc.value)
    assert f"num_bus ({num_bus}) * port width (64 bits) = {num_bus * 64} bits" in message
    assert (
        f"spm_banks_per_group ({banks_per_group}) * bank width (64 bits) "
        f"= {banks_per_group * 64} bits"
    ) in message


def test_invalid_hardware_is_rejected_before_workload_parsing(tmp_path):
    workload = tmp_path / "invalid.yaml"
    workload.write_text(yaml.safe_dump({
        "hardware": {"num_clusters": 1, "num_bus": 2},
        "tensors": "invalid tensor table that must not be parsed",
    }))
    with pytest.raises(CompilationError, match="num_bus and spm_banks_per_group must match"):
        parse_workload(workload)
