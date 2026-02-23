# Format A & B 實作指南 (針對 Linker/ELF 初學者)

這份文件將引導你從零開始理解 Linker 與 ELF，並詳細說明如何在你的 HybridAcc 專案中實作 Format A (自定義格式) 與 Format B (ELF 標準格式)。

## 基礎觀念：什麼是 Linker 與 Executable Format？

在深入程式碼之前，我們先建立一個心智模型。

### 1. 什麼是 Loadable Package (執行檔)？
想像你要去旅行 (執行一個程式)，**Loadable Package** 就是你的 **行李箱**。
- **內容物**：衣服 (程式碼/Instructions)、盥洗用具 (資料/Data/Weights)、行程表 (設定/Config)。
- **格式**：行李箱的隔層設計。
    - **Format A (自定義)**：你自己縫製的袋子，只有你知道哪裡放牙刷，哪裡放內褲。優點是輕便，缺點是別人(標準工具)打不開。
    - **Format B (ELF)**：標準的 Samsonite 行李箱。它有標準的標籤位置、拉鍊設計。海關 (OS/Debugger) 都有萬用鑰匙可以檢查它。

### 2. 什麼是 Linker (連結器)？
**Linker** 就是 **打包行李的人**。
- 如果你有多個小袋子 (多個 `.o` object files，例如 `conv_layer.o`, `relu_layer.o`)。
- Linker 的工作是把它們合併成一個大行李箱 (Executable)。
- **重要任務**：
    - **Relocation (重定位)**：假設 `conv_layer` 原本以為自己住在 0 號房，但在大行李箱裡它被放在 100 號房。Linker 要修改程式碼裡的地址，把 "跳到 0 號房" 改成 "跳到 100 號房"。
    - **Symbol Resolution (符號解析)**：`relu_layer` 想要呼叫 `conv_layer` 的函式。Linker 要告訴 `relu_layer` 那個函式最後被放在哪裡。

---

## 實作一：Format A (HAP - HybridAcc Package)

這是最簡單、最直觀的實作方式。我們定義一個簡單的 Header，後面跟著一堆資料塊 (Blobs)。

### 1. 檔案結構設計 (Spec)
我們使用 C 語言的 `struct` 來定義檔案開頭。

```cpp
// 檔案開頭 (16 bytes)
struct HAP_Header {
    uint32_t magic;        // 0x48415031 ("HAP1" ASCII) -用來確認這是不是我們的檔案
    uint32_t version;      // 版本號，例如 1
    uint32_t num_segments; // 檔案裡有幾個區塊
    uint32_t entry_point;  // 程式開始執行的 PC 位址
};

// 每個區塊的標頭 (16 bytes)
struct HAP_Segment_Header {
    uint32_t type;       // 0=INSTRUCTION, 1=DATA/WEIGHT, 2=CONFIG (MMIO)
    uint32_t addr;       // 這個區塊要被載入到 Simulator 記憶體的哪裡 (SPM Address)
    uint32_t offset;     // 這個區塊的資料在檔案中的什麼位置 (File Offset)
    uint32_t size;       // 這個區塊的大小 (Bytes)
};
```

**檔案佈局範例**：
```text
[ HAP_Header ] (0-15 bytes)
[ Segment 1 Header ] (16-31 bytes)
[ Segment 2 Header ] (32-47 bytes)
...
[ Segment 1 Data Body ] (例如：指令碼)
[ Segment 2 Data Body ] (例如：權重)
```

### 2. Linker / Packer (Python 實作)
不需要寫複雜的 C++ Linker，用 Python 寫一個 "打包工具" 即可。

建立 `scripts/hap_packer.py`：

