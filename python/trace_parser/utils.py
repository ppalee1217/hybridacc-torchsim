"""小型工具函式。"""
from typing import Dict, List, Tuple, Any


def top_n_counts(counter_dict: Dict[str, int], n: int = 10) -> List[Tuple[str, int]]:
    """從 count dict 取前 n 名。"""
    return sorted(counter_dict.items(), key=lambda x: x[1], reverse=True)[:n]


def durations_summary(durs: Dict[Tuple[int, int, str], List[float]]) -> Dict[str, Any]:
    """對 durations 結果做統計 summary（平均/最大/最小/樣本數）。"""
    out = {}
    for key, arr in durs.items():
        if not arr:
            continue
        count = len(arr)
        s = sum(arr)
        out[key] = {"count": count, "avg": s / count, "min": min(arr), "max": max(arr)}
    return out
