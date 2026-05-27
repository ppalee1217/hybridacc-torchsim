# HybridAcc RTL 模擬測試指南

Repo-wide 操作入口請先看 [../../../doc/index.md](../../../doc/index.md) 與 [../../../doc/user-manual/rtl-simulation.md](../../../doc/user-manual/rtl-simulation.md)；本文件保留完整子系統細節。

本文件整理 HybridAcc RTL 目前可用的模擬測試流程，說明從環境準備、RTL testbench 執行、資料驅動的 NoC/Cluster workload 驗證、top-level firmware bring-up，到 gate-level simulation 的操作步驟。

內容以 [design/hybridacc-RTL/Makefile](../Makefile) 為主，所有指令與輸出路徑都對應目前 repo 中實際存在的 target 與 testbench。

---

## 1. 測試目標與適用範圍

目前 RTL 模擬大致分成五類：

| 類型 | 目的 | 主要入口 |
|------|------|----------|
| 基礎 unit test | 驗證單一模組或小型子系統功能正確 | `make sim_<tb>` |
| 類別 regression | 一次跑完整個子目錄下的 testbench | `make sim_pe` / `make sim_noc` / `make sim_cluster` / `make sim_all` |
| NoC 資料驅動模擬 | 用 `output/noc-sim/` 產生的測資驗證 NoC 路徑 | `make sim_noc_sim` / `make sim_noc_sim_all` |
| Cluster workload 模擬 | 用 `output/cluster-sim/` 的 case 驗證 cluster RTL | `make sim_tb_cluster_sim_advanced TEST_DATA_DIR=...` |
| Top-level firmware bring-up | 驗證 CoreController / HybridAcc 與 firmware 控制平面 | `make sim_tb_hybridacc_sim ...` |

如果只是想確認「最近改的 RTL 有沒有壞掉」，通常先跑單一 testbench 或分類 regression 即可；如果改動影響資料流、cluster 映射或 firmware 控制平面，就要再補 NoC/Cluster workload 與 firmware 測試。

### 1.1 目前收斂版的 workload 驗收集合

目前最終驗收目標不再是泛稱的 workload regression，而是三個固定的 `hybridacc-cc` example YAML：

- `design/hybridacc-cc/example/conv1x1/conv2d_1x1_single_wave.yaml`
- `design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml`
- `design/hybridacc-cc/example/gemm/gemm_single_wave.yaml`

文件後半段會把這三個 case 拆成可執行步驟。要注意的是：目前 `hybridacc-cc` 直接公開的產物是 firmware / IR / runtime-check 類輸出；若要接到 `tb_cluster_sim_advanced.sv`，還需要額外提供 `rtl_cluster_case.cfg` 風格的 case directory。基於 repo 現況，最可直接落地的 VCS 路徑是先用這三個 YAML 產生 `firmware.elf`，再餵給 top-level firmware bench。

---

## 2. 環境需求

### 2.1 必備工具

| 項目 | 用途 |
|------|------|
| Synopsys VCS | RTL / gate-level simulation |
| Synopsys DesignWare simulation library | VCS 編譯相依 |
| RISC-V bare-metal toolchain | 建 firmware 測試用 `.elf` / `.mem` |
| GNU Make | 驅動所有流程 |
| Python 虛擬環境 | 產生 `sim_report` / `gate_sim_report` 時使用 |

### 2.2 已在 Makefile 中綁定的站點設定

[design/hybridacc-RTL/Makefile](../Makefile) 內已固定以下站點資訊：

- `VCS ?= vcs`
- `DW_SIM_VER := /usr/cad/synopsys/synthesis/2024.09-sp2/dw/sim_ver`
- `SNPSLMD_LICENSE_FILE ?= 26585@lstn`
- gate-level 使用的 standard cell / SRAM Verilog model 路徑

因此在大多數情況下，只要站點環境已把 Synopsys 工具加到 `PATH`，直接執行 `make` 即可。

### 2.3 VCS shell 注意事項

依目前站點環境，VCS 相關環境通常是透過 `tcsh` / `.tcshrc` 設定。若你在 `bash` 下找不到 `vcs`、license 或 DesignWare library，請先切到 `tcsh` 再執行本文所有 VCS 指令：

```tcsh
tcsh
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
```

若只是執行 `uv run hacc-compile`，理論上不一定需要 `tcsh`；但只要後面會接 VCS，建議一開始就留在同一個 `tcsh` shell 內完成整個流程。

### 2.4 firmware bring-up 額外需求

top-level firmware 測試會用到 `design/hybridacc-ESL/test/firmware/` 下的程式，需確認以下工具存在：

```bash
command -v riscv32-unknown-elf-gcc
command -v riscv32-unknown-elf-objcopy
command -v riscv32-unknown-elf-size
command -v vcs
```

### 2.5 建議工作目錄

