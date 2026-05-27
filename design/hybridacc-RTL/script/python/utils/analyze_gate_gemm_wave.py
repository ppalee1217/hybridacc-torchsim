 #!/usr/bin/env python3
"""Analyze the gate GEMM return path from FSDB/VCD waveforms.

This script builds a focused VCD from the gate FSDB and correlates four
observable stages of the Cluster return path:

1. PE-side PLO responses visible at the NoC scope.
2. NoC aggregated bus_to_noc response snapshots.
3. HDDU receive-plane response / SPM write payload.
4. Scratchpad bank 9 SRAM write data.

The goal is to tell whether corruption first appears before or after the
NoC/HDDU/SPM transport path, without adding new TB probes or recompiling.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


ROOT_SUFFIX = "gen_clusters_0__cluster"
FOCUS_PORT = 2
FOCUS_PE = 0
FOCUSED_PE_LABEL = f"p{FOCUS_PORT}pe{FOCUS_PE}"
PE15_SCOPE = f"{ROOT_SUFFIX}.noc.gen_ports_{FOCUS_PORT}__gen_pe_{FOCUS_PE}__pe"

EXE_A_STATE_NAMES = {
    0: "S_IDLE",
    1: "S_NORMAL_MODE",
    2: "S_VMAC_S1",
    3: "S_VMAC_S2",
    4: "S_VMAC_S3",
    5: "S_WAIT_PLI",
    6: "S_EXEC_PLI_VADDU",
    7: "S_WAIT_PLO",
}
EXE_A_EXEC_PLI_VADDU = 6
EXE_A_WAIT_PLO = 7

WANTED_SUFFIXES = {
    "hddu_resp_valid": f"{ROOT_SUFFIX}.hddu_noc_plo_resp_valid",
    "hddu_resp_ready": f"{ROOT_SUFFIX}.hddu_noc_plo_resp_ready",
    "hddu_resp_data": f"{ROOT_SUFFIX}.hddu_noc_plo_resp_data",
    "spm_req_valid": f"{ROOT_SUFFIX}.hddu_spm_req_valid_sig",
    "spm_req_ready": f"{ROOT_SUFFIX}.hddu_spm_req_ready_sig",
    "spm_req_payload": f"{ROOT_SUFFIX}.hddu_spm_req_payload_sig",
    "bus_resp_valid": f"{ROOT_SUFFIX}.noc.bus_to_noc_resp_valid",
    "bus_resp_data": f"{ROOT_SUFFIX}.noc.bus_to_noc_resp_data",
    "pe_resp_valid": f"{ROOT_SUFFIX}.noc.pe_to_bus_plo_valid",
    "pe_resp_data": f"{ROOT_SUFFIX}.noc.pe_to_bus_plo_data",
    "router_pending_read": f"{ROOT_SUFFIX}.noc.router.pending_read_reg",
    "router_pending_read_ultra": f"{ROOT_SUFFIX}.noc.router.pending_read_ultra_reg",
    "bank9_a": f"{ROOT_SUFFIX}.spm.gen_spm_bank_9__u_bank.A",
    "bank9_d": f"{ROOT_SUFFIX}.spm.gen_spm_bank_9__u_bank.D",
    "bank9_ceb": f"{ROOT_SUFFIX}.spm.gen_spm_bank_9__u_bank.CEB",
    "bank9_web": f"{ROOT_SUFFIX}.spm.gen_spm_bank_9__u_bank.WEB",
    "pe15_route_mode": f"{PE15_SCOPE}.router_mode",
    "pe15_ps_valid": f"{PE15_SCOPE}.ps_valid",
    "pe15_ps_data": f"{PE15_SCOPE}.ps_data",
    "pe15_pd_valid": f"{PE15_SCOPE}.pd_valid",
    "pe15_pd_data": f"{PE15_SCOPE}.pd_data",
    "pe15_pd_set_valid": f"{PE15_SCOPE}.pd_set_valid",
    "pe15_pd_set_data": f"{PE15_SCOPE}.pd_set_data",
    "pe15_ln_pli_valid": f"{PE15_SCOPE}.ln_pli_valid",
    "pe15_ln_pli_data": f"{PE15_SCOPE}.ln_pli_data",
    "pe15_pli_valid": f"{PE15_SCOPE}.pli_valid",
    "pe15_pli_ready": f"{PE15_SCOPE}.pli_ready",
    "pe15_pli_data": f"{PE15_SCOPE}.pli_data",
    "pe15_plo_valid": f"{PE15_SCOPE}.plo_valid",
    "pe15_plo_ready": f"{PE15_SCOPE}.plo_ready",
    "pe15_plo_data": f"{PE15_SCOPE}.plo_data",
    "pe15_exe_a_stall_port_pli": f"{PE15_SCOPE}.exe_a_stage.stall_port_pli",
    "pe15_exe_a_stall_port_plo": f"{PE15_SCOPE}.exe_a_stage.stall_port_plo",
    "pe15_exe_a_plo_buf_valid_next": f"{PE15_SCOPE}.exe_a_stage.plo_buf_valid_next",
    "pe15_exe_a_vaddu_result_sig": f"{PE15_SCOPE}.exe_a_stage.vaddu_result_sig",
    "pe15_exe_a_pr_vp_in": f"{PE15_SCOPE}.exe_a_stage.pr_vp_in",
    "pe15_exe_a_pr_vp_out": f"{PE15_SCOPE}.exe_a_stage.pr_vp_out",
    "pe15_exe_a_state": f"{PE15_SCOPE}.exe_a_stage.state_reg",
    "pe15_exe_a_pli_data_reg": f"{PE15_SCOPE}.exe_a_stage.pli_data_reg",
    "pe15_exe_a_vaddu_result_reg": f"{PE15_SCOPE}.exe_a_stage.vaddu_result_reg",
}

SELECTED_SCOPES: tuple[tuple[str, int], ...] = (
    ("/tb_hybridacc_sim/dut/gen_clusters_0__cluster", 0),
    ("/tb_hybridacc_sim/dut/gen_clusters_0__cluster/hddu", 1),
    ("/tb_hybridacc_sim/dut/gen_clusters_0__cluster/noc", 1),
    ("/tb_hybridacc_sim/dut/gen_clusters_0__cluster/spm/gen_spm_bank_9__u_bank", 0),
)

FALLBACK_SCOPE = ("/tb_hybridacc_sim/dut/gen_clusters_0__cluster", 3)

NUM_NOC_PORTS = 3
NUM_PES = 48
SPM_REQ_COUNT = 4
SPM_REQ_WIDTH = 225
NOC_RESP_WIDTH = 66

PE15_EVENT_SPECS: tuple[tuple[str, str, str], ...] = (
    ("ps", "pe15_ps_valid", "pe15_ps_data"),
    ("pd", "pe15_pd_valid", "pe15_pd_data"),
    ("pd_set", "pe15_pd_set_valid", "pe15_pd_set_data"),
    ("ln_pli", "pe15_ln_pli_valid", "pe15_ln_pli_data"),
    ("pli", "pe15_pli_valid", "pe15_pli_data"),
    ("plo", "pe15_plo_valid", "pe15_plo_data"),
)

SETTLED_WINDOW_KINDS: set[str] = set()


@dataclass(frozen=True)
class SignalDef:
    alias: str
    path: str
    code: str
    width: int
    msb: int
    lsb: int


@dataclass
class HdduRespEvent:
    time_ps: int
    lanes: tuple[int | None, int | None, int | None]


@dataclass
class SpmWriteEvent:
    time_ps: int
    bank9_row: int | None
    bank9_data: int | None
    chunk_index: int | None
    addr: int | None
    lanes: tuple[int | None, int | None, int | None]
    wen: int | None


@dataclass
class BusEvent:
    time_ps: int
    pending_read: int | None
    ultra: int | None
    port_responses: list[dict]
    active_pe_responses: list[dict]


@dataclass
class FocusedPeEvent:
    time_ps: int
    kind: str
    data: int | None
    route_mode: int | None
    pli_ready: int | None
    plo_ready: int | None
    exe_a_state: int | None
    stall_port_pli: int | None
    stall_port_plo: int | None
    plo_buf_valid_next: int | None
    vaddu_result_sig: int | None
    vaddu_result_reg: int | None
    pr_vp_in: int | None
    pr_vp_out: int | None
    pli_data_reg: int | None


@dataclass
class SettledPloAlignment:
    sample_index: int
    plo_event: FocusedPeEvent
    hddu_index: int | None
    hddu_event: HdduRespEvent | None
    spm_index: int | None
    spm_event: SpmWriteEvent | None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Correlate gate GEMM return-path waveforms.")
    parser.add_argument(
        "--fsdb",
        type=Path,
        default=Path("tb_hybridacc_sim.fsdb"),
        help="Input FSDB waveform.",
    )
    parser.add_argument(
        "--vcd",
        type=Path,
        default=None,
        help="Optional focused VCD path. If absent, derive it from --fsdb.",
    )
    parser.add_argument(
        "--max-events",
        type=int,
        default=16,
        help="Number of events to print per section.",
    )
    parser.add_argument(
        "--reuse-vcd",
        action="store_true",
        help="Reuse an existing VCD instead of rebuilding it from FSDB.",
    )
    return parser.parse_args()


def find_fsdb2vcd() -> str:
    candidates = [
        shutil.which("fsdb2vcd"),
        "/usr/cad/synopsys/verdi/2024.09-sp1//bin/fsdb2vcd",
    ]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            return candidate
    raise FileNotFoundError("fsdb2vcd not found in PATH or known Verdi location")


def build_focused_vcd(fsdb_path: Path, vcd_path: Path) -> None:
    fsdb2vcd = find_fsdb2vcd()
    scoped_cmd = [fsdb2vcd, str(fsdb_path), "-keep_last_time", "-o", str(vcd_path)]
    for scope, level in SELECTED_SCOPES:
        scoped_cmd.extend(["-s", scope, "-level", str(level)])

    result = subprocess.run(scoped_cmd, capture_output=True, text=True)
    if result.returncode == 0:
        return

    fallback_cmd = [
        fsdb2vcd,
        str(fsdb_path),
        "-keep_last_time",
        "-s",
        FALLBACK_SCOPE[0],
        "-level",
        str(FALLBACK_SCOPE[1]),
        "-o",
        str(vcd_path),
    ]
    fallback = subprocess.run(fallback_cmd, capture_output=True, text=True)
    if fallback.returncode != 0:
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        sys.stderr.write(fallback.stdout)
        sys.stderr.write(fallback.stderr)
        raise RuntimeError("fsdb2vcd failed for both scoped and fallback conversions")


def parse_var_range(tokens: list[str], width: int) -> tuple[int, int]:
    if not tokens:
        return (width - 1, 0) if width > 1 else (0, 0)
    text = "".join(tokens)
    if not (text.startswith("[") and text.endswith("]") and ":" in text):
        return (width - 1, 0) if width > 1 else (0, 0)
    left, right = text[1:-1].split(":", 1)
    return int(left), int(right)


def parse_header(vcd_path: Path) -> dict[str, SignalDef]:
    scopes: list[str] = []
    found: dict[str, SignalDef] = {}

    with vcd_path.open("r", encoding="utf-8", errors="ignore") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            if line.startswith("$scope"):
                parts = line.split()
                scopes.append(parts[2])
                continue
            if line.startswith("$upscope"):
                if scopes:
                    scopes.pop()
                continue
            if line.startswith("$var"):
                parts = line.split()
                width = int(parts[2])
                code = parts[3]
                name = parts[4]
                msb, lsb = parse_var_range(parts[5:-1], width)
                full_path = ".".join(scopes + [name])
                for alias, suffix in WANTED_SUFFIXES.items():
                    if full_path.endswith(suffix):
                        found[alias] = SignalDef(
                            alias=alias,
                            path=full_path,
                            code=code,
                            width=width,
                            msb=msb,
                            lsb=lsb,
                        )
                continue
            if line.startswith("$enddefinitions"):
                break

    missing = sorted(set(WANTED_SUFFIXES) - set(found))
    if missing:
        raise RuntimeError(f"Missing required signals in VCD header: {', '.join(missing)}")
    return found


def normalize_bits(value: str, width: int) -> str:
    value = value.strip().lower()
    if not value:
        return "x" * width
    if len(value) == 1 and value[0] in "01xz":
        bit = value[0]
        return bit * width
    if value[0] in "br":
        payload = value[1:]
    else:
        payload = value
    fill = payload[0] if payload and payload[0] in "xz" else "0"
    if len(payload) < width:
        payload = payload.rjust(width, fill)
    elif len(payload) > width:
        payload = payload[-width:]
    return payload


def bit_at(bits: str, sig: SignalDef, index: int) -> str:
    if sig.msb <= sig.lsb:
        offset = sig.lsb - index
    else:
        offset = sig.msb - index
    if offset < 0 or offset >= len(bits):
        return "x"
    return bits[offset]


def slice_desc(bits: str, sig: SignalDef, hi: int, lo: int) -> str:
    if sig.msb < sig.lsb:
        raise ValueError(f"Signal {sig.alias} is not descending: [{sig.msb}:{sig.lsb}]")
    start = sig.msb - hi
    end = sig.msb - lo + 1
    return bits[start:end]


def bits_to_int(bits: str) -> int | None:
    if any(ch in bits for ch in "xz"):
        return None
    return int(bits, 2)


def state_to_int(state: dict[str, str], alias: str) -> int | None:
    return bits_to_int(state.get(alias, "x"))


def decode_lanes_192(bits: str) -> tuple[int | None, int | None, int | None]:
    if len(bits) != 192:
        raise ValueError(f"Expected 192 bits, got {len(bits)}")
    lane0 = bits_to_int(bits[-64:])
    lane1 = bits_to_int(bits[-128:-64])
    lane2 = bits_to_int(bits[:-128])
    return (lane0, lane1, lane2)


def decode_spm_req_chunk(bits: str) -> dict:
    if len(bits) != SPM_REQ_WIDTH:
        raise ValueError(f"Expected {SPM_REQ_WIDTH} bits, got {len(bits)}")
    wen = bits_to_int(bits[-1:])
    wdata_bits = bits[-193:-1]
    addr_bits = bits[:-193]
    return {
        "addr": bits_to_int(addr_bits),
        "wen": wen,
        "lanes": decode_lanes_192(wdata_bits),
    }


def decode_spm_req_chunks(bits: str, sig: SignalDef) -> list[dict]:
    chunks: list[dict] = []
    for idx in range(SPM_REQ_COUNT):
        lo = idx * SPM_REQ_WIDTH
        hi = lo + SPM_REQ_WIDTH - 1
        chunk_bits = slice_desc(bits, sig, hi, lo)
        chunk = decode_spm_req_chunk(chunk_bits)
        chunk["chunk_index"] = idx
        chunks.append(chunk)
    return chunks


def decode_noc_resp_chunks(bits: str, sig: SignalDef, count: int) -> list[dict]:
    chunks: list[dict] = []
    for idx in range(count):
        lo = idx * NOC_RESP_WIDTH
        hi = lo + NOC_RESP_WIDTH - 1
        chunk_bits = slice_desc(bits, sig, hi, lo)
        chunks.append(
            {
                "index": idx,
                "data": bits_to_int(chunk_bits[:-2]),
                "status": bits_to_int(chunk_bits[-2:]),
            }
        )
    return chunks


def decode_active_bus_ports(state: dict[str, str], signals: dict[str, SignalDef]) -> list[dict]:
    valid_bits = state.get("bus_resp_valid")
    data_bits = state.get("bus_resp_data")
    if valid_bits is None or data_bits is None:
        return []
    decoded = decode_noc_resp_chunks(data_bits, signals["bus_resp_data"], NUM_NOC_PORTS)
    active: list[dict] = []
    valid_sig = signals["bus_resp_valid"]
    for port in range(NUM_NOC_PORTS):
        if bit_at(valid_bits, valid_sig, port) == "1":
            active.append(decoded[port])
    return active


def decode_active_pe_responses(state: dict[str, str], signals: dict[str, SignalDef]) -> list[dict]:
    valid_bits = state.get("pe_resp_valid")
    data_bits = state.get("pe_resp_data")
    if valid_bits is None or data_bits is None:
        return []
    valid_sig = signals["pe_resp_valid"]
    decoded = decode_noc_resp_chunks(data_bits, signals["pe_resp_data"], NUM_PES)
    active: list[dict] = []
    for slot in range(NUM_PES):
        if bit_at(valid_bits, valid_sig, slot) == "1":
            entry = dict(decoded[slot])
            entry["slot"] = slot
            entry["approx_port"] = slot // 16
            entry["approx_pe"] = slot % 16
            active.append(entry)
    return active


def hddu_triggered(state: dict[str, str]) -> bool:
    return state.get("hddu_resp_valid") == "1" and state.get("hddu_resp_ready") == "1" and "hddu_resp_data" in state


def spm_triggered(state: dict[str, str]) -> bool:
    return (
        state.get("bank9_ceb") == "0"
        and state.get("bank9_web") == "0"
        and "bank9_a" in state
        and "bank9_d" in state
        and "spm_req_payload" in state
    )


def bus_triggered(state: dict[str, str], signals: dict[str, SignalDef]) -> bool:
    valid_bits = state.get("bus_resp_valid")
    if valid_bits is None:
        return False
    valid_sig = signals["bus_resp_valid"]
    return any(bit_at(valid_bits, valid_sig, port) == "1" for port in range(NUM_NOC_PORTS))


def collect_events(vcd_path: Path, signals: dict[str, SignalDef]) -> tuple[list[HdduRespEvent], list[SpmWriteEvent], list[BusEvent]]:
    code_to_sigs: dict[str, list[SignalDef]] = {}
    for sig in signals.values():
        code_to_sigs.setdefault(sig.code, []).append(sig)

    state: dict[str, str] = {}
    current_time: int | None = None
    last_hddu_sig: tuple[int | None, int | None, int | None] | None = None
    last_spm_sig: tuple | None = None
    last_bus_sig: tuple | None = None
    hddu_events: list[HdduRespEvent] = []
    spm_events: list[SpmWriteEvent] = []
    bus_events: list[BusEvent] = []

    hddu_active = False
    spm_active = False
    bus_active = False
    hddu_snapshot: dict[str, str] | None = None
    spm_snapshot: dict[str, str] | None = None
    bus_snapshot: dict[str, str] | None = None
    hddu_start_time: int | None = None
    spm_start_time: int | None = None
    bus_start_time: int | None = None

    def flush_hddu() -> None:
        nonlocal hddu_snapshot, hddu_start_time, last_hddu_sig
        if hddu_snapshot is None or hddu_start_time is None:
            return
        event = decode_hddu_resp_event(hddu_start_time, hddu_snapshot, signals)
        if event is not None and event.lanes != last_hddu_sig:
            hddu_events.append(event)
            last_hddu_sig = event.lanes
        hddu_snapshot = None
        hddu_start_time = None

    def flush_spm() -> None:
        nonlocal spm_snapshot, spm_start_time, last_spm_sig
        if spm_snapshot is None or spm_start_time is None:
            return
        event = decode_spm_write_event(spm_start_time, spm_snapshot, signals)
        if event is not None:
            signature = (event.bank9_row, event.bank9_data, event.chunk_index, event.addr, event.lanes)
            if signature != last_spm_sig:
                spm_events.append(event)
                last_spm_sig = signature
        spm_snapshot = None
        spm_start_time = None

    def flush_bus() -> None:
        nonlocal bus_snapshot, bus_start_time, last_bus_sig
        if bus_snapshot is None or bus_start_time is None:
            return
        event = decode_bus_event(bus_start_time, bus_snapshot, signals)
        if event is not None:
            signature = signature_bus_event(event)
            if signature != last_bus_sig:
                bus_events.append(event)
                last_bus_sig = signature
        bus_snapshot = None
        bus_start_time = None

    def update_intervals() -> None:
        nonlocal hddu_active, spm_active, bus_active
        nonlocal hddu_snapshot, spm_snapshot, bus_snapshot
        nonlocal hddu_start_time, spm_start_time, bus_start_time

        if current_time is None:
            return

        snapshot = dict(state)

        hddu_now = hddu_triggered(snapshot)
        if hddu_now:
            if not hddu_active:
                hddu_start_time = current_time
            hddu_snapshot = snapshot
        elif hddu_active:
            flush_hddu()
        hddu_active = hddu_now

        spm_now = spm_triggered(snapshot)
        if spm_now:
            if not spm_active:
                spm_start_time = current_time
            spm_snapshot = snapshot
        elif spm_active:
            flush_spm()
        spm_active = spm_now

        bus_now = bus_triggered(snapshot, signals)
        if bus_now:
            if not bus_active:
                bus_start_time = current_time
            bus_snapshot = snapshot
        elif bus_active:
            flush_bus()
        bus_active = bus_now

    with vcd_path.open("r", encoding="utf-8", errors="ignore") as handle:
        in_header = True
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            if in_header:
                if line.startswith("$enddefinitions"):
                    in_header = False
                continue

            if line[0] == "#":
                current_time = int(line[1:])
                continue

            if line[0] == "$":
                continue

            if line[0] in "01xz":
                code = line[1:]
                sigs = code_to_sigs.get(code)
                if not sigs:
                    continue
                for sig in sigs:
                    state[sig.alias] = normalize_bits(line[0], sig.width)
                if current_time is None:
                    current_time = 0
                update_intervals()
                continue

            if line[0] in "br":
                payload = line[1:].split(None, 1)
                if len(payload) != 2:
                    continue
                value, code = payload
                sigs = code_to_sigs.get(code)
                if not sigs:
                    continue
                for sig in sigs:
                    state[sig.alias] = normalize_bits(value, sig.width)
                if current_time is None:
                    current_time = 0
                update_intervals()
                continue

    flush_hddu()
    flush_spm()
    flush_bus()
    return hddu_events, spm_events, bus_events


def collect_focused_pe_events(vcd_path: Path, signals: dict[str, SignalDef]) -> list[FocusedPeEvent]:
    code_to_sigs: dict[str, list[SignalDef]] = {}
    for sig in signals.values():
        code_to_sigs.setdefault(sig.code, []).append(sig)

    state: dict[str, str] = {}
    current_time: int | None = None
    events: list[FocusedPeEvent] = []
    last_signature: dict[str, tuple[int | None, int | None] | None] = {
        kind: None for kind, _, _ in PE15_EVENT_SPECS
    }
    window_active: dict[str, bool] = {kind: False for kind, _, _ in PE15_EVENT_SPECS}
    window_pending: dict[str, FocusedPeEvent | None] = {kind: None for kind, _, _ in PE15_EVENT_SPECS}

    def build_event(kind: str, snapshot: dict[str, str], data_alias: str) -> FocusedPeEvent:
        return FocusedPeEvent(
            time_ps=current_time,
            kind=kind,
            data=bits_to_int(snapshot.get(data_alias, "x")),
            route_mode=bits_to_int(snapshot.get("pe15_route_mode", "x")),
            pli_ready=state_to_int(snapshot, "pe15_pli_ready"),
            plo_ready=state_to_int(snapshot, "pe15_plo_ready"),
            exe_a_state=state_to_int(snapshot, "pe15_exe_a_state"),
            stall_port_pli=state_to_int(snapshot, "pe15_exe_a_stall_port_pli"),
            stall_port_plo=state_to_int(snapshot, "pe15_exe_a_stall_port_plo"),
            plo_buf_valid_next=state_to_int(snapshot, "pe15_exe_a_plo_buf_valid_next"),
            vaddu_result_sig=state_to_int(snapshot, "pe15_exe_a_vaddu_result_sig"),
            vaddu_result_reg=state_to_int(snapshot, "pe15_exe_a_vaddu_result_reg"),
            pr_vp_in=state_to_int(snapshot, "pe15_exe_a_pr_vp_in"),
            pr_vp_out=state_to_int(snapshot, "pe15_exe_a_pr_vp_out"),
            pli_data_reg=state_to_int(snapshot, "pe15_exe_a_pli_data_reg"),
        )

    def emit_event(kind: str, event: FocusedPeEvent) -> None:
        signature = (event.data, event.route_mode)
        if signature == last_signature[kind]:
            return
        events.append(event)
        last_signature[kind] = signature

    def update_intervals() -> None:
        if current_time is None:
            return
        snapshot = dict(state)
        for kind, valid_alias, data_alias in PE15_EVENT_SPECS:
            active = snapshot.get(valid_alias) == "1" and data_alias in snapshot

            if kind in SETTLED_WINDOW_KINDS:
                if active:
                    window_pending[kind] = build_event(kind, snapshot, data_alias)
                elif window_active[kind] and window_pending[kind] is not None:
                    emit_event(kind, window_pending[kind])
                    window_pending[kind] = None

                if not active:
                    last_signature[kind] = None
                    window_pending[kind] = None

                window_active[kind] = active
                continue

            if not active:
                last_signature[kind] = None
                continue

            emit_event(kind, build_event(kind, snapshot, data_alias))

    with vcd_path.open("r", encoding="utf-8", errors="ignore") as handle:
        in_header = True
        for raw_line in handle:
            line = raw_line.strip()
            if not line:
                continue
            if in_header:
                if line.startswith("$enddefinitions"):
                    in_header = False
                continue

            if line[0] == "#":
                current_time = int(line[1:])
                continue

            if line[0] == "$":
                continue

            if line[0] in "01xz":
                code = line[1:]
                sigs = code_to_sigs.get(code)
                if not sigs:
                    continue
                for sig in sigs:
                    state[sig.alias] = normalize_bits(line[0], sig.width)
                if current_time is None:
                    current_time = 0
                update_intervals()
                continue

            if line[0] in "br":
                payload = line[1:].split(None, 1)
                if len(payload) != 2:
                    continue
                value, code = payload
                sigs = code_to_sigs.get(code)
                if not sigs:
                    continue
                for sig in sigs:
                    state[sig.alias] = normalize_bits(value, sig.width)
                if current_time is None:
                    current_time = 0
                update_intervals()
                continue

    for kind in SETTLED_WINDOW_KINDS:
        if window_pending[kind] is not None:
            emit_event(kind, window_pending[kind])

    return events


def decode_hddu_resp_event(time_ps: int, state: dict[str, str], signals: dict[str, SignalDef]) -> HdduRespEvent | None:
    valid = state.get("hddu_resp_valid")
    ready = state.get("hddu_resp_ready")
    data = state.get("hddu_resp_data")
    if valid != "1" or ready != "1" or data is None:
        return None
    return HdduRespEvent(time_ps=time_ps, lanes=decode_lanes_192(data))


def decode_spm_write_event(time_ps: int, state: dict[str, str], signals: dict[str, SignalDef]) -> SpmWriteEvent | None:
    ceb = state.get("bank9_ceb")
    web = state.get("bank9_web")
    bank9_a = state.get("bank9_a")
    bank9_d = state.get("bank9_d")
    payload = state.get("spm_req_payload")
    if ceb != "0" or web != "0" or bank9_a is None or bank9_d is None or payload is None:
        return None

    chunks = decode_spm_req_chunks(payload, signals["spm_req_payload"])
    bank9_lane0 = bits_to_int(bank9_d)
    bank9_row = bits_to_int(bank9_a)

    matched_chunk = None
    for chunk in chunks:
        if chunk["wen"] == 1 and chunk["lanes"][0] == bank9_lane0:
            matched_chunk = chunk
            break

    return SpmWriteEvent(
        time_ps=time_ps,
        bank9_row=bank9_row,
        bank9_data=bank9_lane0,
        chunk_index=None if matched_chunk is None else matched_chunk["chunk_index"],
        addr=None if matched_chunk is None else matched_chunk["addr"],
        lanes=(None, None, None) if matched_chunk is None else matched_chunk["lanes"],
        wen=None if matched_chunk is None else matched_chunk["wen"],
    )


def decode_bus_event(time_ps: int, state: dict[str, str], signals: dict[str, SignalDef]) -> BusEvent | None:
    active_ports = decode_active_bus_ports(state, signals)
    if not active_ports:
        return None
    return BusEvent(
        time_ps=time_ps,
        pending_read=bits_to_int(state.get("router_pending_read", "x")),
        ultra=bits_to_int(state.get("router_pending_read_ultra", "x")),
        port_responses=active_ports,
        active_pe_responses=decode_active_pe_responses(state, signals),
    )


def signature_bus_event(event: BusEvent) -> tuple:
    return (
        tuple((resp["index"], resp["data"], resp["status"]) for resp in event.port_responses),
        tuple((resp["slot"], resp["data"], resp["status"]) for resp in event.active_pe_responses),
    )


def infer_recv_chunk(spm_events: list[SpmWriteEvent]) -> int | None:
    counts = Counter(event.chunk_index for event in spm_events if event.chunk_index is not None)
    if not counts:
        return None
    return counts.most_common(1)[0][0]


def match_lane_tuples(lhs: tuple[int | None, int | None, int | None], rhs: tuple[int | None, int | None, int | None]) -> bool:
    return lhs == rhs and all(value is not None for value in lhs)


def fmt_hex64(value: int | None) -> str:
    return "unknown" if value is None else f"0x{value:016x}"


def fmt_hex32(value: int | None) -> str:
    return "unknown" if value is None else f"0x{value:08x}"


def fmt_lanes(lanes: tuple[int | None, int | None, int | None]) -> str:
    return "[" + ", ".join(fmt_hex64(value) for value in lanes) + "]"


def fmt_hex(value: int | None, width_bits: int) -> str:
    if value is None:
        return "unknown"
    width_nibbles = max(1, (width_bits + 3) // 4)
    return f"0x{value:0{width_nibbles}x}"


def route_mode_name(mode: int | None) -> str:
    mapping = {
        0: "PLI_FROM_LN_PLO_TO_LN",
        1: "PLI_FROM_BUS_PLO_TO_LN",
        2: "PLI_FROM_LN_PLO_TO_BUS",
        3: "PLI_FROM_BUS_PLO_TO_BUS",
    }
    return mapping.get(mode, "unknown")


def exe_a_state_name(state: int | None) -> str:
    return EXE_A_STATE_NAMES.get(state, "unknown")


def infer_repeat_period(values: list[int | None], max_period: int = 8) -> int | None:
    filtered = [value for value in values if value is not None]
    if len(filtered) < 4:
        return None
    max_candidate = min(max_period, len(filtered) // 2)
    for period in range(1, max_candidate + 1):
        sample_len = min(len(filtered), period * 3)
        if sample_len < period * 2:
            continue
        base = filtered[:period]
        if all(filtered[idx] == base[idx % period] for idx in range(sample_len)):
            return period
    return None


def collapse_event_bursts(events: list[FocusedPeEvent], max_gap_ps: int = 100) -> list[FocusedPeEvent]:
    if not events:
        return []

    collapsed: list[FocusedPeEvent] = []
    current = events[0]
    for event in events[1:]:
        if event.time_ps - current.time_ps <= max_gap_ps:
            current = event
            continue
        collapsed.append(current)
        current = event

    collapsed.append(current)
    return collapsed


def align_settled_plo_to_downstream(
    plo_events: list[FocusedPeEvent],
    hddu_events: list[HdduRespEvent],
    spm_events: list[SpmWriteEvent],
) -> list[SettledPloAlignment]:
    alignments: list[SettledPloAlignment] = []
    hddu_cursor = 0

    for sample_index, plo_event in enumerate(plo_events):
        matched_hddu_index: int | None = None
        matched_hddu_event: HdduRespEvent | None = None
        matched_spm_index: int | None = None
        matched_spm_event: SpmWriteEvent | None = None

        if plo_event.data is not None:
            for hddu_index in range(hddu_cursor, len(hddu_events)):
                if hddu_events[hddu_index].lanes[0] == plo_event.data:
                    matched_hddu_index = hddu_index
                    matched_hddu_event = hddu_events[hddu_index]
                    hddu_cursor = hddu_index + 1
                    if hddu_index < len(spm_events):
                        matched_spm_index = hddu_index
                        matched_spm_event = spm_events[hddu_index]
                    break

        alignments.append(
            SettledPloAlignment(
                sample_index=sample_index,
                plo_event=plo_event,
                hddu_index=matched_hddu_index,
                hddu_event=matched_hddu_event,
                spm_index=matched_spm_index,
                spm_event=matched_spm_event,
            )
        )

    return alignments


def print_summary(
    hddu_events: list[HdduRespEvent],
    spm_events: list[SpmWriteEvent],
    bus_events: list[BusEvent],
    pe15_events: list[FocusedPeEvent],
    max_events: int,
) -> None:
    recv_chunk = infer_recv_chunk(spm_events)
    compare_count = min(len(hddu_events), len(spm_events))
    hddu_to_spm_matches = 0
    for idx in range(compare_count):
        if match_lane_tuples(hddu_events[idx].lanes, spm_events[idx].lanes):
            hddu_to_spm_matches += 1

    bank_matches = 0
    for event in spm_events:
        if event.lanes[0] is not None and event.lanes[0] == event.bank9_data:
            bank_matches += 1
    spm_payload_decoded = sum(1 for event in spm_events if event.addr is not None and event.lanes[0] is not None)

    bus_compare_count = min(len(bus_events), len(hddu_events))
    bus_to_hddu_matches = 0
    pe_cover_matches = 0
    bus_pe_decoded = sum(1 for event in bus_events if event.active_pe_responses)
    for idx in range(bus_compare_count):
        event = bus_events[idx]
        if event.ultra == 1 and len(event.port_responses) == NUM_NOC_PORTS:
            lanes = tuple(event.port_responses[port]["data"] for port in range(NUM_NOC_PORTS))
            if match_lane_tuples(hddu_events[idx].lanes, lanes):
                bus_to_hddu_matches += 1
        elif event.port_responses:
            expected = event.port_responses[0]["data"]
            observed = hddu_events[idx].lanes
            if observed[0] == expected and observed[1] in (0, None) and observed[2] in (0, None):
                bus_to_hddu_matches += 1
        pe_values = {resp["data"] for resp in event.active_pe_responses if resp["data"] is not None}
        bus_values = {resp["data"] for resp in event.port_responses if resp["data"] is not None}
        if bus_values and bus_values.issubset(pe_values):
            pe_cover_matches += 1

    pe15_by_kind: dict[str, list[FocusedPeEvent]] = {kind: [] for kind, _, _ in PE15_EVENT_SPECS}
    for event in pe15_events:
        pe15_by_kind[event.kind].append(event)

    pe15_route_modes = sorted({event.route_mode for event in pe15_events if event.route_mode is not None})
    pe15_ln_period = infer_repeat_period([event.data for event in pe15_by_kind["ln_pli"]])
    pe15_pli_period = infer_repeat_period([event.data for event in pe15_by_kind["pli"]])
    pe15_raw_plo_events = pe15_by_kind["plo"]
    pe15_plo_events = collapse_event_bursts(pe15_raw_plo_events)
    pe15_downstream_alignments = align_settled_plo_to_downstream(pe15_plo_events, hddu_events, spm_events)
    pe15_plo_period = infer_repeat_period([event.data for event in pe15_plo_events])
    pe15_exe_a_vaddu_sig_period = infer_repeat_period([event.vaddu_result_sig for event in pe15_plo_events])
    pe15_exe_a_vaddu_reg_period = infer_repeat_period([event.vaddu_result_reg for event in pe15_plo_events])
    pe15_exe_a_pr_vp_out_period = infer_repeat_period([event.pr_vp_out for event in pe15_plo_events])
    pe15_exe_a_pli_reg_period = infer_repeat_period([event.pli_data_reg for event in pe15_plo_events])

    pe15_plo_state_counts = Counter(event.exe_a_state for event in pe15_plo_events if event.exe_a_state is not None)
    pe15_wait_plo_events = sum(1 for event in pe15_plo_events if event.exe_a_state == EXE_A_WAIT_PLO)
    pe15_exec_events = sum(1 for event in pe15_plo_events if event.exe_a_state == EXE_A_EXEC_PLI_VADDU)
    pe15_wait_plo_replay_hits = sum(
        1
        for event in pe15_plo_events
        if event.exe_a_state == EXE_A_WAIT_PLO
        and event.data is not None
        and event.data == event.vaddu_result_reg
        and event.data != event.vaddu_result_sig
    )
    pe15_exec_sig_hits = sum(
        1
        for event in pe15_plo_events
        if event.exe_a_state == EXE_A_EXEC_PLI_VADDU
        and event.data is not None
        and event.data == event.vaddu_result_sig
    )
    pe15_reg_matches = sum(
        1
        for event in pe15_plo_events
        if event.data is not None and event.data == event.vaddu_result_reg
    )
    pe15_sig_matches = sum(
        1
        for event in pe15_plo_events
        if event.data is not None and event.data == event.vaddu_result_sig
    )
    pe15_pr_vp_out_matches = sum(
        1
        for event in pe15_plo_events
        if event.data is not None and event.data == event.pr_vp_out
    )

    print("[SUMMARY]")
    print(f"Focused HDDU/NoC/SPM events: hddu={len(hddu_events)} spm_bank9={len(spm_events)} bus={len(bus_events)}")
    print(
        f"Focused {FOCUSED_PE_LABEL} events: "
        + " ".join(f"{kind}={len(pe15_by_kind[kind])}" for kind, _, _ in PE15_EVENT_SPECS)
    )
    print(
        f"Observed {FOCUSED_PE_LABEL} route modes: "
        + (", ".join(route_mode_name(mode) for mode in pe15_route_modes) if pe15_route_modes else "unknown")
    )
    print(f"Inferred receive-plane SPM chunk: {recv_chunk if recv_chunk is not None else 'unknown'}")
    print(f"HDDU response -> SPM payload match: {hddu_to_spm_matches}/{compare_count}")
    print(f"SPM lane0 -> bank9 D match: {bank_matches}/{len(spm_events)}")
    print(f"NoC bus response -> HDDU response match: {bus_to_hddu_matches}/{bus_compare_count}")
    print(f"Active PE responses contain bus payloads: {pe_cover_matches}/{bus_compare_count}")
    print(
        f"{FOCUSED_PE_LABEL} repeat-period hint: "
        f"ln_pli={pe15_ln_period if pe15_ln_period is not None else 'none'} "
        f"pli={pe15_pli_period if pe15_pli_period is not None else 'none'} "
        f"plo={pe15_plo_period if pe15_plo_period is not None else 'none'}"
    )
    if pe15_plo_events:
        print(f"{FOCUSED_PE_LABEL} settled PLO burst-collapsed samples: {len(pe15_plo_events)}/{len(pe15_raw_plo_events)}")
        matched_downstream = sum(1 for item in pe15_downstream_alignments if item.hddu_event is not None)
        print(f"{FOCUSED_PE_LABEL} settled PLO -> downstream sequence matches: {matched_downstream}/{len(pe15_plo_events)}")
        state_text = ", ".join(
            f"{exe_a_state_name(state)}={count}" for state, count in sorted(pe15_plo_state_counts.items())
        )
        print(
            f"{FOCUSED_PE_LABEL} EXE_A repeat-period hint at PLO: "
            f"vaddu_sig={pe15_exe_a_vaddu_sig_period if pe15_exe_a_vaddu_sig_period is not None else 'none'} "
            f"vaddu_reg={pe15_exe_a_vaddu_reg_period if pe15_exe_a_vaddu_reg_period is not None else 'none'} "
            f"pr_vp_out={pe15_exe_a_pr_vp_out_period if pe15_exe_a_pr_vp_out_period is not None else 'none'} "
            f"pli_reg={pe15_exe_a_pli_reg_period if pe15_exe_a_pli_reg_period is not None else 'none'}"
        )
        print(
            f"{FOCUSED_PE_LABEL} EXE_A alignment at PLO: "
            f"state[{state_text if state_text else 'unknown'}] "
            f"plo==vaddu_reg {pe15_reg_matches}/{len(pe15_plo_events)} "
            f"plo==vaddu_sig {pe15_sig_matches}/{len(pe15_plo_events)} "
            f"plo==pr_vp_out {pe15_pr_vp_out_matches}/{len(pe15_plo_events)} "
            f"wait_plo_replay {pe15_wait_plo_replay_hits}/{len(pe15_plo_events)} "
            f"exec_sig {pe15_exec_sig_hits}/{len(pe15_plo_events)}"
        )

    if not spm_payload_decoded:
        print("Transport verdict: SPM payload decode is incomplete in this focused VCD, so use the printed HDDU/bank9 rows and EXE_A verdict instead of this transport summary line.")
    elif compare_count and hddu_to_spm_matches == compare_count and bank_matches == len(spm_events):
        print("Transport verdict: HDDU/SPM bank9 write path preserved the sampled payloads.")
    else:
        print("Transport verdict: mismatch appears between HDDU response and SPM bank9 write path.")

    if not bus_pe_decoded:
        print("Upstream verdict: active PE response decode is incomplete in this focused VCD slice; rely on the focused-PE/EXE_A sections for first-corruption localization.")
    elif bus_compare_count and bus_to_hddu_matches == bus_compare_count and pe_cover_matches == bus_compare_count:
        print("Upstream verdict: sampled bus aggregation already matches active PE return data; NoC/HDDU are unlikely the first corruption point.")
    else:
        print("Upstream verdict: sampled bus aggregation does not fully match PE return data; inspect MBUS/NoCRouter timing next.")

    if pe15_plo_period is not None and pe15_pli_period is not None:
        print(f"Focused-PE verdict: sampled {FOCUSED_PE_LABEL} ingress already shows a short repeating pattern before PLO emission; inspect the feed path into this PE before blaming EXE_A or NoC/HDDU.")
    elif pe15_plo_period is not None and pe15_ln_period is not None:
        print(f"Focused-PE verdict: {FOCUSED_PE_LABEL} local-network ingress already shows a short repeating pattern; corruption is upstream of this PE's compute/output buffering.")
    elif pe15_plo_period is not None:
        print(f"Focused-PE verdict: {FOCUSED_PE_LABEL} PLO repeats, but the sampled ingress did not show the same short repeat; inspect EXE_A / PE state next.")
    else:
        print(f"Focused-PE verdict: no short repeat period detected on sampled {FOCUSED_PE_LABEL} PLO events; inspect the raw event lists below.")

    if not pe15_plo_events:
        print(f"EXE_A verdict: no sampled {FOCUSED_PE_LABEL} PLO events were collected.")
    elif pe15_wait_plo_events >= 4 and pe15_wait_plo_replay_hits == pe15_wait_plo_events:
        print(
            f"EXE_A verdict: every settled {FOCUSED_PE_LABEL} PLO sample observed in S_WAIT_PLO replays vaddu_result_reg while vaddu_result_sig has already moved on; the repeated payload is being re-emitted from EXE_A's wait-for-PLO path, not recomputed upstream."
        )
    elif (
        pe15_plo_period is not None
        and pe15_exe_a_vaddu_reg_period == pe15_plo_period
        and pe15_exe_a_vaddu_sig_period != pe15_plo_period
        and pe15_wait_plo_replay_hits >= max(2, len(pe15_plo_events) // 4)
    ):
        print(
            f"EXE_A verdict: sampled {FOCUSED_PE_LABEL} PLO repeats align with latched vaddu_result_reg during S_WAIT_PLO, while vaddu_result_sig does not show the same period; the replay is most likely inside EXE_A output buffering/state handling."
        )
    elif (
        pe15_exec_events >= 4
        and (
            pe15_exe_a_vaddu_sig_period == pe15_plo_period
            or pe15_exe_a_pr_vp_out_period == pe15_plo_period
        )
        and (pe15_sig_matches or pe15_pr_vp_out_matches)
    ):
        print(
            f"EXE_A verdict: sampled {FOCUSED_PE_LABEL} PLO repeats already line up with vaddu_result_sig/pr_vp_out, so the repeated value is present before the final PLO buffering step; inspect the accumulator/VADDU feed next."
        )
    else:
        print(
            f"EXE_A verdict: internal snapshots narrowed the issue to EXE_A, but the replay-vs-compute split is still ambiguous; inspect the detailed PLO snapshots below."
        )

    print("\n[HDDU->SPM->BANK9 EVENTS]")
    for idx in range(min(max_events, compare_count)):
        hddu = hddu_events[idx]
        spm = spm_events[idx]
        print(
            f"#{idx:02d} hddu_t={hddu.time_ps:>10}ps spm_t={spm.time_ps:>10}ps "
            f"hddu={fmt_lanes(hddu.lanes)} spm_addr={fmt_hex32(spm.addr)} spm={fmt_lanes(spm.lanes)} "
            f"bank9_row={spm.bank9_row if spm.bank9_row is not None else 'unknown'} bank9_D={fmt_hex64(spm.bank9_data)}"
        )

    print("\n[BUS/PER-PE SNAPSHOTS]")
    for idx in range(min(max_events, len(bus_events))):
        event = bus_events[idx]
        port_text = ", ".join(
            f"port{resp['index']}={fmt_hex64(resp['data'])}/st{resp['status']}"
            for resp in event.port_responses
        )
        pe_text = ", ".join(
            f"slot{resp['slot']}~p{resp['approx_port']}pe{resp['approx_pe']}={fmt_hex64(resp['data'])}/st{resp['status']}"
            for resp in event.active_pe_responses[:8]
        )
        if len(event.active_pe_responses) > 8:
            pe_text += ", ..."
        print(
            f"#{idx:02d} t={event.time_ps:>10}ps pending_read={event.pending_read} ultra={event.ultra} "
            f"bus[{port_text}] pe[{pe_text if pe_text else 'none'}]"
        )

    print(f"\n[{FOCUSED_PE_LABEL.upper()} SNAPSHOTS]")
    for kind, _, data_alias in PE15_EVENT_SPECS:
        events = pe15_by_kind[kind]
        if not events:
            print(f"{kind}: none")
            continue
        width_bits = 64
        if data_alias == "pe15_pd_data":
            width_bits = 16
        sample = ", ".join(
            f"t={event.time_ps:>10}ps {fmt_hex(event.data, width_bits)} mode={route_mode_name(event.route_mode)}"
            for event in events[:max_events]
        )
        if len(events) > max_events:
            sample += ", ..."
        print(f"{kind}: {sample}")

    print(f"\n[{FOCUSED_PE_LABEL.upper()} EXE_A SETTLED PLO SNAPSHOTS]")
    if not pe15_plo_events:
        print("none")
    else:
        for event in pe15_plo_events[:max_events]:
            print(
                f"t={event.time_ps:>10}ps plo={fmt_hex64(event.data)} plo_ready={event.plo_ready if event.plo_ready is not None else 'unknown'} "
                f"state={exe_a_state_name(event.exe_a_state)} stall_pli={event.stall_port_pli if event.stall_port_pli is not None else 'unknown'} "
                f"stall_plo={event.stall_port_plo if event.stall_port_plo is not None else 'unknown'} buf_valid_next={event.plo_buf_valid_next if event.plo_buf_valid_next is not None else 'unknown'} "
                f"vaddu_sig={fmt_hex64(event.vaddu_result_sig)} vaddu_reg={fmt_hex64(event.vaddu_result_reg)} "
                f"pr_vp_in={fmt_hex64(event.pr_vp_in)} pr_vp_out={fmt_hex64(event.pr_vp_out)} pli_reg={fmt_hex64(event.pli_data_reg)}"
            )

    print(f"\n[{FOCUSED_PE_LABEL.upper()} SETTLED PLO TO DOWNSTREAM]")
    if not pe15_downstream_alignments:
        print("none")
    else:
        for item in pe15_downstream_alignments[:max_events]:
            hddu_lane0 = None if item.hddu_event is None else item.hddu_event.lanes[0]
            bank9_row = None if item.spm_event is None else item.spm_event.bank9_row
            bank9_data = None if item.spm_event is None else item.spm_event.bank9_data
            hddu_time = "unknown" if item.hddu_event is None else f"{item.hddu_event.time_ps:>10}ps"
            spm_time = "unknown" if item.spm_event is None else f"{item.spm_event.time_ps:>10}ps"
            print(
                f"#{item.sample_index:02d} plo_t={item.plo_event.time_ps:>10}ps plo={fmt_hex64(item.plo_event.data)} "
                f"state={exe_a_state_name(item.plo_event.exe_a_state)} "
                f"hddu_idx={item.hddu_index if item.hddu_index is not None else 'none'} hddu_t={hddu_time} hddu_lane0={fmt_hex64(hddu_lane0)} "
                f"spm_idx={item.spm_index if item.spm_index is not None else 'none'} spm_t={spm_time} bank9_row={bank9_row if bank9_row is not None else 'unknown'} bank9_D={fmt_hex64(bank9_data)}"
            )


def main() -> int:
    args = parse_args()
    fsdb_path = args.fsdb.resolve()
    if not fsdb_path.exists():
        raise FileNotFoundError(f"FSDB not found: {fsdb_path}")

    vcd_path = args.vcd.resolve() if args.vcd else fsdb_path.with_suffix(".cluster_return.vcd")
    if not args.reuse_vcd or not vcd_path.exists():
        build_focused_vcd(fsdb_path, vcd_path)

    signals = parse_header(vcd_path)
    hddu_events, spm_events, bus_events = collect_events(vcd_path, signals)
    pe15_events = collect_focused_pe_events(vcd_path, signals)

    print(f"[INFO] Focused VCD: {vcd_path}")
    print_summary(hddu_events, spm_events, bus_events, pe15_events, args.max_events)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())