from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def _project_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _run(cmd: list[str]) -> int:
    proc = subprocess.run(cmd, check=False)
    return proc.returncode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="hacc-setup",
        description="HybridAcc unified setup entry for install/env/fast-entry",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    p_all = subparsers.add_parser("all", help="Run all installers and apply shell env")
    p_all.add_argument("--riscv-prefix", default=str(Path.home() / ".local" / "riscv"))
    p_all.add_argument("--no-source", action="store_true")

    p_install = subparsers.add_parser("install", help="Install one tool or all")
    p_install.add_argument("target", choices=["systemc", "riscv", "pe", "python", "all"])
    p_install.add_argument("extra", nargs=argparse.REMAINDER)

    p_env = subparsers.add_parser("env", help="Apply shell exports into ~/.bashrc")
    p_env.add_argument("--riscv-prefix", default=str(Path.home() / ".local" / "riscv"))
    p_env.add_argument("--no-source", action="store_true")

    p_fast = subparsers.add_parser("fast", help="Run fast-entry scripts")
    p_fast.add_argument(
        "target",
        choices=["cluster-sim", "noc-sim", "pe-sim", "cluster-test", "run-test"],
    )
    p_fast.add_argument("extra", nargs=argparse.REMAINDER)

    args = parser.parse_args(argv)

    setup_script = _project_root() / "scripts" / "setup.sh"
    if not setup_script.exists():
        print(f"Error: setup script not found: {setup_script}", file=sys.stderr)
        return 1

    if args.command == "all":
        cmd = [str(setup_script), "all", "--riscv-prefix", args.riscv_prefix]
        if args.no_source:
            cmd.append("--no-source")
        return _run(cmd)

    if args.command == "install":
        cmd = [str(setup_script), "install", args.target]
        if args.extra:
            cmd.extend(args.extra)
        return _run(cmd)

    if args.command == "env":
        cmd = [str(setup_script), "env", "--riscv-prefix", args.riscv_prefix]
        if args.no_source:
            cmd.append("--no-source")
        return _run(cmd)

    if args.command == "fast":
        cmd = [str(setup_script), "fast", args.target]
        if args.extra:
            cmd.extend(args.extra)
        return _run(cmd)

    return 1


if __name__ == "__main__":
    raise SystemExit(main())
