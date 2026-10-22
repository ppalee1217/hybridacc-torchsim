"""Common Schedule IR v1: the committed schedule of one cc output directory (M20).

The event order comes from the cc-generated firmware itself. The firmware is built for
the host with ``HACC_HOST_TRACE`` (see ``templates/firmware_hw.h.j2``), run against a
recorder that answers every wait as complete, and the recorded register writes, waits
and wave markers are decoded here with the register protocol of the HybridAcc
hardware. Nothing re-derives the firmware's loops.

What the recorder's "complete" answers mean: they let extraction move on. The IR keeps
the issue order and, for every wait, what it waits for; it does not claim when anything
completes. Timing is the TLS's job.

Usage::

    python -m hybridacc_cc.schedule_ir CC_OUTPUT_DIR --platform PLATFORM.json -o schedule_ir.json

Every unknown register, hardware read outside a wait helper, missing wave marker,
unresolved wait target or missing required field raises ``ScheduleIRError``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import tempfile
from pathlib import Path

SCHEMA = "common-schedule-ir/v1"
DECODER_VERSION = "3"
BEAT_BYTES_FIELD = "dma_beat_bytes"
PLATFORM_FIELDS = ("clock_mhz", BEAT_BYTES_FIELD, "dma_cmd_fifo_depth")
FIRMWARE_FILES = ("firmware_ops.c", "firmware_data.c", "firmware_hw.h", "firmware_payload.h", "firmware_main.c")

RECORDER_C = r"""
#include <stdio.h>
#include "firmware_hw.h"

extern const LayerConfig layer_configs[];
void hacc_host_run_layers(void);

void hacc_host_write(uint32_t addr, uint32_t val) { printf("W %u %u\n", addr, val); }
uint32_t hacc_host_read(uint32_t addr) { printf("R %u\n", addr); return 0u; }
void hacc_host_wait(uint32_t kind, uint32_t arg) { printf("WAIT %u %u\n", kind, arg); }
void hacc_host_wave(uint32_t oc0, uint32_t ocn, uint32_t h, uint32_t w, uint32_t ic0, uint32_t icn) {
    printf("WAVE %u %u %u %u %u %u\n", oc0, ocn, h, w, ic0, icn);
}
void hacc_host_layer(uint32_t i) { printf("LAYER %u\n", i); }

int main(void) {
    printf("WAITKINDS %u %u %u %u %u %u %u %u\n",
           (unsigned)HACC_HOST_WAIT_DMA_IDLE, (unsigned)HACC_HOST_WAIT_DMA_TAG,
           (unsigned)HACC_HOST_WAIT_HDDU_DONE, (unsigned)HACC_HOST_WAIT_AGU_IDLE,
           (unsigned)HACC_HOST_WAIT_PE_HALTED, (unsigned)HACC_HOST_WAIT_NOC_QUIESCED,
           (unsigned)HACC_HOST_WAIT_CLUSTER_IDLE, (unsigned)HACC_HOST_WAIT_CLUSTER_QUIESCED);
    hacc_host_run_layers();
    return 0;
}
"""
WAIT_NAMES = ("dma_idle", "dma_tag", "hddu_done", "agu_idle", "pe_halted", "noc_quiesced",
              "cluster_idle", "cluster_quiesced")
PLANES = ("ps", "pd", "pli", "plo")


class ScheduleIRError(ValueError):
    """The committed schedule cannot be extracted without guessing."""


def _require(test, message):
    if not test:
        raise ScheduleIRError(message)


def sha256_file(path: Path) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode()).hexdigest()


# ── Register map ─────────────────────────────────────────────────────────────

_DEFINE = re.compile(r"^#define ([A-Z][A-Z0-9_]*)\s+(.+?)\s*(?:/\*.*)?$")


def parse_defines(header_text: str) -> dict:
    """Evaluate the integer ``#define``s of ``firmware_hw.h`` (the firmware's own register map)."""
    raw = {}
    for line in header_text.splitlines():
        m = _DEFINE.match(line)
        if m and "(" not in m.group(1):
            raw[m.group(1)] = m.group(2)
    values: dict = {}

    def value(name, depth=0):
        if name in values:
            return values[name]
        values[name] = None
        expr = re.sub(r"\b(0x[0-9A-Fa-f]+|\d+)[uU]\b", r"\1", raw[name])
        for token in set(re.findall(r"\b[A-Z][A-Z0-9_]*\b", expr)):
            if token not in raw or depth > 16:
                return None
            v = value(token, depth + 1)
            if v is None:
                return None
            expr = re.sub(rf"\b{token}\b", str(v), expr)
        if not re.fullmatch(r"[0-9xXa-fA-F()+\-*<>| ]+", expr):
            return None
        try:
            values[name] = int(eval(expr, {"__builtins__": {}}))  # noqa: S307 - digits and operators only
        except (SyntaxError, TypeError, ValueError):
            values[name] = None
        return values[name]

    for name in raw:
        value(name)
    return {k: v for k, v in values.items() if v is not None}


