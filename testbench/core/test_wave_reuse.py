"""Which waves of one schedule share a profile (hybridacc_cc.wave_reuse, M22 condition 3)."""
import copy
import json

import pytest

from hybridacc_cc import wave_reuse as wr


def _wave_map():
    # oc: two tiles of 16 (the second valid 8); ic: three reduction tiles of 8.
    return {"layers": [{"index": 0, "axes": [
        {"grid": "oc", "axis": "oc", "size": 24, "reduction": False, "segments": [[0, 16, 16], [16, 24, 16]]},
        {"grid": "ic", "axis": "ic", "size": 24, "reduction": True, "segments": [[0, 8, 8], [8, 16, 8], [16, 24, 8]]},
    ]}]}


def _wave(i, oc, ic, base, cmap=180):
    return {"id": i, "covers": [[oc, ic]], "spm_config_map": cmap,
            "agu": {"pd": {"BASE_ADDR": base, "BASE_ADDR_H": 0}, "pli": {"BASE_ADDR": 0, "BASE_ADDR_H": 0}}}


def _ir(waves):
    return {"layers": [{"index": 0, "execution_waves": waves}]}


def _group_of(tab):
    return {(r["layer"], r["wave"]): tuple(r["representative"]) for r in tab["waves"]}


def test_groups_follow_pattern_context_buffers_and_config_map():
    # Reduction order over ic on oc tile 0, alternating ping/pong; then the same on oc tile 1.
    waves = [_wave(0, 0, 0, 0), _wave(1, 0, 1, 4096), _wave(2, 0, 2, 0), _wave(3, 1, 0, 4096), _wave(4, 1, 1, 0),
             _wave(5, 1, 2, 4096, cmap=228)]
    rep = _group_of(wr.table(_ir(waves), _wave_map(), "a" * 64, "b" * 64))
    assert rep[(0, 0)] == (0, 0)                 # first wave of the layer: context "first"
    assert rep[(0, 1)] == (0, 1)                 # ic position "mid", buffer 4096
    assert rep[(0, 2)] == (0, 2)                 # ic position "last"
    assert rep[(0, 3)] == (0, 3)                 # oc tile 1 (valid 8) and both axes changed
    assert rep[(0, 4)] == (0, 4)                 # mid again, but on oc tile 1 and buffer 0 (wave 1: tile 0, 4096)
    assert rep[(0, 5)] == (0, 5)                 # last, another spm_config_map
    same = [_wave(0, 0, 0, 0), _wave(1, 0, 1, 4096), _wave(2, 0, 1, 4096), _wave(3, 0, 1, 4096)]
    rep = _group_of(wr.table(_ir(same), _wave_map(), "a" * 64, "b" * 64))
    # waves 2 and 3 repeat wave 2's pattern, context (no axis changed) and buffers; wave 1's context is "1".
    assert (rep[(0, 1)], rep[(0, 2)], rep[(0, 3)]) == ((0, 1), (0, 2), (0, 2))


def _profile(rows, ir_sha="a" * 64, **extra):
    return {"schema": "hybridacc-wave-profile/v1", "label": "t", "ir_sha256": ir_sha, "clock_mhz": 500,
            "waves": rows, **extra}


def _row(w, done=90, idle=95, claims=([3, 4, 1, 0],), quiesced=None):
    observed = {"hddu_done": {"condition_minus_start_cycles": done}, "agu_idle": {"condition_minus_start_cycles": idle}}
    if quiesced is not None:
        observed["cluster_quiesced"] = {"condition_minus_start_cycles": quiesced}
    return {"layer": 0, "wave": w, "tokens": {"hddu_done": done + 7}, "observed": observed,
            "spm_claims": [list(c) for c in claims]}


@pytest.fixture
def tab():
    same = [_wave(0, 0, 0, 0), _wave(1, 0, 1, 4096), _wave(2, 0, 1, 4096), _wave(3, 0, 1, 4096)]
    return wr.table(_ir(same), _wave_map(), "a" * 64, "b" * 64)


