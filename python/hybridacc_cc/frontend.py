"""Stage 0: Frontend — YAML parser and WorkloadIR builder.

Parses Workload YAML into type-safe WorkloadIR, performing:
  1. YAML syntax validation
  2. Schema validation (required fields, type checks)
  3. Tensor shape legality checks
  4. Operator parameter legality checks
  5. Hardware parameter legality checks
"""

from __future__ import annotations

import math
from pathlib import Path
from typing import Any, Dict, List

import yaml

from .ir import HardwareDesc, OpDesc, TensorDesc, WorkloadIR


class CompilationError(Exception):
    """All frontend validation failures."""

    def __init__(self, category: str, path: str, message: str):
        self.category = category
        self.path = path
        super().__init__(f"[{category}] {path}: {message}")


# ---------------------------------------------------------------------------
# Schema helpers
# ---------------------------------------------------------------------------

_SUPPORTED_OP_TYPES = {"conv2d_3x3", "conv2d_1x1", "gemm"}
_GEMM_AUTO_SPLIT_K = 96
_OUTPUT_EPILOGUE_ATTRS = (
    "activation", "epilogue", "relu", "fuse_relu", "output_relu",
)


def _require(d: dict, key: str, path: str, expected_type: type | None = None) -> Any:
    if key not in d:
        raise CompilationError("schema", path, f"missing required field '{key}'")
    val = d[key]
    if expected_type is not None and not isinstance(val, expected_type):
        raise CompilationError(
            "schema", path,
            f"field '{key}' must be {expected_type.__name__}, got {type(val).__name__}",
        )
    return val


def _get(d: dict, key: str, default: Any, expected_type: type | None = None) -> Any:
    val = d.get(key, default)
    if expected_type is not None and not isinstance(val, expected_type):
        raise CompilationError(
            "schema", f"(field '{key}')",
            f"must be {expected_type.__name__}, got {type(val).__name__}",
        )
    return val


# ---------------------------------------------------------------------------
# Hardware parse
# ---------------------------------------------------------------------------

def _parse_hardware(raw: dict) -> HardwareDesc:
    path = "hardware"
    num_clusters = _require(raw, "num_clusters", path, int)
    if not (1 <= num_clusters <= 16):
        raise CompilationError("schema", path, "num_clusters must be in [1, 16]")

    num_pes = _get(raw, "num_pes", 48, int)
    if not (1 <= num_pes <= 256):
        raise CompilationError("schema", path, "num_pes must be in [1, 256]")

    num_bus = _get(raw, "num_bus", 3, int)
    if not (1 <= num_bus <= 16):
        raise CompilationError("schema", path, "num_bus must be in [1, 16]")

    if num_pes % num_bus != 0:
        raise CompilationError("schema", path, f"num_pes ({num_pes}) must be divisible by num_bus ({num_bus})")

    spm_banks_per_group = _get(raw, "spm_banks_per_group", 3, int)
    if not (1 <= spm_banks_per_group <= 8):
        raise CompilationError("schema", path, "spm_banks_per_group must be in [1, 8]")

    spm_bank_depth = _get(raw, "spm_bank_depth", 8192, int)
    if spm_bank_depth < 1024 or (spm_bank_depth & (spm_bank_depth - 1)):
        raise CompilationError("schema", path, "spm_bank_depth must be power of 2, >= 1024")

    dram_base = _get(raw, "dram_base", 0x8000_0000, int)
    if dram_base & 0xFFF:
        raise CompilationError("schema", path, "dram_base must be 4KB aligned")

    return HardwareDesc(
        num_clusters=num_clusters,
        num_pes=num_pes,
        num_bus=num_bus,
        spm_banks_per_group=spm_banks_per_group,
        spm_bank_depth=spm_bank_depth,
        dram_base=dram_base,
    )


# ---------------------------------------------------------------------------
# Tensor / Op parse
# ---------------------------------------------------------------------------

def _parse_tensor(raw: dict, path: str, default_layout: str = "NHWC") -> TensorDesc:
    name = _require(raw, "name", path, str)
    shape = _require(raw, "shape", path, list)
    if not all(isinstance(s, int) and s > 0 for s in shape):
        raise CompilationError("schema", f"{path}.shape", "all dims must be positive int")
    dtype = _require(raw, "dtype", path, str)
    if dtype != "fp16":
        raise CompilationError("schema", f"{path}.dtype", f"unsupported dtype '{dtype}', must be 'fp16'")
    layout = raw.get("layout", default_layout)
    return TensorDesc(name=name, shape=shape, dtype=dtype, layout=layout)


