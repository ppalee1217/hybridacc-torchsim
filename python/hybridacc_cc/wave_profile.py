"""Raw per-wave profile from an ESL run (M21; F2 measurement contract, 2026-10-18 token ruling).

    python -m hybridacc_cc.wave_profile CC_DIR IR.json WAVE_LOG -o PROFILE.json [--label TEXT]

Inputs: the cc output directory (its firmware_hw.h gives the register map), the Common Schedule IR built
from it, and the ESL's --wave-log of a run of the same firmware.

Every MMIO read the firmware makes is inside a wait helper (the IR decoder refuses a recording with any
other read). Walking the IR's waits in order, each wait consumes the ESL's reads until the read that
satisfied it: the status register and condition its helper polls (all four AGU STATUS reads of one
iteration for agu_idle; once per cluster for cluster waits). Reads of TIMER_MTIME_LO (the WFI tick) and,
inside hddu_done, the debug probe's registers are allowed in between; anything else, a wait that never
succeeds, or reads left over after the last wait stop the producer.

Output: schema hybridacc-wave-profile/v1 with, per (layer, wave) the IR's waits need, the token cycles
(the edge the firmware's poll for that wait succeeded, minus the wave's START edge; 500 MHz cycles). It
also records, per token, the START, the poll success and the edge the hardware condition first held
(hddu_done: DONE set; agu_idle: AGUs idle; pe_halted / cluster_quiesced: the condition's last rise
before the poll succeeded). These are raw observations (kind "raw"): they include waits for SPM, NoC
and DMA that a TLS models itself, so a TLS run on them is not qualified (2026-10-18 ruling).
"""
import argparse
import hashlib
import json
from pathlib import Path

from .schedule_ir import RegisterMap, ScheduleIRError, _require

SCHEMA = "hybridacc-wave-profile/v1"
TOKEN_WAITS = ("hddu_done", "agu_idle", "pe_halted", "cluster_quiesced")
CLUSTER_WAITS = ("hddu_done", "agu_idle", "pe_halted", "noc_quiesced", "cluster_idle", "cluster_quiesced")
# hddu_wait_debug_probe(): read after a status read with QUIESCED set on every 256th spin.
PROBE = ("HDDU_ERR_CODE", "HDDU_ERR_INFO0", "HDDU_ERR_INFO1", "HDDU_COUNTER_STALL",
         "ps.STATUS", "pd.STATUS", "pli.STATUS", "plo.STATUS")


def parse_wave_log(text: str) -> dict:
    out = {"S": [], "E": [], "R": [], "C": [], "M": []}
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        kind, *fields = line.split()
        _require(kind in "TSREMC" and len(kind) == 1, f"wave log line {n}: unknown record {kind!r}")
        rec = dict(f.split("=", 1) for f in fields)
        if kind in out:
            out[kind].append(rec)
    return out


class Reads:
    """The ESL's MMIO reads in order, with the register each one hit."""

    def __init__(self, records, regmap):
        self.rows = []
        for r in records:
            addr, data, t = int(r["addr"]), int(r["data"]), int(r["t"])
            space, target, reg = regmap.classify(addr)
            self.rows.append((space, target, reg, data, t))
        self.pos = 0

    def next(self, what):
        _require(self.pos < len(self.rows), f"{what}: the ESL reads ended before the wait succeeded")
        self.pos += 1
        return self.rows[self.pos - 1]

    def take(self, accept, allowed, what):
        """Consume reads until accept(row) is True; return that row. Rows allowed(row) may come first."""
        while self.pos < len(self.rows):
            row = self.rows[self.pos]
            self.pos += 1
            if accept(row):
                return row
            _require(allowed(row), f"{what}: unexpected read of {row[0]} {row[2]} (cluster {row[1]}) at {row[4]} ps")
        raise ScheduleIRError(f"{what}: the ESL reads ended before the wait succeeded")