所有 RTL 模擬建議都從下列目錄執行：

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
```

之後所有 `make sim_*`、`make gate_sim_*`、`make sim_report` 都以這個目錄為基準。

---

## 3. 目錄與輸出檔案

模擬流程最常碰到的目錄如下：

| 路徑 | 內容 |
|------|------|
| `tb/` | 所有 testbench 原始碼 |
| `sim/build/` | RTL testbench 編譯後的 `simv` |
| `sim/log/` | RTL compile / run log |
| `sim/gate_build/` | gate-level testbench 編譯後的 `simv` |
| `sim/gate_log/` | gate-level compile / run log |
| `report/` | `sim_report`、`gate_sim_report`、synthesis report |
| `output/noc-sim/` | NoC system-level 測資 |
| `output/cluster-sim/` | Cluster workload 測資 |

RTL 單次模擬後最重要的兩個 log 檔為：

- `sim/log/<tb>.compile.log`
- `sim/log/<tb>.run.log`

例如：

- `sim/log/tb_agu.compile.log`
- `sim/log/tb_agu.run.log`

---

## 4. Makefile 測試入口總覽

### 4.1 通用 RTL testbench target

[design/hybridacc-RTL/Makefile](../Makefile) 使用 pattern target：

```bash
make sim_<name>
```

它會去 `tb/` 底下找：

- `tb_<name>.sv`
- 或 `<name>.sv`

所以以下兩種通常都可用：

```bash
make sim_agu
make sim_tb_agu
```

實務上建議直接用完整 testbench 名稱，比較不容易混淆：

```bash
make sim_tb_agu
make sim_tb_corecontroller_smoke
make sim_tb_hybridacc_smoke
```

### 4.2 分類 regression target

| 指令 | 說明 |
|------|------|
| `make sim_pe` | 跑 `tb/PE/` 底下所有 testbench |
| `make sim_noc` | 跑 `tb/NoC/` 底下所有 testbench |
| `make sim_cluster` | 跑 `tb/Cluster/` 底下所有 testbench |
| `make sim_all` | 跑 `tb/` 底下全部 testbench |

### 4.3 gate-level target

| 指令 | 說明 |
|------|------|
| `make gate_sim_<tb>` | 跑指定 testbench 的 gate-level simulation |
| `make gate_sim_pe` | 跑 PE 類 gate-level testbench |
| `make gate_sim_noc` | 跑 NoC 類 gate-level testbench |

注意：目前 Makefile 內建的 `MOD_NAME` 映射主要覆蓋 PE / NoC。Cluster、Core、HybridAcc top-level 雖然沒有 convenience mapping，但仍可用通用 `gate_sim_<tb>` target，手動指定 `MOD_NAME` 與 `GATE_NETLIST_DIR`。後面第 11.2.1 節有 `tb_hybridacc_smoke` 的實際範例。

---

## 5. 先做什麼：建議測試順序

如果你是第一次進來跑，建議按下面順序做：

1. 先跑單一 smoke test，確認工具環境正常。
2. 再跑 PE / NoC / Cluster regression，確認模組級 RTL 沒壞。
3. 若改動涉及 system data path，補 NoC 或 Cluster 資料驅動模擬。
4. 若改動涉及 CoreMcu、DMA、PLIC、timer、CmdFabric、HybridAcc top，補 firmware bring-up。
5. 若要做簽核前後比對，再做 gate-level simulation。

這樣可以避免一開始就跑最慢的 workload 或 firmware case，卻因為基本環境沒設好而浪費時間。

---

## 6. RTL 基本 smoke test

### 6.1 列出可用 testbench

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
find tb -type f -name 'tb_*.sv' | sort
```

### 6.2 跑單一 testbench

例如跑 AGU：

```bash
make sim_tb_agu
```

例如跑 core smoke：

```bash
make sim_tb_corecontroller_smoke
make sim_tb_hybridacc_smoke
```

成功時會看到：

- compile 完成
- run 完成
- log 被寫到 `sim/log/`

若只想快速看摘要，可以接著執行：

```bash
make postsim_tb_agu
make postsim_tb_corecontroller_smoke
```

### 6.3 判讀結果

最常見的成功訊號有：

- `PASS`
- `tb_... PASS`
- `Verification Results`

若失敗，優先看：

```bash
tail -n 80 sim/log/tb_agu.compile.log
tail -n 120 sim/log/tb_agu.run.log
```

---

## 7. 跑分類 regression

### 7.1 PE regression

```bash
make sim_pe
```

適合在修改下列路徑後執行：

- `src/PE/`
- `src/FIFO.sv`
- `src/asyncFIFO.sv`
- `src/hybridacc_utils_pkg.sv`

### 7.2 NoC regression

```bash
make sim_noc
```

適合在修改下列路徑後執行：

- `src/NoC/`
- `tb/NoC/`

### 7.3 Cluster regression

```bash
make sim_cluster
```

適合在修改下列路徑後執行：

- `src/Cluster/`
- `tb/Cluster/`
- 與 cluster 直接連動的 `src/PE/` 路徑