```python
import struct
import sys

# 定義常數
MAGIC = 0x48415031 # "HAP1" (Little Endian: 0x31 0x50 0x41 0x48 -> '1' 'P' 'A' 'H')
TYPE_INST = 0
TYPE_DATA = 1
TYPE_CONFIG = 2

def create_hap(output_file, entry_point, segments):
    """
    segments: list of dict {'type': int, 'addr': int, 'data': bytes}
    """
    num_segments = len(segments)
    
    # 1. 準備 Header
    # <I 代表 Little Endian Unsigned 32-bit Integer
    header = struct.pack('<IIII', MAGIC, 1, num_segments, entry_point)
    
    # 2. 計算 Offset
    # Header 大小 + 所有 Segment Headers 的大小
    current_offset = 16 + (16 * num_segments)
    
    seg_headers_bytes = bytearray()
    data_blobs_bytes = bytearray()
    
    for seg in segments:
        data = seg['data']
        size = len(data)
        
        # 建立 Segment Header
        # type, addr, offset, size
        s_header = struct.pack('<IIII', 
                             seg['type'], 
                             seg['addr'], 
                             current_offset, 
                             size)
        seg_headers_bytes.extend(s_header)
        data_blobs_bytes.extend(data)
        
        current_offset += size
        
    # 3. 寫入檔案
    with open(output_file, 'wb') as f:
        f.write(header)
        f.write(seg_headers_bytes)
        f.write(data_blobs_bytes)
    print(f"Packed {output_file} with {num_segments} segments.")

# 使用範例
if __name__ == "__main__":
    # 假設這是 compiler 產生的 binary
    fake_inst = b'\x00\x10\x00\x20' * 10  # 40 bytes instructions
    fake_weight = b'\xAA\xBB\xCC\xDD' * 4 # 16 bytes weights
    
    segs = [
        # 指令放 SPM 0x0000
        {'type': TYPE_INST, 'addr': 0x0000, 'data': fake_inst},
        # 權重放 SPM 0x2000
        {'type': TYPE_DATA, 'addr': 0x2000, 'data': fake_weight},
        # HDDU 設定 (MMIO) 放 0x1000
        {'type': TYPE_CONFIG, 'addr': 0x1000, 'data': b'\x01\x00\x00\x00'} 
    ]
    
    create_hap("output.hap", 0x0000, segs)
```

### 3. Simulator Loader (C++ 實作)
在你的 Simulator (`driver` 部分) 加入這段程式碼來讀取 HAP 檔。

```cpp
#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>

struct HAP_Header {
    uint32_t magic;
    uint32_t version;
    uint32_t num_segments;
    uint32_t entry_point;
};

struct HAP_Segment_Header {
    uint32_t type;
    uint32_t addr;
    uint32_t offset;
    uint32_t size;
};

void load_hap(std::string filename, SomeBusController* bus) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Cannot open file!" << std::endl;
        return;
    }

    // 1. 讀取 Global Header
    HAP_Header header;
    file.read((char*)&header, sizeof(header));
    
    if (header.magic != 0x48415031) { // 檢查 Magic Number
        std::cerr << "Invalid HAP file format!" << std::endl;
        return;
    }

    // 2. 讀取所有 Segment Headers
    std::vector<HAP_Segment_Header> seg_headers(header.num_segments);
    file.read((char*)seg_headers.data(), sizeof(HAP_Segment_Header) * header.num_segments);

    // 3. 處理每個 Segment
    for (const auto& seg : seg_headers) {
        // 跳到資料位置
        file.seekg(seg.offset, std::ios::beg);
        
        // 讀取資料
        std::vector<char> buffer(seg.size);
        file.read(buffer.data(), seg.size);

        if (seg.type == 0 || seg.type == 1) { // INST or DATA
            // 透過 Data Port 寫入 SPM
            // bus->burst_write(seg.addr, buffer.data(), seg.size);
            std::cout << "Loading Data/Inst to SPM Addr: 0x" << std::hex << seg.addr << std::endl;
        } else if (seg.type == 2) { // CONFIG
            // 透過 Command Port 寫入暫存器
            // bus->write_register(seg.addr, buffer_value);
            std::cout << "Configuring MMIO Addr: 0x" << std::hex << seg.addr << std::endl;
        }
    }
}
```

---

## 實作二：Format B (Standard ELF)

使用業界標準的 ELF (Executable and Linkable Format)。這需要你對 ELF 結構有基本認識。

### 1. ELF 結構快速入門
ELF 檔就像一個洋蔥，我們只關心其中幾層：
1.  **ELF Header**: 最外層，告訴我們這是 32/64 bit、Big/Little Endian、Entry Point 在哪。
2.  **Program Headers (Phdrs)**: **最重要！** 這是給 Loader (Simulator) 看的。它告訴你："請把檔案裡Offset X 的資料，搬到記憶體 Address Y"。
3.  **Section Headers (Shdrs)**: 這是給 Linker/Debugger 看的。告訴你哪裡是 Code (`.text`)，哪裡是 Symbol Table (`.symtab`)。Simulator `執行` 時其實不需要這個，但 `Debug` 時需要。

### 2. 如何產生 ELF (Linker Script)
你不需要自己寫 Python 腳本來組裝 ELF (太難了)。你應該使用標準的 `ld` (GNU Linker) 配合 **Linker Script (`.ld`)**。

假設你有 PE 的 object file `kernel.o`。