def poll_success(wait, reads, d):
    """Consume the reads of one wait; return (success time in ps, error seen)."""
    kind, arg = wait["kind"], wait["arg"]
    what = f"wait {wait['id']} ({kind})"
    timer = lambda r: r[0] == "timer" and r[2] == "MTIME_LO"
    if kind == "dma_idle":
        row = reads.take(lambda r: r[0] == "dma" and r[2] == "STATUS" and r[3] & d["DMA_STATUS_IDLE"],
                         lambda r: r[0] == "dma" and r[2] == "STATUS", what)
        return row[4], False
    if kind == "dma_tag":
        def ok(r):
            if not (r[0] == "dma" and r[2] == "DONE_TAG"):
                return False
            diff = (r[3] - arg) & 0xFFFFFFFF
            return diff < 0x80000000
        row = reads.take(ok, lambda r: r[0] == "dma" and r[2] == "DONE_TAG", what)
        return row[4], False
    _require(kind in CLUSTER_WAITS, f"{what}: unknown wait kind")
    t_last, err = None, False
    for c in range(arg):
        mine = lambda r, reg: r[0] in ("cluster", "agu") and r[1] == c and r[2] == reg
        if kind == "agu_idle":
            planes = ("ps", "pd", "pli", "plo")
            while True:
                vals = []
                for p in planes:
                    row = reads.take(lambda r, p=p: mine(r, f"{p}.STATUS"), timer, what)
                    vals.append(row)
                if all((r[3] & 0x2) == 0 for r in vals):
                    t_last = vals[-1][4]
                    break
            continue
        if kind == "hddu_done":  # wait_all_clusters_hddu_done(), read by read
            spin = 0
            while True:
                row = reads.next(what)
                if timer(row):
                    continue
                _require(mine(row, "HDDU_STATUS"), f"{what}: unexpected read of {row[0]} {row[2]} at {row[4]} ps")
                v = row[3]
                if v & d["HDDU_STATUS_QUIESCED"] and spin % 256 == 0:
                    for reg in PROBE:
                        r2 = reads.next(what)
                        _require(mine(r2, reg), f"{what}: debug probe read {r2[2]} where {reg} was expected")
                if v & d["HDDU_STATUS_ERROR"]:
                    err = True
                if v & (d["HDDU_STATUS_ERROR"] | d["HDDU_STATUS_DONE"]):
                    t_last = row[4]
                    break
                spin += 1
            continue
        reg, cond = {
            "pe_halted": ("NOC_STATUS", lambda v: v & d["NOC_STATUS_ALL_ACTIVE_PES_HALTED"]),
            "noc_quiesced": ("NOC_STATUS", lambda v: (v & d["NOC_STATUS_ALL_ACTIVE_PES_HALTED"]) and not
                             (v & (d["NOC_STATUS_ANY_ROUTER_PENDING_RESP"] | d["NOC_STATUS_ANY_ROUTER_FIFO_NONEMPTY"]))),
            "cluster_idle": ("CLUSTER_STATUS", lambda v: v & (d["CLUSTER_STATUS_IDLE"] | d["CLUSTER_STATUS_ERROR"])),
            "cluster_quiesced": ("CLUSTER_STATUS", lambda v: v & (d["CLUSTER_STATUS_QUIESCED"] | d["CLUSTER_STATUS_ERROR"])),
        }[kind]
        row = reads.take(lambda r: mine(r, reg) and cond(r[3]), lambda r: mine(r, reg) or timer(r), what)
        if kind in ("cluster_idle", "cluster_quiesced") and row[3] & d["CLUSTER_STATUS_ERROR"]:
            err = True
        t_last = row[4]
    _require(t_last is not None, f"{what}: no cluster to wait for")
    return t_last, err


def last_rise(changes, cond, t):
    """The last edge at or before t at which the condition became 1 (None if it never did)."""
    best = None
    for c in changes:
        if c["cond"] == cond and int(c["t"]) <= t and c["value"] == "1":
            best = int(c["t"])
    return best


