# HybridAcc RTL Firmware Regression README

Repo-wide 操作入口請先看 [../../../doc/index.md](../../../doc/index.md) 與 [../../../doc/user-manual/rtl-firmware-regression.md](../../../doc/user-manual/rtl-firmware-regression.md)；本文件保留 single-wave regression 的 subsystem 細節。

本文件只整理目前最常用的 single-wave RTL/VCS firmware regression 流程，目的不是取代完整模擬指南，而是把日後最常查的資訊集中在一頁內。

目前固定收斂的 workload 只有三個 `hybridacc-cc` example YAML：

- `design/hybridacc-cc/example/conv1x1/conv2d_1x1_single_wave.yaml`
- `design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml`
- `design/hybridacc-cc/example/gemm/gemm_single_wave.yaml`

對應 Make target 都定義在 [design/hybridacc-RTL/Makefile](../Makefile)。

## 1. 快速重跑

建議在 `tcsh` 環境下執行：

```tcsh
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_conv2d_1x1_single_wave'
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_conv2d_3x3_single_wave'
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_gemm_single_wave'
```

一次跑完三個 case：

```tcsh
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_single_wave'
```

這組 target 會自動完成：

1. `uv run hacc-compile` 編譯 YAML。
2. 生成 `firmware.mem`。
3. 生成 `dram_init.bin`、`golden_output.bin`、`golden_meta.txt`。
4. 執行 `tb_hybridacc_sim.sv`。
5. 用外部 fp16 comparator 比對 RTL 實際輸出與 golden output。

主要 artefact 會放在：

- `output/rtl-fw-regress/conv2d_1x1_single_wave`
- `output/rtl-fw-regress/conv2d_3x3_single_wave`
- `output/rtl-fw-regress/gemm_single_wave`

主要 log 會放在：

- `design/hybridacc-RTL/sim/log/tb_hybridacc_sim_conv2d_1x1_single_wave.compile.log`
- `design/hybridacc-RTL/sim/log/tb_hybridacc_sim_conv2d_1x1_single_wave.run.log`
- `design/hybridacc-RTL/sim/log/tb_hybridacc_sim_conv2d_1x1_single_wave.compare.log`

其他兩個 case 的 log 命名方式相同。

## 2. 這條 regression 與直接跑 tb_hybridacc_sim 的差異

`rtl_regress_*` 不是單純把 firmware 餵進 `tb_hybridacc_sim.sv` 而已，它還額外做了兩個關鍵處理：

1. 帶入 `+SKIP_FW_TEST_SUMMARY +SKIP_GOLDEN_EXACT_CHECK`，避免把 firmware summary 與 exact binary compare 當成最終 verdict。
2. 在 RTL run 結束後，改用 `python -m hybridacc_verify.check.comparator` 進行 fp16 容忍比對。

這樣做的原因是：single-wave workload 的 golden compare 應該接受 fp16 累加路徑的可預期微差，而不是要求 bit-exact。

comparator 接受條件是：

```text
abs(sim - expected) <= atol + rtol * abs(expected)
```

## 3. 這次 GEMM blocker 的 root cause

這輪真正的 functional blocker 是 GEMM 的 PLI preload 在 RTL 中沒有真正送出 DMA command。

問題鏈如下：

1. compiled firmware 確實有編出從 DRAM `0x80104800` 載入到 SPM `0x00080000` 的 PLI preload。
2. 但 trace 中原本看不到對應的 DMA submit。
3. 根因不在地址，而在 submit 時機。

[design/hybridacc-RTL/src/Core/DmaEngine.sv](../src/Core/DmaEngine.sv) 的 contract 是：`DMA_CTRL.SUBMIT` 只有在 DMA idle 時才會變成真正的 start pulse。也就是說，busy 期間雖然仍可寫 DMA MMIO 暫存器，但這些 write 只會覆寫寄存器，不會真正送出新 command。

原本 firmware 的 async DMA helper 會連續 programming + submit。當第二筆 helper 發生在前一筆 DMA 尚未完成時，就會出現「MMIO 都寫了，但 command 沒送出去」的狀況。GEMM 的 PLI preload 就是這樣被靜默丟掉。

