"""Raw firmware issue timing from an ESL run (M21; Review C-05 (b), timing contracts v1 of 2026-10-19 and v2 of 2026-10-21).

    python -m hybridacc_cc.issue_timing CC_DIR IR.json --wave-log W --mmio-log M --dram-log D --contract v2 -o TIMING.json

Inputs: the cc output directory (firmware_hw.h gives the register map), the Common Schedule IR built from it, and
the ESL's --wave-log, --mmio-log and --dram-log of one run of the same firmware (the dram log must carry the
"D" completion lines).

Every IR event is matched to the ESL observation that produced it, in order: a write event (dma_submit,
hddu_start, hddu_ctrl, noc_command, spm_config_update, cluster_*) to the MMIO write that triggers it, and a wait to
the reads its firmware helper polled (walked as wave_profile does). Per event, with the contract's edges:
  issue edge I   writes: the triggering MMIO request edge; hddu_start: the HDDU's accepted START (S);
                 waits: the request edge of the first read of the wait (not counting timer reads);
  release edge R writes: I; DMA waits (dma_tag, dma_idle): max(I, H), H being the completion edge ("D" line)
                 that makes the condition true; HDDU waits (hddu_done, agu_idle, pe_halted, cluster_quiesced):
                 v1: P, the edge the successful poll's response is observed (the raw token's end);
                 v2: max(I, H), H being the edge the hardware condition held, as for DMA waits;
  issue_cycles   I - R of the previous event (0 for the first event, which is the origin);
  reaction       P - R, the polling reaction (v1: DMA waits only; v2: every wait), which TLS gets inside the next
                 event's issue cycles (``work_cycles`` = issue_cycles minus the previous wait's reaction).
v2 implements the researcher's 2026-10-21 composition ruling A (agent_run/261021-m22-composition/decision.md): a TLS
times an HDDU wave by its hardware span (condition - START) and the firmware's polling by the issue model, so the
wave's release must not include the poll. It pairs with hardware-span wave tokens; v1 pairs with raw (poll) tokens.
Pairing a contract with the other token kind counts the poll twice or not at all.
Also recorded: the boot prefix (simulation start to the first event), the terminal tail (last release to the end
of simulation), each hddu_start's and dma_submit's accept latency (request to S / to the engine taking the
command), and, for HDDU waits, the edge the hardware condition held. Times are 500 MHz cycles of simulation time
(not the testbench's EBREAK counter). Anything the IR does not explain (an extra or missing write, a read outside a
wait, a wait that never succeeds, reads left over, a DMA completion out of order) stops the producer.

Output: schema hybridacc-issue-timing/v1 (contract v1) or /v2 (contract v2), kind "raw": valid only for the same IR,
firmware, binary, config and clock; a TLS run on it is a raw replay, not qualified (2026-10-18 token ruling).
"""
import argparse
import hashlib
import json
from pathlib import Path

from .schedule_ir import RegisterMap, ScheduleIRError, _require
from .wave_profile import CLUSTER_WAITS, Reads, last_rise, parse_wave_log, poll_success

CONTRACTS = {  # contract -> (schema, contract name)
    "v1": ("hybridacc-issue-timing/v1", "c05-v1 (2026-10-19)"),
    "v2": ("hybridacc-issue-timing/v2", "c05-v2 (2026-10-21)"),
}
WRITE_EVENTS = ("dma_submit", "hddu_start", "hddu_ctrl", "noc_command", "spm_config_update",
                "cluster_start", "cluster_stop", "cluster_soft_reset")
HDDU_WAITS = ("hddu_done", "agu_idle", "pe_halted", "cluster_quiesced")
DMA_WAITS = ("dma_tag", "dma_idle")


def write_event(rm: RegisterMap, addr: int, val: int):
    """The IR event type a firmware MMIO write produces (as schedule_ir.decode does), or None."""
    d = rm.d
    space, _target, reg = rm.classify(addr)
    if space == "dma" and reg == "CTRL" and val & d["DMA_CTRL_SUBMIT"]:
        return "dma_submit"
    if space != "cluster":
        return None
    if reg == "HDDU_CTRL":
        return "hddu_start" if val & d["HDDU_CTRL_START"] else "hddu_ctrl"
    if reg == "CLUSTER_CTRL":
        names = {d["CLUSTER_CTRL_START"]: "start", d["CLUSTER_CTRL_STOP"]: "stop",
                 d["CLUSTER_CTRL_SOFT_RESET"]: "soft_reset"}
        return "cluster_" + names[val] if val in names else None
    if reg == "NOC_CMD":
        return "noc_command"
    if reg == "SPM_CONFIG_UPDATE":
        return "spm_config_update"
    return None


def parse_mmio_log(text: str) -> list:
    rows = []
    for n, line in enumerate(text.splitlines(), 1):
        f = line.split()
        _require(len(f) == 5 and f[0] in ("R", "W"), f"mmio log line {n}: malformed")
        rows.append((f[0], int(f[1]), int(f[2]), int(f[4])))
    return rows