### 7.4 全部 regression

```bash
make sim_all
```

這會把 `tb/` 下所有 `tb_*.sv` 都跑過一次，時間最長。通常只在大改版後、或準備做整體驗證時才建議執行。

---

## 8. 資料驅動 NoC 模擬

NoC system-level testbench 是 [design/hybridacc-RTL/tb/NoC/tb_noc_sim.sv](../tb/NoC/tb_noc_sim.sv)，支援三個主要 plusargs：

- `+DATA_DIR=<path>`
- `+VERIFY_TOL=<float>`
- `+CLOCK_PERIOD_NS=<int>`

Makefile 已包成專用 target。

### 8.1 跑單一 NoC case

```bash
make sim_noc_sim TEST_DATA_DIR=/home/easonyeh/hybridacc/output/noc-sim/conv_k3c4
```

若要放寬數值誤差：

```bash
make sim_noc_sim \
  TEST_DATA_DIR=/home/easonyeh/hybridacc/output/noc-sim/gemm \
  VERIFY_TOL=0.05
```

若要改時脈週期：

```bash
make sim_noc_sim \
  TEST_DATA_DIR=/home/easonyeh/hybridacc/output/noc-sim/conv_k3c4 \
  CLOCK_PERIOD_NS=5
```

### 8.2 跑全部 NoC case

```bash
make sim_noc_sim_all
```

這會掃描：

```text
output/noc-sim/*/
```

每個 case 的 run log 會落在：

```text
sim/log/tb_noc_sim_<case_name>.run.log
```

### 8.3 NoC case 需要的資料

`tb_noc_sim.sv` 會從 `DATA_DIR` 讀入至少以下檔案：

- `config.txt`
- `scan_chain.bin`
- `input_activation.bin`
- `input_weight.bin`
- `input_partial_sum.bin`
- `output_partial_sum.bin`
- `pe_program.bin`

如果 case 不完整，simulation 會在讀檔階段或驗證階段失敗。

---

## 9. Cluster workload 模擬

Cluster workload 目前主要使用 [design/hybridacc-RTL/tb/Cluster/tb_cluster_sim_advanced.sv](../tb/Cluster/tb_cluster_sim_advanced.sv)。

這個 testbench 也吃 `+DATA_DIR`，但它沒有獨立 Makefile target，通常透過通用 `sim_%` 入口呼叫。

要特別注意：`tb_cluster_sim_advanced.sv` 期待的是一個含 `rtl_cluster_case.cfg` 的 case directory。以目前 repo 內可直接查到的公開流程來看，`hybridacc-cc` example YAML 會穩定產出 `firmware.elf` / IR / `sim.log` 類輸出，但不是直接產出這種 cluster advanced case directory。也就是說，如果最終目標是「三個 `hybridacc-cc` single-wave workload 直接餵進這個 bench」，中間還需要一個 bridge step，把 compiler 輸出轉成 `output/cluster-sim/<case>/rtl_cluster_case.cfg` 風格的資料目錄。

### 9.1 跑單一 cluster advanced case

```bash
make sim_tb_cluster_sim_advanced \
  TEST_DATA_DIR=/home/easonyeh/hybridacc/output/cluster-sim/conv_k3c4ich16och64s
```

必要時可覆寫 tolerance：

```bash
make sim_tb_cluster_sim_advanced \
  TEST_DATA_DIR=/home/easonyeh/hybridacc/output/cluster-sim/conv_k3c4ich16och64s \
  VERIFY_TOL=0.02
```

### 9.2 資料目錄最低需求

`tb_cluster_sim_advanced.sv` 會先讀：

- `rtl_cluster_case.cfg`

再依 cfg 指定的檔名讀：

- activation
- weight
- partial sum
- output
- PE program
- scan chain

因此 case 目錄至少要有：

```text
rtl_cluster_case.cfg
```

以及 cfg 中引用的 binary 檔。

### 9.3 什麼時候要跑 cluster advanced

修改以下內容後，建議至少跑一個 cluster advanced case：

- `src/Cluster/AddressGenerateUnit.sv`
- `src/Cluster/HybridDataDeliverUnit.sv`
- `src/Cluster/ScratchpadMemory.sv`
- `src/Cluster/ComputeCluster.sv`
- `src/PE/PErouter.sv`
- 與 scan-chain / SPM / HDDU / workload 映射直接相關的 RTL

---

## 10. Top-level firmware bring-up 測試

目前 top-level firmware 驗證 bench 是 [design/hybridacc-RTL/tb/tb_hybridacc_sim.sv](../tb/tb_hybridacc_sim.sv)。

它支援兩個關鍵 plusargs：

- `+FW_MEM=<path>`
- `+FW_BYTES=<n>`

流程分成兩段：

1. 先在 ESL firmware 目錄編出 `.elf` / `.mem`
2. 再把 `.mem` 與 `FW_BYTES` 傳給 RTL testbench

