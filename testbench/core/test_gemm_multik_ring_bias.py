"""Native multi-K GEMM: the next tile's bias must not overwrite live ring partial sums.

With ``num_ic_tiles > 1`` the generic GEMM loop keeps partial sums in the SPM
reduction ring (groups 2 and 3, see ``build_wave_runtime_gemm`` in
``firmware_ops.c.j2``). The bias that initialises the next output tile's first
K-wave is loaded into group 2. The loop used to prefetch it while the current
tile's last K-wave was still running, so the bias overwrote the partial sums
before writeback: every output tile except the last came out as zeros (M19 A1
smoke, 2026-10-06; 2 buses with 64 < K <= 96).

The test renders the firmware with cc, compiles it for the host with its MMIO
accessors redirected to a recorder, runs it, and checks the recorded DMA
submissions and HDDU starts against the ring contract: between a tile's first
HDDU start and its last writeback, no DMA may write into an SPM region that
holds that tile's partial sums, and every tile still gets its bias.
"""

import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

from hybridacc_cc import frontend
from hybridacc_cc.codegen import generate_firmware
from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload

KERNEL_DIR = Path(__file__).resolve().parents[2] / "design/hybridacc-cc/kernel/json"
BEAT_BYTES = 8
SPM_ENDPOINT, DRAM_ENDPOINT = 1, 0

HARNESS = r"""
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "firmware_hw.h"

extern const LayerConfig layer_configs[];
extern const uint32_t num_layers;
void run_layer_gemm(const LayerConfig* cfg);

static uint32_t dma_regs[64];
static uint32_t last_tag;
#define R(a) dma_regs[((a) - DMA_MMIO_BASE) / 4u]

void host_mmio_write32(uint32_t addr, uint32_t val) {
    if (addr >= DMA_MMIO_BASE && addr < DMA_MMIO_BASE + 4u * 64u) {
        dma_regs[(addr - DMA_MMIO_BASE) / 4u] = val;
        if (addr == DMA_CTRL && (val & DMA_CTRL_SUBMIT)) {
            last_tag = R(DMA_CMD_TAG);
            printf("DMA %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
                   R(DMA_SRC_KIND), R(DMA_DST_KIND), R(DMA_SRC_ADDR_LO), R(DMA_DST_ADDR_LO),
                   R(DMA_COUNT_D0), R(DMA_COUNT_D1), R(DMA_COUNT_D2), R(DMA_COUNT_D3),
                   R(DMA_SRC_STRIDE_D0), R(DMA_SRC_STRIDE_D1), R(DMA_SRC_STRIDE_D2), R(DMA_SRC_STRIDE_D3),
                   R(DMA_DST_STRIDE_D0), R(DMA_DST_STRIDE_D1), R(DMA_DST_STRIDE_D2), R(DMA_DST_STRIDE_D3));
        }
        return;
    }
    if ((addr == CLUSTER_BCAST_BASE + HDDU_BASE + HDDU_CTRL
         || addr == CLUSTER_UNICAST_BASE + HDDU_BASE + HDDU_CTRL)
        && (val & HDDU_CTRL_START)) {
        printf("HDDU_START\n");
    }
}

/* Every engine completes instantly: DMA idle and all tags done, HDDU done,
 * AGUs idle, PEs halted, cluster idle and quiesced. */
uint32_t host_mmio_read32(uint32_t addr) {
    if (addr == DMA_STATUS) return DMA_STATUS_IDLE;
    if (addr == DMA_DONE_TAG) return last_tag;
    if (addr == CLUSTER_UNICAST_BASE + HDDU_BASE + HDDU_STATUS) return HDDU_STATUS_DONE;
    if (addr == CLUSTER_UNICAST_BASE + NOC_STATUS) return NOC_STATUS_ALL_ACTIVE_PES_HALTED;
    if (addr == CLUSTER_UNICAST_BASE + CLUSTER_STATUS)
        return CLUSTER_STATUS_IDLE | CLUSTER_STATUS_QUIESCED;
    return 0u;
}

int main(void) {
    alarm(20);
    for (uint32_t i = 0; i < num_layers; i++) {
        run_layer_gemm(&layer_configs[i]);
    }
    return 0;
}
"""


def _gemm_yaml(tmp_path, m, k, n, num_pes, num_bus):
    path = tmp_path / f"gemm_{m}_{k}_{n}_pe{num_pes}_b{num_bus}.yaml"
    path.write_text(yaml.safe_dump({
        "name": path.stem,
        "hardware": {
            "num_clusters": 1,
            "num_pes": num_pes,
            "num_bus": num_bus,
            "spm_banks_per_group": num_bus,
            "spm_bank_depth": 8192,
            "dram_base": 0x80000000,
        },
        "tensors": {
            "A": {"shape": [m, k], "dtype": "fp16"},
            "B": {"shape": [k, n], "dtype": "fp16"},
            "C": {"shape": [m, n], "dtype": "fp16"},
        },
        "ops": [{"name": "gemm1", "type": "gemm", "inputs": ["A", "B"], "outputs": ["C"]}],
    }))
    return path


