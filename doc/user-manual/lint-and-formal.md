# Lint And Formal

## 1. 適用範圍

這份文件整理 `design/hybridacc-RTL` 目前與 Jasper / Superlint 相關的操作流程。它的重點不是介紹 Jasper 語法，而是說清楚：

1. 哪個腳本才是主入口。
2. `make superlint`、`make superlint_report`、`make superlint_hotspot` 各自做什麼。
3. query / extract / waiver 要怎麼理解與排錯。

## 2. 固定工作模式

1. 建議在 `tcsh` 執行。
2. 工作目錄固定在 `/home/easonyeh/hybridacc/design/hybridacc-RTL`。
3. 真正的 canonical 主流程只有 `script/tcl/superlint/jasper_superlint.tcl`。

## 3. 主要入口命令

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make superlint'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make superlint_report'
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make superlint_hotspot'
```

### 3.1 什麼時候用哪個 target

| target | 用途 | 什麼時候先用 |
| --- | --- | --- |
| `make superlint` | 跑 canonical Jasper 主流程 | 想確認整體 flow 是否可起 |
| `make superlint_report` | 產生報表與摘要 | 想看 warning / error 分布 |
| `make superlint_hotspot` | 熱點分析 | 想縮小特定 tag / 模組問題 |

## 4. 腳本結構

### 4.1 主流程

- `script/tcl/superlint/jasper_superlint.tcl`: 唯一 canonical 主流程
- `script/tcl/superlint/jasper_superlint_waivers.tcl`: 在 main flow 中被 source 的 waiver 定義

### 4.2 helper / query script

- `script/tcl/superlint/jasper_*_query.tcl`
- `script/tcl/superlint/jasper_*_hotspot_query.tcl`
- `script/tcl/superlint/jasper_*_extract_query.tcl`
- `script/tcl/superlint/jasper_*_probe.tcl`

重點是：這些 helper 不是被主流程自動呼叫，而是各自重用主流程後再做自己的查詢。

## 5. 主流程內部概念

主流程大致會做以下事情：

1. 找 project root
2. 收集 RTL 與 include dir
3. 設 run dir
4. `check_superlint -init`
5. analyze / elaborate
6. 設定 clock / reset
7. source waiver
8. extract / prove / report

理解這個順序很重要，因為很多問題其實是在 analyze / elaborate 階段就已經出現，不是 report 才出錯。

## 6. 常用 override 變數

| 變數 | 用途 |
| --- | --- |
| `HACC_JG_PROJECT_ROOT` | 指定 RTL root |
| `HACC_JG_RUN_DIR` | 指定 Jasper run dir |
| `HACC_JG_TOP` | 指定 top module |
| `HACC_JG_SKIP_PROVE` | 跳過 prove，加速 debug |
| `HACC_JG_SKIP_EXTRACT` | 跳過 extract |
| `HACC_JG_SKIP_REPORT` | 跳過 report |
| `HACC_JG_WAIVER_TCL` | 指定自訂 waiver，設 `NONE` 可停用 |
| `HACC_JG_USE_DW_STUBS` | 是否使用 DW stub |
| `HACC_JG_USE_SRAM_STUBS` | 是否使用 SRAM stub |

## 7. 輸出與你應該看什麼

### 7.1 主流程輸出

- Jasper run dir
- extract / prove / report 結果

### 7.2 query / hotspot 輸出

- warning / error tag 分布
- 模組 / instance 熱點
- 局部 extract 結果

### 7.3 排錯時先看什麼

1. 是主流程起不來，還是 query 結果不對。
2. 如果是主流程起不來，先懷疑環境、analyze、elaborate。
3. 如果主流程有起來但內容怪，才回頭看 waiver 或 helper query。

## 8. 常見失敗點

1. 直接改 helper script，卻忘了主流程才是真正入口。
2. 把 waiver / extract / report 混成同一件事。
3. 未載入 DW / SRAM stub，導致大量 helper logic 類警告污染結果。
4. 站點環境與 shell 未就緒，導致 Jasper 無法啟動。

## 9. 成功判準

1. `make superlint` 可完成 canonical 主流程。
2. `make superlint_report`、`make superlint_hotspot` 能輸出可讀查詢結果。
3. 你能分辨「主流程失敗」與「query 結果不理想」是不同問題。

## 10. 相關文件

- [debug-playbook.md](../developer-manual/debug-playbook.md)
- [artefact-and-log-map.md](../developer-manual/artefact-and-log-map.md)
- [../../design/hybridacc-RTL/doc/superlint_guide.md](../../design/hybridacc-RTL/doc/superlint_guide.md)
- [../../design/hybridacc-RTL/doc/README.md](../../design/hybridacc-RTL/doc/README.md)