def _resolve_tensor(ref, idx: int, path: str,
                     tensor_table: Dict[str, TensorDesc]) -> TensorDesc:
    """Resolve a tensor reference — either a name string or inline dict."""
    if isinstance(ref, str):
        if ref not in tensor_table:
            raise CompilationError(
                "schema", path,
                f"tensor '{ref}' referenced but not defined in 'tensors' section")
        return tensor_table[ref]
    elif isinstance(ref, dict):
        return _parse_tensor(ref, path)
    else:
        raise CompilationError("schema", path,
                               "must be a tensor name (str) or inline tensor (dict)")


def _parse_op(raw: dict, idx: int,
              tensor_table: Dict[str, TensorDesc]) -> OpDesc:
    path = f"ops[{idx}]"
    name = _require(raw, "name", path, str)
    if not name:
        raise CompilationError("schema", path, "name must be non-empty")

    op_type = _require(raw, "type", path, str)
    if op_type not in _SUPPORTED_OP_TYPES:
        raise CompilationError("schema", f"{path}.type", f"unknown op type '{op_type}'")

    raw_inputs = _require(raw, "inputs", path, list)
    inputs = [_resolve_tensor(t, i, f"{path}.inputs[{i}]", tensor_table)
              for i, t in enumerate(raw_inputs)]

    raw_weights = raw.get("weights", [])
    if isinstance(raw_weights, list):
        for i, w in enumerate(raw_weights):
            inputs.append(
                _resolve_tensor(w, i, f"{path}.weights[{i}]", tensor_table))

    raw_outputs = _require(raw, "outputs", path, list)
    outputs = [_resolve_tensor(t, i, f"{path}.outputs[{i}]", tensor_table)
               for i, t in enumerate(raw_outputs)]

    attrs = raw.get("attrs", {})
    if not isinstance(attrs, dict):
        raise CompilationError(
            "schema", f"{path}.attrs",
            f"must be dict, got {type(attrs).__name__}",
        )
    attrs = dict(attrs)
    if "bias_from" in raw:
        if "bias_from" in attrs and attrs["bias_from"] != raw["bias_from"]:
            raise CompilationError(
                "schema", f"{path}.bias_from",
                "conflicts with attrs.bias_from",
            )
        attrs["bias_from"] = raw["bias_from"]
    return OpDesc(op_type=op_type, name=name, inputs=inputs, outputs=outputs, attrs=attrs)


# ---------------------------------------------------------------------------
# Semantic validation
# ---------------------------------------------------------------------------

def _validate_conv2d(op: OpDesc, hw: HardwareDesc) -> None:
    """Validate conv2d_3x3 or conv2d_1x1 semantics."""
    if len(op.inputs) < 2:
        raise CompilationError("semantic", op.name, "conv2d requires at least 2 inputs (activation + weight)")
    if len(op.outputs) < 1:
        raise CompilationError("semantic", op.name, "conv2d requires at least 1 output")

    act = op.inputs[0]
    wt = op.inputs[1]
    out = op.outputs[0]

    if len(act.shape) != 4:
        raise CompilationError("semantic", op.name, f"activation must be 4D NHWC, got {len(act.shape)}D")
    if len(wt.shape) != 4:
        raise CompilationError("semantic", op.name, f"weight must be 4D, got {len(wt.shape)}D")
    if len(out.shape) != 4:
        raise CompilationError("semantic", op.name, f"output must be 4D, got {len(out.shape)}D")

    N, H_in, W_in, C_in = act.shape
    OC, KH, KW, wt_cin = wt.shape
    N_out, H_out_y, W_out_y, OC_out = out.shape

    if N_out != N:
        raise CompilationError(
            "shape_mismatch", op.name,
            f"output batch N={N_out} != input batch N={N}",
        )

    if wt_cin != C_in:
        raise CompilationError("semantic", op.name, f"weight IC={wt_cin} != input C_in={C_in}")
    if OC_out != OC:
        raise CompilationError("semantic", op.name, f"output OC={OC_out} != weight OC={OC}")

    stride = op.attrs.get("stride", 1)
    padding = op.attrs.get("padding", 0)

    if op.op_type != "conv2d_3x3" and "padding" in op.attrs:
        raise CompilationError(
            "semantic",
            op.name,
            f"{op.op_type} does not support the padding attribute; only conv2d_3x3 may set padding",
        )

    H_out_expected = (H_in + 2 * padding - KH) // stride + 1
    W_out_expected = (W_in + 2 * padding - KW) // stride + 1

    if H_out_y != H_out_expected or W_out_y != W_out_expected:
        raise CompilationError(
            "shape_mismatch", op.name,
            f"output shape [{N},{H_out_y},{W_out_y},{OC_out}] does not match "
            f"computed [{N},{H_out_expected},{W_out_expected},{OC}]",
        )

    # Kernel shape.  C_in alignment is handled losslessly by the lowering
    # path: conv3x3 pads to a 4-channel tile and conv1x1 pads to a 12-channel
    # tile, with zero-filled activation/weight tails in the DRAM generator.
    if op.op_type == "conv2d_3x3":
        if KH != 3 or KW != 3:
            raise CompilationError("semantic", op.name, f"conv2d_3x3 requires KH=KW=3, got {KH}x{KW}")
    elif op.op_type == "conv2d_1x1":
        if KH != 1 or KW != 1:
            raise CompilationError("semantic", op.name, f"conv2d_1x1 requires KH=KW=1, got {KH}x{KW}")


