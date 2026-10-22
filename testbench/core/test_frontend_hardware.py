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


def test_dram_burst_bytes_defaults_to_64_and_enters_the_fingerprint():
    hw = _parse_hardware({"num_clusters": 1})
    record = hw.config_fingerprint_record()
    assert hw.dram_burst_bytes == 64
    assert record["version"] == "hac-v2" and record["fields"]["dram_burst_bytes"] == 64
    single_beat = _parse_hardware({"num_clusters": 1, "dram_burst_bytes": 8})
    assert single_beat.config_fingerprint() != hw.config_fingerprint()


@pytest.mark.parametrize("burst_bytes", [0, 4, 12, 48, 256])
def test_unsupported_dram_burst_bytes_is_rejected(burst_bytes):
    with pytest.raises(CompilationError, match="dram_burst_bytes must be a power of 2 in"):
        _parse_hardware({"num_clusters": 1, "dram_burst_bytes": burst_bytes})
