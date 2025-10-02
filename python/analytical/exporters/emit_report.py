# ...new file...
from __future__ import annotations
import json, yaml
from pathlib import Path

def write_analysis_artifacts(out_dir, tiling, mapping, ir, report, hw_cfg=None):
    p = Path(out_dir)
    p.mkdir(parents=True, exist_ok=True)
    with open(p/'tiling_parameters.json','w') as f: json.dump(tiling.to_dict(), f, indent=2)
    with open(p/'schedule_ir.json','w') as f: json.dump(ir.to_dict(), f, indent=2)
    with open(p/'analysis_report.json','w') as f: json.dump(report.to_dict(), f, indent=2)
    if hw_cfg:
        with open(p/'hw_config.yaml','w') as f: yaml.safe_dump(hw_cfg, f, sort_keys=False)
    return str(p)
