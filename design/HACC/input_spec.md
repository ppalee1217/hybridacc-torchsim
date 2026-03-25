# HACC CLI Input Spec

本文件定義目前 Python CLI `hacc compile` 接受的輸入 schema。

## 命令格式

```bash
hacc compile <spec.yaml|spec.json> -o <output_prefix>
```

限制如下：

1. 目前一次只接受單一 op spec。
2. 輸入格式可為 YAML 或 JSON。
3. 檔案根節點必須是 mapping。
4. `op_type` 目前只支援 `conv2d` 與 `gemm`。

## 文件慣例

本文件採用以下記號：

| 記號 | 意義 |
|---|---|
| Required | 欄位必填 |
| Optional | 欄位可省略 |
| Default | CLI 缺省時使用的值 |
| Scope | 欄位適用的 op 類型 |

## Top-Level Schema

### 根節點要求

| 項目 | 規則 |
|---|---|
| Document type | YAML mapping 或 JSON object |
| Cardinality | 一次只描述一個 op |
| Dispatch key | `op_type` |

### Top-level 欄位總表

| Field | Type | Required | Default | Scope | Description |
|---|---|---:|---|---|---|
| `name` | string | yes | none | all | op 名稱，用於 compiler 訊息與 debug artifact |
| `op_type` | enum string | yes | none | all | `conv2d` 或 `gemm` |
| `dtype` | enum string | no | `int16` | all | `int8`、`int16`、`fp16`、`fp32` |
| `with_nlu` | bool | no | `false` | all | 是否允許 compiler 產生 NLU phase |

## Op Support Matrix

| `op_type` | Status | Required field set | Optional field set | Example |
|---|---|---|---|---|
| `conv2d` | supported | `n`, `ic`, `ih`, `iw`, `oc`, `kh`, `kw` | `stride_h`, `stride_w`, `pad_h`, `pad_w`, `with_nlu` | `design/HACC/examples/conv2d_basic.yaml` |
| `gemm` | supported | `M`, `N`, `K` | `trans_a`, `trans_b`, `with_nlu` | `design/HACC/examples/gemm_basic.yaml` |

## Common Fields

所有 spec 都支援以下欄位：

| Field | Type | Required | Default | Description |
|---|---|---:|---|---|
| `name` | string | yes | none | op 名稱，用於 compiler 輸出訊息與 debug artifact |
| `op_type` | string | yes | none | `conv2d` 或 `gemm` |
| `dtype` | string | no | `int16` | `int8`、`int16`、`fp16`、`fp32` |
| `with_nlu` | bool | no | `false` | 是否允許產生 NLU phase |

## Conv2D Schema

### Conv2D Field Table

| Field | Type | Required | Default | Description |
|---|---|---:|---|---|
| `n` | int | yes | none | batch size |
| `ic` | int | yes | none | input channels |
| `ih` | int | yes | none | input height |
| `iw` | int | yes | none | input width |
| `oc` | int | yes | none | output channels |
| `kh` | int | yes | none | kernel height |
| `kw` | int | yes | none | kernel width |
| `stride_h` | int | no | `1` | vertical stride |
| `stride_w` | int | no | `1` | horizontal stride |
| `pad_h` | int | no | `0` | vertical padding |
| `pad_w` | int | no | `0` | horizontal padding |
| `with_nlu` | bool | no | `false` | 是否允許 NLU phase |

### Conv2D Semantic Constraints

| Rule | Description |
|---|---|
| Positive dimensions | `n`, `ic`, `ih`, `iw`, `oc`, `kh`, `kw` 必須為正整數 |
| Positive strides | `stride_h`、`stride_w` 必須為正整數 |
| Output validity | 推導後的 `OH`、`OW` 必須大於 0 |

### 輸出 shape

frontend 會根據以下公式推導 output shape：

$$
OH = \left\lfloor \frac{IH + 2 \cdot pad_h - KH}{stride_h} \right\rfloor + 1
$$

$$
OW = \left\lfloor \frac{IW + 2 \cdot pad_w - KW}{stride_w} \right\rfloor + 1
$$

若 `OH <= 0` 或 `OW <= 0`，validation 會失敗。

### Conv2D YAML 範例

```yaml
name: conv2d_basic
op_type: conv2d
dtype: int16
n: 1
ic: 16
ih: 8
iw: 8
oc: 32
kh: 3
kw: 3
stride_h: 1
stride_w: 1
pad_h: 1
pad_w: 1
with_nlu: false
```

對應範例檔：

- `design/HACC/examples/conv2d_basic.yaml`

## GEMM Schema

### GEMM Field Table

| Field | Type | Required | Default | Description |
|---|---|---:|---|---|
| `M` | int | yes | none | output rows |
| `N` | int | yes | none | output cols |
| `K` | int | yes | none | reduction dimension |
| `trans_a` | bool | no | `false` | 是否轉置 A |
| `trans_b` | bool | no | `false` | 是否轉置 B |
| `with_nlu` | bool | no | `false` | 是否允許 NLU phase |

### GEMM Semantic Constraints

| Rule | Description |
|---|---|
| Positive dimensions | `M`、`N`、`K` 必須為正整數 |
| Shape mapping | frontend 固定映射 input=`(1,1,M,K)`、weight=`(1,1,K,N)`、output=`(1,1,M,N)` |

### 形狀映射

目前 frontend 固定將 GEMM 映射為：

1. input = `(1, 1, M, K)`
2. weight = `(1, 1, K, N)`
3. output = `(1, 1, M, N)`

### GEMM YAML 範例

```yaml
name: gemm_basic
op_type: gemm
dtype: int16
M: 64
N: 64
K: 128
trans_a: false
trans_b: false
with_nlu: false
```

對應範例檔：

- `design/HACC/examples/gemm_basic.yaml`

## Extensibility Rules

未來若要擴充多 op schema，建議遵守以下結構：

| Rule | Recommendation |
|---|---|
| Add new op type | 先在 `op_type` dispatch table 新增一列，再新增對應章節 |
| Keep common fields stable | `name`、`op_type`、`dtype`、`with_nlu` 儘量維持 top-level 共通語意 |
| Separate field table from semantic constraints | 欄位表只列 key/type/default；語意限制獨立放在 constraint table |
| Example-first rollout | 每新增一個 op type，都應同時提供 `design/HACC/examples/` 下的最小合法範例 |
| Validation parity | 文件的 constraint table 必須和 CLI / frontend validation 行為一致 |

## 最小工作流程

Conv2D：

```bash
hacc compile design/HACC/examples/conv2d_basic.yaml -o /tmp/conv2d_basic
hacc-elfdump -d /tmp/conv2d_basic.hacc.elf
```

GEMM：

```bash
hacc compile design/HACC/examples/gemm_basic.yaml -o /tmp/gemm_basic
hacc-elfdump -d /tmp/gemm_basic.hacc.elf
```

## 產物

成功編譯後，`-o <output_prefix>` 會輸出：

1. `<output_prefix>.hacc.elf`
2. `<output_prefix>.debug.json`