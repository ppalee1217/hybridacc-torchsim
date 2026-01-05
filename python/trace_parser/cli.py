"""簡易 command-line 介面"""
import argparse
import json
from .parser import TraceParser
from .utils import top_n_counts, durations_summary


def parse_args():
    p = argparse.ArgumentParser(description="Trace parser - filter & stats")
    p.add_argument("trace", help="trace json file")
    p.add_argument("-d", "--dir", dest="trace_dir", help="(unused placeholder)", default=None)
    p.add_argument("-n", "--name", help="filter by name")
    p.add_argument("-c", "--cat", help="filter by category")
    p.add_argument("--pid", type=int, help="filter by pid")
    p.add_argument("--tid", type=int, help="filter by tid")
    p.add_argument("--tmin", type=float, help="min timestamp (inclusive)")
    p.add_argument("--tmax", type=float, help="max timestamp (inclusive)")
    p.add_argument("-o", "--out", help="save filtered events to file")
    p.add_argument("--top", type=int, default=10, help="top N for counts")
    p.add_argument("--stats", choices=["count", "dur", "summary", "all"], default="all")
    p.add_argument("--dur-by-name", action="store_true", help="aggregate durations by event name and show totals/pct")
    return p.parse_args()


def main():
    args = parse_args()
    tp = TraceParser.load(args.trace)
    if any([args.name, args.cat, args.pid is not None, args.tid is not None, args.tmin is not None, args.tmax is not None]):
        tp = tp.filter(name=args.name, cat=args.cat, pid=args.pid, tid=args.tid, tmin=args.tmin, tmax=args.tmax)
    if args.out:
        tp.save(args.out)
        print(f"Filtered saved to {args.out}")

    if args.stats in ("count", "all"):
        counts = tp.count_by("name")
        print("Top names:")
        for k, v in top_n_counts(counts, args.top):
            print(f"  {k}: {v}")

    if args.stats in ("summary", "all"):
        s = tp.summary()
        print("Summary:")
        print(json.dumps(s, indent=2))

    if args.stats in ("dur", "all"):
        if args.dur_by_name:
            dby = tp.durations_by_name()
            # sort by total desc
            items = sorted(dby.items(), key=lambda x: x[1].get("total", 0.0), reverse=True)
            print("Durations by name (total / count / avg / pct):")
            for name, stat in items:
                print(f"  {name}: total={stat['total']:.6f}s, count={stat['count']}, avg={stat['avg']:.6f}s, pct={stat['pct']*100:.2f}%")
        else:
            d = tp.durations()
            ds = durations_summary(d)
            print("Durations summary (key -> count/avg/min/max):")
            for k, v in ds.items():
                print(f"  {k}: {v}")


if __name__ == "__main__":
    main()