def _host_firmware_events(tmp_path, ir):
    """Render the firmware, build it for the host with recording MMIO, return its events."""
    src = tmp_path / "fw"
    generate_firmware(ir, src, kernel_json_dir=KERNEL_DIR)
    hw = (src / "firmware_hw.h").read_text()
    for old, new in (
        ("*(volatile uint32_t*)addr = val;", "host_mmio_write32(addr, val);"),
        ("return *(volatile uint32_t*)addr;", "return host_mmio_read32(addr);"),
    ):
        assert hw.count(old) == 1, old
        hw = hw.replace(old, new)
    hw = ("#include <stdint.h>\n"
          "void host_mmio_write32(uint32_t addr, uint32_t val);\n"
          "uint32_t host_mmio_read32(uint32_t addr);\n"
          "#define HOST_NO_ASM(...) ((void)0)\n"
          + hw.replace("__asm__ volatile(", "HOST_NO_ASM("))
    (src / "firmware_hw.h").write_text(hw)
    for name in ("firmware_ops.c", "firmware_data.c"):
        text = (src / name).read_text()
        assert "__asm__" not in text, name
    (src / "harness.c").write_text(HARNESS)
    exe = tmp_path / "fw_host"
    subprocess.run(["gcc", "-std=gnu11", "-O0", "-w", "-I", str(src), "-o", str(exe),
                    str(src / "harness.c"), str(src / "firmware_ops.c"), str(src / "firmware_data.c")],
                   check=True, capture_output=True, text=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=60).stdout
    events = []
    for line in out.splitlines():
        if line == "HDDU_START":
            events.append(("start",))
        elif line.startswith("DMA "):
            v = [int(x) for x in line.split()[1:]]
            events.append(("dma", v[0], v[1], v[2], v[3], v[4:8], v[8:12], v[12:16]))
    return events


def _footprint(addr, counts, strides):
    extent = sum((c - 1) * s for c, s in zip(counts, strides) if c > 0)
    return addr, addr + extent + BEAT_BYTES


def _overlaps(a, b):
    return a[0] < b[1] and b[0] < a[1]


def _check_ring_contract(ir, events):
    t = ir.layers[0].tiling_params
    n = t.num_ic_tiles
    assert n > 1, "the reduction ring is used only with several K-waves"
    half = lambda g: (t.spm_ping[g], t.spm_pong[g])  # noqa: E731
    # Both ring groups hold a tile's partial sums; its first K-wave reads its bias
    # from group 2.
    live = [half(2), half(3)]
    bias_region = half(2)

    starts = [i for i, e in enumerate(events) if e[0] == "start"]
    tiles = t.num_oc_tiles * t.num_h_tiles * t.num_w_tiles
    assert len(starts) == tiles * n, (len(starts), tiles, n)
    loads = [(i, _footprint(e[4], e[5], e[7])) for i, e in enumerate(events)
             if e[0] == "dma" and e[2] == SPM_ENDPOINT]
    stores = [i for i, e in enumerate(events)
              if e[0] == "dma" and e[1] == SPM_ENDPOINT and e[2] == DRAM_ENDPOINT]

    hazards, missing_bias = [], []
    for k in range(tiles):
        first = starts[k * n]
        nxt = starts[(k + 1) * n] if k + 1 < tiles else len(events)
        tile_stores = [i for i in stores if first < i < nxt]
        assert tile_stores, f"tile {k} has no writeback"
        last_store = tile_stores[-1]
        hazards += [(k, i, fp) for i, fp in loads
                    if first < i < last_store and any(_overlaps(fp, r) for r in live)]
        prev = starts[(k - 1) * n] if k else -1
        if not any(prev < i < first and _overlaps(fp, bias_region) for i, fp in loads):
            missing_bias.append(k)
    return tiles, hazards, missing_bias


def _require_gcc():
    if shutil.which("gcc") is None:
        pytest.skip("host gcc is required to run the firmware harness")


# (M, K, N, PEs, buses): 2 K-waves on 2 buses (K-wave = 2 x 32), several output tiles.
# 247x81x225 and 247x96x225 are A1 smoke/probe cells (uneven tail, and K=96 with the
# tail scan-chain reconfiguration); 72x96x16 / 36x96x32 have 2 M tiles / 2 N tiles.
@pytest.mark.parametrize("m,k,n,num_pes,num_bus", [
    (72, 96, 16, 12, 2),
    (36, 96, 32, 12, 2),
    (247, 81, 225, 12, 2),
    (247, 96, 225, 48, 2),
])
def test_multik_bias_waits_for_writeback(tmp_path, m, k, n, num_pes, num_bus):
    _require_gcc()
    ir = lower_workload(parse_workload(_gemm_yaml(tmp_path, m, k, n, num_pes, num_bus)))
    assert ir.layers[0].tiling_params.num_ic_tiles == 2
    tiles, hazards, missing_bias = _check_ring_contract(ir, _host_firmware_events(tmp_path, ir))
    assert tiles > 1
    assert hazards == []
    assert missing_bias == []


def test_odd_k_wave_count_protects_the_carried_pli(tmp_path, monkeypatch):
    """Three K-waves: the last wave reads the carry from group 2, where the next bias lands.

    Native K=192 on 2 buses is reachable only without the K>96 auto-split (D-16 removes it),
    so the split is bypassed here, as agent_run/260711-a5-pe-program/compile_native.py does.
    """
    _require_gcc()
    monkeypatch.setattr(frontend, "_auto_split_large_gemms", lambda ops, hw: ops)
    ir = lower_workload(parse_workload(_gemm_yaml(tmp_path, 72, 192, 16, 12, 2)))
    assert ir.layers[0].tiling_params.num_ic_tiles == 3
    tiles, hazards, missing_bias = _check_ring_contract(ir, _host_firmware_events(tmp_path, ir))
    assert tiles > 1
    assert hazards == []
    assert missing_bias == []

