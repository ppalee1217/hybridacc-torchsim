# HybridAcc Documentation

這裡是 repo 文件入口。README 只給最短總覽；需要照流程操作、查 debug 分流或維護 repo 結構時，從這份 index 進入。

## 1. 文件分區

| 分區 | 用途 | 入口 |
| --- | --- | --- |
| repo 使用手冊 | 環境建置、ESL/RTL 執行、synthesis/signoff、常用命令 | [user-manual/index.md](user-manual/index.md) |
| repo 開發維護手冊 | repo map、artefact/log 對照、debug playbook、cleanup 規則 | [developer-manual/index.md](developer-manual/index.md) |
| Compiler 設計文件 | lowering、codegen、ELF layout、compiler user guide | [../design/hybridacc-cc/doc/00_Overview.md](../design/hybridacc-cc/doc/00_Overview.md) |
| RTL 子系統細節 | RTL simulation、synthesis、PrimeTime、PrimePower、Superlint 深入文件 | [../design/hybridacc-RTL/doc/README.md](../design/hybridacc-RTL/doc/README.md) |
| ESL 子系統細節 | ESL simulator 使用、測試、component 規格與歷史 guide | [../design/hybridacc-ESL/doc/index.md](../design/hybridacc-ESL/doc/index.md) |
| PE-ISA 工具鏈文件 | assembler、objdump、package 與 ISA 規格 | [../design/hybridacc-pe-isa/README.md](../design/hybridacc-pe-isa/README.md) |
| Python / parser 文件 | repo CLI、legacy 驗證框架、trace parser | [user-manual/python-cli-reference.md](user-manual/python-cli-reference.md) |

## 2. 最短路徑

| 你要做的事 | 先看 |
| --- | --- |
| 第一次把 repo 跑起來 | [user-manual/quick-start.md](user-manual/quick-start.md) |
| 檢查環境與工具 | [user-manual/environment-and-toolchains.md](user-manual/environment-and-toolchains.md) |
| 找常用命令 | [user-manual/command-cheatsheet.md](user-manual/command-cheatsheet.md) |
| 跑 ESL / E2E | [user-manual/esl-workflows.md](user-manual/esl-workflows.md) |
| 跑 RTL unit / smoke | [user-manual/rtl-simulation.md](user-manual/rtl-simulation.md) |
| 跑 RTL single-wave regression | [user-manual/rtl-firmware-regression.md](user-manual/rtl-firmware-regression.md) |
| 跑 synthesis / STA / PrimePower | [user-manual/synthesis-and-postsim.md](user-manual/synthesis-and-postsim.md) |
| 跑 Jasper / Superlint | [user-manual/lint-and-formal.md](user-manual/lint-and-formal.md) |
| 看 compiler lowering / codegen 細節 | [../design/hybridacc-cc/doc/00_Overview.md](../design/hybridacc-cc/doc/00_Overview.md) |
| 查 PE template / ISA 工具鏈 | [../design/hybridacc-pe-isa/README.md](../design/hybridacc-pe-isa/README.md) |
| 查 Python CLI / parser 入口 | [user-manual/python-cli-reference.md](user-manual/python-cli-reference.md) |
| 找輸出與 log | [developer-manual/artefact-and-log-map.md](developer-manual/artefact-and-log-map.md) |
| 查 repo 結構 | [developer-manual/repo-map.md](developer-manual/repo-map.md) |
| 做 cleanup / 搬檔案 | [developer-manual/repo-cleanup-guide.md](developer-manual/repo-cleanup-guide.md) |

## 3. Shell 規則

一般 Python、compiler、ESL 與 report parser 使用 repo root 的 `bash`/`uv` 入口。

```bash
cd /home/easonyeh/hybridacc
uv sync
scripts/env_check.sh
```

RTL / EDA flow 請用 `tcsh` 載入站點工具後再執行 Makefile。

```bash
tcsh -ic 'source ~/.tcshrc; cd /home/easonyeh/hybridacc/design/hybridacc-RTL; make <target>'
```

## 4. 文件站與 CI 檢查

文件入口變更後，先在 repo root 跑本地檢查，再視需要開啟靜態文件站預覽。

```bash
cd /home/easonyeh/hybridacc
uv run python scripts/check/validate_markdown_links.py
uv run python scripts/check/sync_docs_site.py
uvx --with mkdocs-material mkdocs build --config-file mkdocs.yml
```

CI workflow 位置在 [../.github/workflows/docs.yml](../.github/workflows/docs.yml)，導航設定在 [../mkdocs.yml](../mkdocs.yml)。

## 5. 長期維護規則

新增 workflow、改命令、搬 script 或改輸出路徑時，請同步更新：

1. 對應的 user manual。
2. [developer-manual/artefact-and-log-map.md](developer-manual/artefact-and-log-map.md) 或 [developer-manual/repo-map.md](developer-manual/repo-map.md)。
3. 子系統細節文件，例如 [../design/hybridacc-RTL/doc/README.md](../design/hybridacc-RTL/doc/README.md)、[../design/hybridacc-ESL/doc/index.md](../design/hybridacc-ESL/doc/index.md)、[../design/hybridacc-pe-isa/README.md](../design/hybridacc-pe-isa/README.md)。
4. [../mkdocs.yml](../mkdocs.yml) 的導航入口，以及 [../.github/workflows/docs.yml](../.github/workflows/docs.yml) 的檢查範圍。

臨時 plan 或一次性分析報告不作為入口文件長期保留；確認吸收到正式文件後應移除。
