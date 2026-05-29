# Glossary

## 1. 適用範圍

這份術語表整理 manual 中高頻出現的名詞，特別偏重「操作時會實際碰到的 artefact、IR、工具與硬體縮寫」。

## 2. 主要術語

| 術語 | 定義 | 為什麼你會在操作流程中碰到 |
| --- | --- | --- |
| `ESL` | SystemC-based simulator 與其工作流 | 用來跑 compiler + simulator + golden compare |
| `RTL` | Verilog / SystemVerilog 設計與 testbench flow | VCS / synthesis / signoff 的主要輸入 |
| `PE` | Process Element，運算處理單元 | 在 RTL、NoC、compiler 與 testbench 中都常出現 |
| `NoC` | Network on Chip | Cluster 與 PE 間的資料傳輸網路 |
| `SPM` | Scratchpad Memory | 本地資料暫存，常出現在 DMA / runtime trace |
| `AGU` | Address Generation Unit | 負責 DRAM / SPM 位址生成 |
| `HDDU` | Hybrid Data Deliver Unit | 常出現在 data path 與 debug 文件 |
| `MMIO` | Memory-mapped I/O | firmware 透過 register programming 控制 cluster / DMA |
| `PLI` | Partial Line Interface | 與 preload DMA / runtime sequencing 相關 |
| `single-wave` | 固定收斂的 firmware regression workload 類型 | 指 conv1x1、conv3x3、gemm 三個固定案例 |
| `runtime-check` | 由 compiler / verify flow 生成的檢查 artefact | 常見於 `output/` 內的驗證資料 |
| `gate sim` | 以 synthesis 後 netlist 與 SDF 執行的模擬流程 | 常作為 PrimePower 前置 activity 來源 |
| `Superlint` | Jasper / lint 類靜態檢查流程 | 用於 RTL 靜態檢查 |
| `PrimeTime` | STA 分析流程，主要使用 `pt_shell` | 用於 timing signoff |
| `PrimePower` | 以 `pt_shell` 啟用 power analysis 的功耗分析流程 | 用於 gate-level activity-based power 分析 |
| `artefact` | workflow 產生的結果檔 | 例如 netlist、SDF、report、log、DRAM image |

## 3. IR 與檔案術語

| 術語 | 定義 | 常見檔案 |
| --- | --- | --- |
| `WorkloadIR` | 前端 workload 的中介表示 | 通常由 YAML 經 compiler 解析後形成 |
| `HardwareIR` | lower 後的硬體配置與 tiling 描述 | `hardware_ir.json` |
| `tiling_params` | tile 尺寸與 DRAM 佈局資訊 | `hardware_ir.json` 內的重要欄位 |
| `hardware_ir.json` | HardwareIR 的 JSON 輸出 | `gen_test_dram` 的主要輸入 |
| `firmware.elf` | RISC-V firmware 二進位 | ESL / RTL top-level workflow 會載入 |
| `firmware.mem` | 提供 RTL testbench 載入的 firmware 映像 | firmware regression 常見輸出 |
| `dram_init.bin` | DRAM 初始化映像 | simulator / RTL 會使用 |
| `golden_output.bin` | golden 參考輸出 | compare 時的 expected data |
| `golden_meta.txt` | golden 相關摘要資訊 | compare 與 debug 時常回查 |

## 4. 驗證與比較術語

| 術語 | 定義 |
| --- | --- |
| `rtol / atol` | 浮點比較的相對 / 絕對誤差容忍值 |
| `compare_golden` | 以 golden 輸出檢查 simulator / RTL 結果的流程 |
| `TEST_DATA_DIR` | data-driven simulation 的輸入資料目錄 |

## 5. EDA 與 signoff 術語

| 術語 | 定義 |
| --- | --- |
| `SDC` | Synopsys Design Constraint，定義 clock / I/O / operating condition |
| `DW stub` | DesignWare 簡化模型，用於避免把 helper 內部實作一起當成 lint 主體 |
| `SRAM stub` | Jasper-only SRAM macro placeholder |
| `CTS` | Clock Tree Synthesis，分析前後的 timing 假設會改變 |
| `clk_1p25ns` | synthesis / signoff 的 clock tag 命名格式 |

## 6. 相關文件

- [python-cli-reference.md](python-cli-reference.md)
- [esl-workflows.md](esl-workflows.md)
- [rtl-firmware-regression.md](rtl-firmware-regression.md)
- [synthesis-and-postsim.md](synthesis-and-postsim.md)

## 7. 長期追加規則

只要有新的術語、artefact、IR 欄位、report 名稱或慣用縮寫進入 manual，就應同步更新本文件。

新增條目時請遵守：

1. 定義要短，優先用操作情境來說明。
2. 若只是舊詞的別名，優先併入現有條目而不是新增重複術語。
3. 若術語會影響命令、輸出或排錯，請同時補到對應 manual。