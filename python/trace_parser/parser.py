"""
Trace parser for HybridAcc trace JSON.
簡易 API:
  tp = TraceParser.load("trace.json")
  tp2 = tp.filter(name="RUNNING", pid=0)
  counts = tp2.count_by("name")
  durations = tp.durations()  # dict: (pid,tid,name) -> [durations]
  tp2.save("out.json")
"""
import json
from collections import Counter, defaultdict
from typing import List, Dict, Optional, Any, Tuple


class TraceParser:
    def __init__(self, events: Optional[List[Dict[str, Any]]] = None):
        self.events = events or []

    @classmethod
    def load(cls, path: str) -> "TraceParser":
        """載入 trace 檔案。支援整個 JSON array 或每行一個 JSON 物件的格式。"""
        with open(path, "r", encoding="utf-8") as f:
            txt = f.read().strip()
            if not txt:
                return cls([])
            try:
                data = json.loads(txt)
                if isinstance(data, dict):
                    return cls(data.get("traceEvents", []))
                if isinstance(data, list):
                    return cls(data)
            except json.JSONDecodeError:
                pass
            # fallback: 每行一個 json object
            events = []
            with open(path, "r", encoding="utf-8") as fh:
                for ln in fh:
                    ln = ln.strip()
                    if not ln:
                        continue
                    try:
                        obj = json.loads(ln.rstrip(","))
                        events.append(obj)
                    except Exception:
                        # skip invalid lines
                        continue
            return cls(events)

    def filter(
        self,
        name: Optional[str] = None,
        cat: Optional[str] = None,
        pid: Optional[int] = None,
        tid: Optional[int] = None,
        ph: Optional[str] = None,
        tmin: Optional[float] = None,
        tmax: Optional[float] = None,
    ) -> "TraceParser":
        """回傳新的 TraceParser，包含符合條件的 events。"""
        def ok(ev):
            if name is not None and ev.get("name") != name:
                return False
            if cat is not None and ev.get("cat") != cat:
                return False
            if pid is not None and ev.get("pid") != pid:
                return False
            if tid is not None and ev.get("tid") != tid:
                return False
            if ph is not None and ev.get("ph") != ph:
                return False
            ts = ev.get("ts")
            if ts is not None:
                try:
                    t = float(ts)
                    if tmin is not None and t < tmin:
                        return False
                    if tmax is not None and t > tmax:
                        return False
                except Exception:
                    pass
            return True

        return TraceParser([e for e in self.events if ok(e)])

    def count_by(self, key: str = "name") -> Dict[str, int]:
        """根據 key (例如 'name' or 'cat') 計數並回傳 dict。"""
        c = Counter()
        for e in self.events:
            c[e.get(key, "<none>")] += 1
        return dict(c)

    def summary(self) -> Dict[str, Any]:
        """回傳簡單摘要（事件總數、時間範圍、不同 name/cat 數量）。"""
        total = len(self.events)
        names = set()
        cats = set()
        times = [float(e.get("ts")) for e in self.events if e.get("ts") is not None]
        for e in self.events:
            if "name" in e:
                names.add(e["name"])
            if "cat" in e:
                cats.add(e["cat"])
        return {
            "total_events": total,
            "unique_names": len(names),
            "unique_cats": len(cats),
            "time_min": min(times) if times else None,
            "time_max": max(times) if times else None,
        }

    def durations(self) -> Dict[Tuple[int, int, str], List[float]]:
        """
        計算以 B/E 形式標記事件的 durations。
        返回 dict: (pid, tid, name) -> [duration_seconds]
        若遇到 unmatched E/B 則跳過。
        """
        stacks = defaultdict(list)  # key -> list of start timestamps
        results = defaultdict(list)
        for e in self.events:
            ph = e.get("ph")
            name = e.get("name")
            pid = e.get("pid", -1)
            tid = e.get("tid", -1)
            ts = e.get("ts")
            if ts is None:
                continue
            try:
                t = float(ts)
            except Exception:
                continue
            key = (pid, tid, name)
            if ph == "B":
                stacks[key].append(t)
            elif ph == "E":
                if stacks[key]:
                    start = stacks[key].pop()
                    results[key].append(t - start)
            else:
                # support 'X' full events with dur field
                if ph == "X":
                    dur = e.get("dur")
                    if dur is not None:
                        try:
                            results[key].append(float(dur))
                        except Exception:
                            pass
        return {k: v for k, v in results.items()}

    def durations_by_name(self) -> Dict[str, Dict[str, float]]:
        """Aggregate durations by event name.

        Returns a dict mapping name -> {"total": float, "count": int, "avg": float, "pct": float}
        where "pct" is the fraction of total duration occupied by this name (0..1).
        """
        raw = self.durations()
        agg = defaultdict(list)
        for (_pid, _tid, name), arr in raw.items():
            if name is None:
                continue
            agg[name].extend(arr)

        total_all = sum(sum(v) for v in agg.values())
        out: Dict[str, Dict[str, float]] = {}
        for name, arr in agg.items():
            s = sum(arr)
            cnt = len(arr)
            out[name] = {
                "total": float(s),
                "count": int(cnt),
                "avg": float(s / cnt) if cnt else 0.0,
                "pct": float(s / total_all) if total_all > 0 else 0.0,
            }
        return out

    def save(self, path: str) -> None:
        """將目前 events 以 JSON array 寫出（pretty）。"""
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.events, f, indent=2, ensure_ascii=False)