class RegisterMap:
    """Address decoding for the host-visible HybridAcc registers, built from ``firmware_hw.h``."""

    DMA_REGS = ("CAP0", "STATUS", "CTRL", "SRC_KIND", "DST_KIND", "SRC_ADDR_LO", "SRC_ADDR_HI",
                "DST_ADDR_LO", "DST_ADDR_HI", "SRC_CLUSTER_ID", "DST_CLUSTER_ID",
                "COUNT_D0", "COUNT_D1", "COUNT_D2", "COUNT_D3",
                "SRC_STRIDE_D0", "SRC_STRIDE_D1", "SRC_STRIDE_D2", "SRC_STRIDE_D3",
                "DST_STRIDE_D0", "DST_STRIDE_D1", "DST_STRIDE_D2", "DST_STRIDE_D3",
                "CMD_TAG", "DONE_TAG", "ERR_CODE", "ERR_INFO", "DEBUG_STATE", "XFORM_CTRL",
                "PAD_WINDOW_H0", "PAD_WINDOW_W0", "PAD_SRC_H", "PAD_SRC_W", "BEATS_PER_PIXEL",
                "FILL_VALUE_LO", "FILL_VALUE_HI", "EPILOGUE_CTRL", "EPILOGUE_PARAM0")
    DMA_READ_ONLY = ("CAP0", "STATUS", "DONE_TAG", "ERR_CODE", "ERR_INFO", "DEBUG_STATE")
    AGU_REGS = ("BASE_ADDR", "BASE_ADDR_H", "ITER01", "ITER23", "STRIDE0", "STRIDE1", "STRIDE2",
                "STRIDE3", "CTRL", "STATUS", "LANE_CFG", "TAG_BASE", "TAG_STRIDE0", "TAG_STRIDE1",
                "TAG_CTRL", "MASK_CFG")
    HDDU_REGS = ("CTRL", "STATUS", "PLANE_EN", "PLANE_MODE", "ERR_CODE", "ERR_INFO0", "ERR_INFO1",
                 "COUNTER_STALL")
    CLUSTER_REGS = ("SPM_CONFIG_MAP", "SPM_CONFIG_UPDATE", "NOC_CMD", "NOC_STATUS", "CLUSTER_MODE",
                    "CLUSTER_CTRL", "CLUSTER_STATUS", "CLUSTER_ERROR_CODE", "CLUSTER_SUBSTATE")
    TIMER_REGS = ("MSIP", "MTIMECMP_LO", "MTIMECMP_HI", "MTIME_LO", "MTIME_HI", "CTRL")
    LOCAL_REGS = ("LOCAL_CLUSTER_MASK_LO", "LOCAL_CLUSTER_MASK_HI")

    def __init__(self, header_text: str):
        d = parse_defines(header_text)
        self.d = d
        need = ("DMA_MMIO_BASE", "LOCAL_CTRL_BASE", "LOCAL_TIMER_BASE", "CLUSTER_UNICAST_BASE",
                "CLUSTER_BCAST_BASE", "CLUSTER_STRIDE", "HDDU_BASE", "AGU_BANK_STRIDE", "PLIC_BASE",
                "DMA_CTRL_SUBMIT", "HDDU_CTRL_START", "HDDU_CTRL_STOP", "HDDU_CTRL_SOFT_RESET",
                "CLUSTER_CTRL_START", "CLUSTER_CTRL_STOP", "CLUSTER_CTRL_SOFT_RESET",
                "DMA_ENDPOINT_DRAM", "DMA_ENDPOINT_CLUSTER_SPM", "DMA_XFORM_LOAD_PAD_EN_MASK")
        missing = [n for n in need if n not in d]
        _require(not missing, f"firmware_hw.h lacks register defines: {missing}")
        self.dma = {d[f"DMA_{r}"]: r for r in self.DMA_REGS}
        self.timer = {d[f"TIMER_{r}"]: r for r in self.TIMER_REGS}
        self.local = {d[r]: r for r in self.LOCAL_REGS}
        self.cluster = {d[r]: r for r in self.CLUSTER_REGS}
        self.cluster.update({d["HDDU_BASE"] + d[f"HDDU_{r}"]: f"HDDU_{r}" for r in self.HDDU_REGS})
        self.agu_off = {d[f"AGU_REG_{r}"]: r for r in self.AGU_REGS}
        self.noc_cmds = {d[n]: n[len("NOC_CMD_"):] for n in d
                         if n.startswith("NOC_CMD_") and n != "NOC_CMD" and isinstance(d[n], int)}
        self.plic_range = (d["PLIC_BASE"], d["PLIC_BASE"] + 0x2000)

    def classify(self, addr: int) -> tuple:
        """Return (space, target, register). ``target`` is 'bcast' or a cluster id."""
        d = self.d
        if addr in self.dma:
            return "dma", None, self.dma[addr]
        if addr in self.timer:
            return "timer", None, self.timer[addr]
        if addr in self.local:
            return "local", None, self.local[addr]
        if self.plic_range[0] <= addr < self.plic_range[1]:
            return "plic", None, hex(addr - self.plic_range[0])
        for base, target in ((d["CLUSTER_BCAST_BASE"], "bcast"), (d["CLUSTER_UNICAST_BASE"], "unicast")):
            span = (d["CLUSTER_STRIDE"] if target == "bcast"
                    else d["CLUSTER_BCAST_BASE"] - d["CLUSTER_UNICAST_BASE"])
            if base <= addr < base + span:
                cluster = "bcast" if target == "bcast" else (addr - base) // d["CLUSTER_STRIDE"]
                off = (addr - base) % d["CLUSTER_STRIDE"]
                if off in self.cluster:
                    return "cluster", cluster, self.cluster[off]
                agu = off - d["HDDU_BASE"]
                if 0 <= agu < 4 * d["AGU_BANK_STRIDE"] and agu % d["AGU_BANK_STRIDE"] in self.agu_off:
                    plane = PLANES[agu // d["AGU_BANK_STRIDE"]]
                    return "agu", cluster, f"{plane}.{self.agu_off[agu % d['AGU_BANK_STRIDE']]}"
                raise ScheduleIRError(f"unknown cluster register offset {off:#x} (address {addr:#010x})")
        raise ScheduleIRError(f"unknown register address {addr:#010x}")


# ── Recording ───────────────────────────────────────────────────────────────

def layer_runners(firmware_main: str) -> list:
    m = re.search(r"layer_runners\[\] = \{(.*?)\};", firmware_main, re.S)
    _require(m, "firmware_main.c has no layer_runners table")
    runners = [x.strip() for x in m.group(1).split(",") if x.strip()]
    _require(runners and all(re.fullmatch(r"run_layer_\w+", r) for r in runners), "unexpected layer_runners table")
    return runners


def record(cc_dir: Path, gcc: str = "gcc", timeout_s: int = 120) -> dict:
    """Build the firmware for the host with HACC_HOST_TRACE, run it, and return the raw record."""
    cc_dir = Path(cc_dir)
    runners = layer_runners((cc_dir / "firmware_main.c").read_text())
    glue = ('#include "firmware_hw.h"\nvoid hacc_host_layer(uint32_t i);\n'
            + "".join(f"void {r}(const LayerConfig* cfg);\n" for r in sorted(set(runners)))
            + "extern const LayerConfig layer_configs[];\nvoid hacc_host_run_layers(void) {\n"
            + "".join(f"    hacc_host_layer({i}u);\n    {r}(&layer_configs[{i}]);\n" for i, r in enumerate(runners))
            + "}\n")
    with tempfile.TemporaryDirectory(prefix="hacc_host_trace_") as tmp:
        tmp = Path(tmp)
        (tmp / "recorder.c").write_text(RECORDER_C)
        (tmp / "glue.c").write_text(glue)
        exe = tmp / "trace"
        build = subprocess.run([gcc, "-std=gnu11", "-O0", "-w", "-DHACC_HOST_TRACE", f"-I{cc_dir}",
                                str(cc_dir / "firmware_ops.c"), str(cc_dir / "firmware_data.c"),
                                str(tmp / "glue.c"), str(tmp / "recorder.c"), "-o", str(exe)],
                               capture_output=True, text=True)
        _require(build.returncode == 0, f"host build failed: {build.stderr[-2000:]}")
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=timeout_s)
        _require(run.returncode == 0, f"host run failed ({run.returncode}): {run.stderr[-2000:]}")
    version = subprocess.run([gcc, "--version"], capture_output=True, text=True).stdout.splitlines()[0]
    return {"lines": run.stdout.splitlines(), "runners": runners, "gcc": version,
            "recorder_sha256": sha256_text(RECORDER_C), "glue_sha256": sha256_text(glue)}


