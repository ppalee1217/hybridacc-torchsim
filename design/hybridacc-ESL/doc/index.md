# HybridAcc ESL Documentation

文件樹： [../../../README.md](../../../README.md) -> [../../../doc/index.md](../../../doc/index.md) -> [../README.md](../README.md) -> 本頁。

這裡是 ESL 子系統的文件總索引。repo-level 執行流程請先看 [../../../doc/user-manual/esl-workflows.md](../../../doc/user-manual/esl-workflows.md)；本目錄保留 component 規格、legacy guide、profiling note 與測試細節。

## 1. Section Index

| 區塊 | 用途 | 入口 |
| --- | --- | --- |
| component | 硬體 / 模組規格、coding convention、cluster generation note | [component/README.md](component/README.md) |
| guide | 歷史 user / developer guide，保留背景與細節 | [guide/README.md](guide/README.md) |
| report | profiling / utilization 報告筆記 | [report/README.md](report/README.md) |
| test | 測試框架與案例撰寫規則 | [../test/README.md](../test/README.md) |

## 2. 建議閱讀路徑

1. 先看 [../../../doc/user-manual/esl-workflows.md](../../../doc/user-manual/esl-workflows.md) 取得 repo-level workflow。
2. 需要 subsystem 入口時，看 [../README.md](../README.md)。
3. 要查模組行為時，進 [component/README.md](component/README.md)。
4. 要追歷史背景或較完整流程時，進 [guide/README.md](guide/README.md)。
5. 要查 profiling note 或 utilization 整理時，進 [report/README.md](report/README.md)。

## 3. 相關入口

- compiler/workload docs: [../../hybridacc-cc/doc/00_Overview.md](../../hybridacc-cc/doc/00_Overview.md)
- RTL docs: [../../hybridacc-RTL/doc/README.md](../../hybridacc-RTL/doc/README.md)
- PE-ISA docs: [../../hybridacc-pe-isa/README.md](../../hybridacc-pe-isa/README.md)