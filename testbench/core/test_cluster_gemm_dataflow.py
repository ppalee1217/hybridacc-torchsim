"""The cluster GEMM test feeds every PE of every wave its operands (D-34).

test_cluster_sim / test_cluster_sim_advanced send START_PE once per layer and
run cluster plan i after DMA wave i. The PE program loops over the plan's N
and M waves and, per wave, consumes one weight set (first M wave of an N wave
only), one activation tile and one partial-sum tile, then returns one output
tile. The scan chain gives every (m, n) tile of a wave its own wave-local tags.

The old generator emitted one plan per (K tile, M tile) inside each wave and a
single DMA wave, sent the activation with a fixed tag 0 and read row-major
tensors with AGU programs that assume transposed packing; ESL then stalled on
the second plan (w1) or the first plan (w16).

These tests drive the generated plans, DMA waves and DRAM images through an
untimed model of the delivery path (DMA copy -> SPM -> AGU (addr, tag) ->
router -> MBUS tag match) and check every PE stream against the operands the
PE program consumes. The model follows the ESL sources it names; the order in
which a PE consumes and returns words is the order test_noc_sim uses
(design/hybridacc-ESL/test/test_noc_sim.cpp distribute_gemm_*), the reference
flow that passes on the same PE program.
"""

import json
import sys
import types
from pathlib import Path

import numpy as np
import pytest

from hybridacc_cc.lowering import compute_scan_chain_gemm
from hybridacc_verify.utils.config import ClusterGemmConfig


