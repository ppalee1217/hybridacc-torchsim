from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import yaml

from .compiler import HaccCompiler
from .frontend import Frontend
from .ir import DataType


def _parse_dtype(raw: str) -> DataType:
    return DataType[raw.upper()]


def _load_spec(path: Path) -> dict[str, Any]:
    text = path.read_text(encoding="utf-8")
    if path.suffix.lower() in {".yaml", ".yml"}:
        loaded = yaml.safe_load(text)
    else:
        loaded = json.loads(text)
    if not isinstance(loaded, dict):
        raise ValueError("Input spec must be a mapping")
    return loaded


def _build_op(spec: dict[str, Any]):
    op_type = str(spec["op_type"]).lower()
    dtype = _parse_dtype(str(spec.get("dtype", "INT16")))
    if op_type == "conv2d":
        return Frontend.create_conv2d(
            spec["name"],
            dtype,
            spec["n"],
            spec["ic"],
            spec["ih"],
            spec["iw"],
            spec["oc"],
            spec["kh"],
            spec["kw"],
            spec.get("stride_h", 1),
            spec.get("stride_w", 1),
            spec.get("pad_h", 0),
            spec.get("pad_w", 0),
            spec.get("with_nlu", False),
        )
    if op_type == "gemm":
        return Frontend.create_gemm(
            spec["name"],
            dtype,
            spec["M"],
            spec["N"],
            spec["K"],
            spec.get("trans_a", False),
            spec.get("trans_b", False),
            spec.get("with_nlu", False),
        )
    raise ValueError(f"Unsupported op_type: {spec['op_type']}")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="hacc")
    subparsers = parser.add_subparsers(dest="command", required=True)

    compile_parser = subparsers.add_parser("compile")
    compile_parser.add_argument("input", type=Path)
    compile_parser.add_argument("-o", "--output-prefix", type=Path, required=True)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "compile":
        spec = _load_spec(args.input)
        op = _build_op(spec)
        compiler = HaccCompiler()
        result = compiler.compile(op)
        if not result.success:
            print(result.error)
            return 1
        compiler.write_outputs(result, str(args.output_prefix))
        print(f"compiled {op.name} -> {args.output_prefix}.hacc.elf")
        return 0
    return 1