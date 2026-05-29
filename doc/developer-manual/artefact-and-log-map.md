# Artefact And Log Map

## 1. 適用範圍

這份文件用來回答「東西跑完了，但輸出到底落在哪裡」這個問題。當你只想先定位 log、report、netlist、golden 或 sweep 結果，不想先讀完整 workflow 文件時，直接查這份表。

## 2. 最常用的總路徑

| 路徑 | 內容 |
| --- | --- |
| `output/` | repo-wide 生成結果、sweep、runtime-check、regression artefact |
| `design/hybridacc-RTL/sim/log/` | RTL compile / run / compare log |
| `design/hybridacc-RTL/sim/gate_log/` | gate sim compile / run log |
| `design/hybridacc-RTL/build/` | synthesis / signoff / Jasper workdir |
| `design/hybridacc-RTL/syn/` | synthesis netlist / SDF / DDC |
| `design/hybridacc-RTL/report/` | synthesis / signoff / analysis report |

## 3. workflow 對照

### 3.1 ESL / compiler case

典型位置：`output/<case>/`

最常見檔案：

- `firmware.elf`
- `hardware_ir.json`
- `hardware_viz.html`
- `dram_init.bin`
- `golden_output.bin`
- `golden_meta.txt`

### 3.2 RTL simulation

| 類型 | 路徑 |
| --- | --- |
| compile log | `design/hybridacc-RTL/sim/log/<tb>.compile.log` |
| run log | `design/hybridacc-RTL/sim/log/<tb>.run.log` |
| compare log | `design/hybridacc-RTL/sim/log/<tb>.compare.log` |

### 3.3 gate simulation

| 類型 | 路徑 |
| --- | --- |
| compile log | `design/hybridacc-RTL/sim/gate_log/<tb>.compile.log` |
| run log | `design/hybridacc-RTL/sim/gate_log/<tb>.run.log` |

### 3.4 synthesis

| 類型 | 路徑 | 命名規則 |
| --- | --- | --- |
| netlist | `syn/clk_<period>ns/<Module>/<Module>_syn.v` | 例如 `clk_1p25ns` |
| SDF | `syn/clk_<period>ns/<Module>/<Module>.sdf` | 與同 clock tag 對應 |
| compile log | `report/clk_<period>ns/<Module>/syn_compile_<Module>.log` | module 名含大小寫 |
| timing | `report/clk_<period>ns/<Module>/timing_{max,min}_rpt_<Module>.txt` | setup / hold |
| power / area / cell | `report/clk_<period>ns/<Module>/{power,area,cell}_rpt_<Module>.txt` | 靜態報告 |

### 3.5 PrimeTime

| 類型 | 路徑 |
| --- | --- |
| build | `design/hybridacc-RTL/build/primetime/clk_<period>ns/` |
| raw report | `design/hybridacc-RTL/report/primetime/clk_<period>ns/` |
| HTML / PDF | `design/hybridacc-RTL/report/primetime/clk_<period>ns/analysis/` |

### 3.6 PrimePower

| 類型 | 路徑 |
| --- | --- |
| build | `design/hybridacc-RTL/build/primepower/clk_<period>ns/<activity_tag>/` |
| raw report | `design/hybridacc-RTL/report/primepower/clk_<period>ns/<activity_tag>/` |
| HTML / PDF | `design/hybridacc-RTL/report/primepower/clk_<period>ns/<activity_tag>/analysis/` |
| consolidated dashboard | `design/hybridacc-RTL/report/signoff-dashboard/clk_<period>ns__<activity_tag>/index.html` |

### 3.7 firmware regression

主要路徑：`output/rtl-fw-regress/`

結構大致如下：

```text
output/rtl-fw-regress/
├── conv2d_1x1_single_wave/
├── conv2d_3x3_single_wave/
└── gemm_single_wave/
```

每個 case 下最常看：

- `firmware.elf`
- `firmware.mem`
- `dram_init.bin`
- `golden_output.bin`
- `golden_meta.txt`

### 3.8 sweep / e2e pipeline

| 類型 | 路徑 | 內容 |
| --- | --- | --- |
| sweep input | `output/hacc-<workload>-sweeps/` | `manifest.json`、`lists/` |
| sweep result | `output/hacc-<workload>-results/` | case 執行結果 |
| sweep report | `output/hacc-<workload>-report/` | summary report |

### 3.9 NoC / Cluster data-driven case

| 類型 | 路徑 | 最低要求 |
| --- | --- | --- |
| NoC case | `output/noc-sim/<case>/` | `config.txt`、`scan_chain.bin`、`input_*.bin`、`output_*.bin` |
| Cluster case | `output/cluster-sim/<case>/` | `rtl_cluster_case.cfg` 與其引用的 binary |

## 4. 找不到結果時的最短分流

1. 不確定是不是命令根本沒跑起來：先回 terminal log。
2. RTL / gate sim：先看 `sim/log/` 或 `sim/gate_log/`。
3. synthesis / signoff：先看 `report/`，再看 `build/`。
4. compiler / ESL / regression：先看 `output/<case>/` 或 `output/rtl-fw-regress/`。

## 5. 成功判準

當 workflow 跑完時，你應該知道：

1. 第一個該看的 log 在哪裡。
2. 原始 report 在哪裡。
3. HTML / PDF analysis 在哪裡。
4. golden、netlist、SDF、FSDB 各自落在哪裡。

## 6. 相關文件

- [repo-map.md](repo-map.md)
- [rtl-simulation.md](../user-manual/rtl-simulation.md)
- [rtl-firmware-regression.md](../user-manual/rtl-firmware-regression.md)
- [synthesis-and-postsim.md](../user-manual/synthesis-and-postsim.md)
- [command-cheatsheet.md](../user-manual/command-cheatsheet.md)