# ── Decoding ────────────────────────────────────────────────────────────────

def _dma_command(regs: dict, beat_bytes: int) -> dict:
    count = [regs.get(f"COUNT_D{i}", 0) for i in range(4)]
    beats = 1
    for c in count:
        beats *= c if c else 1  # DmaCommand::total_beats(): a zero count is one
    return {
        "tag": regs.get("CMD_TAG", 0),
        "src_kind": regs.get("SRC_KIND", 0), "dst_kind": regs.get("DST_KIND", 0),
        "src_addr": regs.get("SRC_ADDR_LO", 0) | (regs.get("SRC_ADDR_HI", 0) << 32),
        "dst_addr": regs.get("DST_ADDR_LO", 0) | (regs.get("DST_ADDR_HI", 0) << 32),
        "src_cluster": regs.get("SRC_CLUSTER_ID", 0), "dst_cluster": regs.get("DST_CLUSTER_ID", 0),
        "count": count,
        "src_stride": [regs.get(f"SRC_STRIDE_D{i}", 0) for i in range(4)],
        "dst_stride": [regs.get(f"DST_STRIDE_D{i}", 0) for i in range(4)],
        "beats": beats, "bytes": beats * beat_bytes,
        "transform": {k.lower(): regs.get(k, 0) for k in ("XFORM_CTRL", "PAD_WINDOW_H0", "PAD_WINDOW_W0",
                                                          "PAD_SRC_H", "PAD_SRC_W", "BEATS_PER_PIXEL",
                                                          "FILL_VALUE_LO", "FILL_VALUE_HI")},
        "epilogue": {"ctrl": regs.get("EPILOGUE_CTRL", 0), "param0": regs.get("EPILOGUE_PARAM0", 0)},
    }