def _import_cluster_gen():
    """Import cluster_gen; without torch, use an import-only placeholder.

    cluster_gen imports torch for tensor data only. These tests use its
    torch-free planning and packing code with numpy data; any attribute access
    on the placeholder raises, so a test can never pass on fake torch behaviour.
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
            from hybridacc_verify.gen import cluster_gen, noc_gen
        finally:
            for name in names:
                sys.modules.pop(name, None)
        return cluster_gen, noc_gen
    from hybridacc_verify.gen import cluster_gen, noc_gen
    return cluster_gen, noc_gen


cluster_gen, noc_gen = _import_cluster_gen()

REPO = Path(__file__).resolve().parents[2]
CLUSTER_DIR = REPO / "testbench" / "cluster"
GEMM_FIXTURES = sorted(p.name for p in CLUSTER_DIR.glob("gemm*") if (p / "config.json").exists())

PE_M, PE_N, PE_K = 12, 8, 32
# Words one PE takes per wave in the fixture PE program
# (testbench/cluster/gemm_ultra_w*/pe_program.asm): SDMA.LEN 64 weight words
# per N wave, LOOPIN 32 x 3 VTSTORE activation words and 23 VPSUMR + 1 VPSUM
# partial-sum words per M wave, and as many output words.
PS_WORDS, PD_WORDS, C_WORDS = 64, 96, 24

# ESL SPM geometry (ScratchpadMemory.hpp: GROUP_LINEAR_WORDS / GROUP_SPAN_WORDS).
BANK_DEPTH, BANKS, GROUP_LINEAR, GROUP_SPAN = 8192, 3, 3 * 8192, 4 * 8192
PLANES = ("ps", "pd", "pli", "plo")


def _config(name, **overrides):
    raw = json.loads((CLUSTER_DIR / name / "config.json").read_text())
    raw["pe_program"] = str(REPO / raw["pe_program"])
    raw.update(overrides)
    return ClusterGemmConfig(**raw)


# ---------------------------------------------------------------------------
# Untimed model of the ESL delivery path
# ---------------------------------------------------------------------------

def addr_list(gen, fallback_base, words):
    """tb_utils.hpp build_dma_addr_list."""
    if not gen:
        return [fallback_base + 8 * i for i in range(words)]
    out = []
    it, st = gen["iter"], gen["stride"]
    for i0 in range(it[0]):
        for i1 in range(it[1]):
            for i2 in range(it[2]):
                for i3 in range(it[3]):
                    if len(out) < words:
                        out.append(gen["base_addr"] + i0 * st[0] + i1 * st[1] + i2 * st[2] + i3 * st[3])
    out += [fallback_base + 8 * i for i in range(len(out), words)]
    return out


def agu_emissions(agu):
    """(addr, tag) of one AGU program: AddressGenerateUnit.hpp seq_process.

    idx0 is innermost; tag = tag_base + idx[tag_ctrl & 3] * (tag_stride0 if
    level 0 else tag_stride1), low 6 bits.
    """
    it = [max(1, agu[f"iter{i}"]) for i in range(4)]
    st = [agu[f"stride{i}"] for i in range(4)]
    level = agu["tag_ctrl"] & 0x3
    step = agu["tag_stride0"] if level == 0 else agu["tag_stride1"]
    out = []
    for i3 in range(it[3]):
        for i2 in range(it[2]):
            for i1 in range(it[1]):
                for i0 in range(it[0]):
                    idx = (i0, i1, i2, i3)
                    addr = agu["base_addr"] + sum(i * s for i, s in zip(idx, st))
                    out.append((addr, (agu["tag_base"] + idx[level] * step) & 0x3F))
    return out


def scan_ids(entry):
    return {"ps": entry.ps_id, "pd": entry.pd_id, "pli": entry.pli_id, "plo": entry.plo_id}


class Fabric:
    """DRAM, SPM and NoC delivery of one cluster test, without timing.

    - DMA: tb_utils address lists; SPM DMA decode is linear
      (ScratchpadMemory.hpp: bank = local_word / BANK_DEPTH).
    - NoC-port reads: local word >= GROUP_LINEAR reads one row of every bank
      of the group (bank k -> slice k), a linear read returns slice 0 only.
    - Router: an ultra request gives bus p slice p, a normal one broadcasts
      slice 0 (NoCRouter.hpp process_requests_noc_*).
    - MBUS: every enabled PE whose channel id equals the tag receives the word
      (MBUS.hpp calculate_target_pe_mask); with no match the word is dropped.
    """

    def __init__(self, software, scan_chain, num_bus, num_pes):
        self.software = software
        self.dram = {}
        self.spm = {}
        self.num_bus = num_bus
        self.pes_per_bus = num_pes // num_bus
        self.ids = [scan_ids(e) for e in scan_chain]
        self.enabled = [bool(e.enable) for e in scan_chain]

    def load_dram(self, base, elements):
        flat = np.asarray(elements, dtype=np.float32).reshape(-1)
        pad = (-len(flat)) % 4
        flat = np.concatenate([flat, np.zeros(pad, dtype=np.float32)])
        for i, word in enumerate(flat.reshape(-1, 4)):
            self.dram[base + 8 * i] = word

    def _spm_key(self, global_byte):
        group, local = divmod(global_byte // 8, GROUP_SPAN)
        bank, row = divmod(local, BANK_DEPTH)
        return group, bank, row

    def dma(self, wave, direction):
        zero = np.zeros(4, dtype=np.float32)
        for t in wave["transfers"]:
            if t["direction"] != direction:
                continue
            words = t["size_words64"]
            if direction == "dram_to_spm":
                src = addr_list(t.get("src_addr_gen"), t["src_dram_addr"], words)
                dst = addr_list(t.get("dst_addr_gen"), t["dst_spm_addr"], words)
                for s, d in zip(src, dst):
                    self.spm[self._spm_key(d)] = self.dram.get(s, zero)
            else:
                src = addr_list(t.get("src_addr_gen"), t["src_spm_addr"], words)
                dst = addr_list(t.get("dst_addr_gen"), t["dst_dram_addr"], words)
                for s, d in zip(src, dst):
                    self.dram[d] = self.spm.get(self._spm_key(s), zero)

    def read_slices(self, group, local_word):
        zero = np.zeros(4, dtype=np.float32)
        if local_word >= GROUP_LINEAR:
            row = local_word - GROUP_LINEAR
            return [self.spm.get((group, k, row), zero) for k in range(BANKS)]
        bank, row = divmod(local_word, BANK_DEPTH)
        return [self.spm.get((group, bank, row), zero)] + [zero] * (BANKS - 1)

    def targets(self, bus, plane, tag):
        base = bus * self.pes_per_bus
        return [base + j for j in range(self.pes_per_bus)
                if self.enabled[base + j] and self.ids[base + j][plane] == tag]

    def run_plan(self, plan, spm_map, outputs_of):
        """Run one plan; returns per-PE received words and the PLO responders.

        outputs_of(pe) yields the words a PE returns for PLO in this wave.
        """
        received = {plane: {} for plane in ("ps", "pd", "pli")}
        plo_responders = []
        port_group = {plane: (spm_map >> (2 * i)) & 0x3 for i, plane in enumerate(PLANES)}
        for i, plane in enumerate(("ps", "pd", "pli")):
            agu = plan["agu_" + plane]
            if not ((plan["global_mask"] >> i) & 1):
                continue
            # A masked-in plane whose AGU the bench skips (enable=false) would
            # restart with the previous configuration; the generator must not
            # emit that combination.
            assert agu["enable"], f"{plan['name']}: plane {plane} enabled without an AGU program"
            for addr, tag in agu_emissions(agu):
                slices = self.read_slices(port_group[plane], addr)
                for bus in range(self.num_bus):
                    data = slices[bus] if agu["ultra"] else slices[0]
                    for pe in self.targets(bus, plane, tag):
                        received[plane].setdefault(pe, []).append(data)
        agu = plan["agu_plo"]
        if (plan["global_mask"] >> 3) & 1:
            assert agu["enable"], f"{plan['name']}: PLO plane enabled without an AGU program"
            assert not agu["ultra"], "the model covers single-responder PLO reads only"
            streams = {}
            for addr, tag in agu_emissions(agu):
                pes = [pe for bus in range(self.num_bus) for pe in self.targets(bus, "plo", tag)]
                plo_responders.append(tuple(pes))
                if len(pes) != 1:
                    continue  # no or several responders: the router would wait or mix words
                pe = pes[0]
                if pe not in streams:
                    streams[pe] = iter(outputs_of(pe))
                word = next(streams[pe], None)
                if word is None:
                    continue
                group = port_group["plo"]
                bank, row = divmod(addr, BANK_DEPTH)
                assert addr < GROUP_LINEAR
                self.spm[(group, bank, row)] = word
        return received, plo_responders


# ---------------------------------------------------------------------------
# What each PE consumes (test_noc_sim order) and returns
# ---------------------------------------------------------------------------

def _pad(x, rows, cols):
    out = np.zeros((rows, cols), dtype=np.float32)
    out[:x.shape[0], :x.shape[1]] = x
    return out


def expected_words(plane, bus, m_row0, n_col0, A, B, D, C):
    """Words one PE of tile (rows m_row0.., cols n_col0..) on K stage `bus` takes or returns."""
    k0 = bus * PE_K
    if plane == "ps":   # per K: 2 words of 4 columns (distribute_gemm_ps)
        return [B[k0 + k, n_col0 + 4 * w:n_col0 + 4 * w + 4]
                for k in range(PE_K) for w in range(PE_N // 4)]
    if plane == "pd":   # per K: 3 words of 4 rows (distribute_gemm_pd)
        return [A[m_row0 + 4 * w:m_row0 + 4 * w + 4, k0 + k]
                for k in range(PE_K) for w in range(PE_M // 4)]
    src = D if plane == "pli" else C   # per column: 3 words of 4 rows (distribute_gemm_pli/plo)
    return [src[m_row0 + 4 * w:m_row0 + 4 * w + 4, n_col0 + c]
            for c in range(PE_N) for w in range(PE_M // 4)]


def run_layer(config, pe_program=None, seed=7):
    """Generate one cluster GEMM test with numpy data and run it through Fabric.

    Returns a list of contract violations (empty when every PE of every wave
    gets exactly its operands and the output region equals the gold image).
    """
    planned = cluster_gen.plan_cluster_gemm(config, pe_program)
    plan, layout, software = planned["plan"], planned["layout"], planned["software"]
    rng = np.random.default_rng(seed)
    M, N, K = config.M, config.N, config.K
    A = rng.standard_normal((M, K)).astype(np.float32)
    B = rng.standard_normal((K, N)).astype(np.float32)
    D = rng.standard_normal((M, N)).astype(np.float32)
    C = A @ B + D
    inputs, gold = cluster_gen.pack_cluster_gemm_tensors(A, B, D, C, layout)
    Ap = _pad(A, layout["m_pad"], layout["k_pad"])
    Bp = _pad(B, layout["k_pad"], layout["n_pad"])
    Dp = _pad(D, layout["m_pad"], layout["n_pad"])
    Cp = _pad(C, layout["m_pad"], layout["n_pad"])

    fabric = Fabric(software, planned["scan_chain"], config.num_bus, config.num_pes)
    for tensor, elems in inputs.items():
        fabric.load_dram(software["dram_mapping"][tensor], elems)

    plans, waves = software["cluster_plans"], software["dma"]["waves"]
    problems = []
    if len(plans) != len(waves):
        problems.append(f"{len(plans)} plans but {len(waves)} DMA waves")
    gm, gn, grid_k = layout["grid_m_wave"], layout["grid_n_wave"], layout["grid_k"]
    last_bus = grid_k - 1
    for i, (p, w) in enumerate(zip(plans, waves)):
        if w["sync"]["compute_plan_idx"] != i:
            problems.append(f"wave {i} syncs plan {w['sync']['compute_plan_idx']}")
        wn, wm = divmod(i, layout["wave_m"])   # N waves outer, M waves inner

        def tile_origin(pe):
            j = pe % fabric.pes_per_bus
            m, n = divmod(j, gn)
            return (wm * gm + m) * PE_M, (wn * gn + n) * PE_N

        def outputs_of(pe):
            m0, n0 = tile_origin(pe)
            return expected_words("plo", last_bus, m0, n0, Ap, Bp, Dp, Cp)

        fabric.dma(w, "dram_to_spm")
        received, responders = fabric.run_plan(p, w["spm_map"]["map_val"], outputs_of)
        for bus in range(config.num_bus):
            for j in range(fabric.pes_per_bus):
                pe = bus * fabric.pes_per_bus + j
                if not fabric.enabled[pe]:
                    for plane in ("ps", "pd", "pli"):
                        if received[plane].get(pe):
                            problems.append(f"wave {i}: disabled PE {pe} got {plane}")
                    continue
                m0, n0 = tile_origin(pe)
                wants = {
                    "ps": expected_words("ps", bus, m0, n0, Ap, Bp, Dp, Cp) if wm == 0 else [],
                    "pd": expected_words("pd", bus, m0, n0, Ap, Bp, Dp, Cp),
                    "pli": expected_words("pli", bus, m0, n0, Ap, Bp, Dp, Cp) if bus == 0 else [],
                }
                for plane, want in wants.items():
                    got = received[plane].get(pe, [])
                    if len(got) != len(want):
                        problems.append(f"wave {i} PE {pe} (bus {bus}, j {j}) {plane}: "
                                        f"{len(got)} words, program takes {len(want)}")
                    elif want and not np.array_equal(np.array(got), np.array(want)):
                        problems.append(f"wave {i} PE {pe} {plane}: wrong words")
        expected_responders = sorted(
            last_bus * fabric.pes_per_bus + j for j in range(gm * gn))
        counts = {}
        for pes in responders:
            if len(pes) != 1:
                problems.append(f"wave {i}: PLO request with responders {pes}")
            else:
                counts[pes[0]] = counts.get(pes[0], 0) + 1
        if sorted(counts) != expected_responders or set(counts.values()) != {C_WORDS}:
            problems.append(f"wave {i}: PLO reads {counts}, want {C_WORDS} from each of "
                            f"{expected_responders}")
        fabric.dma(w, "spm_to_dram")

    out_base = software["dram_mapping"]["output"]
    words = len(gold) // 4
    got = np.array([fabric.dram.get(out_base + 8 * i, np.full(4, np.nan, np.float32))
                    for i in range(words)]).reshape(-1)
    if not np.array_equal(got, gold):
        problems.append("output region differs from the gold image")
    return problems, {"A": A, "B": B, "D": D, "C": C, "gold": gold, "inputs": inputs,
                      "layout": layout, "plan": plan}


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_fixture_set():
    assert GEMM_FIXTURES == ["gemm_ultra_w1", "gemm_ultra_w16"]


@pytest.mark.parametrize("name", GEMM_FIXTURES)
def test_every_pe_gets_its_operands(name):
    """Each wave delivers every PE its weights, activations and partial sums,
    collects its outputs, and the output region equals the gold image."""
    config = _config(name)
    problems, _ = run_layer(config, config.pe_program)
    assert problems == []


@pytest.mark.parametrize("name", GEMM_FIXTURES)
def test_one_plan_per_wave_in_pe_loop_order(name):
    config = _config(name)
    planned = cluster_gen.plan_cluster_gemm(config, config.pe_program)
    plan, software = planned["plan"], planned["software"]
    names = [p["name"] for p in software["cluster_plans"]]
    assert names == [f"GEMM_K0_N{wn}_M{wm}" for wn in range(plan["wave_n"])
                     for wm in range(plan["wave_m"])]
    assert [w["sync"]["compute_plan_idx"] for w in software["dma"]["waves"]] == list(range(len(names)))
    # PS (plane bit 0) only on the first M wave of each N wave.
    masks = [p["global_mask"] for p in software["cluster_plans"]]
    assert masks == [0xF if wm == 0 else 0xE for wn in range(plan["wave_n"])
                     for wm in range(plan["wave_m"])]


@pytest.mark.parametrize("name", GEMM_FIXTURES)
def test_scan_chain_matches_cc_wave_local_scan_chain(name):
    config = _config(name)
    planned = cluster_gen.plan_cluster_gemm(config, config.pe_program)
    plan = planned["plan"]
    cc_chain = compute_scan_chain_gemm(
        config.num_pes, config.num_bus,
        grid_m=plan["grid_m_per_wave"][0], grid_n=plan["grid_n_per_wave"][0],
        grid_k=plan["grid_k"], use_ultra=True)
    ours = planned["scan_chain"]
    assert [(e.ps_id, e.pd_id, e.pli_id, e.plo_id, bool(e.enable)) for e in ours] == \
        [(e.ps_id, e.pd_id, e.pli_id, e.plo_id, bool(e.enable)) for e in cc_chain]
    assert [e.route_mode for e in ours if e.enable] == [e.route_mode for e in cc_chain if e.enable]


def test_w1_agu_programs_match_cc_lowering():
    """w1 has the same 4 x 4 x 3 wave tile in cc; its AGU programs match cc's."""
    from hybridacc_cc.frontend import parse_workload
    from hybridacc_cc.lowering import lower_workload

    config = _config("gemm_ultra_w1")
    plan0 = cluster_gen.plan_cluster_gemm(config, config.pe_program)["software"]["cluster_plans"][0]
    workload = {
        "name": "w1", "hardware": {"num_clusters": 1, "num_pes": 48, "num_bus": 3,
                                    "spm_banks_per_group": 3, "spm_bank_depth": 8192,
                                    "dram_base": 0x80000000},
        "tensors": {"A": {"shape": [48, 96], "dtype": "fp16"},
                    "B": {"shape": [96, 32], "dtype": "fp16"},
                    "C": {"shape": [48, 32], "dtype": "fp16"}},
        "ops": [{"name": "g", "type": "gemm", "inputs": ["A", "B"], "outputs": ["C"]}],
    }
    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "w.yaml"
        path.write_text(json.dumps(workload))
        layer = lower_workload(parse_workload(path)).layers[0]
    assert layer.pe_program.params["GRID_M_PER_WAVE"] == 4
    assert layer.pe_program.params["GRID_N_PER_WAVE"] == 4
    keys = ("iter0", "iter1", "iter2", "tag_base", "tag_ctrl")
    for plane in PLANES:
        ours, cc = plan0["agu_" + plane], getattr(layer, "agu_" + plane)
        assert {k: ours[k] for k in keys} == {k: getattr(cc, k) for k in keys}, plane
        assert [ours[f"stride{i}"] for i in range(3)] == [cc.stride0, cc.stride1, cc.stride2], plane
        level = cc.tag_ctrl & 3
        cc_step = cc.tag_stride0 if level == 0 else cc.tag_stride1
        our_step = ours["tag_stride0"] if level == 0 else ours["tag_stride1"]
        assert our_step == cc_step, plane
        assert ours["ultra"] == bool(cc.ctrl & 0x8), plane


