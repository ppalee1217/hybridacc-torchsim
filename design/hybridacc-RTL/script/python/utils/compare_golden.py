#!/usr/bin/env python3
"""compare_golden.py — Compare ESL golden log with RTL simulation log.

Usage:
    python3 compare_golden.py golden_noc.log rtl_noc.log
    python3 compare_golden.py --field-level golden_noc.log rtl_noc.log

Log format (one event per line, field-separated):
    CYCLE <cycle> <channel> <direction> addr=<hex> data=<hex> mask=<hex> status=<status>

The script reports field-level mismatches and summary statistics.
"""
import argparse
import re
import sys
from pathlib import Path


EVENT_RE = re.compile(
    r"CYCLE\s+(\d+)\s+(\w+)\s+(\w+)"
    r"(?:\s+addr=0x([0-9a-fA-F]+))?"
    r"(?:\s+data=0x([0-9a-fA-F]+))?"
    r"(?:\s+mask=0x([0-9a-fA-F]+))?"
    r"(?:\s+status=(\w+))?"
)


def parse_events(path: Path) -> list[dict]:
    events = []
    for line in path.read_text().splitlines():
        m = EVENT_RE.search(line)
        if m:
            events.append({
                "cycle": int(m.group(1)),
                "channel": m.group(2),
                "direction": m.group(3),
                "addr": m.group(4) or "",
                "data": m.group(5) or "",
                "mask": m.group(6) or "",
                "status": m.group(7) or "",
                "raw": line.strip(),
            })
    return events


def compare(golden: list[dict], rtl: list[dict], field_level: bool) -> int:
    mismatches = 0
    max_len = max(len(golden), len(rtl))
    for i in range(max_len):
        g = golden[i] if i < len(golden) else None
        r = rtl[i] if i < len(rtl) else None

        if g is None:
            print(f"[EXTRA-RTL] event #{i}: {r['raw']}")
            mismatches += 1
            continue
        if r is None:
            print(f"[MISSING-RTL] event #{i}: {g['raw']}")
            mismatches += 1
            continue

        if field_level:
            for key in ("cycle", "channel", "direction", "addr", "data", "mask", "status"):
                if g[key] != r[key]:
                    print(f"[MISMATCH] event #{i} field={key}: golden={g[key]} rtl={r[key]}")
                    mismatches += 1
        else:
            if g["raw"] != r["raw"]:
                print(f"[DIFF] event #{i}:")
                print(f"  golden: {g['raw']}")
                print(f"  rtl:    {r['raw']}")
                mismatches += 1

    return mismatches


def main():
    parser = argparse.ArgumentParser(description="Compare ESL golden vs RTL simulation logs.")
    parser.add_argument("golden", type=Path, help="Golden (ESL) log file")
    parser.add_argument("rtl", type=Path, help="RTL simulation log file")
    parser.add_argument("--field-level", action="store_true", help="Compare per-field")
    args = parser.parse_args()

    golden_events = parse_events(args.golden)
    rtl_events = parse_events(args.rtl)

    print(f"Golden events: {len(golden_events)}")
    print(f"RTL events:    {len(rtl_events)}")

    n = compare(golden_events, rtl_events, args.field_level)
    print(f"\n{'='*60}")
    if n == 0:
        print("RESULT: MATCH — all events identical")
    else:
        print(f"RESULT: {n} mismatch(es) found")
        print("Priority checklist:")
        print("  1. Handshake (valid/ready) race")
        print("  2. Reset timing / uninitialized registers")
        print("  3. Bit packing / lane ordering (64-bit packing)")
        print("  4. addr & mask calculation (MBUS calc_mask, rx_mask)")
        print("  5. Scan-chain shift order")
        print("  6. FIFO depth / overflow behavior")
        print("  7. Endianness (LSB/MSB)")
    sys.exit(0 if n == 0 else 1)


if __name__ == "__main__":
    main()