def _validate_gemm(op: OpDesc, hw: HardwareDesc) -> None:
    """Validate GEMM semantics."""
    if len(op.inputs) < 2:
        raise CompilationError("semantic", op.name, "gemm requires at least 2 inputs (A + B)")
    if len(op.outputs) < 1:
        raise CompilationError("semantic", op.name, "gemm requires at least 1 output")

    A = op.inputs[0]
    B = op.inputs[1]
    C = op.outputs[0]

    if len(A.shape) != 2 or len(B.shape) != 2 or len(C.shape) != 2:
        raise CompilationError("semantic", op.name, "GEMM tensors must be 2D")

    M, K_a = A.shape
    K_b, N = B.shape
    M_c, N_c = C.shape

    if K_a != K_b:
        raise CompilationError("semantic", op.name, f"A cols={K_a} != B rows={K_b}")
    if M_c != M or N_c != N:
        raise CompilationError("semantic", op.name, f"output [{M_c},{N_c}] != expected [{M},{N}]")
    # Any positive logical K is legal.  GEMM lowering and DRAM packing use the
    # PE_K-padded physical footprint, which is packet aligned independently of K.


def _validate_spm_capacity(op: OpDesc, hw: HardwareDesc) -> None:
    """Check that at least tile_ic/tile_oc fit in half group capacity."""
    half_cap = hw.half_group_capacity
    pkt = 8
    if op.op_type == "conv2d_3x3":
        # Minimum PS: tile_oc=min(OC,16), tile_ic=4
        OC = op.inputs[1].shape[0]
        tile_oc = min(OC, 16)
        ps_min = tile_oc * 3 * 3 * 1 * pkt
        if ps_min > half_cap:
            raise CompilationError(
                "semantic", op.name,
                f"minimum weight tile ({ps_min} B) > half_group_capacity ({half_cap} B)")
    elif op.op_type == "conv2d_1x1":
        OC = op.inputs[1].shape[0]
        tile_oc = min(OC, 16)
        ps_min = tile_oc * 1 * pkt
        if ps_min > half_cap:
            raise CompilationError(
                "semantic", op.name,
                f"minimum weight tile ({ps_min} B) > half_group_capacity ({half_cap} B)")


def _fresh_generated_name(base: str, used_names: set) -> str:
    """Return a deterministic synthetic name that cannot shadow workload names."""
    candidate = base
    suffix = 1
    while candidate in used_names:
        candidate = f"{base}_{suffix}"
        suffix += 1
    used_names.add(candidate)
    return candidate


