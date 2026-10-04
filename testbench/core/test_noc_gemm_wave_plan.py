"""The NoC GEMM generator and fixtures follow the per-wave plan (D-29).

Upstream a090a7b changed how noc_gen picks the per-wave M/N tile shape, so N
can span several waves. The scan-chain tags must then be wave-local (as in the
cc lowering and in test_noc_sim), and the hand-written PE programs must loop
over the plan's N and M waves and prefetch one weight set (SDMA.LOOP phase)
per N wave.
"""

import json
import sys
import types
from pathlib import Path

import pytest

from hybridacc_cc.lowering import compute_scan_chain_gemm
from hybridacc_verify.utils.config import NocGemmConfig


def _import_noc_gen():
    """Import noc_gen; without torch, use an import-only placeholder.

    noc_gen imports torch for tensor data only. These tests use its torch-free
    planning, scan-chain and PE-program code; any attribute access on the
    placeholder raises, so a test can never pass on fake torch behaviour.
    """
    try:
        import torch  # noqa: F401
    except ModuleNotFoundError:
        def _missing(name):
            raise AttributeError(f"torch placeholder: '{name}' needs the real torch")

        names = ("torch", "torch.nn", "torch.nn.functional")
        stubs = {name: types.ModuleType(name) for name in names}
        for stub in stubs.values():
            stub.__getattr__ = _missing
        stubs["torch"].nn = stubs["torch.nn"]
        stubs["torch.nn"].functional = stubs["torch.nn.functional"]
        sys.modules.update(stubs)
        try:
            from hybridacc_verify.gen import noc_gen
        finally:
            for name in names:
                sys.modules.pop(name, None)
        return noc_gen
    from hybridacc_verify.gen import noc_gen
    return noc_gen


noc_gen = _import_noc_gen()

NOC_DIR = Path(__file__).resolve().parents[1] / "noc"
GEMM_FIXTURES = sorted(p.name for p in NOC_DIR.glob("gemm*") if (p / "config.json").exists())


def _config(name):
    return NocGemmConfig(**json.loads((NOC_DIR / name / "config.json").read_text()))


def _fields(entry):
    return (entry.ps_id, entry.pd_id, entry.pli_id, entry.plo_id, bool(entry.enable))


def test_fixture_set_is_the_known_nine():
    assert GEMM_FIXTURES == [
        "gemm", "gemm_ultra", "gemm_ultra_w1", "gemm_ultra_w16", "gemm_ultra_w2",
        "gemm_ultra_w32", "gemm_ultra_w4", "gemm_ultra_w64", "gemm_ultra_w8",
    ]


@pytest.mark.parametrize("name", GEMM_FIXTURES)
def test_scan_chain_matches_cc_wave_local_scan_chain(name):
    """Tags equal the cc lowering's scan chain built from per-wave dims."""
    config = _config(name)
    plan, chain = noc_gen.plan_gemm_test(config)
    cc_chain = compute_scan_chain_gemm(
        config.num_pes, config.num_bus,
        grid_m=plan["grid_m_per_wave"][0],
        grid_n=plan["grid_n_per_wave"][0],
        grid_k=plan["grid_k"],
        use_ultra=bool(config.ultra_mode),
    )
    assert len(chain) == len(cc_chain)
    for idx, (ours, cc) in enumerate(zip(chain, cc_chain)):
        assert _fields(ours) == _fields(cc), f"PE {idx}"
        if ours.enable:
            assert ours.route_mode == cc.route_mode, f"PE {idx}"


def test_w16_tags_are_wave_local():
    """gemm_ultra_w16 runs 2 x 8 tiles per wave: PE j holds (j // 8, j % 8)."""
    plan, chain = noc_gen.plan_gemm_test(_config("gemm_ultra_w16"))
    assert (plan["grid_n"], plan["grid_m_per_wave"][0], plan["grid_n_per_wave"][0]) == (16, 2, 8)
    for bus in range(3):
        for j in range(16):
            entry = chain[bus * 16 + j]
            m_idx, n_idx = divmod(j, 8)
            assert entry.enable
            assert (entry.ps_id, entry.pd_id) == (n_idx, m_idx)
            assert entry.pli_id == (m_idx * 8 + n_idx if bus == 0 else 63)
            assert entry.plo_id == (m_idx * 8 + n_idx if bus == 2 else 63)


def _ragged_config():
    # grid_n = 17 tiles over 2 N waves -> per-wave N tiles [9, 8].
    return NocGemmConfig(num_pes=48, num_bus=3, M=12, N=136, K=96, ultra_mode=True)