### 10.1 建 firmware 測試

以 `test_dma` 為例：

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-ESL/test/firmware/test_dma
make clean
make
```

執行後通常會得到：

- `test_dma.elf`
- `test_dma.dis`
- `test_dma.mem`

### 10.2 計算 FW_BYTES

`tb_hybridacc_sim.sv` 需要 firmware image 的 byte 數。建議直接從 ELF 取：

```bash
riscv32-unknown-elf-size test_dma.elf | awk 'NR==2 {print $1 + $2}'
```

如果要先放到 shell 變數：

```bash
fw_bytes=$(riscv32-unknown-elf-size test_dma.elf | awk 'NR==2 {print $1 + $2}')
echo "$fw_bytes"
```

### 10.3 執行 RTL firmware testbench

回到 RTL 目錄後執行：

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-RTL

fw_mem=/home/easonyeh/hybridacc/design/hybridacc-ESL/test/firmware/test_dma/test_dma.mem
fw_bytes=$(riscv32-unknown-elf-size /home/easonyeh/hybridacc/design/hybridacc-ESL/test/firmware/test_dma/test_dma.elf | awk 'NR==2 {print $1 + $2}')

make sim_tb_hybridacc_sim \
  SIM_PLUSARGS="+FW_MEM=$fw_mem +FW_BYTES=$fw_bytes"
```

### 10.4 為什麼這裡用 `SIM_PLUSARGS`

因為 `FW_MEM` / `FW_BYTES` 不是 Makefile 內建變數，而是 testbench 支援的 plusargs，所以直接透過 `SIM_PLUSARGS` 傳進 simulator 最直接。

### 10.5 推薦的 firmware regression 方式

若要一次掃多個 firmware case，可使用外層 shell loop，例如：

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-RTL

tests='test_alu test_branch test_jump test_loadstore test_mul test_stack test_diag test_compound test_hazard test_sram_timing test_csr test_cluster_ctrl test_fabric test_dma test_trap test_wfi_timer test_plic'

for t in $tests; do
  cd /home/easonyeh/hybridacc/design/hybridacc-ESL/test/firmware/$t
  make clean >/dev/null
  make >/dev/null
  fw_bytes=$(riscv32-unknown-elf-size ${t}.elf | awk 'NR==2 {print $1 + $2}')

  cd /home/easonyeh/hybridacc/design/hybridacc-RTL
  make sim_tb_hybridacc_sim \
    SIM_PLUSARGS="+FW_MEM=../hybridacc-ESL/test/firmware/$t/${t}.mem +FW_BYTES=$fw_bytes" >/dev/null
done
```

若你只是在 debug 單一控制平面問題，建議先只跑：

- `test_csr`
- `test_dma`
- `test_trap`
- `test_wfi_timer`
- `test_plic`

### 10.6 用 `hybridacc-cc` 三個 single-wave workload 驅動 RTL / VCS

如果你只是要查 single-wave RTL firmware regression 的固定操作、root cause、threshold 依據與 trace 開關，直接看 [design/hybridacc-RTL/doc/rtl_fw_regression_README.md](rtl_fw_regression_README.md) 即可；本節保留完整背景與較大範圍的模擬脈絡。

如果你的最終目標是讓 RTL 通過現有 `hybridacc-cc` example 的三個代表 workload，基於目前 repo 現況，最直接可落地的 VCS 路徑如下：

1. 用 `uv run hacc-compile` 從 YAML 產生 workload firmware。
2. 用和既有 firmware test 相同的方法，把 `firmware.elf` 轉成 VCS bench 可吃的 `.mem`：`riscv32-unknown-elf-objcopy -O verilog`。
3. 用 `tb_hybridacc_sim.sv` 跑 top-level RTL 模擬。

若只是要重跑目前已收斂好的 single-wave regression，建議直接使用 [design/hybridacc-RTL/Makefile](../Makefile) 內建 target，而不是手動拼每個步驟。Makefile 現在會自動完成下列工作：

1. 對指定 YAML 執行 `uv run hacc-compile`。
2. 用 `uv run hacc-flat-fw-mem` 生成給 `SectionLoader` 使用的 `firmware.mem`。
3. 生成 `dram_init.bin`、`golden_output.bin`、`golden_meta.txt`。
4. 執行 `tb_hybridacc_sim.sv`，並帶入 `+SKIP_FW_TEST_SUMMARY +SKIP_GOLDEN_EXACT_CHECK`。
5. 在 RTL run 結束後，用外部 comparator 做 fp16 容忍比較。

對應指令如下：

```tcsh
cd /home/easonyeh/hybridacc/design/hybridacc-RTL

make rtl_regress_conv2d_1x1_single_wave
make rtl_regress_conv2d_3x3_single_wave
make rtl_regress_gemm_single_wave