def _auto_split_large_gemms(ops: List[OpDesc], hw: HardwareDesc) -> List[OpDesc]:
    """Rewrite source-input GEMMs with K>96 into bias-chained K chunks.

    A chunk is at most one three-bus K wave (3 * PE_K=32).  The original
    output tensor belongs to the last chunk, so downstream users continue to
    see the workload's public tensor name.
    """
    used_op_names = {op.name for op in ops}
    used_tensor_names = {
        tensor.name
        for op in ops
        for tensor in (*op.inputs, *op.outputs)
    }
    workload_output_names = {
        tensor.name for op in ops for tensor in op.outputs
    }
    expanded_ops: List[OpDesc] = []

    for op in ops:
        if op.op_type != "gemm" or op.inputs[0].shape[1] <= _GEMM_AUTO_SPLIT_K:
            expanded_ops.append(op)
            continue

        A, B = op.inputs[:2]
        C = op.outputs[0]
        chained_inputs = [
            tensor.name for tensor in (A, B) if tensor.name in workload_output_names
        ]
        if chained_inputs:
            raise CompilationError(
                "semantic", op.name,
                "automatic K-splitting currently requires source A/B tensors; "
                f"produced input(s) are {', '.join(chained_inputs)}",
            )

        M, K = A.shape
        _, N = B.shape
        previous_output = None
        num_chunks = math.ceil(K / _GEMM_AUTO_SPLIT_K)
        for chunk_index in range(num_chunks):
            k_start = chunk_index * _GEMM_AUTO_SPLIT_K
            k_end = min(K, k_start + _GEMM_AUTO_SPLIT_K)
            chunk_k = k_end - k_start
            is_final = chunk_index == num_chunks - 1

            chunk_name = _fresh_generated_name(
                f"{op.name}__kchunk{chunk_index}", used_op_names
            )
            chunk_a = TensorDesc(
                name=_fresh_generated_name(
                    f"{A.name}__{op.name}_a_k{k_start}_{k_end}", used_tensor_names
                ),
                shape=[M, chunk_k], dtype=A.dtype, layout=A.layout,
            )
            chunk_b = TensorDesc(
                name=_fresh_generated_name(
                    f"{B.name}__{op.name}_b_k{k_start}_{k_end}", used_tensor_names
                ),
                shape=[chunk_k, N], dtype=B.dtype, layout=B.layout,
            )
            if is_final:
                chunk_output = C
            else:
                chunk_output = TensorDesc(
                    name=_fresh_generated_name(
                        f"{C.name}__{op.name}_kchunk{chunk_index}", used_tensor_names
                    ),
                    shape=list(C.shape), dtype=C.dtype, layout=C.layout,
                )

            chunk_attrs = dict(op.attrs)
            if not is_final:
                for attr_name in _OUTPUT_EPILOGUE_ATTRS:
                    chunk_attrs.pop(attr_name, None)
            if previous_output is not None:
                chunk_attrs["bias_from"] = previous_output.name

            chunk_op = OpDesc(
                op_type="gemm", name=chunk_name,
                inputs=[chunk_a, chunk_b], outputs=[chunk_output],
                attrs=chunk_attrs,
            )
            _validate_gemm(chunk_op, hw)
            _validate_spm_capacity(chunk_op, hw)
            expanded_ops.append(chunk_op)
            previous_output = chunk_output

    return expanded_ops


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def parse_workload(yaml_path: str | Path) -> WorkloadIR:
    """Parse a Workload YAML file and return a validated WorkloadIR."""
    with open(yaml_path, "r") as f:
        raw = yaml.safe_load(f)

    if not isinstance(raw, dict):
        raise CompilationError("schema", str(yaml_path), "top level must be a mapping")

    # Name
    wl_name = _get(raw, "name", Path(yaml_path).stem, str)

    # Hardware
    hw_raw = _require(raw, "hardware", "(top-level)", dict)
    hw = _parse_hardware(hw_raw)

    # Tensor table (optional top-level section for named references)
    tensor_table: Dict[str, TensorDesc] = {}
    if "tensors" in raw:
        tensors_raw = raw["tensors"]
        if not isinstance(tensors_raw, dict):
            raise CompilationError("schema", "tensors", "must be a mapping")
        for tname, traw in tensors_raw.items():
            if not isinstance(traw, dict):
                raise CompilationError("schema", f"tensors.{tname}", "must be a mapping")
            traw_with_name = {**traw, "name": tname}
            tensor_table[tname] = _parse_tensor(
                traw_with_name, f"tensors.{tname}",
                default_layout="" if len(traw.get("shape", [])) == 2 else "NHWC")

    # Ops
    ops_raw = _require(raw, "ops", "(top-level)", list)
    if len(ops_raw) == 0:
        raise CompilationError("schema", "ops", "at least one op is required")

    ops: List[OpDesc] = []
    seen_names: set = set()
    for i, op_raw in enumerate(ops_raw):
        if not isinstance(op_raw, dict):
            raise CompilationError("schema", f"ops[{i}]", "each op must be a mapping")
        op = _parse_op(op_raw, i, tensor_table)
        if op.name in seen_names:
            raise CompilationError("schema", f"ops[{i}].name", f"duplicate name '{op.name}'")
        seen_names.add(op.name)

        # Semantic validation
        if op.op_type in ("conv2d_3x3", "conv2d_1x1"):
            _validate_conv2d(op, hw)
        elif op.op_type == "gemm":
            _validate_gemm(op, hw)

        _validate_spm_capacity(op, hw)
        ops.append(op)

    ops = _auto_split_large_gemms(ops, hw)
    return WorkloadIR(name=wl_name, hardware=hw, ops=ops)
