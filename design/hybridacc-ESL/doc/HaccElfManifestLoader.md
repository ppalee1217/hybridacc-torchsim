# HACC ELF Manifest 與 Loader CSR 規格書

本文件定義 HACC Core Controller 使用的 manifest 格式、section type、loader 流程與 loader CSR 行為。目標是讓 host driver、boot firmware、RTL 與驗證平台採用一致協定。

---

## 1. 基本原則

1. ELF 由 host 解析，不由 controller 解析。
2. Controller 只接收 manifest entries 與 section payload 的 DRAM base。
3. 每個 manifest entry 對應一段已知 section。

---

## 2. Section Type 定義

| Section Type | 值 | 說明 |
|---|---:|---|
| `HACC_SEC_META` | 0x00 | meta / version / caps |
| `HACC_SEC_CORE` | 0x01 | native core stub |
| `HACC_SEC_JOB` | 0x02 | job descriptor table |
| `HACC_SEC_BLOCK` | 0x03 | block descriptor table |
| `HACC_SEC_PROFILE` | 0x04 | profile table |
| `HACC_SEC_DMA` | 0x05 | DMA rule table |
| `HACC_SEC_AGU` | 0x06 | AGU rule table |
| `HACC_SEC_PE` | 0x07 | PE program blob |
| `HACC_SEC_SCAN` | 0x08 | scan chain blob |
| `HACC_SEC_PATCH` | 0x09 | patch table |
| `HACC_SEC_DEBUG` | 0x0A | debug section |

### 2.1 Section Type 對應實際硬體路徑

下表定義每種 section 在目前 ComputeCluster/Controller 架構下，最終應走哪條硬體路徑。這張表是給 reviewer 與 RTL/driver 工程師快速對齊用的硬規則。

| Section Type | 主要目的地 | Controller 內部處理 | 最終硬體路徑 |
|---|---|---|---|
| `HACC_SEC_META` | local metadata | host/loader 驗證後保存在 local CSR 或 local memory | 不下發到 cluster |
| `HACC_SEC_CORE` | core instruction SRAM | section loader 寫入 I-SRAM | local I-SRAM |
| `HACC_SEC_JOB` | data SRAM | section loader 直接寫入 descriptor/payload ABI region | local data SRAM |
| `HACC_SEC_BLOCK` | data SRAM | section loader 直接寫入 descriptor/payload ABI region | local data SRAM |
| `HACC_SEC_PROFILE` | data SRAM | section loader 直接寫入 descriptor/payload ABI region，runtime 再由 cmd engine 經 HDDU MMIO 套用 | local data SRAM -> cluster AHB `0x1000~0x1FFF` |
| `HACC_SEC_DMA` | data SRAM | section loader 直接寫入 descriptor/payload ABI region | local data SRAM |
| `HACC_SEC_AGU` | data SRAM | section loader 直接寫入 descriptor/payload ABI region，runtime 再由 cmd engine 經 HDDU MMIO 套用 | local data SRAM -> cluster AHB `0x1000~0x1FFF` |
| `HACC_SEC_PE` | cluster PE program path | loader 將 payload 轉成 `CMD_LOAD_PROGRAM` 命令流 | cluster AHB `0x2000` NoC command sideband |
| `HACC_SEC_SCAN` | cluster scan-chain path | loader 將 payload 轉成 `CMD_NOC_SCAN_CHAIN` 命令流 | cluster AHB `0x2000` NoC command sideband |
| `HACC_SEC_PATCH` | data SRAM | section loader 直接寫入 descriptor/payload ABI region | local data SRAM |
| `HACC_SEC_DEBUG` | data SRAM 或 DRAM | debug-only；若放 local 則進 event/debug ABI region | local data SRAM 或外部 DRAM |

補充：runtime tensor payload，例如 activation、weight、partial_sum、output，不屬於上述 ELF control sections。這些資料的搬運路徑是：controller DMA engine -> cluster AXI4-Lite data slave -> SPM linear window。

