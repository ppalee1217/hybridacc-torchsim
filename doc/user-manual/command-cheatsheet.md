# Command Cheatsheet

## 1. repo root 基本入口

```bash
cd /home/easonyeh/hybridacc
uv sync
scripts/setup.sh all
uv run hacc-setup all
```

## 2. Python / ESL

### compile workload

```bash
uv run hacc-compile design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml -o output/compile-conv3x3 --dump-ir
```

### 生成 DRAM / golden

```bash
uv run python -m hybridacc_verify.gen.gen_test_dram --ir output/compile-conv3x3/hardware_ir.json --workload design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml --output-dir output/compile-conv3x3
```

### ESL build / run / compare

```bash
scripts/setup.sh fast hybridacc-sim build
scripts/setup.sh fast hybridacc-sim run-task output/compile-conv3x3
uv run python -m hybridacc_verify.check.compare_golden output/compile-conv3x3
```

## 3. 進 RTL 目錄的 canonical 入口

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make <target>'
```

下面所有 RTL / signoff 命令都預設套這個模式。

## 4. RTL smoke / regression

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make sim_tb_agu'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make sim_tb_hybridacc_smoke'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make sim_pe'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make sim_noc'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make sim_cluster'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make rtl_regress_single_wave'
```

## 5. firmware regression debug

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"'
```

## 6. synthesis / signoff

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make syn_top CLOCK_PERIOD_NS=1.25'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make syn_pe_ProcessElement'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make syn_pe_all'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make syn_report'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make primetime_full CLOCK_PERIOD_NS=1.25'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make primepower_full CLOCK_PERIOD_NS=1.25 PRIMEPOWER_FSDB=$PWD/tb_hybridacc_sim.fsdb'
```

## 7. gate sim / PrimePower activity

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make gate_sim_tb_hybridacc_smoke MOD_NAME=HybridAcc CLOCK_PERIOD_NS=1.25 GATE_NETLIST_DIR="$PWD/syn/clk_1p25ns" WAVE_DUMP=1 WAVE_DEPTH=0 SIM_PLUSARGS="+SDF_FILE=$PWD/syn/clk_1p25ns/HybridAcc/HybridAcc.sdf"'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make primepower_run CLOCK_PERIOD_NS=1.25 PRIMEPOWER_FSDB=$PWD/tb_hybridacc_sim.fsdb'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make primepower_analyze CLOCK_PERIOD_NS=1.25'
```

## 8. Jasper / Superlint

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make superlint'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make superlint_report'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make superlint_hotspot'
```

## 9. E2E sweep

```bash
uv run hacc-sweep gen --workload conv3x3 --output-dir output/hacc-conv3x3-sweeps
./scripts/fast_entry/run_e2e.sh $(cat output/hacc-conv3x3-sweeps/lists/conv3x3_all.list) --output-dir output/hacc-conv3x3-results --jobs $(nproc)
uv run hacc-sweep report --manifest output/hacc-conv3x3-sweeps/manifest.json --results-root output/hacc-conv3x3-results --output-dir output/hacc-conv3x3-report
```

## 10. 命名與參數速記

1. clock tag 用 `clk_1p25ns`，不是 `clk_1.25ns`。
2. top gate sim 常要補 `MOD_NAME=HybridAcc`。
3. top gate sim 的 `GATE_NETLIST_DIR` 用 `syn/clk_<period>ns`。
4. firmware regression trace 請用 `RTL_FW_DEBUG_PLUSARGS`。