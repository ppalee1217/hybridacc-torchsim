# RTL Simulation

## 1. 適用範圍

這份文件是 `design/hybridacc-RTL` 模擬工作流的完整入口，涵蓋：

1. 單一 testbench smoke
2. 分類 regression
3. data-driven NoC / Cluster 模擬
4. top-level firmware bring-up
5. gate-level simulation

## 2. 固定工作模式

1. 一律在 `tcsh` 執行。
2. 工作目錄固定在 repo root 下的 `design/hybridacc-RTL/`。
3. 先跑最便宜的單元 testbench，再擴到 regression，最後才進 top-level 或 gate sim。

## 3. 推薦執行順序

### 3.1 最便宜的 smoke

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_tb_agu'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_tb_corecontroller_smoke'
```

### 3.2 類別 regression

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_pe'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_noc'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_cluster'
```

### 3.3 top-level smoke / bring-up

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_tb_hybridacc_smoke'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_tb_hybridacc_sim'
```

若是手動 bring-up firmware 測試，而不是走 `rtl_regress_*`，通常要額外帶：

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_tb_hybridacc_sim SIM_PLUSARGS="+FW_MEM=/path/to/firmware.mem +FW_BYTES=12345"'
```

這條路徑適合單獨 debug 控制面；若要做固定 workload 回歸，仍建議直接用 [rtl-firmware-regression.md](rtl-firmware-regression.md)。

### 3.4 data-driven NoC / Cluster case

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_noc_sim TEST_DATA_DIR=output/noc-sim/conv_k3c4 VERIFY_TOL=0.05 CLOCK_PERIOD_NS=5'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_tb_cluster_sim_advanced TEST_DATA_DIR=output/cluster-sim/<case> VERIFY_TOL=0.02'
```

### 3.5 gate-level simulation

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make gate_sim_tb_hybridacc_smoke MOD_NAME=HybridAcc CLOCK_PERIOD_NS=1.25 GATE_NETLIST_DIR="$PWD/syn/clk_1p25ns"'
```

## 4. target 命名怎麼看

1. `make sim_<name>` 對應單一 testbench。
2. 類別 target 有 `sim_pe`、`sim_noc`、`sim_cluster`、`sim_all`。
3. data-driven 模擬會額外要求 `TEST_DATA_DIR`。
4. gate sim 常需要 `MOD_NAME`、`CLOCK_PERIOD_NS`、`GATE_NETLIST_DIR`。

## 5. 輸入與輸出

| 類型 | 主要輸入 | 主要輸出 |
| --- | --- | --- |
| 單一 testbench | testbench + RTL | `sim/log/<tb>.compile.log`、`sim/log/<tb>.run.log` |
| 類別 regression | 多個 testbench | 多組 compile / run log |
| data-driven NoC | `output/noc-sim/<case>` | 模擬結果與 verify log |
| data-driven Cluster | `output/cluster-sim/<case>` | 模擬結果與 verify log |
| gate sim | netlist、SDF | `sim/gate_log/` 下的 compile / run log |

## 6. log 該先看哪裡

### 6.1 RTL simulation

- `sim/log/<tb>.compile.log`
- `sim/log/<tb>.run.log`

### 6.2 gate simulation

- `sim/gate_log/<tb>.compile.log`
- `sim/gate_log/<tb>.run.log`

快速看尾端：

```bash
tail -n 80 sim/log/tb_agu.compile.log
tail -n 120 sim/log/tb_agu.run.log
```

## 7. 成功判準

1. compile 完成。
2. run 完成。
3. 關鍵 testbench 沒有 assertion、fatal、timeout 或 mismatch。
4. data-driven case 的 verify 結果為 PASS。

## 8. 常見失敗點

1. `vcs` 找不到：通常是 shell 不在 `tcsh`。
2. top-level firmware bench 缺 `FW_MEM` / `FW_BYTES`：不要手動硬湊，改走 [rtl-firmware-regression.md](rtl-firmware-regression.md)。
3. `TEST_DATA_DIR` 結構不完整：NoC 與 Cluster case 會直接 fail。
4. gate sim netlist / SDF 路徑不對：先確認 `CLOCK_PERIOD_NS` 對應的 synthesis 產物存在。

## 9. 報告與摘要 target

若你已經跑完模擬，想看 parser 彙整：

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make postsim_tb_agu'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make postsim_all'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make sim_report'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make gate_sim_report'
```

最常見輸出：

1. `report/pre_sim_report_<timestamp>.md`
2. `report/post_sim_report_<timestamp>.md`

## 10. 依改動範圍選測試

### 10.1 改單一 PE / FIFO 類 RTL

1. 先跑對應單一 testbench。
2. 再跑 `make sim_pe`。

### 10.2 改 NoC 路徑

1. 先跑 `make sim_noc`。
2. 再補一個 `sim_noc_sim TEST_DATA_DIR=...`。

### 10.3 改 Cluster / HDDU / SPM / AGU

1. 先跑 `make sim_cluster`。
2. 再補一個 `sim_tb_cluster_sim_advanced TEST_DATA_DIR=...`。

### 10.4 改 CoreMcu / DMA / interrupt / PLIC / top

1. 先跑 `make sim_tb_corecontroller_smoke`。
2. 再跑 `make sim_tb_hybridacc_smoke`。
3. 之後視情況補 `sim_tb_hybridacc_sim` 或 `rtl_regress_*`。

## 11. 失敗時的最短分流

1. compile fail：先看 include / module / library 問題。
2. run fail：先找 assertion、fatal、timeout。
3. compare fail：若是 top-level firmware，改查 [rtl-firmware-regression.md](rtl-firmware-regression.md)。
4. gate sim fail：先看 `GATE_NETLIST_DIR`、`MOD_NAME`、`+SDF_FILE` 是否同一版。

## 12. 相關 artefact 與 log

- `design/hybridacc-RTL/sim/build/`
- `design/hybridacc-RTL/sim/log/`
- `design/hybridacc-RTL/sim/gate_build/`
- `design/hybridacc-RTL/sim/gate_log/`
- `output/noc-sim/`
- `output/cluster-sim/`

## 13. 相關文件

- [rtl-firmware-regression.md](rtl-firmware-regression.md)
- [synthesis-and-postsim.md](synthesis-and-postsim.md)
- [artefact-and-log-map.md](../developer-manual/artefact-and-log-map.md)
- [../../design/hybridacc-RTL/doc/sim_test_guide.md](../../design/hybridacc-RTL/doc/sim_test_guide.md)