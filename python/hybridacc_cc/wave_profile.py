"""Raw per-wave profile from an ESL run (M21; F2 measurement contract, 2026-10-18 token ruling).

    python -m hybridacc_cc.wave_profile CC_DIR IR.json WAVE_LOG -o PROFILE.json [--label TEXT]
        [--schema v2 --simulator-sha256 HEX --platform PLATFORM.json [--from-v1 PROFILE.json]] [--spm-log SPM_LOG]

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

Schema v2 (M21 WP-I, 2026-10-21 ruling): the same rows, plus each wave's tile coordinates (the IR's
``covers``), the units, dtype and config fingerprint, and a ``provenance`` block that binds by sha256 the IR,
the cc wave map (``wave_map.json``), ``hardware_ir.json``, the firmware ELF, the ESL binary, the platform
file, cc's ``toolchain_provenance.json`` and this producer.  ``--from-v1`` rebuilds a sealed v1 profile's
rows from the same wave log, requires them to be identical, and records the v1 file's sha256.

SPM claims (M22 condition 1b, ruling A of 2026-10-22): with ``--spm-log`` (the ESL's --spm-log of the same run)
each wave row also gets ``spm_claims``, the cycles in which the wave's NoC ports held SPM resources that a DMA
request waits for in the ESL's bank arbiter (ScratchpadMemory.hpp comb_bank_req_arb: NoC ports before DMA
writes before DMA reads): the bank a port accessed (every bank of the group for a parallel-view access) and,
for a NoC read, the group's read-metadata slot. Rows are run-length [cycle after START, cycles, bank mask,
read-group mask]; cycles with no claim are left out. Every NoC access must fall after the START of the wave it
follows and no later than that wave's DONE; one cluster only. A TLS uses the claims only to delay DMA outside
the wave; the wave's own time stays its measured span.
"""
import argparse
import hashlib
import json
from pathlib import Path

from .schedule_ir import RegisterMap, ScheduleIRError, _require

SCHEMA = "hybridacc-wave-profile/v1"
SCHEMA_V2 = "hybridacc-wave-profile/v2"
UNITS = {"tokens": "cycles of clock_mhz, poll success edge minus the wave's START edge",
         "observed": "picoseconds of simulated time; *_cycles fields in cycles of clock_mhz"}
SPM_CLAIMS = {"source": "ESL --spm-log of the same run", "row": "[cycle after START, cycles, bank mask, read-group mask]",
              "bank_bit": "group * spm_banks_per_group + bank", "read_group_bit": "group"}
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