---

## 3. Manifest Entry 格式

### 3.1 64-bit 邏輯格式

```cpp
struct HaccManifestEntry {
  uint8_t  section_type;
  uint8_t  dst_kind;
  uint16_t flags;
  uint32_t byte_size;
  uint64_t dram_base;
  uint32_t dst_base;
  uint32_t cluster_mask;
  uint32_t checksum;
};
```

### 3.2 `dst_kind` 定義

| `dst_kind` | 值 | 說明 |
|---|---:|---|
| `DST_ISRAM` | 0x0 | 寫入 core instruction SRAM |
| `DST_DATA_SRAM` | 0x1 | 寫入 unified local data SRAM；細分位置由 section type 與 ABI 決定 |
| `DST_CLUSTER_DATA` | 0x2 | 寫入 cluster AXI4-Lite data window，目前實際對應 SPM 線性視窗 |
| `DST_RSVD` | 0x3 | 保留，不作為現版正式 destination |

`DST_DATA_SRAM` 的正式語意：

1. manifest 只選擇實體目的地為 `cc_data_sram`。
2. 實際落在哪個 ABI region，不由 `dst_kind` 再拆新枚舉，而是由 section type 決定。
3. `HACC_SEC_JOB/HACC_SEC_BLOCK/HACC_SEC_PROFILE/HACC_SEC_DMA/HACC_SEC_AGU/HACC_SEC_PE/HACC_SEC_SCAN/HACC_SEC_PATCH` 預設落在 descriptor/payload ABI region。
4. `HACC_SEC_DEBUG` 若選擇 local load，預設落在 event/debug ABI region。

注意：依 [ComputeCluster.md](ComputeCluster.md) 目前實作，`DST_CLUSTER_DATA` 不應被解讀成通用的 cluster payload 視窗。對現版本 cluster 而言，它主要對應 SPM DMA 線性視窗，而不是 PE program window 或 profile window。

因此：

1. `.hacc.pe` 不建議以 `DST_CLUSTER_DATA` 直接寫入所謂 `PE_IMEM_WINDOW`，因為目前 cluster 並未正式暴露這個 AXI 視窗。
2. PE program 與 scan-chain 載入，應透過 cluster AHB `0x2000` 的 NoC command sideband 路徑完成。
3. profile / AGU 類設定，應透過 cluster AHB `0x1000 ~ 0x1FFF` 的 HDDU MMIO passthrough 路徑完成。

### 3.3 `flags` 定義

| Bit | 名稱 | 說明 |
|---:|---|---|
| 0 | `VERIFY_CHECKSUM` | 啟用 checksum 驗證 |
| 1 | `BROADCAST_CLUSTER_MASK` | 目標為多 cluster fan-out |
| 2 | `MANDATORY_SECTION` | 缺失時不可啟動 job |
| 3 | `WRITE_ONLY_ONCE` | active image 不可覆寫 |

---

## 4. Host 寫入流程

### 4.1 單筆 manifest 寫入順序

1. 寫 `MANIFEST_W0`
2. 寫 `MANIFEST_W1`
3. 寫 `MANIFEST_W2`
4. 寫 `MANIFEST_W3`
5. 寫 `MANIFEST_PUSH=1`

### 4.2 Word Packing 建議

| Register | Bits | 內容 |
|---|---|---|
| `MANIFEST_W0` | `[7:0]` | section_type |
| `MANIFEST_W0` | `[15:8]` | dst_kind |
| `MANIFEST_W0` | `[31:16]` | flags |
| `MANIFEST_W1` | `[31:0]` | byte_size |
| `MANIFEST_W2` | `[31:0]` | dram_base[31:0] |
| `MANIFEST_W3` | `[31:0]` | dst_base |

若需要 64-bit DRAM base 與 checksum，建議使用擴充版 `MANIFEST_W4/W5/W6`，或由平台將 DRAM image 限定在 32-bit addressable 範圍。MVP 建議先使用 32-bit DRAM base 範圍，以降低 host/RTL 介面複雜度。