def test_ragged_plan_raises():
    config = _ragged_config()
    plan = noc_gen.plan_gemm_waves(config.M, config.N, config.K, config.num_pes, config.num_bus)
    assert len(set(plan["grid_m_per_wave"])) > 1 or len(set(plan["grid_n_per_wave"])) > 1
    with pytest.raises(ValueError, match="ragged"):
        noc_gen.plan_gemm_test(config)


def test_non_ultra_multi_wave_raises():
    config = NocGemmConfig(num_pes=48, num_bus=3, M=192, N=128, K=96, ultra_mode=False)
    with pytest.raises(ValueError, match="non-ultra"):
        noc_gen.plan_gemm_test(config)


@pytest.mark.parametrize("name", GEMM_FIXTURES)
def test_fixture_pe_program_follows_plan(name):
    """Every checked-in fixture passes the generator's PE-program check."""
    config = _config(name)
    plan, _ = noc_gen.plan_gemm_test(config, NOC_DIR / name / "pe_program.asm")
    found = noc_gen.parse_gemm_pe_program_waves((NOC_DIR / name / "pe_program.asm").read_text())
    assert (found["wave_n"], found["wave_m"]) == (plan["wave_n"], plan["wave_m"])
    assert found["sdma_loop"] == plan["wave_k"] * plan["wave_n"]


def _with_wave_loops(text, wave_n, wave_m):
    lines = text.splitlines()
    n_at = next(i for i, l in enumerate(lines) if "SYS.SYNC (SWAPDM)" in l) - 1
    m_at = next(i for i, l in enumerate(lines) if "LDMA.ACT" in l and "SYS.CTRL" in l) - 1
    assert lines[n_at].strip().startswith("LOOPIN") and lines[m_at].strip().startswith("LOOPIN")
    lines[n_at] = f"    LOOPIN {wave_n}"
    lines[m_at] = f"    LOOPIN {wave_m}"
    return "\n".join(lines) + "\n"


def test_stale_wave_loops_raise(tmp_path):
    """The pre-fix gemm_ultra_w16 loops (1 N wave x 16 M waves) are rejected."""
    stale = tmp_path / "pe_program.asm"
    stale.write_text(_with_wave_loops((NOC_DIR / "gemm_ultra_w16" / "pe_program.asm").read_text(), 1, 16))
    with pytest.raises(ValueError, match="wave_n 1 != plan 2"):
        noc_gen.plan_gemm_test(_config("gemm_ultra_w16"), stale)


def test_short_sdma_loop_raises(tmp_path):
    """SDMA.LOOP must cover every N wave (cc: wave_k * wave_n prefetch sets).

    With SDMA.LOOP 1 and two N waves, ESL test_noc_sim stalls on noc_ps
    (agent_run/261004-d29-binding, arm A).
    """
    text = (NOC_DIR / "gemm_ultra_w8" / "pe_program.asm").read_text()
    assert text.count("    SDMA.LOOP 2\n") == 1
    short = tmp_path / "pe_program.asm"
    short.write_text(text.replace("    SDMA.LOOP 2\n", "    SDMA.LOOP 1\n"))
    with pytest.raises(ValueError, match="sdma_loop 1 != plan 2"):
        noc_gen.plan_gemm_test(_config("gemm_ultra_w8"), short)


def test_generate_checks_pe_program_before_data(tmp_path):
    """generate_gemm_test(pe_program=...) fails before any tensor is made."""
    stale = tmp_path / "pe_program.asm"
    stale.write_text(_with_wave_loops((NOC_DIR / "gemm_ultra_w8" / "pe_program.asm").read_text(), 1, 8))
    with pytest.raises(ValueError, match="does not follow the GEMM wave plan"):
        noc_gen.generate_gemm_test(_config("gemm_ultra_w8"), pe_program=stale)


def test_single_payload_program_is_one_by_one():
    found = noc_gen.parse_gemm_pe_program_waves((NOC_DIR / "gemm" / "pe_program.asm").read_text())
    assert (found["wave_n"], found["wave_m"]) == (1, 1)


def test_unrecognised_program_shape_raises():
    text = "LOOPIN 2\nVTSTORE vt0\nSYS.SYNC (SWAPDM)\nLOOPEND\nHALT\n"
    with pytest.raises(ValueError):
        noc_gen.parse_gemm_pe_program_waves(text)