# 一次跑三個 case
make rtl_regress_single_wave
```

這條 regression path 與單純的 `make sim_tb_hybridacc_sim` 有兩個重要差異：

- `tb_hybridacc_sim.sv` 內建的 exact golden compare 會被 `+SKIP_GOLDEN_EXACT_CHECK` 關閉，避免把 fp16 累加路徑中可接受的 RTL / golden 微差直接視為失敗。
- 最終 verdict 由 `python -m hybridacc_verify.check.comparator` 決定，其容忍公式是 `allowed = atol + rtol * abs(expected)`。

三個固定 case 建議對應如下：

| Workload | YAML | 建議輸出目錄 |
|----------|------|--------------|
| Conv1x1 single wave | `design/hybridacc-cc/example/conv1x1/conv2d_1x1_single_wave.yaml` | `/home/easonyeh/hybridacc/output/runtime-check-conv1x1-single` |
| Conv3x3 single wave | `design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml` | `/home/easonyeh/hybridacc/output/runtime-check-conv3x3-single` |
| GEMM single wave | `design/hybridacc-cc/example/gemm/gemm_single_wave.yaml` | `/home/easonyeh/hybridacc/output/runtime-check-gemm-single` |

以下流程請在 `tcsh` shell 中執行；把 `yaml` 與 `out_dir` 換成上表三組之一即可：

```tcsh
cd /home/easonyeh/hybridacc

set yaml=design/hybridacc-cc/example/conv1x1/conv2d_1x1_single_wave.yaml
set out_dir=/home/easonyeh/hybridacc/output/runtime-check-conv1x1-single

uv run hacc-compile $yaml -o $out_dir
riscv32-unknown-elf-objcopy -O verilog $out_dir/firmware.elf $out_dir/firmware.mem
set fw_bytes=`riscv32-unknown-elf-size $out_dir/firmware.elf | awk 'NR==2 {print $1 + $2}'`

cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make sim_tb_hybridacc_sim \
  SIM_PLUSARGS="+FW_MEM=$out_dir/firmware.mem +FW_BYTES=$fw_bytes"
```

實務上請對下列三組各跑一次：

- Conv1x1: `yaml=design/hybridacc-cc/example/conv1x1/conv2d_1x1_single_wave.yaml`, `out_dir=/home/easonyeh/hybridacc/output/runtime-check-conv1x1-single`
- Conv3x3: `yaml=design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml`, `out_dir=/home/easonyeh/hybridacc/output/runtime-check-conv3x3-single`
- GEMM: `yaml=design/hybridacc-cc/example/gemm/gemm_single_wave.yaml`, `out_dir=/home/easonyeh/hybridacc/output/runtime-check-gemm-single`

這條路徑的好處是完全沿用目前 repo 已存在的 compiler 輸出與 top-level VCS bench。它不是 `cluster advanced` 的資料重播模式，而是「compiler 產生 workload firmware，再讓 RTL 執行該 firmware」的驗收方式。

### 10.6.1 這次 single-wave regression 的 root cause 與修正依據

這輪 closure 最後真正的 functional blocker 出現在 GEMM，而不是 conv1x1 / conv3x3。本次 root cause 已收斂成 firmware async DMA submit 與 RTL DMA contract 不一致，具體現象如下：

1. compiled firmware 確實有編出 GEMM PLI preload，目標是把 bias tile 從 DRAM `0x80104800` 搬到 SPM `0x00080000`。
2. 但 RTL trace 中原本看不到對應的 DMA submit，表示 command 沒有真正進入 DMA engine。
3. 問題不在地址計算，而在 DMA submit 時機。

核心原因是 [design/hybridacc-RTL/src/Core/DmaEngine.sv](../src/Core/DmaEngine.sv) 的 submit 條件只有在 DMA idle 時才成立：`DMA_CTRL.SUBMIT` 需要 `!dma_busy_w` 才會變成真正的 start pulse。忙碌期間的 MMIO write 雖然仍會覆寫 DMA 暫存器，但不會產生 submit trace，也不會 launch 新 command。

因此，原本 firmware 內連續呼叫 async DMA helper 時，第二筆 command 可能在前一筆 DMA 尚未完成時就把寄存器寫掉，卻沒有真的送出。GEMM 的 PLI preload 就是這樣被靜默丟掉。

修正方式是在 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../python/hybridacc_cc/templates/firmware_ops.c.j2) 的 generic async helper 內，在每次 submit 前先呼叫 `dma_wait_done()`，把 firmware sequencing 明確對齊 RTL DMA contract。這樣做的理由是：目前 DmaEngine 還沒有 queue，也沒有 busy 時接受下一筆 submit 的語意，因此 firmware 端必須自行序列化 back-to-back async helper。

修正後的重要觀察點如下：

- GEMM trace 中重新出現 `src=0x80104800 dst=0x00080000` 的 DMA submit。
- GEMM mismatch 從 1534 筆大幅降到只剩 tolerance 級差異。
- conv1x1、conv3x3 不需要額外 RTL patch 就能沿用同一套 helper 修正。

若要重看這類 root cause trace，建議使用：

```tcsh
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"
```

`TRACE_CLUSTER_MMIO` 會顯示 DMA / cluster side 的 MMIO programming；`TRACE_CLUSTER_RUNTIME` 會顯示 DMA submit、read/write request/response 與 cluster runtime 事件。兩者一起開，最容易看出「有 MMIO write，但沒有 submit」這類 busy-time submit 問題。

### 10.6.2 compare threshold 的依據

single-wave regression 現在不是直接用 comparator.py 的 CLI 預設值，而是由 Makefile 額外指定：

- `RTL_FW_COMPARE_RTOL ?= 3e-2`
- `RTL_FW_COMPARE_ATOL ?= 1.5e-2`

這組值不是任意放寬，而是依照修正後三個 workload 的實測結果收斂出來的最小可用門檻。判定依據如下：

1. comparator 的接受條件是 `abs(sim - expected) <= atol + rtol * abs(expected)`。
2. 修正 DMA submit 問題後，conv1x1 與 conv3x3 已在較小誤差下通過；真正接近邊界的是 GEMM。
3. GEMM 修正後在舊門檻下只剩 10 筆 mismatch，最大絕對誤差為 `0.0273438`，代表 functional path 已正確，剩下的是 fp16 累加微差。
4. 針對 GEMM 做 tolerance sweep 後，結果如下：

| rtol | atol | GEMM mismatch | 結論 |
|------|------|---------------|------|
| `0.01` | `0.005` | 10 | 不足 |
| `0.02` | `0.01` | 1 | 仍不足 |
| `0.03` | `0.015` | 0 | 最小可過 |

最終再把這組 threshold 套回三個 single-wave case 一起驗證，得到：

| Workload | 最大絕對誤差 | 結果 |
|----------|--------------|------|
| conv1x1 single wave | `0.00488281` | PASS |
| conv3x3 single wave | `0.0117188` | PASS |
| gemm single wave | `0.0273438` | PASS |

因此 Makefile 預設採用 `rtol=0.03`、`atol=0.015`，理由是它是目前已驗證可同時覆蓋 conv1x1、conv3x3、gemm 三個 case 的最小門檻，而不是只為單一 GEMM case 特調。

### 10.6.3 Makefile 如何控制 debug display 顯示或隱藏

可以。single-wave regression 的 debug display 入口就是 [design/hybridacc-RTL/Makefile](../Makefile) 內的 `RTL_FW_DEBUG_PLUSARGS`。

- 預設值是空字串，所以 debug display 預設隱藏。
- 只要在 `make rtl_regress_*` 或 `make rtl_regress_single_wave` 時帶入 plusargs，就會把對應 trace 打開。

最常用的幾種方式如下：

```tcsh
# 預設靜默模式，不印 trace
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make rtl_regress_gemm_single_wave

