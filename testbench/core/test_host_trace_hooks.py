"""HACC_HOST_TRACE hooks in ``firmware_hw.h.j2`` (M20 schedule extraction).

With ``HACC_HOST_TRACE`` defined the rendered firmware builds for the host: MMIO
accesses go to ``hacc_host_write``/``hacc_host_read`` and each wait helper reports
what it waits for through ``hacc_host_wait`` instead of polling; before every
HDDU start ``hacc_host_wave`` reports the tiles that run covers. Without the flag
the preprocessed source is the RV32 source, so the target binary does not change.
"""

import shutil
import subprocess
from pathlib import Path

import pytest
import yaml

from hybridacc_cc.codegen import generate_firmware
from hybridacc_cc.frontend import parse_workload
from hybridacc_cc.lowering import lower_workload

KERNEL_DIR = Path(__file__).resolve().parents[2] / "design/hybridacc-cc/kernel/json"

RECORDER = r"""
#include <stdio.h>
#include "firmware_hw.h"

extern const LayerConfig layer_configs[];
extern const uint32_t num_layers;
void run_layer_gemm(const LayerConfig* cfg);

void hacc_host_write(uint32_t addr, uint32_t val) { printf("W %u %u\n", addr, val); }
uint32_t hacc_host_read(uint32_t addr) { printf("R %u\n", addr); return 0u; }
void hacc_host_wait(uint32_t kind, uint32_t arg) { printf("WAIT %u %u\n", kind, arg); }
void hacc_host_wave(uint32_t oc0, uint32_t ocn, uint32_t h, uint32_t w, uint32_t ic0, uint32_t icn) {
    printf("WAVE %u %u %u %u %u %u\n", oc0, ocn, h, w, ic0, icn);
}

int main(void) {
    printf("KINDS %u %u\n", (unsigned)HACC_HOST_WAIT_DMA_TAG, (unsigned)HACC_HOST_WAIT_HDDU_DONE);
    printf("SUBMIT %u %u\n", (unsigned)DMA_CTRL, (unsigned)DMA_CTRL_SUBMIT);
    printf("START %u %u\n", (unsigned)(CLUSTER_BCAST_BASE + HDDU_BASE + HDDU_CTRL), (unsigned)HDDU_CTRL_START);
    for (uint32_t i = 0; i < num_layers; i++) {
        run_layer_gemm(&layer_configs[i]);
    }
    return 0;
}
"""


def _require_gcc():
    if shutil.which("gcc") is None:
        pytest.skip("host gcc is required")


def _render(tmp_path):
    m, k, n = 48, 64, 24
    workload = {
        "name": "gemm_host_trace",
        "hardware": {"num_clusters": 1, "num_pes": 12, "num_bus": 3, "spm_banks_per_group": 3,
                     "spm_bank_depth": 8192, "dram_base": 0x80000000},
        "tensors": {"A": {"shape": [m, k], "dtype": "fp16"},
                    "B": {"shape": [k, n], "dtype": "fp16"},
                    "C": {"shape": [m, n], "dtype": "fp16"}},
        "ops": [{"name": "gemm1", "type": "gemm", "inputs": ["A", "B"], "outputs": ["C"]}],
    }
    path = tmp_path / "workload.yaml"
    path.write_text(yaml.safe_dump(workload))
    src = tmp_path / "fw"
    generate_firmware(lower_workload(parse_workload(path)), src, kernel_json_dir=KERNEL_DIR)
    return src


def test_host_build_records_writes_and_semantic_waits(tmp_path):
    _require_gcc()
    src = _render(tmp_path)
    (src / "recorder.c").write_text(RECORDER)
    exe = tmp_path / "trace"
    subprocess.run(["gcc", "-std=gnu11", "-O0", "-w", "-DHACC_HOST_TRACE", "-I", str(src), "-o", str(exe),
                    str(src / "recorder.c"), str(src / "firmware_ops.c"), str(src / "firmware_data.c")],
                   check=True, capture_output=True, text=True)
    lines = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=60).stdout.splitlines()
    kinds = dict(zip(("dma_tag", "hddu_done"), map(int, lines[0].split()[1:])))
    ctrl, submit = map(int, lines[1].split()[1:])
    start_addr, start_bit = map(int, lines[2].split()[1:])
    events = [line.split() for line in lines[3:]]
    assert [e for e in events if e[0] == "R"] == [], "every hardware read belongs to a wait helper"
    waits = {int(e[1]) for e in events if e[0] == "WAIT"}
    assert {kinds["dma_tag"], kinds["hddu_done"]} <= waits
    submits = [e for e in events if e[0] == "W" and int(e[1]) == ctrl and int(e[2]) & submit]
    assert submits
    # One wave marker immediately before every HDDU start.
    starts = [i for i, e in enumerate(events) if e[0] == "W" and int(e[1]) == start_addr and int(e[2]) & start_bit]
    assert starts and all(events[i - 1][0] == "WAVE" for i in starts)
    assert sum(e[0] == "WAVE" for e in events) == len(starts)


def test_target_preprocessing_has_no_host_hooks(tmp_path):
    _require_gcc()
    src = _render(tmp_path)
    out = subprocess.run(["gcc", "-E", "-P", "-ffreestanding", "-I", str(src), str(src / "firmware_ops.c")],
                         check=True, capture_output=True, text=True).stdout
    assert "hacc_host" not in out
    assert '__asm__ volatile("wfi")' in out
    assert "*(volatile uint32_t*)addr = val" in out
