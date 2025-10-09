// filepath: src/DataLoader.cpp
// ============================================================================
//  File        : DataLoader.cpp
//  Module      : PE::DataLoader
//  Description : 管理資料記憶體/外部匯流排到 PE 內部暫存 (Reg / VALU) 的載入排程。
//
//  Responsibilities (預期):
//    - 解析 Controller 發佈的 Load 指令或微操作
//    - 產生對 DMA / Scratchpad / NoC 的讀取請求
//    - 資料到齊 (alignment) 與 擷取 (gather) / 散佈 (scatter) 支援 (未來)
//
//  Future Extensions:
//    - Prefetch / Streaming 模式
//    - 多通道 (multi-bank) 衝突偵測與重新排序
//    - Bandwidth / Latency 統計
//
//  Change Log:
//    - 2025-10-05  Initial skeleton.
// ============================================================================
#include "hybridacc/pe/DataLoader.hpp"
#include <cassert>

namespace hybridacc { namespace pe {

// ---- Helpers ----
uint8_t DataLoader::mode_to_mask(uint8_t mode) const {
    switch(mode & 0x7) {
        case 0: return 0x01; // LB 1B
        case 1: return 0x03; // LH 2B
        case 2: return 0x0F; // LW 4B
        case 3: return 0xFF; // LD 8B
        case 4: return 0x01; // LBB broadcast byte
        case 5: return 0x03; // LHB broadcast half
        case 6: return 0x0F; // LWB broadcast word
        default: return 0xFF;
    }
}

sc_uint<64> DataLoader::expand_broadcast(uint8_t mode, sc_uint<64> raw) const {
    if(!is_broadcast_mode(mode)) return raw;
    unsigned eb = element_bytes(mode);
    sc_uint<64> out = 0;
    if(eb==1){
        uint8_t b = raw & 0xFF; for(int i=0;i<8;i++) out |= (sc_uint<64>)b << (8*i);
    } else if(eb==2){
        uint16_t hw = raw & 0xFFFF; for(int i=0;i<4;i++) out |= (sc_uint<64>)hw << (16*i);
    } else if(eb==4){
        uint32_t w = raw & 0xFFFFFFFFu; out = (sc_uint<64>)w | ((sc_uint<64>)w << 32);
    } else { // eb==8
        out = raw;
    }
    return out;
}

// ---- Sequential: register updates ----
void DataLoader::seq_proc() {
    if(!rst_n.read()) {
        base_addr_r = 0; remaining_r = 0; stride_r = 1; mask_r = 0; is_store_r = false;
        state_r = 0; // IDLE
        store_transfers_cnt_r = 0; load_outputs_cnt_r = 0;
        return;
    }
    // 寫入 next-state
    base_addr_r = base_addr_n;
    remaining_r = remaining_n;
    stride_r    = stride_n;
    mask_r      = mask_n;
    is_store_r  = is_store_n;
    state_r     = state_n;
    store_transfers_cnt_r = store_transfers_cnt_n;
    load_outputs_cnt_r    = load_outputs_cnt_n;
}

// ---- Combinational: next-state + outputs ----
void DataLoader::comb_proc() {
    // 預設: 維持當前值
    base_addr_n = base_addr_r.read();
    remaining_n = remaining_r.read();
    stride_n    = stride_r.read();
    mask_n      = mask_r.read();
    is_store_n  = is_store_r.read();
    state_n     = state_r.read();
    store_transfers_cnt_n = store_transfers_cnt_r.read();
    load_outputs_cnt_n    = load_outputs_cnt_r.read();

    // 預設輸出
    done_o.write(false);
    busy_o.write(false);
    dm_en_o.write(false);
    dm_wr_o.write(false);
    dm_addr_o.write(base_addr_r.read());
    dm_mask_o.write(mask_r.read());
    dm_wdata_o.write(0);
    data_o.write(expand_broadcast(mode_i.read(), dm_rdata_i.read())); // 提供近期將輸出的資料
    data_valid_o.write(false);
    ps_ready_o.write(false);

    if(!rst_n.read()) { return; }

    // 即時參數
    if(set_addr_i.read()) base_addr_n = addr_len_i.read();
    if(set_len_i.read())  remaining_n = addr_len_i.read();
    {
        unsigned eb = element_bytes(mode_i.read());
        unsigned sv = stride_i.read(); if(sv==0) sv=1;
        stride_n = (sc_uint<16>)(sv * eb);
        mask_n = mode_to_mask(mode_i.read());
    }
    // 狀態機 (含 PREFETCH)
    enum {IDLE=0, PREFETCH=1, BUSY=2, DONE=3};
    switch(state_r.read()) {
    case IDLE: {
        if(activate_i.read()) {
            if(remaining_n == 0) {
                state_n = DONE;
                break;
            }
            is_store_n = wen_i.read();
            if(is_store_n) {
                state_n = BUSY;
            } else {
                state_n = PREFETCH;
            }
        }
    } break;

    case PREFETCH: {
        dm_en_o.write(true); dm_wr_o.write(false); // read request
        dm_addr_o.write(base_addr_r.read()); // write address for read request
        state_n = BUSY;
    } break;

    case BUSY: {
        if(is_store_r.read()) {
            busy_o.write(true);
            if(remaining_n > 0) {
                ps_ready_o.write(true);
                if(ps_valid_i.read()) {
                    dm_en_o.write(true); dm_wr_o.write(true);
                    dm_addr_o.write(base_addr_r.read()); dm_mask_o.write(mask_n);
                    dm_wdata_o.write(ps_data_i.read());
                    store_transfers_cnt_n = store_transfers_cnt_n + 1;
                    if(remaining_n > 0) remaining_n = remaining_n - 1;
                    if(remaining_n > 0) base_addr_n = base_addr_n + stride_n;
                }
            }
            if(remaining_n == 0) { state_n = DONE; }
        } else { // laod
            busy_o.write(false);
            data_valid_o.write(true); // 當前拍輸出資料
            // 處理 next_i：當拍只設定 out_pending 與發下一筆 read；不立即輸出
            if(next_i.read()) {
                load_outputs_cnt_n = load_outputs_cnt_n + 1;
                if(remaining_n > 0) {
                    remaining_n = remaining_n - 1;
                    if(remaining_n > 0) {
                        // 發出下一筆地址 (t1)
                        sc_uint<16> next_addr = base_addr_n + stride_n;
                        base_addr_n = next_addr;
                        dm_en_o.write(true); dm_wr_o.write(false);
                        dm_addr_o.write(next_addr); dm_mask_o.write(mask_n);
                    }
                }
            }
            // 完成條件：沒有剩餘、沒有待輸出、沒有在路上的資料
            if(remaining_n == 0) {
                state_n = DONE;
            }
        }
    } break;

    case DONE: {
        done_o.write(true);
        state_n = IDLE;
        is_store_n = false;
    } break;

    default: state_n = IDLE; break;
    }

    if(clr_counters_flag){
        clr_counters_flag = false;
        store_transfers_cnt_n = 0;
        load_outputs_cnt_n = 0;
    }
}

}} // namespace hybridacc::pe