def test_packed_images_are_a_permutation_of_the_tensors():
    config = _config("gemm_ultra_w16")
    _, data = run_layer(config, config.pe_program)
    for name, src in (("activation", data["A"]), ("weight", data["B"]), ("partial_sum", data["D"])):
        assert np.array_equal(np.sort(data["inputs"][name]), np.sort(src.reshape(-1))), name
    assert np.array_equal(np.sort(data["gold"]), np.sort(data["C"].reshape(-1)))


def test_padding_tail_shape():
    """M, N, K that are not tile multiples are zero-padded consistently."""
    config = _config("gemm_ultra_w1", M=40, N=30, K=80)
    problems, data = run_layer(config, config.pe_program)
    assert problems == []
    assert data["layout"]["m_pad"] == 48 and data["layout"]["n_pad"] == 32
    assert data["layout"]["k_pad"] == 96


def test_stale_w16_wave_loops_raise(tmp_path):
    """The pre-fix cluster gemm_ultra_w16 loops (1 N wave x 16 M waves) are rejected."""
    text = (CLUSTER_DIR / "gemm_ultra_w16" / "pe_program.asm").read_text()
    stale = tmp_path / "pe_program.asm"
    stale.write_text(text.replace("    LOOPIN 2  # N-tiles", "    LOOPIN 1  # N-tiles")
                     .replace("    LOOPIN 8  # M-tiles", "    LOOPIN 16  # M-tiles"))
    assert stale.read_text() != text
    with pytest.raises(ValueError, match="wave_n 1 != plan 2"):
        cluster_gen.plan_cluster_gemm(_config("gemm_ultra_w16"), stale)


@pytest.mark.parametrize("overrides,match", [
    ({"ultra_mode": False}, "ultra K-chain"),
    ({"K": 32}, "grid_k=1"),
    ({"K": 128}, "grid_k=4"),
    ({"N": 136, "M": 12}, "ragged"),
])
def test_unsupported_plans_raise(overrides, match):
    with pytest.raises(ValueError, match=match):
        cluster_gen.plan_cluster_gemm(_config("gemm_ultra_w1", **overrides))
