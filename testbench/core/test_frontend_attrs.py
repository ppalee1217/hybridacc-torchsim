"""Op attr schema: unknown attrs are rejected, conv stride/padding must be ints (M20 D-6)."""

import pytest
import yaml

from hybridacc_cc.frontend import CompilationError, parse_workload

HW = {"num_clusters": 1, "num_pes": 12, "num_bus": 3, "spm_banks_per_group": 3,
      "spm_bank_depth": 8192, "dram_base": 0x80000000}


def _conv(op_type, attrs, h=6, w=7, c=12, oc=24):
    k = 3 if op_type == "conv2d_3x3" else 1
    out = (h - k + 1, w - k + 1)
    return {
        "name": "attrs",
        "hardware": HW,
        "tensors": {
            "input": {"shape": [1, h, w, c], "dtype": "fp16", "layout": "NHWC"},
            "weight": {"shape": [oc, k, k, c], "dtype": "fp16", "layout": "OIHW"},
            "output": {"shape": [1, *out, oc], "dtype": "fp16", "layout": "NHWC"},
        },
        "ops": [{"name": "conv1", "type": op_type, "inputs": ["input", "weight"],
                 "outputs": ["output"], "attrs": attrs}],
    }


def _gemm(attrs):
    return {
        "name": "attrs",
        "hardware": HW,
        "tensors": {
            "a": {"shape": [8, 16], "dtype": "fp16"},
            "b": {"shape": [16, 12], "dtype": "fp16"},
            "y": {"shape": [8, 12], "dtype": "fp16"},
        },
        "ops": [{"name": "gemm1", "type": "gemm", "inputs": ["a", "b"],
                 "outputs": ["y"], "attrs": attrs}],
    }


def _parse(tmp_path, workload):
    path = tmp_path / "w.yaml"
    path.write_text(yaml.safe_dump(workload))
    return parse_workload(path)


@pytest.mark.parametrize("op_type,attrs,key", [
    ("conv2d_3x3", {"stride": 1, "padding": 0, "groups": 2}, "groups"),
    ("conv2d_3x3", {"stride": 1, "padding": 0, "dilation": 2}, "dilation"),
    ("conv2d_1x1", {"stride": 1, "groups": 1}, "groups"),
    ("conv2d_3x3", {"stride": 1, "padding": 0, "pad": 1}, "pad"),
])
def test_unknown_conv_attr_is_rejected(tmp_path, op_type, attrs, key):
    with pytest.raises(CompilationError) as exc:
        _parse(tmp_path, _conv(op_type, attrs))
    assert exc.value.category == "schema"
    assert exc.value.path == "ops[0].attrs"
    assert f"unsupported attr(s) for {op_type}: {key}" in str(exc.value)


@pytest.mark.parametrize("attrs,key", [({"stride": 1}, "stride"), ({"padding": 0}, "padding")])
def test_conv_only_attr_on_gemm_is_rejected(tmp_path, attrs, key):
    with pytest.raises(CompilationError, match=f"unsupported attr\\(s\\) for gemm: {key}"):
        _parse(tmp_path, _gemm(attrs))


@pytest.mark.parametrize("op_type,key,value", [
    ("conv2d_3x3", "stride", 0),
    ("conv2d_3x3", "stride", -1),
    ("conv2d_3x3", "stride", "2"),
    ("conv2d_3x3", "stride", True),
    ("conv2d_3x3", "stride", 1.0),
    ("conv2d_3x3", "padding", -1),
    ("conv2d_3x3", "padding", "valid"),
    ("conv2d_1x1", "stride", 0),
])
def test_conv_int_attr_must_be_valid(tmp_path, op_type, key, value):
    attrs = {"stride": 1, "padding": 0} if op_type == "conv2d_3x3" else {"stride": 1}
    attrs[key] = value
    with pytest.raises(CompilationError) as exc:
        _parse(tmp_path, _conv(op_type, attrs))
    assert exc.value.category == "schema"
    assert exc.value.path == f"ops[0].attrs.{key}"


def test_conv1x1_padding_keeps_its_semantic_error(tmp_path):
    with pytest.raises(CompilationError, match="only conv2d_3x3 may set padding"):
        _parse(tmp_path, _conv("conv2d_1x1", {"stride": 1, "padding": "valid"}))


@pytest.mark.parametrize("workload", [
    _conv("conv2d_3x3", {}),
    _conv("conv2d_3x3", {"stride": 1, "padding": 0, "activation": "relu"}),
    _conv("conv2d_1x1", {"stride": 1, "fuse_relu": True}),
    _gemm({}),
    _gemm({"epilogue": "none"}),
])
def test_supported_attrs_are_accepted(tmp_path, workload):
    ir = _parse(tmp_path, workload)
    assert len(ir.ops) == 1


@pytest.mark.parametrize("op_type,attrs", [
    ("conv2d_3x3", {"stride": 2, "padding": 0}),
    ("conv2d_3x3", {"stride": 3, "padding": 1}),
    ("conv2d_1x1", {"stride": 2}),
    ("conv2d_1x1", {"stride": 3}),
])
def test_conv_stride_above_one_is_rejected(tmp_path, op_type, attrs):
    """The lowering implements stride 1 only; stride 2 and 3 compiled but never halted on ESL."""
    k = 3 if op_type == "conv2d_3x3" else 1
    s, p, h, w = attrs["stride"], attrs.get("padding", 0), 6, 7
    workload = _conv(op_type, attrs, h=h, w=w)
    workload["tensors"]["output"]["shape"] = [1, (h + 2 * p - k) // s + 1, (w + 2 * p - k) // s + 1, 24]
    with pytest.raises(CompilationError) as exc:
        _parse(tmp_path, workload)
    assert exc.value.category == "semantic"
    assert f"stride {s} is not implemented" in str(exc.value)
