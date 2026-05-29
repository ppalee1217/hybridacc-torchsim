# RTL Firmware Regression

## 1. 適用範圍

這份文件整理目前固定收斂的 single-wave RTL/VCS firmware regression 流程，對應三個已驗證 workload：

1. `conv2d_1x1_single_wave`
2. `conv2d_3x3_single_wave`
3. `gemm_single_wave`

這不是單純跑 `tb_hybridacc_sim`，而是一條完整 pipeline：compile workload、生成 golden、執行 RTL、再做 fp16 compare。

## 2. 什麼時候該跑這個

1. top-level RTL 有改動。
2. firmware template / runtime 行為有改動。
3. 要確認 conv1x1、conv3x3、gemm 三個代表 workload 仍維持收斂。

## 3. 前置條件

- 在 `tcsh`
- 工作目錄在 repo root 下的 `design/hybridacc-RTL/`
- `uv run hacc-compile`、RISC-V toolchain、`vcs` 都可用

## 4. 主要入口命令

### 4.1 單一 workload

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make rtl_regress_conv2d_1x1_single_wave'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make rtl_regress_conv2d_3x3_single_wave'
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make rtl_regress_gemm_single_wave'
```

### 4.2 一次跑完三個 case

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make rtl_regress_single_wave'
```

### 4.3 開 trace debug

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"'
```

## 5. target 內部實際做了什麼

每個 regression target 會自動串起以下階段：

1. `uv run hacc-compile`
2. 生成 `firmware.mem`
3. 生成 `dram_init.bin`、`golden_output.bin`、`golden_meta.txt`
4. 啟動 `tb_hybridacc_sim`
5. 以 fp16 tolerance 做 compare

這也是為什麼它比直接跑 `make sim_tb_hybridacc_sim` 更適合做回歸。

## 6. tolerance 規則

目前已驗證的 compare threshold 為：

1. `rtol=0.03`
2. `atol=0.015`

除非重新對三個 fixed case 做完整再驗證，否則不要任意 override。

## 7. artefact 與 log

### 7.1 case artefact

- `output/rtl-fw-regress/conv2d_1x1_single_wave/`
- `output/rtl-fw-regress/conv2d_3x3_single_wave/`
- `output/rtl-fw-regress/gemm_single_wave/`

每個 case 內最常看的檔：

- `firmware.elf`
- `firmware.mem`
- `dram_init.bin`
- `golden_output.bin`
- `golden_meta.txt`

### 7.2 simulator log

- `design/hybridacc-RTL/sim/log/tb_hybridacc_sim_<case>.compile.log`
- `design/hybridacc-RTL/sim/log/tb_hybridacc_sim_<case>.run.log`
- `design/hybridacc-RTL/sim/log/tb_hybridacc_sim_<case>.compare.log`

## 8. trace plusargs 怎麼用

請使用 `RTL_FW_DEBUG_PLUSARGS`，不要誤塞到一般 `SIM_PLUSARGS`。

最常用的選項：

- `+TRACE_CLUSTER_MMIO`: 看 DMA / cluster register programming
- `+TRACE_CLUSTER_RUNTIME`: 看 DMA submit / response、AGU / SPM runtime 事件
- `+TRACE_CLUSTER_DEBUG`: 大量總 trace 開關

範例：

```bash
cd "$(git rev-parse --show-toplevel)" && tcsh -ic 'source ~/.tcshrc; cd design/hybridacc-RTL; make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"'
```

## 9. 成功判準

1. compile / run / compare 三階段都完成。
2. comparator 沒有 functional mismatch。
3. 若有少量 fp16 誤差，仍必須落在已驗證 threshold 內。

## 10. 常見失敗點

1. 沒用 `tcsh`，導致 `vcs` 或 toolchain 找不到。
2. comparator threshold 被 override，造成本來應該通過的 case 被誤判 fail。
3. runtime / MMIO trace 想看卻把 plusargs 放錯變數。
4. workload 改了卻沿用舊 artefact，導致 compare 結果失真。

## 11. 已知 GEMM 問題背景

GEMM 曾有過 DMA submit 在前一筆 DMA 尚未清空時遺失的問題。若後續 GEMM 再次出現異常，優先把它當成 runtime / DMA scheduling 類問題檢查，而不是先懷疑 compare threshold。

## 12. 最短排錯順序

1. 先看 `.compile.log` 是否乾淨。
2. 再看 `.run.log` 是否有 fatal / hang。
3. 最後看 `.compare.log` 分辨是全壞還是少量 fp16 誤差。

## 13. 相關文件

- [rtl-simulation.md](rtl-simulation.md)
- [debug-playbook.md](../developer-manual/debug-playbook.md)
- [artefact-and-log-map.md](../developer-manual/artefact-and-log-map.md)
- [../../design/hybridacc-RTL/doc/rtl_fw_regression_README.md](../../design/hybridacc-RTL/doc/rtl_fw_regression_README.md)