def _int32(value: int) -> int:
    return value - (1 << 32) if value & 0x80000000 else value


def _agu_addresses(base: int, count: list, stride: list):
    """ESL DmaEngine AGU: d0 fastest, a zero count is one, 32-bit address arithmetic."""
    lim = [c if c else 1 for c in count]
    for i3 in range(lim[3]):
        for i2 in range(lim[2]):
            for i1 in range(lim[1]):
                for i0 in range(lim[0]):
                    off = sum((i * s) & 0xFFFFFFFF for i, s in zip((i0, i1, i2, i3), stride))
                    yield (base + off) & 0xFFFFFFFF


def dram_beats(cmd: dict, beat_bytes: int, dram_kind: int, load_pad_bit: int):
    """Yield ("R"|"W", address) for every DRAM beat of one DMA command, in ESL issue order.

    Mirrors HybridAcc ESL ``DmaEngine.hpp`` (bb4be74): only the low 32 address bits are used.
    Every beat is listed; the engine groups consecutive beats into DRAM bursts
    (``Core/DramBurst.hpp``), which changes neither the beats nor their order. A load-pad read ignores the source strides and addresses a
    dense [pad_src_h, pad_src_w, beats_per_pixel] image at the source address; beats outside
    the image are synthesised and read nothing (``decode_load_pad_request``).
    """
    t = cmd["transform"]
    if cmd["src_kind"] == dram_kind:
        base = cmd["src_addr"] & 0xFFFFFFFF
        if t["xform_ctrl"] & load_pad_bit:
            row_beats = cmd["count"][0] or 1
            bpp = t["beats_per_pixel"]
            h0, w0 = _int32(t["pad_window_h0"]), _int32(t["pad_window_w0"])
            for i in range(cmd["beats"]):
                row, in_row = divmod(i, row_beats)
                col, beat = divmod(in_row, bpp)
                r, c = h0 + row, w0 + col
                if 0 <= r < t["pad_src_h"] and 0 <= c < t["pad_src_w"]:
                    yield "R", (base + ((r * t["pad_src_w"] + c) * bpp + beat) * beat_bytes) & 0xFFFFFFFF
        else:
            for a in _agu_addresses(base, cmd["count"], cmd["src_stride"]):
                yield "R", a
    if cmd["dst_kind"] == dram_kind:
        for a in _agu_addresses(cmd["dst_addr"] & 0xFFFFFFFF, cmd["count"], cmd["dst_stride"]):
            yield "W", a


