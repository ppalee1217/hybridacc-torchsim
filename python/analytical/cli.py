from __future__ import annotations
import json, yaml, sys, argparse
from pathlib import Path
from .gen_tiling import generate_tiling
from .mapper import build_mapping
from .ir_builder import build_schedule_ir
from .cost_model import CostModel
from .workloads import workload_from_dict
from .hardware import hardware_spec_from_config, DEFAULT_HW_SPEC
from .exporters.emit_ir import write_schedule_ir  # 新增匯入


def parse_args(argv=None):
    p = argparse.ArgumentParser(prog='ha-analytical', description='HybridAcc analytical workflow')
    p.add_argument('--workload', required=True, help='Path to workload JSON / YAML file')
    p.add_argument('--arch', help='Optional architecture yaml/json (hw_config.yaml)')
    p.add_argument('--outdir', default='python/analytical/outputs', help='Output directory')
    p.add_argument('--emit-hw-config', action='store_true', help='Emit hw_config.yaml skeleton')
    p.add_argument('--opt', choices=['heuristic'], default='heuristic')
    p.add_argument('--print_summary', action='store_true', help='Print concise summary')
    p.add_argument('--compact-ir', action='store_true', help='Compress PE group 3D arrays (store *_compact only)')
    p.add_argument('--ir-no-pretty', action='store_true', help='Output schedule_ir.json in single-line (no indentation)')
    p.add_argument('--ir-join-lines', action='store_true', help='Join each array rows with semicolons in *_compact')
    p.add_argument('--force-policy', choices=['channel-first','spatial-first','hybrid'], help='Override automatic mapping policy (hybrid: intra-cluster channel-first, inter-cluster spatial-first)')
    return p.parse_args(argv)


def load_any(path:str):
    with open(path,'r') as f:
        if path.endswith('.json'):
            return json.load(f)
        else:
            return yaml.safe_load(f)


def main(argv=None):
    args = parse_args(argv)
    wl_dict = load_any(args.workload)
    wl = workload_from_dict(wl_dict)
    hw = hardware_spec_from_config(load_any(args.arch)) if args.arch else DEFAULT_HW_SPEC

    tiling = generate_tiling(wl, hw.pe)
    mapping = build_mapping(tiling, hw)
    ir = build_schedule_ir(tiling, mapping)
    cm = CostModel(hw)
    report = cm.estimate(tiling, mapping)

    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    with open(outdir/'tiling_parameters.json','w') as f: json.dump(tiling.to_dict(), f, indent=2)
    write_schedule_ir(ir, outdir/'schedule_ir.json', remove_original=args.compact_ir, pretty=not args.ir_no_pretty, join_lines=args.ir_join_lines)
    with open(outdir/'analysis_report.json','w') as f: json.dump(report.to_dict(), f, indent=2)
    if args.emit_hw_config and not args.arch:
        from .exporters.emit_hw_config import write_hw_config
        write_hw_config(outdir/'hw_config.yaml')
    if args.print_summary:
        print(json.dumps({
            'macs': report.macs,
            'latency_cycles': report.latency_cycles,
            'energy_pj': report.energy_pj,
            'buffer_fits': report.detail['buffer_capacity']['fits']
        }, indent=2))
    print(f"Outputs written to {outdir}")

if __name__ == '__main__':
    main()