def test_reduce_keeps_the_representatives_and_binds_the_table(tab):
    full = _profile([_row(w) for w in range(4)])
    out = wr.reduce(full, tab, "c" * 64)
    assert [(r["layer"], r["wave"]) for r in out["waves"]] == [(0, 0), (0, 1), (0, 2)]
    assert out["reuse"] == {"table_sha256": "c" * 64, "key_version": "wave-reuse/v1"}
    assert {k: v for k, v in out.items() if k not in ("waves", "reuse")} == \
        {k: v for k, v in full.items() if k != "waves"}


@pytest.mark.parametrize("case,message", [
    ("other_ir", "another IR"),
    ("other_wave_map", "another wave map"),
    ("missing_wave", "waves differ from the table"),
])
def test_reduce_and_check_refuse(tab, case, message):
    full = _profile([_row(w) for w in range(4)])
    if case == "other_ir":
        full["ir_sha256"] = "f" * 64
    elif case == "other_wave_map":
        full["provenance"] = {"wave_map_sha256": "f" * 64}
    else:
        full["waves"].pop()
    for fn in (lambda: wr.reduce(full, tab, "c" * 64), lambda: wr.check(full, tab)):
        with pytest.raises(wr.WaveReuseError, match=message):
            fn()


def test_check_compares_conditions_and_claims_but_not_cluster_quiesced(tab):
    rows = [_row(0), _row(1), _row(2, quiesced=120), _row(3, quiesced=140)]
    assert wr.check(_profile(rows), tab)["differ"] == []  # cluster_quiesced (the cluster's state) is not shared
    rows[3] = _row(3, done=91)
    rows[2] = _row(2, claims=([3, 4, 2, 0],))  # wave 2 is the representative: wave 3 now differs in both
    out = wr.check(_profile(rows), tab)
    assert out["reusable"] == 1 and out["differ"] == [
        {"wave": [0, 3], "representative": [0, 2], "conditions_equal": False, "claims_equal": False}]


def test_cli_check_exit_codes(tmp_path, tab):
    ir = _ir([_wave(0, 0, 0, 0), _wave(1, 0, 1, 4096), _wave(2, 0, 1, 4096), _wave(3, 0, 1, 4096)])
    (tmp_path / "ir.json").write_text(json.dumps(ir))
    (tmp_path / "map.json").write_text(json.dumps(_wave_map()))
    assert wr.main(["table", str(tmp_path / "map.json"), str(tmp_path / "ir.json"), "-o", str(tmp_path / "t.json")]) == 0
    t = json.loads((tmp_path / "t.json").read_text())
    rows = [_row(w) for w in range(4)]
    (tmp_path / "p.json").write_text(json.dumps(_profile(rows, ir_sha=t["ir_sha256"])))
    assert wr.main(["check", str(tmp_path / "p.json"), str(tmp_path / "t.json")]) == 0
    bad = copy.deepcopy(rows)
    bad[3] = _row(3, idle=96)
    (tmp_path / "p.json").write_text(json.dumps(_profile(bad, ir_sha=t["ir_sha256"])))
    assert wr.main(["check", str(tmp_path / "p.json"), str(tmp_path / "t.json")]) == 1
    (tmp_path / "p.json").write_text(json.dumps(_profile(rows, ir_sha="f" * 64)))
    assert wr.main(["reduce", str(tmp_path / "p.json"), str(tmp_path / "t.json"), "-o", str(tmp_path / "r.json")]) == 2


def test_reduce_pins_the_esl_binary(tab):
    full = _profile([_row(w) for w in range(4)], provenance={"wave_map_sha256": "b" * 64, "simulator_sha256": "d" * 64})
    assert wr.reduce(full, tab, "c" * 64, simulator_sha256="d" * 64)["provenance"]["simulator_sha256"] == "d" * 64
    with pytest.raises(wr.WaveReuseError, match="another ESL binary"):
        wr.reduce(full, tab, "c" * 64, simulator_sha256="e" * 64)
    with pytest.raises(wr.WaveReuseError, match="needs a v2 profile"):
        wr.reduce(_profile([_row(w) for w in range(4)]), tab, "c" * 64, simulator_sha256="d" * 64)