def _dram_side(cmd: dict, beat_bytes: int, d: dict) -> dict:
    """DRAM-side amount and address segments of one command (M20 B2).

    ``beats``/``bytes`` of a command are what the SPM side delivers or consumes; the DRAM side
    differs for a load-pad read, whose out-of-window beats read nothing.
    """
    beats = list(dram_beats(cmd, beat_bytes, d["DMA_ENDPOINT_DRAM"], d["DMA_XFORM_LOAD_PAD_EN_MASK"]))
    reads = sum(1 for kind, _ in beats if kind == "R")
    return {"read_beats": reads, "write_beats": len(beats) - reads,
            "read_bytes": reads * beat_bytes, "write_bytes": (len(beats) - reads) * beat_bytes,
            "segments": dram_segments(beats, beat_bytes)}


def dram_segments(beats, beat_bytes: int) -> list:
    """Merge consecutive same-direction beats at consecutive addresses into [dir, start, bytes]."""
    out = []
    for kind, addr in beats:
        if out and out[-1][0] == kind and out[-1][1] + out[-1][2] == addr:
            out[-1][2] += beat_bytes
        else:
            out.append([kind, addr, beat_bytes])
    return out


def _tag_reached(done, tag):
    return ((done - tag) & 0xFFFFFFFF) < 0x80000000  # firmware: (int32_t)(done - tag) >= 0