---

## 5. Mandatory Section Policy

若要允許 run，至少必須看到以下 sections：

1. `HACC_SEC_CORE`
2. `HACC_SEC_JOB`
3. `HACC_SEC_BLOCK`
4. `HACC_SEC_DMA`
5. `HACC_SEC_AGU`
6. `HACC_SEC_PATCH`

若 design 需要 cluster 載入 PE code，則 `HACC_SEC_PE` 也是 mandatory。

但對目前 ComputeCluster 版本，`HACC_SEC_PE` 的載入路徑應明確規定為：

1. Host 仍可用 manifest 宣告 `.hacc.pe` section 的 DRAM base 與大小。
2. Controller 的 section loader 讀出 `.hacc.pe` payload 後，不是直接寫 cluster AXI data window。
3. Controller 必須將 payload 轉成一連串 AHB `NOC_CMD_DATA @ 0x2000` 的 `CMD_LOAD_PROGRAM` 命令，送入 NoC command sideband。

同理，`.hacc.scan` 也應轉成 AHB `CMD_NOC_SCAN_CHAIN` 命令，而不是直接寫某個獨立 AXI scan window。

---

## 6. Loader CSR 規格摘要

完整 CSR 位址請以 [Core.md](Core.md) 的第 17 章為準。這裡列出 loader bring-up 最常用項目。

| Register | 作用 |
|---|---|
| `LD_CTRL` | 啟動或清空 loader |
| `LD_STATUS` | 觀察 loader idle/busy/done/error |
| `LD_EXPECT_SECTIONS` | 宣告 mandatory sections |
| `LD_LOADED_SECTIONS` | 檢查實際已載 sections |
| `LD_ERR_CODE` | 錯誤碼 |
| `LD_ERR_AUX` | 錯誤補充資訊 |
| `LD_MANIFEST_LEVEL` | FIFO level |
| `LD_DOORBELL` | 觸發 loader FSM |

---

## 7. Loader 錯誤碼

| Code | 名稱 | 說明 |
|---:|---|---|
| 0x01 | `LD_ERR_SECTION_TYPE` | 不支援的 section type |
| 0x02 | `LD_ERR_DST_KIND` | 不支援的目的地 |
| 0x03 | `LD_ERR_RANGE` | 寫入位址越界 |
| 0x04 | `LD_ERR_CHECKSUM` | checksum 錯誤 |
| 0x05 | `LD_ERR_FIFO_PROTO` | manifest push 協定錯誤 |
| 0x06 | `LD_ERR_MEM_AXI` | DRAM AXI 讀取錯誤 |
| 0x07 | `LD_ERR_CLUSTER_AXI` | cluster data write 錯誤 |

---

## 8. Host Driver 最小流程

```text
1. read CC_ID / CC_VERSION / CC_CAP0
2. write LD_EXPECT_SECTIONS
3. for each ELF section:
     pack manifest words
     write MANIFEST_W0..W3
     write MANIFEST_PUSH
4. write LD_DOORBELL
5. poll LD_STATUS.done or wait controller_irq
6. verify LD_LOADED_SECTIONS covers mandatory bitmap
7. program JOB_BASE / BLOCK_BEGIN / BLOCK_COUNT
8. write RUN_CTRL.run
```

補充說明：

1. 對 `.hacc.job/.hacc.block/.hacc.dma/.hacc.agu/.hacc.patch`，loader 的主要目的地是 `cc_data_sram` 的 descriptor/payload ABI region。
2. 對 `.hacc.pe/.hacc.scan`，loader 的最終 cluster-facing 輸出應是 AHB NoC command stream，而不是 AXI4-Lite data stream。
3. 對 activation / weight / partial_sum / output 這類 runtime tensor 資料，才使用 cluster AXI4-Lite data path 寫入 SPM。