def parse_dram_log(text: str) -> tuple:
    starts, done = [], []
    for n, line in enumerate(text.splitlines(), 1):
        f = line.split()
        if f[0] == "C":
            starts.append((int(f[1]), int(f[5])))
        elif f[0] == "D":
            _require(len(f) == 4, f"dram log line {n}: malformed D line")
            _require(f[2] == "0", f"dram log line {n}: DMA command {f[1]} ended in error")
            done.append((int(f[1]), int(f[3])))
    return starts, done


def build(cc_dir: Path, ir: dict, ir_sha256: str, wave_log: str, mmio_log: str, dram_log: str, label: str, *,
          contract: str) -> dict:
    _require(contract in CONTRACTS, f"unknown issue-timing contract {contract!r}")
    rm = RegisterMap((cc_dir / "firmware_hw.h").read_text())
    d = rm.d
    clock_mhz = ir.get("resource", {}).get("platform", {}).get("clock_mhz")
    _require(isinstance(clock_mhz, int) and clock_mhz > 0, "IR platform lacks clock_mhz")
    ps = 1_000_000 // clock_mhz
    wl = parse_wave_log(wave_log)
    _require(not wl["R"], f"{len(wl['R'])} START writes outside the measurement contract")
    mm = parse_mmio_log(mmio_log)
    reads_mm = [r for r in mm if r[0] == "R"]
    _require(len(reads_mm) == len(wl["M"]), f"{len(reads_mm)} MMIO reads but {len(wl['M'])} read responses logged")
    for r, m in zip(reads_mm, wl["M"]):
        _require(r[1] == int(m["addr"]), f"read at {r[3]} ps and its logged response disagree on the address")
    req_of_read = [r[3] for r in reads_mm]
    writes = [(addr, val, t, write_event(rm, addr, val)) for k, addr, val, t in mm if k == "W"]
    writes = [w for w in writes if w[3] is not None]
    starts_s = [int(s["t"]) for s in wl["S"]]
    cmd_start, cmd_done = parse_dram_log(dram_log)
    reads = Reads(wl["M"], rm)
    E = wl["E"]
    changes = wl["C"]

    events = []
    wi = si = dsub = 0
    prev_release = None
    prev_reaction = 0
    waves_started = []          # E record index per started wave, global order
    submitted_tags = []
    for layer in ir["layers"]:
        li = layer["index"]
        waits = layer["waits"]
        for ev in layer["events"]:
            kind = ev["type"]
            here = f"layer {li} event {ev['id']} ({kind})"
            row = {"layer": li, "event": ev["id"], "type": kind}
            if kind in WRITE_EVENTS:
                _require(wi < len(writes), f"{here}: the ESL wrote fewer events than the IR lists")
                addr, val, t_req, got = writes[wi]
                wi += 1
                _require(got == kind, f"{here}: the ESL's write at {t_req} ps is a {got}")
                _require(reads.pos >= len(req_of_read) or req_of_read[reads.pos] > t_req,
                         f"{here}: an ESL read at {req_of_read[min(reads.pos, len(req_of_read) - 1)]} ps "
                         "comes before this write but belongs to no wait")
                issue = t_req
                if kind == "hddu_start":
                    _require(si < len(starts_s), f"{here}: no accepted START left in the wave log")
                    issue = starts_s[si]
                    _require(issue >= t_req, f"{here}: START accepted before it was written")
                    row["accept_latency"] = (issue - t_req) // ps
                    waves_started.append(si)
                    si += 1
                elif kind == "dma_submit":
                    cmd = layer["dma"][ev["dma"]]
                    _require(dsub < len(cmd_start) and cmd_start[dsub][0] == cmd["tag"],
                             f"{here}: the DMA engine did not take command tag {cmd['tag']} next")
                    _require(cmd_start[dsub][1] >= t_req, f"{here}: DMA command started before its submit")
                    row["accept_latency"] = (cmd_start[dsub][1] - t_req) // ps
                    submitted_tags.append(cmd["tag"])
                    dsub += 1
                release = issue
            elif kind == "wait":
                w = waits[ev["wait"]]
                _require(w["id"] == ev["id"], f"{here}: wait identity mismatch")
                wkind = w["kind"]
                row["wait_kind"] = wkind
                first = reads.pos
                p_edge, err = poll_success(w, reads, d)
                _require(not err, f"{here}: the hardware reported an error")
                rows_used = range(first, reads.pos)
                polls = [i for i in rows_used if not (reads.rows[i][0] == "timer" and reads.rows[i][2] == "MTIME_LO")]
                _require(polls, f"{here}: no poll read")
                issue = req_of_read[polls[0]]
                row["polls"] = len(polls)
                if wkind in DMA_WAITS:
                    if wkind == "dma_tag":
                        target = layer["dma"][w["target_dma"]]["tag"]
                        _require(target in submitted_tags, f"{here}: waits for tag {target} not yet submitted")
                    else:
                        target = submitted_tags[-1] if submitted_tags else None
                    if target is None:
                        hw = None
                    else:
                        idx = len(submitted_tags) - 1 - submitted_tags[::-1].index(target)
                        _require(idx < len(cmd_done) and cmd_done[idx][0] == target,
                                 f"{here}: no completion logged for DMA tag {target}")
                        hw = cmd_done[idx][1]
                        if wkind == "dma_idle":
                            _require(idx == len(submitted_tags) - 1, f"{here}: dma_idle before the last submit")
                    release = issue if hw is None else max(issue, hw)
                    _require(p_edge >= release, f"{here}: the poll succeeded before the condition held")
                    row["hw_edge"] = None if hw is None else hw
                    row["reaction_cycles"] = (p_edge - release) // ps
                else:
                    _require(wkind in HDDU_WAITS, f"{here}: wait kind {wkind} is not in the contract")
                    if wkind == "hddu_done":
                        g = sum(len(L["execution_waves"]) for L in ir["layers"] if L["index"] < li) + w["target_wave"]
                        hw = int(E[g]["done"])
                    elif wkind == "agu_idle":
                        hw = int(E[waves_started[-1]]["agu_idle"]) if waves_started else None
                    else:
                        hw = last_rise(changes, wkind, p_edge)
                    row["hw_edge"] = hw
                    if contract == "v1":
                        release = p_edge
                    else:
                        _require(hw is not None, f"{here}: no edge where the hardware condition held")
                        release = max(issue, hw)
                        _require(p_edge >= release, f"{here}: the poll succeeded before the condition held")
                        row["reaction_cycles"] = (p_edge - release) // ps
                _require(release >= issue, f"{here}: released before it was issued")
            else:
                raise ScheduleIRError(f"{here}: event type not in the contract")
            row["issue_edge"], row["release_edge"] = issue, release
            if prev_release is None:
                row["issue_cycles"] = 0
                row["work_cycles"] = 0
                origin = issue
            else:
                _require(issue >= prev_release, f"{here}: issued before the previous event released")
                row["issue_cycles"] = (issue - prev_release) // ps
                row["work_cycles"] = row["issue_cycles"] - prev_reaction
            prev_reaction = row.get("reaction_cycles", 0)
            prev_release = release
            events.append(row)
    _require(wi == len(writes), f"{len(writes) - wi} event writes in the ESL beyond the IR's")
    _require(si == len(starts_s), f"{len(starts_s) - si} accepted STARTs beyond the IR's")
    _require(dsub == len(cmd_start) == len(cmd_done), "DMA commands started or completed beyond the IR's submits")
    _require(reads.pos == len(reads.rows), f"{len(reads.rows) - reads.pos} reads left after the last wait")
    _require(events, "the IR has no events")
    ends = [int(e["t"]) for e in E if e.get("close") == "end"]
    end = max(ends) if ends else None
    for row in events:  # edges relative to the origin (the first event's issue edge), in cycles
        for k in ("issue_edge", "release_edge", "hw_edge"):
            if row.get(k) is not None:
                row[k] = (row[k] - origin) // ps
    return {
        "schema": CONTRACTS[contract][0], "kind": "raw", "contract": CONTRACTS[contract][1], "label": label,
        "ir_sha256": ir_sha256,
        "clock_mhz": clock_mhz, "origin_ps": origin, "boot_prefix_cycles": origin // ps,
        "terminal_tail_cycles": None if end is None else (end - prev_release) // ps,
        "note": ("raw ESL issue timing; valid only for this IR, firmware, binary, config and clock; "
                 "a TLS run on it is a raw replay, not qualified"),
        "events": events,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cc_dir", type=Path)
    ap.add_argument("ir", type=Path)
    ap.add_argument("--wave-log", type=Path, required=True)
    ap.add_argument("--mmio-log", type=Path, required=True)
    ap.add_argument("--dram-log", type=Path, required=True)
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--contract", required=True, choices=sorted(CONTRACTS))
    ap.add_argument("--label", help="default: raw ESL issue timing (C-05 contract <contract>)")
    a = ap.parse_args(argv)
    irb = a.ir.read_bytes()
    out = build(a.cc_dir, json.loads(irb), hashlib.sha256(irb).hexdigest(), a.wave_log.read_text(),
                a.mmio_log.read_text(), a.dram_log.read_text(),
                a.label or f"raw ESL issue timing (C-05 contract {a.contract})", contract=a.contract)
    a.output.write_text(json.dumps(out, indent=1) + "\n")
    print(f"{len(out['events'])} events, boot {out['boot_prefix_cycles']} cycles, "
          f"tail {out['terminal_tail_cycles']} cycles -> {a.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
