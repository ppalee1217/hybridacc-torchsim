# GEMM Gate Debug Chain

這份文件整理 `gate_regress_gemm_single_wave CLOCK_PERIOD_NS=2.00` 到目前為止的分析線索鎖鏈，目標是把「症狀」、「排除過的邊界」、「最後釘住 root cause 的證據」放到同一份可回查文件，而不是散落在 session note、臨時 probe 或單次 terminal output。

## 1. 當前結論

目前已經可以把 root cause 精確落在 `p2pe0.exe_a_stage` 的 `S_WAIT_PLO` replay path：

1. `EXE_A_Stage` 在 `S_WAIT_PLO` 時仍允許 `plo_buf_produce_w` 成立。
2. 這條 produce 路徑在 `S_WAIT_PLO` 會選 `vaddu_result_reg`，不是當前的 `vaddu_result_sig`。
3. 只要 `plo_ready` 在 `S_WAIT_PLO` 重新變成高，`plo_buf_do_pop_w` 與 `plo_buf_produce_w` 會在同一拍同時成立。
4. 這一拍會把舊 beat pop 掉，接著立刻把 stale `vaddu_result_reg` 再寫回 `plo_buf_data_next`。
5. 因此重複值不是當拍重新算錯，而是 wait-for-PLO path 把舊結果重新送出。

補做 RTL pre-sim 波形後，結論要再精確一層：RTL 與 gate 都會進到 `p2pe0.exe_a_stage` 的 `S_WAIT_PLO` backpressure path，但 RTL 在 release cycle 做的是正確的 buffer rollover，gate 才把同一條 release path 變成 stale replay。也就是說，shared source path 已經確定；gate-only 的是 release-cycle 語義在 implementation 後失真，不是 RTL 完全沒有踩到這段控制邏輯。

對應 RTL 路徑在：