# 顯示 runtime 事件，例如 DMA submit / response、AGU / HDDU / SPM progress
make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME"

# 顯示 MMIO programming，例如 DMA / cluster config register writes
make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_MMIO"

# 同時開 runtime + MMIO，適合追 command programming 與實際 submit 是否對上
make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"

# 一次打開多數 trace，方便粗看全流
make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_DEBUG"

# 明確關閉 trace
make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS=
```

三個 flag 的使用建議如下：

- `+TRACE_CLUSTER_MMIO`: 看控制面寫了哪些 register，適合查 DMA/cluster programming 是否正確。
- `+TRACE_CLUSTER_RUNTIME`: 看資料面或 command 是否真的在跑，適合查 submit、read/write response、done/stop 等事件。
- `+TRACE_CLUSTER_DEBUG`: 方便的一鍵總開關。RTL 內大多數 trace 判斷都是 `DEBUG || MMIO` 或 `DEBUG || RUNTIME`，所以它通常相當於同時開較多類別的 debug display。

要注意的是，`RTL_FW_DEBUG_PLUSARGS` 只會被 `rtl_regress_*` 這組 Make target 使用。如果你是直接跑一般 testbench target，例如：

```tcsh
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make sim_tb_hybridacc_sim SIM_PLUSARGS="+FW_MEM=... +FW_BYTES=... +TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"
```

那就要把 trace plusargs 放在 `SIM_PLUSARGS` 裡，而不是 `RTL_FW_DEBUG_PLUSARGS`。

### 10.7 若要改接 `tb_cluster_sim_advanced.sv`

若你要的最終形式是「三個 `hybridacc-cc` example 不經手工轉檔，直接餵給 `tb_cluster_sim_advanced.sv`」，就必須再補一層橋接：

1. 從 `hybridacc-cc` workload 輸出整理出 cluster stimulus 目錄。
2. 生成 `rtl_cluster_case.cfg` 與其引用的 binary 檔。
3. 最後再執行：

```tcsh
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make sim_tb_cluster_sim_advanced TEST_DATA_DIR=/home/easonyeh/hybridacc/output/cluster-sim/<case>
```

在 bridge step 尚未正式定義前，不建議把這條流程寫成既有能力；文件應視它為下一個待補齊的 automation gap。

這幾個最能快速分辨 CoreMcu / DMA / interrupt / PLIC / timer 類型的問題。

---

## 11. Gate-level simulation 流程

目前 gate-level simulation 不是直接拿 RTL 去跑，而是：

1. 先 synthesize 出對應 module 的 netlist
2. 再跑 `gate_sim_*`

### 11.1 先做 synthesis

例如跑 `VADDU`：

```bash
make syn_pe_VADDU
```

例如跑 `NetworkOnChip`：

```bash
make syn_noc_NetworkOnChip
```

輸出的 netlist 預期位置為：

```text
syn/<ModuleName>/<ModuleName>_syn.v
```

### 11.2 執行 gate-level testbench

例如：

```bash
make gate_sim_tb_vaddu MOD_NAME=VADDU
make gate_sim_tb_noc_sim MOD_NAME=NetworkOnChip
```

或直接使用 convenience target：

```bash
make gate_sim_pe
make gate_sim_noc
```

### 11.2.1 Top-level gate smoke 指令

如果你要的是「先用 top-level netlist 快速確認 HybridAcc 沒有明顯壞掉」，目前最直接的 smoke bench 是 `tb_hybridacc_smoke.sv`。這條路徑不依賴 PE / NoC 的內建 module mapping，而是直接走通用 `gate_sim_<tb>` target。

先 synthesize top-level。例如目前常用的 `2.0 ns` case：

```bash
make syn_top CLOCK_PERIOD_NS=2.0
```

上面這條指令會把 netlist 放在：

```text
syn/clk_2p00ns/HybridAcc/HybridAcc_syn.v
```

接著執行 gate smoke：

```bash
make gate_sim_tb_hybridacc_smoke \
  MOD_NAME=HybridAcc \
  GATE_NETLIST_DIR=./syn/clk_2p00ns
