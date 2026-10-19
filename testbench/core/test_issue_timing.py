"""Raw firmware issue timing from ESL logs (hybridacc_cc.issue_timing, M21; C-05 contract v1)."""
import pytest

from hybridacc_cc import schedule_ir as si
from hybridacc_cc import issue_timing as it

from test_wave_profile import C, cc_dir, d  # noqa: F401  (fixtures: a generated firmware_hw.h)


def _ir():
    waits = [{"id": 2, "kind": "dma_tag", "arg": 5, "target_dma": 0},
             {"id": 3, "kind": "hddu_done", "arg": 1, "target_wave": 0}]
    events = [{"id": 0, "type": "dma_submit", "dma": 0}, {"id": 1, "type": "hddu_start", "wave": 0},
              {"id": 2, "type": "wait", "wait": 0}, {"id": 3, "type": "wait", "wait": 1},
              {"id": 4, "type": "hddu_ctrl", "target": "bcast", "value": 2}]
    return {"resource": {"platform": {"clock_mhz": 500}},
            "layers": [{"index": 0, "events": events, "execution_waves": [{"id": 0, "event": 1}],
                        "dma": [{"id": 0, "tag": 5, "event": 0}], "waits": waits}]}


def _logs(d, done_at=140, extra_write=False, extra_read=None):
    """Writes: DMA submit (request 100), START (request 110, accepted 113), STOP (request 260).
    Reads: DONE_TAG polls at 120/130 (old tag), 150 (tag 5); HDDU_STATUS polls 200 (busy), 230 (done)."""
    u, b = d["CLUSTER_UNICAST_BASE"], d["CLUSTER_BCAST_BASE"]
    hs = u + d["HDDU_BASE"] + d["HDDU_STATUS"]
    ctrl = b + d["HDDU_BASE"] + d["HDDU_CTRL"]
    reads = [(d["DMA_DONE_TAG"], 4, 120), (d["DMA_DONE_TAG"], 4, 130), (d["DMA_DONE_TAG"], 5, 150),
             (hs, 0, 200), (hs, d["HDDU_STATUS_DONE"], 230)]
    if extra_read:
        reads.append((hs, d["HDDU_STATUS_DONE"], extra_read))
    writes = [(d["DMA_SRC_ADDR_LO"], 0, 90), (d["DMA_CTRL"], d["DMA_CTRL_SUBMIT"], 100),
              (ctrl, d["HDDU_CTRL_START"], 110), (ctrl, d["HDDU_CTRL_STOP"], 260)]
    if extra_write:
        writes.append((d["DMA_CTRL"], d["DMA_CTRL_SUBMIT"], 270))
    mmio = sorted([("W", a, v, t - 2) for a, v, t in writes] + [("R", a, 0, t - 2) for a, _v, t in reads],
                  key=lambda x: x[3])
    mmio_log = "".join(f"{k} {a} {v} 15 {t * C}\n" for k, a, v, t in mmio)
    wave = ["T hddu=0 epoch=0 cause=reset t=0", f"S hddu=0 epoch=0 seq=1 g=1 plane_en=15 plane_mode=2 t={111 * C}"]
    wave += [f"M addr={a} data={v} t={t * C}" for a, v, t in reads]
    wave.append(f"E hddu=0 epoch=0 seq=1 g=1 start={111 * C} agu_idle={180 * C} drained={185 * C} "
                f"answered={185 * C} done={186 * C} stop={258 * C} error=- close=end ps=1 pd=1 pli=1 plo_req=1 "
                f"plo_resp=1 plo_spm=1 plo_ack=1 last_ps=- last_pd=- last_pli=- last_plo_spm=- overlap=0 t={300 * C}")
    dram = f"C 5 0 1 8 {100 * C}\nR 2147483648 {101 * C}\nD 5 0 {done_at * C}\n"
    return "\n".join(wave) + "\n", mmio_log, dram


def _build(cc_dir, d, **kw):
    w, m, dr = _logs(d, **kw)
    return it.build(cc_dir, _ir(), "0" * 64, w, m, dr, "test")


def test_contract_edges(cc_dir, d):
    t = _build(cc_dir, d)
    ev = {e["event"]: e for e in t["events"]}
    assert t["origin_ps"] == 98 * C and t["boot_prefix_cycles"] == 98
    assert ev[0]["issue_cycles"] == 0 and ev[0]["accept_latency"] == 2
    # hddu_start issues at the accepted START (111), 13 cycles after the DMA submit's request (98)
    assert ev[1]["issue_edge"] == 111 - 98 and ev[1]["issue_cycles"] == 13 and ev[1]["accept_latency"] == 3
    # dma_tag: first poll requested at 118; the command completed at 140; poll seen at 150 -> reaction 10
    assert ev[2]["issue_cycles"] == 118 - 111 and ev[2]["release_edge"] == 140 - 98
    assert ev[2]["reaction_cycles"] == 10 and ev[2]["polls"] == 3
    # hddu_done: first poll requested at 198 = 140 + 10 reaction + 48 work; released at the poll seen at 230
    assert ev[3]["issue_cycles"] == 58 and ev[3]["work_cycles"] == 48
    assert ev[3]["release_edge"] == 230 - 98 and ev[3]["hw_edge"] == 186 - 98
    assert ev[4]["issue_cycles"] == 258 - 230
    assert t["terminal_tail_cycles"] == 300 - 258


def test_poll_after_completion_releases_at_issue(cc_dir, d):
    ev = {e["event"]: e for e in _build(cc_dir, d, done_at=115)["events"]}
    assert ev[2]["release_edge"] == 118 - 98 and ev[2]["reaction_cycles"] == 150 - 118


@pytest.mark.parametrize("kw,msg", [
    ({"extra_write": True}, "beyond the IR"),
    ({"extra_read": 240}, "belongs to no wait"),
    ({"extra_read": 280}, "reads left"),
    ({"done_at": 160}, "before the condition held"),
])
def test_refuses_what_the_ir_does_not_explain(cc_dir, d, kw, msg):
    with pytest.raises(si.ScheduleIRError, match=msg):
        _build(cc_dir, d, **kw)


def test_refuses_missing_completion(cc_dir, d):
    w, m, dr = _logs(d)
    with pytest.raises(si.ScheduleIRError, match="no completion logged|beyond the IR"):
        it.build(cc_dir, _ir(), "0" * 64, w, m, dr.replace("D 5 0", "X 5 0"), "test")