- `plo_buf_produce_w` / `plo_buf_vpsum_data_w`：[design/hybridacc-RTL/src/PE/EXE_A_Stage.sv](../../design/hybridacc-RTL/src/PE/EXE_A_Stage.sv#L259-L285)
- `S_EXEC_PLI_VADDU` / `S_WAIT_PLO` next-state：[design/hybridacc-RTL/src/PE/EXE_A_Stage.sv](../../design/hybridacc-RTL/src/PE/EXE_A_Stage.sv#L469-L529)
- `plo_ready` 來源不是 HDDU 最終消化，而是 router FIFO 是否已滿：[design/hybridacc-RTL/src/PE/PErouter.sv](../../design/hybridacc-RTL/src/PE/PErouter.sv#L156-L209)

## 2. 為什麼目前看起來是 Gate-Only 失敗

補做 RTL pre-sim 波形後，這一節需要修正成更精確的說法：gate-only 的不是「RTL 完全沒有 backpressure」，而是「同一條 release boundary 在 RTL 仍保持正確 rollover，在 gate 才失真成 stale replay」。

1. `ProcessElement` 把 EXE_A 的 `plo_ready` 接到 router 的 `pe_plo_ready`。[design/hybridacc-RTL/src/PE/ProcessElement.sv](../../design/hybridacc-RTL/src/PE/ProcessElement.sv#L91-L133)
2. `PErouter` 直接定義 `pe_plo_ready = !plo_full`，所以 EXE_A 看到的是 router 內部 PLO FIFO 是否有空位，而不是最終 HDDU 是否已經真正寫完。[design/hybridacc-RTL/src/PE/PErouter.sv](../../design/hybridacc-RTL/src/PE/PErouter.sv#L156-L209)
3. 重新執行 `make rtl_regress_gemm_single_wave WAVE_DUMP=1 WAVE_DEPTH=9` 後，RTL compare 仍為 PASS，但 focused RTL `p2pe0` 波形清楚顯示 `state_reg` 真的有進 `S_WAIT_PLO`，而且 `router.plo_full` 與 `stall_port_plo` 也都會拉高。
4. RTL 的差別在 release cycle 語義：對 18 個 collapsed `S_WAIT_PLO` handshake，全部都觀察到 `plo_buf_do_pop_w && plo_buf_produce_w` 同時成立、`plo_data == plo_buf_data_reg == previous vpsum`，且 `plo_buf_data_next == plo_buf_vpsum_data_w`。
5. 這表示 RTL 在 `S_WAIT_PLO` 做的是「吐出舊 buffer，並把下一筆 pending vpsum 補回 buffer」的正確 rollover；gate 失敗則是把這條同拍 release path 實作成 stale `vaddu_result_reg` replay。

換句話說，gate-only 的不是「重複值從哪裡生成」，也不是「RTL 完全沒有踩到 backpressure」，而是「同一條 `S_WAIT_PLO` release 邊界在 gate 實作後不再保持 RTL 的 rollover 語義」。

## 3. 分析線索鎖鏈

### 3.1 起點：compare symptom

最早的固定症狀是：每 48 個 fp16 output 裡，只有前 12 個和 RTL/golden 一致，後面 36 個會重複前面片段。

### 3.2 先排除 output 端假象

先前 probe 階段已經把幾個最常見的假象排除掉：

1. `TB_OUTPUT_WRITE` 地址遞增正常，所以不是 output address decode 問題。
2. `TRACE_DRAM_READS` 顯示 gate 與 RTL 的 external DRAM read response 一致，所以不是 TB 外部 DRAM mirror / readback path。
3. `TRACE_DMA_CLUSTER_READS` 顯示 cluster-read request address 仍正確遞增，但 response data 已經重複，所以 corruption 早於 external writeback。

### 3.3 再往 cluster / SRAM 內縮

接著 probe 將問題繼續往上游推：

1. `TRACE_CLUSTER_FABRIC` 顯示重複值到達 `CoreController.m_cl_data_r_data_i` 前就已存在，因此不是 `ClusterDataFabric` 新引入。
2. `TRACE_GATE_SPM_READS` 顯示 bank 9 DMA read issue row 遞增正常，因此不是 SPM read address path。
3. `TRACE_GATE_SPM_BANK9_WRITES` 顯示 bank 9 row 0..15 的 write data 已經是重複 pattern，因此不是 SRAM macro readback；問題在 upstream producer/write-data path。

### 3.4 從 probe 切到 wave-based

為了避免一直改 probe / recompiling，之後改用現有 FSDB/VCD：

1. 保留現成 `tb_hybridacc_sim.fsdb` 與 focused `tb_hybridacc_sim.cluster_return.vcd`。
2. 建立 `design/hybridacc-RTL/script/python/utils/analyze_gate_gemm_wave.py` 做離線分析。
3. 修掉 analyzer 的 bit ordering、valid window、vector parsing 問題。
4. 修正焦點 PE：從一開始誤對到的 `p0pe15` 改成真正來源 `p2pe0`。

### 3.5 p2pe0 focused wave 證據

wave-based 分析把邊界再往前推了一層：

1. `ln_pli` / `pli` 沒有出現跟 `plo` 一樣的短週期重複。
2. `plo` 出現短週期重複，所以問題已經進入 PE 內部的 compute/output buffer 區。
3. raw `plo` 在同一個 beat 內有短暫 glitch，因此 analyzer 改成 burst-collapsing，只保留每個短 burst 的最後穩定樣本。

更新後的 analyzer 在現成 VCD 上得到：

1. settled `p2pe0` PLO sample 為 `24/47`，重複週期是 `3`。
2. 這 24 筆裡，`6` 筆在 `S_EXEC_PLI_VADDU`，`18` 筆在 `S_WAIT_PLO`。
3. `18/18` 個 `S_WAIT_PLO` sample 都滿足 `plo == vaddu_result_reg` 且 `plo != vaddu_result_sig`。

這是把 root cause 從「PLO 重複」推進成「wait-for-PLO path 重放 stale result」的關鍵一步。

### 3.6 補做 RTL pre-sim wave 對照

為了回答「RTL pre-sim 會不會也有同樣問題」，之後補做了一次帶波形的 RTL run：

1. 重新執行 `make rtl_regress_gemm_single_wave WAVE_DUMP=1 WAVE_DEPTH=9`。
2. Compare 結果仍為 PASS，`1536` 筆 fp16 在 `rtol=0.03 / atol=0.015` 下 `不吻合數: 0`。
3. RTL FSDB 的 scope 命名和 gate 不同：gate analyzer 先前用的是 flatten 後的 `gen_clusters_0__cluster`，RTL 這次需要改從原生 generate path `/tb_hybridacc_sim/dut/gen_clusters[0]/cluster/noc/gen_ports[2]/gen_pe[0]/pe` 抽 focused VCD。
4. 這份 focused RTL `p2pe0` 波形顯示 shared control path 真的被踩到：sampled `state_reg` 會進 `S_WAIT_PLO`，`router.plo_full` 會拉高，`stall_port_plo` 也會拉高。
5. 但 RTL 的關鍵反證是：對 18 個 collapsed `S_WAIT_PLO` handshake，全部都滿足 `plo_buf_do_pop_w && plo_buf_produce_w`、`plo_data == plo_buf_data_reg == previous vpsum`，且 `plo_buf_data_next == plo_buf_vpsum_data_w`。

這條 RTL 波形證據代表：RTL 並不是沒碰到 wait-for-PLO，而是在同一條控制路徑上維持正確 rollover；因此 gate-only failure 的真正差異，不是「有沒有 backpressure」，而是「release cycle 實際上有沒有把 next pending result 正確補回 buffer」。

## 4. 下游對齊證據

Analyzer 現在還會把 settled PLO 依序對到下游 HDDU / bank9：

1. `24/24` 個 settled `p2pe0` PLO sample 都能按順序對到 downstream HDDU lane0 與 bank9 row。
2. 這代表 downstream transport 沒有把值再改壞一次；它只是忠實搬運 `p2pe0` 已經重放出來的舊 payload。

這條對齊輸出由：

- settled PLO burst collapse：[design/hybridacc-RTL/script/python/utils/analyze_gate_gemm_wave.py](../../design/hybridacc-RTL/script/python/utils/analyze_gate_gemm_wave.py#L831-L842)
- downstream alignment：[design/hybridacc-RTL/script/python/utils/analyze_gate_gemm_wave.py](../../design/hybridacc-RTL/script/python/utils/analyze_gate_gemm_wave.py#L845-L883)
- summary / print section：[design/hybridacc-RTL/script/python/utils/analyze_gate_gemm_wave.py](../../design/hybridacc-RTL/script/python/utils/analyze_gate_gemm_wave.py#L894-L1084)

## 5. 真正的 RTL 根源

真正導致 stale replay 的根源，是 `EXE_A_Stage` 這個組合：

1. `plo_buf_produce_w = (state_reg == S_EXEC_PLI_VADDU || state_reg == S_WAIT_PLO) && plo_buf_can_push_w`
2. `plo_buf_vpsum_data_w = (state_reg == S_WAIT_PLO) ? v_fp16_to_u64(vaddu_result_reg) : v_fp16_to_u64(vaddu_result_sig)`
3. `plo_buf_do_pop_w = plo_buf_valid_reg && plo_ready`
4. 當 `plo_buf_do_pop_w && plo_buf_produce_w` 同時成立時，`plo_buf_valid_next` 維持 1，`plo_buf_data_next` 直接重載 `plo_buf_vpsum_data_w`

這代表：

1. 只要 state 還停在 `S_WAIT_PLO`，release 當拍就會同時發生 pop 與 refill，而 refill source 綁在 `vaddu_result_reg`。
2. RTL pre-sim 目前觀察到的是正確 rollover；gate 則是在同一條 release 邊界上把 refill 變成 stale beat replay，然後被 router 收走。

就 debug 角度來說，這已經把 gate failure 釘在可執行層級的 source boundary，而不是模糊的「可能是 EXE_A 附近」。

從 RTL-vs-gate 對照來看，這也說明 fix 不應只停留在「把 mux 從 reg 換成 sig」這種單點 patch。真正敏感的是 `S_WAIT_PLO` release 當拍把 pop / produce / state advance 疊在同一個 combinational boundary 上，讓 intended rollover 語義對 implementation 太脆弱。

## 6. Rewritten Fix Plan

這次重新掃視 RTL 後，修正方向要比前一版更保守，也更貼近目前 codebase 的既有寫法。新的結論是：這個 bug 仍然應該只在 `EXE_A_Stage` 內修，不需要先動 `PErouter`、`ProcessElement` 或 router FIFO。

重新掃視後新增的設計約束如下：

1. `ProcessElement` 只是把 `plo_ready` 直接從 router 帶進 EXE_A，沒有在 PE top 再疊第二層控制；問題控制點仍在 `EXE_A_Stage`。[design/hybridacc-RTL/src/PE/ProcessElement.sv](../../design/hybridacc-RTL/src/PE/ProcessElement.sv#L56-L133)
2. `PErouter` 的 `pe_plo_ready = !plo_full` 只是容量回授，router 端沒有 replay source，也沒有第二份 shadow buffer。[design/hybridacc-RTL/src/PE/PErouter.sv](../../design/hybridacc-RTL/src/PE/PErouter.sv#L156-L220)
3. 共用 `FIFO` 本身允許 pop/push 同拍，表示 router FIFO 不需要改；真正敏感的是 EXE_A 自己在 `S_WAIT_PLO` 這拍同時做 pop、produce、state advance 與 upstream accept。[design/hybridacc-RTL/src/FIFO.sv](../../design/hybridacc-RTL/src/FIFO.sv#L41-L84)
4. `EXE_A_Stage` 目前 3-bit enum 已經用滿 8 個 state，而且 `ready_out_w`、`pli_ready`、`plo_buf_produce_w`、main FSM 都直接以 `state_reg` 為判斷；擴 state 雖然做得到，但修改面會明顯放大。[design/hybridacc-RTL/src/PE/EXE_A_Stage.sv](../../design/hybridacc-RTL/src/PE/EXE_A_Stage.sv#L40-L180)

基於這些約束，新的首選修法不是「先加一個 refill state」，而是「保留現有 8-state FSM，改成 explicit pending-valid control，把 `S_WAIT_PLO` 內的 drain 與 refill 拆成兩個 cycle，但不新增 state」。

### 6.1 修正目標

新的實作目標要明確縮成三件事：

1. `S_WAIT_PLO` release cycle 不可再出現 `plo_buf_do_pop_w && plo_buf_produce_w` 同拍成立。
2. `S_WAIT_PLO` 期間不可再同拍接受新的 decode / PLI input。
3. pending VPSUM 必須有顯式 valid 語義，不能再只靠 `state_reg == S_WAIT_PLO` 隱含表示。

### 6.2 推薦 RTL 改法

建議把修正限制在 `EXE_A_Stage`，並採以下步驟：

1. 新增 `plo_pending_valid_reg` / `plo_pending_valid_next`，資料本體先沿用現有 `vaddu_result_reg`，不要再多引一份 payload register。這樣只新增一個 control bit，而不是新增一整組 data path。
2. 在 `S_EXEC_PLI_VADDU` 中，如果 `plo_buf_valid_reg && !plo_ready`，除了保留 `vaddu_result_next = vaddu_result_sig` 之外，還要把 `plo_pending_valid_next` 置 1，表示「目前有一筆尚未 refill 到 PLO buffer 的 pending VPSUM」。
3. 把目前 generic 的 `plo_buf_produce_w = (state_reg == S_EXEC_PLI_VADDU || state_reg == S_WAIT_PLO) && plo_buf_can_push_w` 改成兩條互斥來源：
	- execute produce：只在 `state_reg == S_EXEC_PLI_VADDU` 且 `plo_buf_can_push_w` 時成立。
	- pending refill produce：只在 `state_reg == S_WAIT_PLO && !plo_buf_valid_reg && plo_pending_valid_reg` 時成立。
4. `plo_buf_data_next` 的資料來源也要拆開：正常 execute path 才能取 `v_fp16_to_u64(vaddu_result_sig)`；wait/refill path 只能取 `v_fp16_to_u64(vaddu_result_reg)`。重點不是資料來自 reg 還是 sig，而是 produce cause 必須是顯式且互斥的。
5. `S_WAIT_PLO` 內部改成兩段式但不新增 state：
	- `plo_buf_valid_reg && !plo_ready`：維持 wait，什麼都不做。
	- `plo_buf_valid_reg && plo_ready`：只做 drain，保留 `state_next = S_WAIT_PLO`，這一拍不 refill。
	- `!plo_buf_valid_reg && plo_pending_valid_reg`：做 refill，把 pending result 補回 `plo_buf_data_next`，清掉 `plo_pending_valid_next`，然後離開 `S_WAIT_PLO`。
6. `ready_out_w` 與 `pli_ready` 在 `S_WAIT_PLO` 應改成保守策略：整個 wait 期間都不接受新 input。也就是說，release/drain/refill 這兩拍都不要再讓 upstream decode 或 PLI 跟著前推。這會多一個 backpressure bubble，但能把修正範圍限制在 EXE_A 內，避免再把 bug 修成另一個握手競爭。

這個方案的核心是：保留現有 state encoding，不碰 router，不碰 FIFO，只在 EXE_A 把「pending result 存在」和「buffer 目前是否可送」拆成兩個獨立控制條件。這樣 pop 與 refill 不再同拍，也不再需要 `state_reg` 同時扮演 pending-valid 與 control-phase 兩種角色。

### 6.3 為什麼這版比前一版更適合先落地

和先前偏向新增 state 的 proposal 相比，這版更適合目前已回退的 RTL 基線，理由是：

1. 不需要擴 `exe_a_state_t` 位寬，也不需要重寫所有依賴 `state_reg` 的 case 分支。
2. 不需要改 `PErouter` / `ProcessElement` / `FIFO` 介面，合成影響面最小。
3. 可以直接沿用 repo 內既有的 pending-valid coding style，而不是引入一套新的控制抽象。[design/hybridacc-RTL/src/Core/CmdFabric.sv](../../design/hybridacc-RTL/src/Core/CmdFabric.sv#L193-L223) [design/hybridacc-RTL/src/Core/CmdFabric.sv](../../design/hybridacc-RTL/src/Core/CmdFabric.sv#L405-L433)
4. 即使之後仍想追求 zero-bubble wait release，也可以在 bug 關閉後再做第二階段優化；第一版修正先以移除 stale replay 為主，不追求完全維持原先 backpressure throughput。

## 7. 實作與驗證順序

新的修正計劃建議按下面順序執行，而不是直接重跑完整 gate flow：

1. 只修改 `EXE_A_Stage`，先把 `plo_pending_valid_reg`、新的 produce cause、以及 `S_WAIT_PLO` 的 drain/refill 分拍控制補齊。
2. 在 module 內先加最小量的 debug/assertion 檢查，至少覆蓋這三個 invariant：
	- `S_WAIT_PLO` 不可再同拍 `plo_buf_do_pop_w && plo_buf_produce_w`
	- `plo_pending_valid_reg` 為 1 時，不可在同拍接受新的 `valid_in && ready_out_w`
	- refill cycle 的 `plo_buf_data_next` 必須來自 pending source，而不是 generic execute mux
3. 先跑最窄的 RTL compile / lint / elaboration，確認這次修法不會再引入合成前結構錯誤。
4. 再跑 focused RTL wave，確認 `p2pe0` 的 `S_WAIT_PLO` 會變成「drain 一拍、refill 一拍」，且 analyzer 不再看到 wait-release stale replay。
5. RTL focused check 通過後，才重跑 gate resynthesis 與 `gate_regress_gemm_single_wave CLOCK_PERIOD_NS=2.00`，確認 12-good / 36-repeat pattern 消失。

如果這版最小修正仍然在 gate 端失敗，下一步才值得考慮升級到「新增 state 或完整 decoupled output queue」；在那之前，不建議先把修改面擴回 router 或下游 transport。