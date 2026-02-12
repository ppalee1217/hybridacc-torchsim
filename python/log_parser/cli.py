"""Command-line interface for log_parser."""

from __future__ import annotations

import argparse
from pathlib import Path

from .parser import NocSimLogParser
from .ploter import LogPlotter


SUPPORTED_PARSE_TYPES = ("noc_sim",)
CHART_TYPES = ("bar", "line", "scatter")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="HybridAcc log parser")
    subparsers = parser.add_subparsers(dest="command", required=True)

    list_cmd = subparsers.add_parser("list", help="List .log files in a folder")
    list_cmd.add_argument("--type", default="noc_sim", choices=SUPPORTED_PARSE_TYPES)
    list_cmd.add_argument("--dir", required=True, help="Log folder path")

    parse_cmd = subparsers.add_parser("parse", help="Parse selected log files")
    parse_cmd.add_argument("--type", default="noc_sim", choices=SUPPORTED_PARSE_TYPES)
    parse_cmd.add_argument("--dir", help="Log folder path")
    parse_cmd.add_argument("--files", nargs="+", help="Specific log files")
    parse_cmd.add_argument("--out-csv", help="Save parsed table to csv")

    plot_cmd = subparsers.add_parser("plot", help="Parse and plot metric")
    plot_cmd.add_argument("--type", default="noc_sim", choices=SUPPORTED_PARSE_TYPES)
    plot_cmd.add_argument("--dir", help="Log folder path")
    plot_cmd.add_argument("--files", nargs="+", help="Specific log files")
    plot_cmd.add_argument("--metric", required=True, help="Metric to plot")
    plot_cmd.add_argument("--chart", default="bar", choices=CHART_TYPES)
    plot_cmd.add_argument("--out", required=True, help="Output figure path or directory")

    gui_cmd = subparsers.add_parser("gui", help="Launch Textual GUI")
    gui_cmd.add_argument("--type", default="noc_sim", choices=SUPPORTED_PARSE_TYPES)

    return parser


def _resolve_files(args, parser_impl: NocSimLogParser):
    if args.files:
        return [Path(file).expanduser() for file in args.files]
    if args.dir:
        return parser_impl.list_log_files(args.dir)
    raise ValueError("Please provide --dir or --files")


def main() -> None:
    args = build_parser().parse_args()

    if args.command == "gui":
        from .gui import run_gui

        run_gui(parse_type=args.type)
        return

    parser_impl = NocSimLogParser(parse_type=args.type)

    if args.command == "list":
        files = parser_impl.list_log_files(args.dir)
        for file in files:
            print(file)
        print(f"Total: {len(files)}")
        return

    files = _resolve_files(args, parser_impl)
    df = parser_impl.parse_files(files)

    if args.command == "parse":
        if args.out_csv:
            out = Path(args.out_csv).expanduser()
            out.parent.mkdir(parents=True, exist_ok=True)
            df.to_csv(out, index=False)
            print(f"Saved parsed results: {out}")
        print(df.to_string(index=False))
        return

    if args.command == "plot":
        output = LogPlotter.save_metric_plot(
            df=df,
            metric=args.metric,
            chart_type=args.chart,
            output_path=args.out,
        )
        print(f"Saved figure: {output}")


if __name__ == "__main__":
    main()