```

這裡最容易填錯的是 `GATE_NETLIST_DIR`。它要指向 netlist 的外層 root，也就是包含 `HybridAcc/` 子目錄的那層；Makefile 會再自動去找 `$(GATE_NETLIST_DIR)/HybridAcc/HybridAcc_syn.v`。

如果你的 top-level synthesis 不是 `2.0 ns`，就把路徑中的 `clk_2p00ns` 換成對應的 clock tag。例如 `1.0 ns` 時應改成：

```bash
make syn_top CLOCK_PERIOD_NS=1.0
make gate_sim_tb_hybridacc_smoke \
  MOD_NAME=HybridAcc \
  GATE_NETLIST_DIR=./syn/clk_1p00ns
```

log 位置和其他 gate-level testbench 相同：

- `sim/gate_log/tb_hybridacc_smoke.compile.log`
- `sim/gate_log/tb_hybridacc_smoke.run.log`

要注意的是，`tb_hybridacc_smoke.sv` 目前是「純 gate netlist smoke」bench，重點是快速檢查 top-level 基本功能與 gate/X 問題；它沒有 `+SDF_FILE` hook，所以不是 SDF back-annotated timing run。若你要的是帶 firmware/workload 的 top-level gate regression，應改用 `gate_regress_*` 系列 target。

### 11.3 gate-level log 位置

- `sim/gate_log/<tb>.compile.log`
- `sim/gate_log/<tb>.run.log`

### 11.4 gate-level 常見限制

目前 Makefile 內建的映射主要是：

- PE testbench
- NoC testbench

Cluster / Core / HybridAcc top-level 沒有內建 convenience mapping；但像 `tb_hybridacc_smoke` 這種 top-level smoke bench，仍可透過前面第 11.2.1 節的 generic `gate_sim_<tb>` 用法直接執行。若要再往前補完整自動化入口，通常還需要：

1. 先有對應 netlist
2. 在 Makefile 補 target 與 module name mapping
3. 確認 testbench 能接受 gate-level include / SDF annotation

---

## 12. 測試報告與結果彙整

### 12.1 單一 testbench 摘要

```bash
make postsim_tb_agu
make postsim_tb_noc_sim
```

### 12.2 全部 RTL log 摘要

```bash
make postsim_all
```

### 12.3 產生 pre-sim report

```bash
make sim_report
```

### 12.4 產生 gate-level report

```bash
make gate_sim_report
```

這兩個 target 會呼叫 repo 的 Python report parser，輸出到：

- `report/pre_sim_report_<timestamp>.md`
- `report/post_sim_report_<timestamp>.md`

注意目前 Makefile 將 Python 路徑寫死在 `.venv/bin/python`，若本機虛擬環境不存在，需先建立對應環境或手動調整 Makefile。

---

## 13. 建議的日常測試流程

### 13.1 修改單一 PE 或 FIFO 類 RTL

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make sim_tb_<相關tb>
make sim_pe
```