def decode(lines: list, regmap: RegisterMap, platform: dict) -> dict:
    """Decode recorder output into per-layer event lists (issue order) with wait targets."""
    _require(lines and lines[0].startswith("WAITKINDS "), "recorder output lacks the wait-kind table")
    kinds = dict(zip(map(int, lines[0].split()[1:]), WAIT_NAMES))
    _require(len(kinds) == len(WAIT_NAMES), "wait kinds are not distinct")
    d = regmap.d
    beat_bytes = platform[BEAT_BYTES_FIELD]
    fifo_depth = platform["dma_cmd_fifo_depth"]
    layers, cur = [], None
    dma_shadow: dict = {}
    agu_shadow: dict = {}
    cluster_state: dict = {}
    pending_wave = None
    unwaited: list = []

    def new_layer(index):
        return {"index": index, "events": [], "execution_waves": [], "dma": [], "waits": [],
                "control": {}, "max_unwaited_dma": 0}

    for n, line in enumerate(lines[1:], start=2):
        f = line.split()
        op = f[0]
        if op == "LAYER":
            index = int(f[1])
            _require(index == len(layers), f"line {n}: layer {index} out of order")
            _require(pending_wave is None, f"line {n}: wave marker without an HDDU start")
            cur = new_layer(index)
            layers.append(cur)
            unwaited: list = []
            continue
        _require(cur is not None, f"line {n}: event before the first layer")
        ev_id = len(cur["events"])
        if op == "R":
            raise ScheduleIRError(f"line {n}: hardware read of {int(f[1]):#010x} outside a wait helper "
                                  "(control flow could depend on timing)")
        if op == "WAVE":
            _require(pending_wave is None, f"line {n}: two wave markers without an HDDU start")
            pending_wave = list(map(int, f[1:]))
            continue
        if op == "WAIT":
            kind = kinds.get(int(f[1]))
            _require(kind is not None, f"line {n}: unknown wait kind {f[1]}")
            arg = int(f[2])
            wait = {"id": ev_id, "kind": kind, "arg": arg}
            if kind == "dma_tag":
                issued = [x for x in cur["dma"] if x["tag"] == arg]
                _require(issued, f"line {n}: waits for DMA tag {arg} that this layer never issued")
                wait["target_dma"] = issued[-1]["id"]
                unwaited = [x for x in unwaited if not _tag_reached(arg, cur["dma"][x]["tag"])]
            elif kind == "dma_idle":
                wait["target_dma"] = cur["dma"][-1]["id"] if cur["dma"] else None
                unwaited = []
            elif kind == "hddu_done":
                _require(cur["execution_waves"], f"line {n}: waits for an HDDU run that never started")
                wait["target_wave"] = cur["execution_waves"][-1]["id"]
            cur["waits"].append(wait)
            cur["events"].append({"id": ev_id, "type": "wait", "wait": len(cur["waits"]) - 1})
            continue
        _require(op == "W", f"line {n}: unknown record {op!r}")
        addr, val = int(f[1]), int(f[2])
        space, target, reg = regmap.classify(addr)
        is_start = space == "cluster" and reg == "HDDU_CTRL" and val & d["HDDU_CTRL_START"]
        # F2 ruling (2026-10-18): a START written together with STOP or SOFT_RESET is outside the
        # measurement contract; no specification defines which one wins.
        _require(not is_start or val == d["HDDU_CTRL_START"],
                 f"line {n}: HDDU start written together with other HDDU_CTRL bits {val:#x}")
        _require(pending_wave is None or is_start, f"line {n}: wave marker not immediately before an HDDU start")
        if space == "dma":
            _require(reg not in RegisterMap.DMA_READ_ONLY, f"line {n}: write to read-only DMA register {reg}")
            if reg == "CTRL":
                _require(val & ~(d["DMA_CTRL_SUBMIT"] | 0x8) == 0, f"line {n}: unknown DMA_CTRL bits {val:#x}")
                if val & d["DMA_CTRL_SUBMIT"]:
                    cmd = _dma_command(dma_shadow, beat_bytes)
                    cmd["dram_side"] = _dram_side(cmd, beat_bytes, d)
                    if cur["dma"]:
                        prev = cur["dma"][-1]["tag"]
                        _require(_tag_reached(cmd["tag"], prev), f"line {n}: DMA tags go backwards")
                    cmd["id"] = len(cur["dma"])
                    cmd["event"] = ev_id
                    cur["dma"].append(cmd)
                    unwaited.append(cmd["id"])
                    cur["max_unwaited_dma"] = max(cur["max_unwaited_dma"], len(unwaited))
                    cur["events"].append({"id": ev_id, "type": "dma_submit", "dma": cmd["id"]})
                continue
            dma_shadow[reg] = val
            continue
        if space == "agu":
            agu_shadow[(target, reg)] = val
            continue
        if space == "cluster":
            if reg == "HDDU_CTRL":
                if is_start:
                    _require(pending_wave is not None, f"line {n}: HDDU start without a wave marker")
                    oc0, ocn, h, w, ic0, icn = pending_wave
                    pending_wave = None
                    covers = [[oc, h, w, ic] for oc in range(oc0, oc0 + ocn) for ic in range(ic0, ic0 + icn)]
                    agu = {p: {r: v for (t, r2), v in agu_shadow.items() if t == target and r2.startswith(p + ".")
                               for r in [r2.split(".", 1)[1]]} for p in PLANES}
                    wave = {"id": len(cur["execution_waves"]), "event": ev_id, "target": target, "covers": covers,
                            "agu": agu,
                            "plane_en": cluster_state.get((target, "HDDU_PLANE_EN")),
                            "plane_mode": cluster_state.get((target, "HDDU_PLANE_MODE")),
                            "spm_config_map": cluster_state.get((target, "SPM_CONFIG_MAP"))}
                    _require(None not in (wave["plane_en"], wave["plane_mode"], wave["spm_config_map"]),
                             f"line {n}: HDDU start before plane enable, plane mode or SPM map were written")
                    cur["execution_waves"].append(wave)
                    cur["events"].append({"id": ev_id, "type": "hddu_start", "wave": wave["id"]})
                else:
                    _require(val & (d["HDDU_CTRL_STOP"] | d["HDDU_CTRL_SOFT_RESET"]) and
                             not val & ~(d["HDDU_CTRL_STOP"] | d["HDDU_CTRL_SOFT_RESET"]),
                             f"line {n}: unknown HDDU_CTRL value {val:#x}")
                    cur["events"].append({"id": ev_id, "type": "hddu_ctrl", "target": target, "value": val})
                continue
            if reg == "CLUSTER_CTRL":
                names = {d["CLUSTER_CTRL_START"]: "start", d["CLUSTER_CTRL_STOP"]: "stop",
                         d["CLUSTER_CTRL_SOFT_RESET"]: "soft_reset"}
                _require(val in names, f"line {n}: unknown CLUSTER_CTRL value {val:#x}")
                cur["events"].append({"id": ev_id, "type": "cluster_" + names[val], "target": target})
                continue
            if reg == "NOC_CMD":
                code = val & 0xF
                _require(code in regmap.noc_cmds, f"line {n}: unknown NoC command code {code}")
                cur["events"].append({"id": ev_id, "type": "noc_command", "target": target,
                                      "command": regmap.noc_cmds[code], "param": val & 0xFFFFFFF0})
                continue
            _require(reg in ("SPM_CONFIG_MAP", "SPM_CONFIG_UPDATE", "CLUSTER_MODE", "HDDU_PLANE_EN",
                             "HDDU_PLANE_MODE"), f"line {n}: write to cluster register {reg}")
            cluster_state[(target, reg)] = val
            if reg == "SPM_CONFIG_UPDATE":
                cur["events"].append({"id": ev_id, "type": "spm_config_update", "target": target,
                                      "map": cluster_state.get((target, "SPM_CONFIG_MAP"))})
            continue
        # Timer, local cluster mask and PLIC are CPU-side control; counted, not scheduled.
        key = f"{space}.{reg}"
        cur["control"][key] = cur["control"].get(key, 0) + 1
    _require(pending_wave is None, "trailing wave marker without an HDDU start")
    for layer in layers:
        layer["dma_fifo_hazard"] = layer["max_unwaited_dma"] > fifo_depth
        _overlap(layer, d["DMA_ENDPOINT_CLUSTER_SPM"])
    return {"layers": layers}


