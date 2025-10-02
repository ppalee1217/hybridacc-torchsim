from __future__ import annotations
import json
from pathlib import Path
from copy import deepcopy


def _compress_groups(g, join_lines:bool=False):
    if not isinstance(g, list) or not g:
        return g
    try:
        A = len(g); R = len(g[0]); C = len(g[0][0])
    except Exception:
        return g
    arrays = []
    for a in range(A):
        rows = []
        for r in range(R):
            row = g[a][r]
            if not isinstance(row, list):
                return g
            rows.append(" ".join(str(x) for x in row))
        if join_lines:
            arrays.append(";".join(rows))  # 以 ; 連結 7 行
        else:
            arrays.append(rows)
    meta_fmt = 'rows_joined_semicolon' if join_lines else 'rows_list'
    return {"shape": [A, R, C], "arrays": arrays, "format": meta_fmt}


def write_schedule_ir(ir_obj, out_path, remove_original:bool=False, pretty:bool=True, join_lines:bool=False):
    p = Path(out_path)
    p.parent.mkdir(parents=True, exist_ok=True)
    data = ir_obj.to_dict() if hasattr(ir_obj,'to_dict') else deepcopy(ir_obj)
    mapping = data.get('mapping') if isinstance(data, dict) else None
    if isinstance(mapping, dict):
        for k in ['pe_weight_groups','pe_activation_groups','pe_output_groups']:
            if k in mapping and isinstance(mapping[k], list):
                mapping[k + '_compact'] = _compress_groups(mapping[k], join_lines=join_lines)
                if remove_original:
                    del mapping[k]
    with open(p,'w') as f:
        if pretty:
            json.dump(data, f, indent=2, ensure_ascii=False)
        else:
            json.dump(data, f, ensure_ascii=False, separators=(',', ':'))
    return str(p)
