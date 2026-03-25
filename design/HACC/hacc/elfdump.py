from __future__ import annotations

import argparse

from .utils import DumpOpts, dump_elf


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="hacc-elfdump")
    parser.add_argument("elf_file")
    parser.add_argument("-d", action="store_true", dest="disasm")
    parser.add_argument("-x", action="store_true", dest="decode")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    ok = dump_elf(args.elf_file, DumpOpts(disasm=args.disasm, decode=args.decode))
    return 0 if ok else 2