def _overlap(layer: dict, spm_kind: int):
    """Structural compute/DMA overlap, read off the issue order (ODR R-5).

    For each execution wave: the DMA submitted after its HDDU start and before the wait for it to finish
    (``dma_during_run``) and their directions (load into SPM, store from SPM). ``overlap_windows`` counts one
    window per direction per wave, the count the X0 criterion S defines; here it comes from the firmware's
    own issue order instead of a re-implementation of the template's branches.
    """
    done = {w["target_wave"]: w["id"] for w in layer["waits"] if w["kind"] == "hddu_done"}
    windows = 0
    for wave in layer["execution_waves"]:
        end = done.get(wave["id"], len(layer["events"]))
        ids = [e["dma"] for e in layer["events"][wave["event"] + 1:end] if e["type"] == "dma_submit"]
        dirs = set()
        for i in ids:
            c = layer["dma"][i]
            dirs.add("load" if c["dst_kind"] == spm_kind else "store" if c["src_kind"] == spm_kind else "other")
        wave["dma_during_run"] = ids
        wave["overlap_directions"] = sorted(dirs)
        windows += len(dirs)
    layer["overlap_windows"] = windows


# ── IR assembly ─────────────────────────────────────────────────────────────

def _grid_check(layer_ir: dict, hw_layer: dict):
    t = hw_layer["tiling_params"]
    grid = {(oc, h, w, ic) for oc in range(t["num_oc_tiles"]) for h in range(t["num_h_tiles"])
            for w in range(t["num_w_tiles"]) for ic in range(t["num_ic_tiles"])}
    seen: dict = {}
    for wave in layer_ir["execution_waves"]:
        for c in map(tuple, wave["covers"]):
            seen[c] = seen.get(c, 0) + 1
    _require(set(seen) == grid and all(v == 1 for v in seen.values()),
             f"layer {layer_ir['index']}: execution waves do not cover the tile grid exactly once")


