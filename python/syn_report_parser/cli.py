from __future__ import annotations

import argparse

from .parser import SynReportParser


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Parse synthesis reports and render RTL module/sub-module area and power breakdown"
    )
    parser.add_argument("--report-dir", required=True, help="Report root or design report folder")
    parser.add_argument("--out-dir", required=True, help="Output folder for JSON/CSV/PNG artifacts")
    parser.add_argument(
        "--focus-module",
        help=(
            "RTL hierarchy path to plot immediate children for. "
            "Default is the top-level design root."
        ),
    )
    parser.add_argument(
        "--top-n",
        type=int,
        default=8,
        help="Show only the top N children in the pie chart and breakdown CSV",
    )
    parser.add_argument(
        "--no-others",
        action="store_true",
        help="Do not collapse remaining children into an 'others' bucket",
    )
    return parser


def main() -> None:
    args = build_parser().parse_args()
    parser = SynReportParser()
    result = parser.parse_report_set(
        report_root=args.report_dir,
        focus_module=args.focus_module,
        top_n=args.top_n,
        include_others=not args.no_others,
    )
    artifacts = parser.write_artifacts(result, args.out_dir)

    print(f"Design: {result['design']}")
    print(f"Report dir: {result['report_dir']}")
    print(f"Area total: {result['area']['metrics'].get('total_cell_area')}")
    print(f"Dynamic power: {result['power'].get('total_dynamic_mw')} mW")
    print(f"Estimated total power: {result['power'].get('table_total_mw')} mW")
    print(f"Focus module: {result['module_report']['focus_module']}")
    print(f"Module rows: {result['module_report']['total_module_rows']}")
    if result["timing"].get("max"):
        timing_max = result["timing"]["max"]
        print(
            "Timing max slack: "
            f"{timing_max.get('slack_ns')} ns ({timing_max.get('slack_status')})"
        )
    if result["timing"].get("min"):
        timing_min = result["timing"]["min"]
        print(
            "Timing min slack: "
            f"{timing_min.get('slack_ns')} ns ({timing_min.get('slack_status')})"
        )
    print("Artifacts:")
    for key, path in artifacts.items():
        print(f"  {key}: {path}")


if __name__ == "__main__":
    main()