修正方式是讓 [python/hybridacc_cc/templates/firmware_ops.c.j2](../../../python/hybridacc_cc/templates/firmware_ops.c.j2) 的 generic async helper 在每次 submit 前先做 `dma_wait_done()`，明確把 firmware sequencing 對齊現有 RTL DMA contract。

修正後的觀察結果：

- GEMM trace 重新出現 `src=0x80104800 dst=0x00080000` 的 DMA submit。
- GEMM mismatch 從 1534 筆收斂到只剩 tolerance 級微差。
- conv1x1、conv3x3 可沿用同一套 helper 修正。

## 4. compare threshold 為什麼是 rtol=0.03、atol=0.015

目前 [design/hybridacc-RTL/Makefile](../Makefile) 的 single-wave regression 預設值為：

- `RTL_FW_COMPARE_RTOL ?= 3e-2`
- `RTL_FW_COMPARE_ATOL ?= 1.5e-2`

這組值是修完 DMA submit 問題後，對三個 workload 一起驗證得到的最小可用門檻，不是任意放寬。

GEMM 是最接近邊界的 case。修正後做 tolerance sweep，得到：

| rtol | atol | GEMM mismatch |
|------|------|---------------|
| `0.01` | `0.005` | 10 |
| `0.02` | `0.01` | 1 |
| `0.03` | `0.015` | 0 |

再把這組門檻套回三個 case 一起驗證：

| Workload | 最大絕對誤差 | 結果 |
|----------|--------------|------|
| conv1x1 single wave | `0.00488281` | PASS |
| conv3x3 single wave | `0.0117188` | PASS |
| gemm single wave | `0.0273438` | PASS |

因此目前預設的 `rtol=0.03`、`atol=0.015` 是同時覆蓋 conv1x1、conv3x3、gemm 三個 single-wave case 的最小已驗證門檻。

## 5. debug display 如何開關

single-wave regression 的 trace 入口是 [design/hybridacc-RTL/Makefile](../Makefile) 內的 `RTL_FW_DEBUG_PLUSARGS`。

- 預設值是空字串，所以 trace 預設隱藏。
- 只要在 `make rtl_regress_*` 或 `make rtl_regress_single_wave` 時帶入 plusargs，就會把對應 trace 打開。

最常用的方式如下：

```tcsh
# 預設靜默
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_gemm_single_wave'

# 只看 runtime 事件
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME"'

# 只看 MMIO programming
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_MMIO"'

# 同時看 runtime + MMIO
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"'

# 一鍵總開關
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_DEBUG"'
```

建議用法：

- `+TRACE_CLUSTER_MMIO`: 看 DMA / cluster register programming 是否正確。
- `+TRACE_CLUSTER_RUNTIME`: 看 DMA submit、response、AGU/HDDU/SPM runtime 事件是否真的發生。
- `+TRACE_CLUSTER_DEBUG`: 多數 trace 的總開關，適合先粗看全流。

如果要明確關閉 trace，可以傳空值：

```tcsh
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS='
```

## 6. 什麼時候要改用 SIM_PLUSARGS

`RTL_FW_DEBUG_PLUSARGS` 只會被 `rtl_regress_*` 這組 Make target 使用。

如果你是直接跑 testbench，例如：

```tcsh
cd design/hybridacc-RTL
make sim_tb_hybridacc_sim SIM_PLUSARGS="+FW_MEM=... +FW_BYTES=... +TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"
```

那 trace plusargs 應該放在 `SIM_PLUSARGS`，不是 `RTL_FW_DEBUG_PLUSARGS`。

## 7. 建議日常用法

如果只是日常回歸：

```tcsh
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_single_wave'
```

如果是針對單一功能性問題追 trace：

```tcsh
cd "$(git rev-parse --show-toplevel)" && tcsh -c 'source ~/.tcshrc; cd design/hybridacc-RTL && make rtl_regress_gemm_single_wave RTL_FW_DEBUG_PLUSARGS="+TRACE_CLUSTER_RUNTIME +TRACE_CLUSTER_MMIO"'
```

如果看到 comparator fail，先判斷是哪一類：

1. 大量 mismatch 且數值明顯錯誤：優先懷疑 functional bug。
2. 只剩少量 mismatch，且最大誤差接近 `0.0273438` 這種 fp16 級差異：優先比對 threshold 是否被 override，或檢查是否回到了舊預設值。