def build(cc_dir: Path, platform: dict, platform_ref: dict, gcc: str = "gcc") -> dict:
    cc_dir = Path(cc_dir)
    missing = [k for k in PLATFORM_FIELDS if not isinstance(platform.get(k), int) or platform[k] <= 0]
    _require(not missing, f"platform constants missing or not positive integers: {missing}")
    hw_ir = json.loads((cc_dir / "hardware_ir.json").read_text())
    for key in ("hardware", "config_fingerprint", "layers"):
        _require(key in hw_ir, f"hardware_ir.json lacks {key}")
    regmap = RegisterMap((cc_dir / "firmware_hw.h").read_text())
    rec = record(cc_dir, gcc=gcc)
    decoded = decode(rec["lines"], regmap, platform)
    _require(len(decoded["layers"]) == len(hw_ir["layers"]) == len(rec["runners"]),
             "layer count differs between the trace, hardware_ir.json and firmware_main.c")
    layers = []
    for layer_ir, hw_layer, runner in zip(decoded["layers"], hw_ir["layers"], rec["runners"]):
        _require(runner == f"run_layer_{hw_layer['op_type']}",
                 f"layer {layer_ir['index']}: runner {runner} does not match op {hw_layer['op_type']}")
        _require(layer_ir["execution_waves"], f"layer {layer_ir['index']}: no HDDU run")
        _grid_check(layer_ir, hw_layer)
        layer_ir["demand"] = {
            "op_type": hw_layer["op_type"], "name": hw_layer["name"],
            "loop_bounds": hw_layer["tiling"]["loop_bounds"],
            "tile_grid": {k: hw_layer["tiling_params"][f"num_{k}_tiles"] for k in ("oc", "h", "w", "ic")},
            # cc's own count; it equals the number of HDDU runs except on GEMM resident paths,
            # where it counts tiles. Recorded, not relied on.
            "cc_total_waves": hw_layer["tiling"]["total_waves"],
            "gemm_k_wave": hw_layer.get("gemm_k_wave"),
        }
        layers.append(layer_ir)
    files = {name: sha256_file(cc_dir / name) for name in FIRMWARE_FILES + ("hardware_ir.json",)}
    return {
        "schema": SCHEMA,
        "resource": {"hardware": hw_ir["hardware"], "platform": {k: platform[k] for k in PLATFORM_FIELDS}},
        "provenance": {
            "config_fingerprint": hw_ir["config_fingerprint"],
            "workload_name": hw_ir.get("workload_name"),
            "files_sha256": files,
            "platform": platform_ref,
            "extraction": {"decoder_version": DECODER_VERSION, "decoder_sha256": sha256_file(Path(__file__)),
                           "recorder_sha256": rec["recorder_sha256"], "glue_sha256": rec["glue_sha256"],
                           "host_gcc": rec["gcc"],
                           "meaning": "issue order and wait targets from a host run of the firmware; "
                                      "every wait was answered as complete; no timing"},
        },
        "layers": layers,
    }


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("cc_dir", type=Path)
    p.add_argument("--platform", type=Path, required=True,
                   help=f"JSON with integer fields {', '.join(PLATFORM_FIELDS)}")
    p.add_argument("-o", "--out", type=Path, required=True, help="new file")
    p.add_argument("--gcc", default="gcc")
    a = p.parse_args(argv)
    platform = json.loads(a.platform.read_text())
    ir = build(a.cc_dir, platform, {"path": str(a.platform), "sha256": sha256_file(a.platform)}, gcc=a.gcc)
    with a.out.open("x") as handle:
        json.dump(ir, handle, indent=1, sort_keys=True)
        handle.write("\n")
    waves = sum(len(l["execution_waves"]) for l in ir["layers"])
    dma = sum(len(l["dma"]) for l in ir["layers"])
    print(json.dumps({"out": str(a.out), "layers": len(ir["layers"]), "execution_waves": waves, "dma": dma}))


if __name__ == "__main__":
    main()