建立 `cluster.ld` (Linker Script):
```ld
/* 定義輸出架構 */
OUTPUT_ARCH(riscv) /* 或你的 PE 架構 */
ENTRY(_start)

/* 定義記憶體區塊 (根據你的 SPM Address Map) */
MEMORY
{
    /* 假設 SPM 從 0x0 開始是指令，0x2000 開始是資料 */
    SPM_INST (rx) : ORIGIN = 0x0000, LENGTH = 4K
    SPM_DATA (rw) : ORIGIN = 0x2000, LENGTH = 4K
    MMIO_CFG (rw) : ORIGIN = 0x10000, LENGTH = 1K
}

SECTIONS
{
    /* 把程式碼 (.text) 放進 SPM_INST */
    .text : {
        *(.text)
        *(.text.*)
    } > SPM_INST

    /* 把資料 (.data, .rodata) 放進 SPM_DATA */
    .data : {
        *(.data)
        *(.rodata)
    } > SPM_DATA

    /* 自定義 Section 用於設定 HDDU */
    .noc_config : {
        KEEP(*(.noc_config))
    } > MMIO_CFG
}
```

**編譯與連結指令**：
```bash
# 1. Compile C/Asm to Object
gcc -c kernel.c -o kernel.o
# 2. Link using script (產生 ELF)
ld -T cluster.ld kernel.o -o output.elf
```

### 3. Simulator Loader (C++ with Structs)
在 C++ 中讀取 ELF 有兩種方法：用 `libelf` (強大但複雜) 或 **直接定義 Structs (簡單直接)**。對於 Simulator，直接定義 Structs 就夠了。

標準的 ELF 定義通常在 `<elf.h>` (/usr/include/elf.h)。如果不想依賴系統 header，可以自己定義簡化版。

```cpp
#include <elf.h> // Linux 標準 header
#include <cstdio>

void load_elf_simple(std::string filename) {
    FILE* f = fopen(filename.c_str(), "rb");
    
    // 1. 讀取 ELF Header
    Elf64_Ehdr ehdr;
    fread(&ehdr, 1, sizeof(ehdr), f);

    // 檢查 Magic (0x7F 'E' 'L' 'F')
    if (ehdr.e_ident[EI_MAG0] != 0x7F || ehdr.e_ident[EI_MAG1] != 'E') {
        printf("Not an ELF file\n"); return;
    }

    // 2. 讀取 Program Headers (Phdrs)
    // 我們只關心 Phdrs，因為它們描述了 "Loadable Segment"
    fseek(f, ehdr.e_phoff, SEEK_SET);
    
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        fread(&phdr, 1, sizeof(phdr), f);

        // 只處理類型為 PT_LOAD (1) 的區段
        if (phdr.p_type == PT_LOAD) {
            printf("Segment %d: Load to Addr 0x%lx, Size 0x%lx\n", 
                   i, phdr.p_paddr, phdr.p_filesz);
            
            if (phdr.p_filesz > 0) {
                // 3. 讀取資料並寫入 Simulator 記憶體
                std::vector<char> buffer(phdr.p_filesz);
                long saved_pos = ftell(f); // 記住當前讀取 ProgramHeader 的位置
                
                fseek(f, phdr.p_offset, SEEK_SET); // 跳到資料處
                fread(buffer.data(), 1, phdr.p_filesz, f);
                
                // bus->burst_write(phdr.p_paddr, buffer.data(), phdr.p_filesz);
                
                fseek(f, saved_pos, SEEK_SET); // 跳回去準備讀下一個 Header
            }
        }
    }
    fclose(f);
}
```

---

## 總結與建議

| 特性 | Format A (HAP - 自定義) | Format B (ELF - 標準) |
| :--- | :--- | :--- |
| **學習曲線** | 低 (只需懂 Struct) | 中 (需懂 Linker Script, Segment vs Section) |
| **實作難度** | 低 (Python Packer + C++ Loader) | 中高 (需正確設定 Linker Script) |
| **工具支援** | 無 (自己寫) | 完美 (objdump, readelf, gdb) |
| **適用場景** | **初期開發、快速驗證、特殊硬體需求** | **成熟階段、需要 Debugger 整合、跑 OS** |

**給你的建議**：
既然你是從 0 開始，**強烈建議先實作 Format A**。
1. 它讓你完全掌握 "Bits on Disk" 到 "Bits in Memory" 的過程，沒有黑盒子。
2. 開發 Python Packer (`hap_packer.py`) 來把你的 PE instructions 和 weights 打包。
3. 等 Simulator 穩定後，若有需要支援 GDB 或標準 Toolchain 整合，再遷移到 Format B。

## 參考資料 (References)

1.  **Linkers and Loaders (Book)** - John R. Levine
    *   這是聖經級的書，雖然有點舊，但觀念完全正確。網路上可以找到免費的草稿版。
2.  **ELF Specification** (System V ABI)
    *   直接看 Spec 可能太硬，建議先看 "ELF101" 類型的文章。
3.  **"Computer Systems: A Programmer's Perspective" (CS:APP)**
    *   第七章 "Linking" 講得非常清楚，適合初學者。
4.  **Linux man pages**
    *   `man elf`: 會有詳細的 C struct 定義。
    *   `man ld`: GNU Linker 的手冊。
