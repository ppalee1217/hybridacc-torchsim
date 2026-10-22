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


# ── schema v2 (M21 WP-I) ──────────────────────────────────────────────────────

@pytest.fixture
def v2_dir(cc_dir, tmp_path):
    """The fixture's cc output plus what v2 binds: CLI-form hardware_ir.json, wave_map.json, an ELF, the
    toolchain record and a platform file; and an IR stub whose provenance names them."""
    import shutil
    from hybridacc_cc import cli
    from hybridacc_cc import wave_map as wm
    out = tmp_path / "cc"
    shutil.copytree(cc_dir, out)
    hir = lower_workload(parse_workload(cc_dir.parent / "gemm.yaml"))
    text = cli._ir_json(hir)
    (out / "hardware_ir.json").write_text(text)
    (out / "wave_map.json").write_text(json.dumps(wm.build(hir, text)))
    (out / "firmware.elf").write_bytes(b"\x7fELF test")
    (out / "toolchain_provenance.json").write_text("{}")
    platform = tmp_path / "platform.json"
    platform.write_text(json.dumps({"clock_mhz": 500}))
    ir = _ir()
    ir["provenance"] = {"files_sha256": {"hardware_ir.json": si.sha256_file(out / "hardware_ir.json")},
                        "platform": {"sha256": si.sha256_file(platform)},
                        "config_fingerprint": {"hash": "hac-v1-test"}}
    ir["layers"][0]["execution_waves"][0]["covers"] = [[0, 0, 0, 0]]
    return out, ir, platform


def test_v2_keeps_the_rows_and_binds_the_provenance(v2_dir, d, tmp_path):
    out, ir, platform = v2_dir
    v1 = wp.build(out, ir, "x" * 64, _log(d), "test")
    v1_path = tmp_path / "v1.json"
    v1_path.write_text(json.dumps(v1))
    v2 = wp.to_v2(v1, out, ir, "a" * 64, platform, from_v1=v1_path)
    assert v2["schema"] == wp.SCHEMA_V2 and v2["dtype"] == "fp16" and v2["config_fingerprint"] == "hac-v1-test"
    assert [r["tokens"] for r in v2["waves"]] == [r["tokens"] for r in v1["waves"]]
    assert v2["waves"][0]["covers"] == [[0, 0, 0, 0]]
    prov = v2["provenance"]
    assert prov["wave_map_sha256"] == si.sha256_file(out / "wave_map.json")
    assert prov["firmware_elf_sha256"] == si.sha256_file(out / "firmware.elf")
    assert prov["simulator_sha256"] == "a" * 64 and prov["ir_sha256"] == "x" * 64
    assert v2["derived_from"]["profile_sha256"] == si.sha256_file(v1_path)


@pytest.mark.parametrize("case, message", [
    ("v1_differs", "differs from the v1 rows"),
    ("other_hardware_ir", "another hardware_ir.json"),
    ("other_platform", "another platform"),
    ("bad_simulator", "64 hex digits"),
])
def test_v2_refuses(v2_dir, d, tmp_path, case, message):
    out, ir, platform = v2_dir
    v1 = wp.build(out, ir, "x" * 64, _log(d), "test")
    v1_path = tmp_path / "v1.json"
    other = json.loads(json.dumps(v1))
    if case == "v1_differs":
        other["waves"][0]["tokens"]["hddu_done"] += 1
    v1_path.write_text(json.dumps(other))
    sim = "a" * 64
    if case == "other_hardware_ir":
        ir["provenance"]["files_sha256"]["hardware_ir.json"] = "0" * 64
    if case == "other_platform":
        platform.write_text("{}")
    if case == "bad_simulator":
        sim = "xyz"
    with pytest.raises(si.ScheduleIRError, match=message):
        wp.to_v2(v1, out, ir, sim, platform, from_v1=v1_path)


# ── SPM claims (M22 condition 1b) ─────────────────────────────────────────────

def _claims_ir():
    ir = _ir()
    ir["resource"]["hardware"] = {"num_clusters": 1, "spm_banks_per_group": 3, "spm_bank_depth": 8192}
    return ir


def _spm(*lines):
    # A DMA record (D) is not a claim and is skipped.
    return "\n".join(["D 0 1 393216 255 %d" % (102 * C)] + list(lines)) + "\n"


def test_spm_claims_are_run_length_bank_and_read_group_masks(cc_dir, d):
    spm = _spm(f"P 0 0 0 1 0 {103 * C}", f"P 0 0 0 1 8 {104 * C}", f"P 0 1 1 0 8192 {104 * C}",
               f"P 0 2 2 1 24576 {106 * C}", f"P 0 2 2 1 24576 {107 * C}")
    prof = wp.build(cc_dir, _claims_ir(), "x" * 64, _log(d), "test", spm)
    # cycle 3: bank 0 written; cycle 4: bank 0 and group 1's bank 1 (a read, so group 1's read slot);
    # cycles 6-7: a parallel-view write claims all of group 2 (banks 6-8).
    assert prof["waves"][0]["spm_claims"] == [[3, 1, 0b1, 0], [4, 1, 0b10001, 0b10], [6, 2, 0b111000000, 0]]
    assert prof["spm_claims"]["row"].startswith("[cycle after START")
    assert "spm_claims" not in wp.build(cc_dir, _claims_ir(), "x" * 64, _log(d), "test")["waves"][0]


@pytest.mark.parametrize("line, message", [
    (f"P 0 0 0 1 0 {100 * C}", "before or at a wave's START"),
    (f"P 0 0 0 1 0 {217 * C}", "after its wave's DONE"),
    (f"P 1 0 0 1 0 {103 * C}", "cluster 1"),
])
def test_spm_claims_refuse(cc_dir, d, line, message):
    with pytest.raises(si.ScheduleIRError, match=message):
        wp.build(cc_dir, _claims_ir(), "x" * 64, _log(d), "test", _spm(line))