def build(cc_dir: Path, ir: dict, ir_sha256: str, wave_log: str, label: str) -> dict:
    regmap = RegisterMap((cc_dir / "firmware_hw.h").read_text())
    d = regmap.d
    clock_mhz = ir.get("resource", {}).get("platform", {}).get("clock_mhz")
    _require(isinstance(clock_mhz, int) and clock_mhz > 0, "IR platform lacks clock_mhz")
    ps_per_cycle = 1_000_000 // clock_mhz
    log = parse_wave_log(wave_log)
    _require(not log["R"], f"{len(log['R'])} START writes outside the measurement contract")
    starts = [int(s["t"]) for s in log["S"]]
    ends = log["E"]
    irw = [(L["index"], w["id"]) for L in ir["layers"] for w in L["execution_waves"]]
    _require(len(starts) == len(irw) == len(ends),
             f"{len(starts)} accepted STARTs and {len(ends)} wave records for {len(irw)} IR execution waves")
    start_of = dict(zip(irw, starts))
    rec_of = dict(zip(irw, ends))
    reads = Reads(log["M"], regmap)
    rows = {}
    for L in ir["layers"]:
        li, latest = L["index"], None
        for ev in L["events"]:
            if ev["type"] == "hddu_start":
                latest = ev["wave"]
                continue
            if ev["type"] != "wait":
                continue
            wait = L["waits"][ev["wait"]]
            t, err = poll_success(wait, reads, d)
            _require(not err, f"layer {li} wait {wait['id']} ({wait['kind']}) saw an error status")
            if wait["kind"] not in TOKEN_WAITS:
                continue
            _require(latest is not None, f"layer {li} wait {wait['id']}: {wait['kind']} before any HDDU start")
            key = (li, latest)
            s = start_of[key]
            e = rec_of[key]
            pred = {"hddu_done": e.get("done"), "agu_idle": e.get("agu_idle")}.get(wait["kind"])
            pred = int(pred) if pred not in (None, "-") else last_rise(log["C"], wait["kind"], t)
            row = rows.setdefault(key, {"layer": li, "wave": latest, "tokens": {}, "observed": {}})
            _require(wait["kind"] not in row["tokens"],
                     f"layer {li} wave {latest}: two {wait['kind']} waits after one START")
            _require(t > s and (t - s) % ps_per_cycle == 0, f"layer {li} wave {latest}: poll edge not after START")
            row["tokens"][wait["kind"]] = (t - s) // ps_per_cycle
            row["observed"][wait["kind"]] = {
                "start_ps": s, "poll_success_ps": t,
                "condition_ps": pred,
                "condition_minus_start_cycles": None if pred is None else (pred - s) // ps_per_cycle}
    _require(reads.pos == len(reads.rows), f"{len(reads.rows) - reads.pos} ESL reads after the last wait")
    return {"schema": SCHEMA, "kind": "raw", "label": label, "ir_sha256": ir_sha256, "clock_mhz": clock_mhz,
            "note": "raw ESL observation; includes waits a TLS models itself; TLS runs on it are not qualified",
            "waves": [rows[k] for k in sorted(rows)]}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cc_dir", type=Path)
    ap.add_argument("ir", type=Path)
    ap.add_argument("wave_log", type=Path)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--label", default="raw ESL per-wave profile (M21)")
    a = ap.parse_args(argv)
    ir_bytes = a.ir.read_bytes()
    prof = build(a.cc_dir, json.loads(ir_bytes), hashlib.sha256(ir_bytes).hexdigest(),
                 a.wave_log.read_text(), a.label)
    a.output.write_text(json.dumps(prof, indent=1) + "\n")
    print(json.dumps({"out": str(a.output), "waves": len(prof["waves"])}))


if __name__ == "__main__":
    main()
