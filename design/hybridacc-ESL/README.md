# HybridAcc ESL Simulator

Repo-wide 操作入口請先看 [../../doc/index.md](../../doc/index.md) 與 [../../doc/user-manual/esl-workflows.md](../../doc/user-manual/esl-workflows.md)；本文件是 ESL 子系統入口，負責把 simulator、test、component docs、legacy guides 與 profiling notes 串成同一棵文件樹。

## 1. 目前目錄角色

| 路徑 | 用途 | 入口文件 |
| --- | --- | --- |
| `simulator/` | SystemC simulator 原始碼與 build 輸出 | `simulator/CMakeLists.txt` |
| `test/` | 單元測試、cluster / NoC / PE 模擬測試 | [test/README.md](test/README.md) |
| `doc/` | ESL subsystem 文件樹 | [doc/index.md](doc/index.md) |
| `firmware/` | ESL 測試使用的韌體與輔助素材 | 依各 test / workflow 使用 |
| `build/` | 歷史或本地 build 輸出 | 非文件入口 |

`design/hybridacc-ESL/` 目前沒有單一頂層 `CMakeLists.txt`；`simulator/` 與 `test/` 各自維護自己的 CMake 專案。

## 2. 建議入口

### 2.1 Repo-level canonical flow

從 repo root 執行，可避免相對路徑漂移：

```bash
cd "$(git rev-parse --show-toplevel)"
uv sync
scripts/setup.sh fast hybridacc-sim build
```

跑完整 workload 時，優先使用 repo-level CLI 與 fast entry：

```bash
uv run hacc-compile design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml -o output/quickstart-conv3x3 --dump-ir
uv run python -m hybridacc_verify.gen.gen_test_dram --ir output/quickstart-conv3x3/hardware_ir.json --workload design/hybridacc-cc/example/conv3x3/conv2d_3x3_single_wave.yaml --output-dir output/quickstart-conv3x3
scripts/setup.sh fast hybridacc-sim run-task output/quickstart-conv3x3
uv run python -m hybridacc_verify.check.compare_golden output/quickstart-conv3x3
```

### 2.2 Local build entry

若你只想在 subsystem 內手動 build，以下命令都從 repo root 開始：

```bash
cd design/hybridacc-ESL/simulator
mkdir -p build && cd build
cmake ..
cmake --build . -j"$(nproc)"
```

測試專案則在 `test/` 目錄下獨立建置：

```bash
cd design/hybridacc-ESL/test
mkdir -p build && cd build
cmake ..
cmake --build . -j"$(nproc)"
```

若 SystemC 不在預設位置，可用 `-DSYSTEMC_HOME=/path/to/systemc` 覆寫。

## 3. 測試與快取入口

日常 cluster / NoC / PE 測試，優先走 repo root 的 `scripts/setup.sh fast ...`：

```bash
cd "$(git rev-parse --show-toplevel)"
scripts/setup.sh fast cluster-sim build
scripts/setup.sh fast cluster-sim run --advanced -d output/cluster-sim conv_k3c4
```

若要直接使用底層腳本，實際位置是 [../../scripts/fast_entry/cluster_sim.sh](../../scripts/fast_entry/cluster_sim.sh)，不是舊文件曾提到的 `design/hybridacc-ESL/scripts/cluster_sim.sh`。

E2E workload pipeline 仍以 [../../scripts/fast_entry/run_e2e.sh](../../scripts/fast_entry/run_e2e.sh) 與 [../../doc/user-manual/esl-workflows.md](../../doc/user-manual/esl-workflows.md) 為主。

## 4. ESL 文件樹

| 節點 | 內容 |
| --- | --- |
| [doc/index.md](doc/index.md) | ESL docs root，整合 component / guide / report / test |
| [doc/component/README.md](doc/component/README.md) | AGU、SPM、NoC、PE、Core、DMA 等 component 規格 |
| [doc/guide/README.md](doc/guide/README.md) | legacy user / developer guides |
| [doc/report/README.md](doc/report/README.md) | profiling 與 utilization 筆記 |
| [test/README.md](test/README.md) | ESL test framework 與測試習慣 |

## 5. 維護規則

改動以下內容時，請同步更新對應文件：

1. simulator / test build 路徑或 CMake 入口。
2. `scripts/setup.sh fast hybridacc-sim`、`cluster-sim`、`run_e2e.sh` 的用法。
3. `doc/` 下的 component、guide、report 索引。
4. repo-level 使用手冊 [../../doc/user-manual/esl-workflows.md](../../doc/user-manual/esl-workflows.md)。