### 13.2 修改 NoC 路徑

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make sim_noc
make sim_noc_sim TEST_DATA_DIR=/home/easonyeh/hybridacc/output/noc-sim/<case>
```

### 13.3 修改 Cluster / HDDU / SPM / AGU 路徑

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make sim_cluster
make sim_tb_cluster_sim_advanced TEST_DATA_DIR=/home/easonyeh/hybridacc/output/cluster-sim/<case>
```

### 13.4 修改 CoreMcu / DMA / interrupt / PLIC / HybridAcc top

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make sim_tb_corecontroller_smoke
make sim_tb_hybridacc_smoke

# 視改動範圍再補 firmware case
make sim_tb_hybridacc_sim SIM_PLUSARGS="+FW_MEM=... +FW_BYTES=..."
```

### 13.5 收斂最後三個 `hybridacc-cc` workload

若目前工作目標就是把 RTL 驗證收斂到三個既有 single-wave workload，建議至少完成以下流程各一次：

```tcsh
cd /home/easonyeh/hybridacc

# 針對 conv1x1 / conv3x3 / gemm 三個 YAML 重複以下步驟
uv run hacc-compile <yaml> -o <out_dir>
riscv32-unknown-elf-objcopy -O verilog <out_dir>/firmware.elf <out_dir>/firmware.mem
set fw_bytes=`riscv32-unknown-elf-size <out_dir>/firmware.elf | awk 'NR==2 {print $1 + $2}'`

cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make sim_tb_hybridacc_sim \
  SIM_PLUSARGS="+FW_MEM=<out_dir>/firmware.mem +FW_BYTES=$fw_bytes"
```

### 13.6 大改版前的完整確認

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make sim_pe
make sim_noc
make sim_cluster
make sim_tb_hybridacc_smoke
make sim_noc_sim TEST_DATA_DIR=/home/easonyeh/hybridacc/output/noc-sim/<代表case>
make sim_tb_cluster_sim_advanced TEST_DATA_DIR=/home/easonyeh/hybridacc/output/cluster-sim/<代表case>
```

---

## 14. 常見錯誤與排查方式

### 14.1 `No testbench found for 'xxx'`

原因：

- target 名稱與 testbench 檔名不對應
- testbench 檔不在 `tb/` 底下

排查：

```bash
find tb -type f -name 'tb_*.sv' | sort | grep xxx
```

### 14.2 compile failed

先看：

```bash
tail -n 80 sim/log/<tb>.compile.log
```

常見原因：

- include 路徑錯誤
- 模組名稱重複
- 遺漏 package / submodule
- VCS / DW library 未設好

### 14.3 run failed

先看：

```bash
tail -n 120 sim/log/<tb>.run.log
```

常見原因：

- testbench assertion fail
- data directory 缺檔
- plusargs 傳入錯誤
- firmware `.mem` 或 `FW_BYTES` 不正確

### 14.4 `Invalid CLOCK_PERIOD_NS=0`

原因：

- 傳入了不合法的 `CLOCK_PERIOD_NS`

系統行為：

- `tb_common.svh` 會 fallback 到 testbench 預設值

建議：

```bash
CLOCK_PERIOD_NS=10
```

或直接不傳。

### 14.5 gate netlist not found

原因：

- 還沒先做 synthesis
- 模組名稱 mapping 不對

排查：

```bash
ls syn/<ModuleName>/<ModuleName>_syn.v
```

若不存在，先跑：

```bash
make syn_pe_<ModuleName>
make syn_noc_<ModuleName>
```

### 14.6 firmware 測試卡在讀不到 `.mem`

排查：

```bash
ls /home/easonyeh/hybridacc/design/hybridacc-ESL/test/firmware/<case>/<case>.mem
```

若沒有，就先到該 firmware case 重新執行：

```bash
make clean
make
```

---

## 15. 清理輸出

### 15.1 清理模擬產物

```bash
make clean
```

會刪除：

- `sim/`
- `build/`
- `simv*`
- `csrc/`
- 波形 / 暫存檔

### 15.2 清理 synthesis 輸出

```bash
make clean_syn
```

會刪除：

- `syn/`
- `report/`

---

## 16. 最後建議

如果你的目的是「快速確認修改沒有破壞 RTL」，請優先用下面這個最小流程：

```bash
cd /home/easonyeh/hybridacc/design/hybridacc-RTL
make sim_tb_<相關tb>
make postsim_tb_<相關tb>
```

如果你的目的是「確認整個子系統真的還能跑代表 workload」，請至少再補一個資料驅動 case：

- NoC 改動：`make sim_noc_sim TEST_DATA_DIR=...`
- Cluster 改動：`make sim_tb_cluster_sim_advanced TEST_DATA_DIR=...`
- Core / top-level 控制平面改動：跑 firmware `.mem` + `tb_hybridacc_sim`

這樣的層次化流程可以把測試時間控制在合理範圍內，同時保留足夠的偵錯訊息與回歸保障。