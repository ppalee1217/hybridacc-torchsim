"""Raw per-wave profile from an ESL --wave-log (hybridacc_cc.wave_profile, M21)."""
import dataclasses
import json

import pytest
import yaml

from hybridacc_cc import schedule_ir as si
from hybridacc_cc import wave_profile as wp
from hybridacc_cc.codegen import generate_firmware
from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload

from test_schedule_ir import KERNEL_DIR

C = 2000  # ps per 500 MHz cycle


@pytest.fixture(scope="module")
def cc_dir(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("wp")
    path = tmp / "gemm.yaml"
    path.write_text(yaml.safe_dump({
        "name": "gemm",
        "hardware": {"num_clusters": 1, "num_pes": 12, "num_bus": 3, "spm_banks_per_group": 3,
                     "spm_bank_depth": 8192, "dram_base": 0x80000000},
        "tensors": {"A": {"shape": [48, 64], "dtype": "fp16"}, "B": {"shape": [64, 24], "dtype": "fp16"},
                    "C": {"shape": [48, 24], "dtype": "fp16"}},
        "ops": [{"name": "gemm1", "type": "gemm", "inputs": ["A", "B"], "outputs": ["C"]}]}))
    hir = lower_workload(parse_workload(path))
    out = tmp / "cc"
    generate_firmware(hir, out, kernel_json_dir=KERNEL_DIR)
    (out / "hardware_ir.json").write_text(json.dumps(dataclasses.asdict(hir)))
    return out


@pytest.fixture(scope="module")
def d(cc_dir):
    return si.RegisterMap((cc_dir / "firmware_hw.h").read_text()).d


def _ir():
    waits = [{"id": 1, "kind": "hddu_done", "arg": 1}, {"id": 2, "kind": "agu_idle", "arg": 1},
             {"id": 3, "kind": "cluster_quiesced", "arg": 1}, {"id": 4, "kind": "dma_tag", "arg": 5}]
    events = [{"id": 0, "type": "hddu_start", "wave": 0}] + [
        {"id": w["id"], "type": "wait", "wait": i} for i, w in enumerate(waits)]
    return {"resource": {"platform": {"clock_mhz": 500}},
            "layers": [{"index": 0, "events": events, "execution_waves": [{"id": 0}], "waits": waits}]}


def _addr(d):
    u = d["CLUSTER_UNICAST_BASE"]
    agu = lambda p: u + d["HDDU_BASE"] + p * d["AGU_BANK_STRIDE"] + d["AGU_REG_STATUS"]
    return {"hs": u + d["HDDU_BASE"] + d["HDDU_STATUS"], "agu": agu, "cs": u + d["CLUSTER_STATUS"],
            "timer": d["TIMER_MTIME_LO"],
            "probe": [u + d["HDDU_BASE"] + d[r] for r in ("HDDU_ERR_CODE", "HDDU_ERR_INFO0", "HDDU_ERR_INFO1",
                                                           "HDDU_COUNTER_STALL")] + [agu(p) for p in range(4)]}


def _m(a, v, t):
    return f"M addr={a} data={v} t={t * C}"


def _log(d, hddu=None, extra=(), status_error=False):
    a = _addr(d)
    done = d["HDDU_STATUS_ERROR"] if status_error else d["HDDU_STATUS_DONE"]
    if hddu is None:  # three spins, no probe (QUIESCED clear), done on the third
        hddu = [_m(a["hs"], 0, 150), _m(a["timer"], 7, 151), _m(a["hs"], 0, 190), _m(a["timer"], 7, 191),
                _m(a["hs"], done, 230)]
    agu = [_m(a["agu"](0), 2, 240), _m(a["agu"](1), 0, 241), _m(a["agu"](2), 0, 242), _m(a["agu"](3), 0, 243),
           _m(a["timer"], 7, 244),
           _m(a["agu"](0), 0, 260), _m(a["agu"](1), 0, 261), _m(a["agu"](2), 0, 262), _m(a["agu"](3), 0, 263)]
    quiesce = [f"C hddu=0 cond=cluster_quiesced value=1 t={280 * C}",
               _m(a["cs"], 0, 270), _m(a["timer"], 7, 271), _m(a["cs"], d["CLUSTER_STATUS_QUIESCED"], 295)]
    tag = [_m(d["DMA_DONE_TAG"], 4, 300), _m(d["DMA_DONE_TAG"], 5, 302)]
    end = [f"E hddu=0 epoch=0 seq=1 g=1 start={100 * C} agu_idle={210 * C} drained={215 * C} "
           f"answered={215 * C} done={216 * C} stop={235 * C} error=- close=end ps=1 pd=1 pli=1 "
           f"plo_req=1 plo_resp=1 plo_spm=1 plo_ack=1 last_ps=- last_pd=- last_pli=- last_plo_spm=- "
           f"overlap=0 t={400 * C}"]
    lines = ["T hddu=0 epoch=0 cause=reset t=0", f"S hddu=0 epoch=0 seq=1 g=1 plane_en=15 plane_mode=2 t={100 * C}"]
    return "\n".join(lines + hddu + agu + quiesce + tag + list(extra) + end) + "\n"


def test_tokens_are_poll_success_minus_start(cc_dir, d):
    prof = wp.build(cc_dir, _ir(), "x" * 64, _log(d), "test")
    assert prof["schema"] == wp.SCHEMA and prof["kind"] == "raw" and prof["clock_mhz"] == 500
    (row,) = prof["waves"]
    assert row["tokens"] == {"hddu_done": 130, "agu_idle": 163, "cluster_quiesced": 195}
    assert row["observed"]["hddu_done"]["condition_minus_start_cycles"] == 116
    assert row["observed"]["agu_idle"]["condition_minus_start_cycles"] == 110
    assert row["observed"]["cluster_quiesced"]["condition_minus_start_cycles"] == 180


def test_debug_probe_after_the_successful_read_is_consumed(cc_dir, d):
    # Spin 0 with QUIESCED set: the firmware reads eight diagnostic registers right after the status read,
    # before it tests DONE, so they follow the successful read.
    a = _addr(d)
    hddu = [_m(a["hs"], d["HDDU_STATUS_DONE"] | d["HDDU_STATUS_QUIESCED"], 120)] + [
        _m(x, 0, 121 + k) for k, x in enumerate(a["probe"])]
    prof = wp.build(cc_dir, _ir(), "x" * 64, _log(d, hddu=hddu), "test")
    assert prof["waves"][0]["tokens"]["hddu_done"] == 20


@pytest.mark.parametrize("case,message", [
    ("unexplained_read", "unexpected read"),
    ("leftover_read", "after the last wait"),
    ("error_status", "error status"),
    ("rejected_start", "outside the measurement contract"),
    ("missing_wave", "IR execution waves"),
])
def test_refuses(cc_dir, d, case, message):
    ir, log = _ir(), _log(d)
    if case == "unexplained_read":
        a = _addr(d)
        log = _log(d, hddu=[_m(a["hs"], 0, 150), _m(d["DMA_STATUS"], 1, 152), _m(a["hs"], d["HDDU_STATUS_DONE"], 230)])
    elif case == "leftover_read":
        log = _log(d, extra=[f"M addr={d['DMA_DONE_TAG']} data=5 t={310 * C}"])
    elif case == "error_status":
        log = _log(d, status_error=True)
    elif case == "rejected_start":
        log += "R hddu=0 epoch=0 stop=1 soft_reset=0 error=0 no_plane=0 t=0\n"
    elif case == "missing_wave":
        ir["layers"][0]["execution_waves"].append({"id": 1})
    with pytest.raises(si.ScheduleIRError, match=message):
        wp.build(cc_dir, ir, "x" * 64, log, "test")
