// filepath: src/controller.cpp
// ============================================================================
//  File        : controller.cpp
//  Module      : PE::Controller
//  Description : PE 內部控制單元骨架 (發佈微指令、狀態機、事件同步)。
//
//  Responsibilities (預期):
//    - 解析指令記憶體 (InstMemory) 內容
//    - 管理迴圈堆疊 (LoopStack) 與 資料載入排程 (DataLoader)
//    - 啟動 VALU / PortIO / Multicast 動作
//
//  Future Extensions:
//    - 動態分支 / predication 支援
//    - 微碼 (microcode) 或 nano-sequencer 模式
//
//  Change Log:
//    - 2025-10-05  Initial skeleton.
// ============================================================================
#include "hybridacc/pe/controller.hpp"

namespace hybridacc {
namespace pe {

void PEController::seq_proc() {
    if(!rst_n.read()) {
        m_pc = 0;
        m_inst = 0;
        m_pending_dm_next = false;
        m_state = State::IDLE;
        im_addr_o.write(0);
        stall_o.write(false);
        push_o.write(false);
        pop_o.write(false);
        jump_o.write(false);
        pc_jump_o.write(0);
        loop_count_o.write(0);
        dm_activate_o.write(false);
        dm_next_o.write(false);
        dm_set_addr_o.write(false);
        dm_set_len_o.write(false);
        dm_addr_len_o.write(0);
        dm_mode_o.write(0);
        dm_stride_o.write(0);
        dm_wen_o.write(false);
        valumode_i.write(VALU_NOP);
        pli_ready_o.write(false);
        plo_valid_o.write(false);
        return;
    }

    if(!en_i.read()) {
        stall_o.write(false);
        im_addr_o.write(m_pc);
        push_o.write(false);
        pop_o.write(false);
        jump_o.write(false);
        pc_jump_o.write(0);
        loop_count_o.write(0);
        dm_activate_o.write(false);
        dm_next_o.write(false);
        dm_set_addr_o.write(false);
        dm_set_len_o.write(false);
        dm_addr_len_o.write(0);
        dm_mode_o.write(0);
        dm_stride_o.write(0);
        dm_wen_o.write(false);
        valumode_i.write(VALU_NOP);
        pli_ready_o.write(false);
        plo_valid_o.write(false);
        return;
    }

    // ================= ENABLED =================
    // 預設單拍脈衝訊號清零 (無論狀態)
    push_o.write(false); pop_o.write(false); jump_o.write(false);
    dm_set_addr_o.write(false); dm_set_len_o.write(false);
    dm_activate_o.write(false); dm_next_o.write(false); dm_wen_o.write(false);
    pli_ready_o.write(false); plo_valid_o.write(false);
    p_we_o.write(false); t_wen_o.write(false); t_shift_en_o.write(false);
    p_clear_o.write(false); t_clear_o.write(false);
    stall_o.write(false);

    // 取指位址送出 (同步/非同步模式共用):
    im_addr_o.write(m_pc);

    // 若使用同步取指: 先用前一拍 m_inst 解碼，最後再 latch 新指令
    sc_uint<16> cur_inst = (m_use_sync_fetch)? m_inst : rd_data_i.read();
    if(!m_use_sync_fetch){ m_inst = cur_inst; } // 保留當前 (非同步模式)

    uint8_t  opcode = fld_opcode(cur_inst);
    uint8_t  funct2 = fld_funct2(cur_inst);
    uint8_t  func1  = fld_func1(cur_inst);
    uint8_t  func3  = fld_func3(cur_inst);
    uint8_t  payload= fld_payload(cur_inst);
    bool     loopend= fld_loopend(cur_inst);

    uint8_t next_pc = m_pc; // 先預設 hold，視情況加一或跳轉

    // 狀態機
    switch(m_state) {
    case State::IDLE:
        next_pc = m_pc; // 等待啟動
        m_state = State::RUNNING;
        break;

    case State::WAIT_DM:
        // 等候 dm_busy_i 釋放後送出 dm_next_o
        if(!dm_busy_i.read()) {
            if(m_pending_dm_next){
                dm_next_o.write(true);
                m_pending_dm_next = false;
            }
            m_state = State::RUNNING;
            next_pc = m_pc + 1; // 完成後才推進 PC
        } else {
            next_pc = m_pc; // 停留
        }
        break;

    case State::RUNNING: {
        // 若有待送出的 dm_next (來自前一個 *N 指令)
        if(m_pending_dm_next) {
            if(dm_busy_i.read()) {
                m_state = State::WAIT_DM;
                next_pc = m_pc; // 停住直到可送出
            } else {
                dm_next_o.write(true);
                m_pending_dm_next = false;
                // 本週期仍可解碼新指令? 為簡化，先停住不解碼 (避免與新指令動作重疊)
                next_pc = m_pc + 1;
            }
            break; // 本 cycle 處理完 pending 即結束
        }

        // 預設 PC++ (若無跳轉或等待)
        next_pc = m_pc + 1;

        // ---- 指令分類 ----
        if(opcode==0x0) { // Data Movement
            if(funct2==0x1) { // DMA.ADDR / DMA.LEN
                uint16_t value10 = decode_compressed10(cur_inst);
                dm_addr_len_o.write(value10 & 0x3FF);
                if(func1==0) dm_set_addr_o.write(true); else dm_set_len_o.write(true);
            } else if(funct2==0x2 || funct2==0x3) { // DMA.L* / DMA.SD (assembler: funct2=11 encoded 為 3? 注意: makeBase(0,2) / makeBase(0,3) 區分 L* vs SD )
                // 依 assembler: makeBase(0,2) 對應 funct2=10 (二進位), makeBase(0,3)=11 => 之前誤解, 這裡對應:
                //  - funct2==2 -> DMA.L* (func3 區種類) stride = bits[12:10] = (cur_inst>>10)&0x7
                //  - funct2==3 && func3==3 -> DMA.SD
                uint8_t stride = (cur_inst >> 10) & 0x7;
                if(funct2==0x2) { // LOAD 類
                    dm_mode_o.write(func3 & 0x7);
                    dm_stride_o.write(stride);
                    dm_activate_o.write(true);
                } else if(funct2==0x3 && func3==0x3) { // SD
                    dm_mode_o.write(0x3); // 可視需要定義
                    dm_stride_o.write(stride);
                    dm_activate_o.write(true);
                }
            }
        }
        else if(opcode==0x1) { // Transform / Jump / Loop
            if(funct2==0x0) { // TSTORE / TSHIFT
                if(func3==0x0) { // TSTORE: payload[8:5]
                    uint8_t trd = (payload & 0x1F); // assembler 使用 (trd &0xf)<<5，但此處容忍範圍
                    t_rd_idx_o.write(trd & 0xF);
                    t_wen_o.write(true);
                } else if(func3==0x1) { // TSHIFT: code bits[12:10]
                    uint8_t kcode = (cur_inst >> 10) & 0x7;
                    t_shift_mode_o.write(kcode);
                    t_shift_en_o.write(true);
                }
            } else if(funct2==0x2) { // J
                uint16_t imm = decode_jump_imm(cur_inst); // 11-bit byte address (2-byte aligned)
                if(imm & 0x1) { // 對齊保護
                    // 失配: 直接忽略或矯正 -> 這裡矯正對齊
                    imm &= ~1u;
                }
                uint16_t word_addr = (imm >> 1) & 0xFF; // 取低 8 bits (PC 寬度限制)
                pc_jump_o.write(word_addr);
                jump_o.write(true);
                next_pc = word_addr;
            } else if(funct2==0x3) { // LOOPIN / LOOPBREAK
                if(func1==0) { // LOOPIN
                    uint16_t cnt = decode_compressed10(cur_inst);
                    loop_count_o.write(cnt & 0x3FF);
                    pc_jump_o.write(next_pc); // body start
                    push_o.write(true);
                } else { // LOOPBREAK
                    pop_o.write(true);
                }
            }
        }
        else if(opcode==0x2) { // Arithmetic & System 部分
            if(funct2==0x0) { // NOP
                // no-op
            } else if(funct2==0x1) { // Vector arithmetic family
                // func1=1 => *N 需觸發下一拍 dm_next
                bool need_next = (func1==1);
                switch(func3) {
                    case 0x0: // VMAC / VMACN
                        valumode_i.write(VMAC); break;
                    case 0x1: // VMACR / VMACRN (暫以 VMAC 處理 + 重設/stride 之後擴充)
                        valumode_i.write(VMAC); break;
                    case 0x2: // VMUL / VMULN
                        valumode_i.write(VMUL); break;
                    case 0x3: // VMULR / VMULRN
                        valumode_i.write(VMUL); break;
                    case 0x4: // VPSUM
                        valumode_i.write(VADD); break;
                    case 0x5: // VPSUMR
                        valumode_i.write(VADD); break;
                    default:
                        valumode_i.write(VALU_NOP); break;
                }
                if(need_next) m_pending_dm_next = true;
            } else if(funct2==0x2) { // SETRID.* (目前僅佔位)
                // 後續可設定 p_mode / 索引
            } else if(funct2==0x3) { // CLEAR.*
                if(func3==0x0) t_clear_o.write(true);
                else if(func3==0x1) p_clear_o.write(true);
            }
        }
        else if(opcode==0x3) { // HALT 群 (opcode=11)
            if(funct2==0x3) { // HALT
                stall_o.write(true);
                next_pc = m_pc; // freeze
            }
        }

        // LOOPEND: 在本指令標記為迴圈結尾時，若 top_count_i > 1 則回跳 (loopstack 將遞減)
        if(loopend && top_count_i.read() > 1) {
            jump_o.write(true);
            pc_jump_o.write(pc_jump_i.read());
            next_pc = pc_jump_i.read().to_uint();
        }
    } break; // RUNNING

    default:
        // 其它等待狀態未實作 (PLI/PLO/PS/PD) -> 回復 RUNNING
        m_state = State::RUNNING;
        break;
    }

    // 更新 PC
    m_pc = next_pc;

    // 同步取指模式: 最後 latch 即將在下一拍被解碼的指令
    if(m_use_sync_fetch) {
        m_inst = rd_data_i.read();
    }
}

} // namespace pe
} // namespace hybridacc