def spm_claims(ir: dict, log: dict, spm_log: str, ps_per_cycle: int) -> dict:
    """Per (layer, wave): run-length NoC claims (see the module docstring) from the ESL's --spm-log."""
    hw = ir.get("resource", {}).get("hardware", {})
    banks, depth = hw.get("spm_banks_per_group"), hw.get("spm_bank_depth")
    _require(isinstance(banks, int) and isinstance(depth, int) and banks > 0 and depth > 0,
             "IR resource lacks spm_banks_per_group / spm_bank_depth")
    _require(hw.get("num_clusters") == 1, "SPM claims are recorded for one cluster only")
    irw = [(L["index"], w["id"]) for L in ir["layers"] for w in L["execution_waves"]]
    starts = [int(s["t"]) for s in log["S"]]
    dones = [int(e["done"]) if e.get("done") not in (None, "-") else None for e in log["E"]]
    per_cycle = [dict() for _ in starts]  # wave index -> {cycle after START: [bank mask, read-group mask]}
    i = -1
    for n, line in enumerate(spm_log.splitlines(), 1):
        f = line.split()
        if not f or f[0] != "P":
            continue
        _require(len(f) == 7, f"spm log line {n}: a P record has 7 fields")
        cluster, group, wen, laddr, t = int(f[1]), int(f[3]), f[4], int(f[5]), int(f[6])
        _require(cluster == 0, f"spm log line {n}: cluster {cluster}")
        while i + 1 < len(starts) and starts[i + 1] <= t:
            i += 1
        _require(i >= 0 and t > starts[i], f"spm log line {n}: NoC access at {t} ps before or at a wave's START")
        _require(dones[i] is not None and t <= dones[i], f"spm log line {n}: NoC access at {t} ps after its wave's DONE")
        _require((t - starts[i]) % ps_per_cycle == 0, f"spm log line {n}: not on a clock edge")
        m = per_cycle[i].setdefault((t - starts[i]) // ps_per_cycle, [0, 0])
        if laddr >= banks * depth:  # parallel view: the whole group
            m[0] |= ((1 << banks) - 1) << (group * banks)
        else:
            m[0] |= 1 << (group * banks + laddr // depth)
        if wen == "0":
            m[1] |= 1 << group
    out = {}
    for key, cycles in zip(irw, per_cycle):
        runs = []
        for c in sorted(cycles):
            b, g = cycles[c]
            if runs and runs[-1][0] + runs[-1][1] == c and runs[-1][2:] == [b, g]:
                runs[-1][1] += 1
            else:
                runs.append([c, 1, b, g])
        out[key] = runs
    return out


def build(cc_dir: Path, ir: dict, ir_sha256: str, wave_log: str, label: str, spm_log: str = None) -> dict:
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
    if spm_log is not None:
        claims = spm_claims(ir, log, spm_log, ps_per_cycle)
        for key, runs in claims.items():
            _require(key in rows or not runs, f"layer {key[0]} wave {key[1]} has SPM claims but no profile row")
            if key in rows:
                rows[key]["spm_claims"] = runs
    out = {"schema": SCHEMA, "kind": "raw", "label": label, "ir_sha256": ir_sha256, "clock_mhz": clock_mhz,
           "note": "raw ESL observation; includes waits a TLS models itself; TLS runs on it are not qualified",
           "waves": [rows[k] for k in sorted(rows)]}
    if spm_log is not None:
        out["spm_claims"] = SPM_CLAIMS
    return out


def _sha256_file(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def to_v2(prof: dict, cc_dir: Path, ir: dict, simulator_sha256: str, platform: Path,
          from_v1: Path = None) -> dict:
    """Schema v2 of a v1 profile built by ``build`` from the same IR (see the module docstring)."""
    _require(isinstance(simulator_sha256, str) and len(simulator_sha256) == 64
             and all(ch in "0123456789abcdef" for ch in simulator_sha256), "simulator sha256 must be 64 hex digits")
    wave_map = json.loads((cc_dir / "wave_map.json").read_text())
    hw_sha = _sha256_file(cc_dir / "hardware_ir.json")
    _require(wave_map.get("hardware_ir_sha256") == hw_sha, "wave_map.json was made for another hardware_ir.json")
    _require(ir["provenance"]["files_sha256"]["hardware_ir.json"] == hw_sha,
             "the IR was built from another hardware_ir.json")
    _require(ir["provenance"]["platform"]["sha256"] == _sha256_file(platform),
             "the IR was built with another platform file")
    dtypes = {layer["dtype"] for layer in wave_map["layers"]}
    _require(len(dtypes) == 1, f"layers have different dtypes {sorted(dtypes)}")
    covers = {(L["index"], w["id"]): w["covers"] for L in ir["layers"] for w in L["execution_waves"]}
    waves = []
    for row in prof["waves"]:
        key = (row["layer"], row["wave"])
        waves.append({"layer": row["layer"], "wave": row["wave"], "covers": covers[key],
                      "tokens": row["tokens"], "observed": row["observed"]})
        if "spm_claims" in row:
            waves[-1]["spm_claims"] = row["spm_claims"]
    out = {"schema": SCHEMA_V2, "kind": prof["kind"], "label": prof["label"], "ir_sha256": prof["ir_sha256"],
           "clock_mhz": prof["clock_mhz"], "units": UNITS, "dtype": dtypes.pop(),
           "config_fingerprint": ir["provenance"]["config_fingerprint"]["hash"], "note": prof["note"],
           "provenance": {
               "ir_sha256": prof["ir_sha256"],
               "wave_map_sha256": _sha256_file(cc_dir / "wave_map.json"),
               "hardware_ir_sha256": hw_sha,
               "firmware_elf_sha256": _sha256_file(cc_dir / "firmware.elf"),
               "simulator_sha256": simulator_sha256,
               "platform_sha256": _sha256_file(platform),
               "toolchain_provenance_sha256": _sha256_file(cc_dir / "toolchain_provenance.json"),
               "producer_sha256": _sha256_file(Path(__file__)),
           },
           "waves": waves}
    if "spm_claims" in prof:
        out["spm_claims"] = prof["spm_claims"]
    if from_v1 is not None:
        old = json.loads(Path(from_v1).read_text())
        _require(old.get("schema") == SCHEMA, f"{from_v1} is not a {SCHEMA} profile")
        _require(old == prof, f"{from_v1} differs from the v1 rows rebuilt from the same wave log")
        out["derived_from"] = {"profile_sha256": _sha256_file(from_v1), "schema": SCHEMA}
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cc_dir", type=Path)
    ap.add_argument("ir", type=Path)
    ap.add_argument("wave_log", type=Path)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--label", default="raw ESL per-wave profile (M21)")
    ap.add_argument("--schema", choices=("v1", "v2"), default="v1")
    ap.add_argument("--simulator-sha256", help="v2: sha256 of the ESL binary that produced the wave log")
    ap.add_argument("--platform", type=Path, help="v2: the platform file the IR was built with")
    ap.add_argument("--from-v1", type=Path, help="v2: a v1 profile of the same run; its rows must match")
    ap.add_argument("--spm-log", type=Path, help="the ESL's --spm-log of the same run: add each wave's SPM claims")
    a = ap.parse_args(argv)
    ir_bytes = a.ir.read_bytes()
    prof = build(a.cc_dir, json.loads(ir_bytes), hashlib.sha256(ir_bytes).hexdigest(),
                 a.wave_log.read_text(), a.label, None if a.spm_log is None else a.spm_log.read_text())
    if a.spm_log is not None:
        prof["spm_claims"] = dict(prof["spm_claims"], spm_log_sha256=_sha256_file(a.spm_log))
    if a.schema == "v2":
        _require(a.simulator_sha256 and a.platform, "--schema v2 needs --simulator-sha256 and --platform")
        prof = to_v2(prof, a.cc_dir, json.loads(ir_bytes), a.simulator_sha256, a.platform, a.from_v1)
    else:
        _require(not (a.simulator_sha256 or a.platform or a.from_v1), "provenance options need --schema v2")
    a.output.write_text(json.dumps(prof, indent=1) + "\n")
    print(json.dumps({"out": str(a.output), "waves": len(prof["waves"])}))


if __name__ == "__main__":
    main()
