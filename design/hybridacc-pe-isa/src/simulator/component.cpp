#include "component.hpp"
#include "../assambler/utils.hpp"
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <cassert>
#include <cstring>
#include <cmath>
#include <limits>

namespace hybridacc {

Element VALU::vmac(const Vector& a, const Vector& b, const Element& c) {
    Element result;
    result = c; // 初始化結果為 c
    for (int i = 0; i < 4; ++i) {
        result = fp16_add(fp16_mul(a.lanes[i], b.lanes[i]), result);
    }
    return result;
}

Vector VALU::vmul(const Vector& a, const Vector& b, const Vector& c) {
    Vector result;
    for (int i = 0; i < 4; ++i) {
        result.lanes[i] = fp16_add(fp16_mul(a.lanes[i], b.lanes[i]), c.lanes[i]);
    }
    return result;
}

Vector VALU::vadd(const Vector& a, const Vector& b) {
    Vector result;
    for (int i = 0; i < 4; ++i) {
        result.lanes[i] = fp16_add(a.lanes[i], b.lanes[i]);
    }
    return result;
}

uint16_t VALU::fp16_mul(uint16_t a, uint16_t b) {
    // IEEE754 half precision 乘法 (round to nearest even)，支援 NaN/Inf/Zero，subnormal 產生或輸入時最終仍可被 flush (測試會 FTZ)
    uint16_t sign_a = (a >> 15) & 0x1;
    uint16_t sign_b = (b >> 15) & 0x1;
    uint16_t exp_a  = (a >> 10) & 0x1F;
    uint16_t exp_b  = (b >> 10) & 0x1F;
    uint16_t mant_a = a & 0x3FF;
    uint16_t mant_b = b & 0x3FF;

    // NaN
    bool a_is_nan = (exp_a==0x1F) && (mant_a!=0);
    bool b_is_nan = (exp_b==0x1F) && (mant_b!=0);
    if(a_is_nan) return (uint16_t)(a | 0x0200); // 確保為 qNaN
    if(b_is_nan) return (uint16_t)(b | 0x0200);

    // Inf / Zero
    bool a_is_inf = (exp_a==0x1F) && (mant_a==0);
    bool b_is_inf = (exp_b==0x1F) && (mant_b==0);
    bool a_is_zero = (exp_a==0) && (mant_a==0);
    bool b_is_zero = (exp_b==0) && (mant_b==0);

    // Inf * 0 => NaN
    if( (a_is_inf && b_is_zero) || (b_is_inf && a_is_zero) ) return 0x7E00;
    if(a_is_inf || b_is_inf){
        uint16_t sign_res = sign_a ^ sign_b;
        return (sign_res<<15)|0x7C00;
    }
    if(a_is_zero || b_is_zero){
        uint16_t sign_res = sign_a ^ sign_b;
        return (sign_res<<15); // 帶符號 0
    }

    // 處理 subnormal 輸入: 正規化 (若測試未提供可忽略，但這裡完整支援)
    auto normalize = [](uint16_t &exp, uint16_t &mant){
        if(exp==0){ // subnormal (mant!=0)
            while((mant & 0x400)==0){ // 直到升出隱含 1 位 (bit10)
                mant <<=1;
                if(mant & 0x800) mant &= 0x7FF; // 防護
                exp--;
            }
            // 調整: subnormal 原 exponent 為 0 (實際為 1-bias)，我們把它拉成 1.x 後 exponent 改為 1
            exp = 1;
            mant &= 0x3FF; // 去掉隱含位外其餘 10 bits (隱含位稍後再補)
        }
    };
    if(exp_a==0 && mant_a){ normalize(exp_a, mant_a); }
    if(exp_b==0 && mant_b){ normalize(exp_b, mant_b); }

    // 組成 11-bit significand (含隱含 1)
    uint32_t sig_a = (0x400u | mant_a); // 11 bits
    uint32_t sig_b = (0x400u | mant_b);

    // 乘法: 11x11 -> 22 bits (最多用到 bit21)
    uint32_t prod = sig_a * sig_b; // bits[21:0]

    int exp_res = (int)exp_a + (int)exp_b - 15; // bias 調整 (半精度 bias=15)

    // 規格化: 若為 2.x (bit21=1) 右移 1 並 exponent++，否則 leading1 在 bit20
    if(prod & (1u<<21)){
        prod >>=1;
        exp_res++;
    }

    // 現在 leading 1 在 bit20。建立 G R S 三位: 將 prod 左移 3 保留 GRS
    // prod bits: [20...(0)]
    uint64_t ext = ((uint64_t)prod) << 3; // leading1 at bit23, fraction 10 bits 接在後面，再 3 bits GRS

    // 取出主要 11 bits (leading1 + 10 fraction)
    uint32_t sig_main = (uint32_t)(ext >> 13); // bits [23:13]
    uint32_t guard = (ext >> 12) & 1;
    uint32_t round = (ext >> 11) & 1;
    uint32_t sticky = (ext & ((1u<<11)-1)) ? 1 : 0;

    // Round to nearest even
    bool inc = false;
    if(guard){
        if(round || sticky || (sig_main & 1)) inc = true;
    }
    if(inc){
        sig_main++;
        if(sig_main == (1u<<11)){ // mantissa overflow 2.0 -> 1.0 x 2
            sig_main >>=1;
            exp_res++;
        }
    }

    uint16_t sign_res = sign_a ^ sign_b;

    // 溢位 -> Inf
    if(exp_res >= 0x1F) return (sign_res<<15)|0x7C00;

    // 下溢或 subnormal -> flush 0 (符合測試 FTZ 行為)
    if(exp_res <= 0) return (sign_res<<15);

    uint16_t frac = sig_main & 0x3FF; // 去除 leading1
    return (sign_res<<15) | ((exp_res & 0x1F)<<10) | frac;
}

uint16_t VALU::fp16_add(uint16_t a, uint16_t b) {
    // 不能使用其他轉換函式，直接在此實作 IEEE754 half precision 加法 (round to nearest even)
    // 只需正確處理 normal / zero / inf / NaN，subnormal 結果或輸入可視為 0（測試輸入已排除 subnormal）

    uint16_t sign_a = (a >> 15) & 0x1;
    uint16_t sign_b = (b >> 15) & 0x1;
    uint16_t exp_a  = (a >> 10) & 0x1F;
    uint16_t exp_b  = (b >> 10) & 0x1F;
    uint16_t mant_a = a & 0x3FF;
    uint16_t mant_b = b & 0x3FF;

    // NaN 判斷
    bool a_is_nan = (exp_a == 0x1F) && (mant_a != 0);
    bool b_is_nan = (exp_b == 0x1F) && (mant_b != 0);
    if (a_is_nan) return a; // 傳回 payload
    if (b_is_nan) return b;

    // Inf 判斷
    bool a_is_inf = (exp_a == 0x1F) && (mant_a == 0);
    bool b_is_inf = (exp_b == 0x1F) && (mant_b == 0);
    if (a_is_inf && b_is_inf) {
        if (sign_a != sign_b) return 0xFE00; // +Inf + -Inf => -NaN
        return a; // 同號 inf
    }
    if (a_is_inf) return a;
    if (b_is_inf) return b;

    // Zero (含 ±0)
    bool a_is_zero = (exp_a == 0) && (mant_a == 0);
    bool b_is_zero = (exp_b == 0) && (mant_b == 0);
    if (a_is_zero && b_is_zero) {
        // IEEE: 符號處理可多樣，這裡採用 +0 (或保留其中一個的符號亦可)。
        return (sign_a & sign_b) << 15; // 若都為 -0 則給 -0，否則 +0
    }
    if (a_is_zero) return b;
    if (b_is_zero) return a;

    // 目前忽略 subnormal 輸入 (測試已過濾)。若有 subnormal，直接當成 0。
    if (exp_a == 0) return b; // treat a as 0
    if (exp_b == 0) return a; // treat b as 0

    // 建立擴展尾數，加入 G (guard), R (round), S (sticky) 三個位元 => 左移 3 bits
    // normal: 有隱含 1 => 1.mantissa
    uint32_t sig_a = (0x400 | mant_a) << 3; // 11 bits -> 14 bits (含 GRS)
    uint32_t sig_b = (0x400 | mant_b) << 3;

    int exp_res = exp_a;
    int diff = (int)exp_a - (int)exp_b;
    if (diff > 0) {
        // 對齊 b
        exp_res = exp_a;
        if (diff > 25) { // 差太大，b 直接變成 0 (sticky)
            sig_b = 1; // 只保留 sticky
        } else {
            // 右移 diff，建立 sticky
            uint32_t sticky = 0;
            if (diff > 0) {
                uint32_t shifted_out_mask = (1u << diff) - 1u;
                uint32_t shifted_out = sig_b & shifted_out_mask;
                if (shifted_out) sticky = 1;
                sig_b >>= diff;
                // 將 sticky 放入最低位 (S)
                sig_b |= sticky;
            }
        }
    } else if (diff < 0) {
        diff = -diff;
        exp_res = exp_b;
        if (diff > 25) {
            sig_a = 1; // 只剩 sticky
        } else {
            uint32_t sticky = 0;
            uint32_t shifted_out_mask = (1u << diff) - 1u;
            uint32_t shifted_out = sig_a & shifted_out_mask;
            if (shifted_out) sticky = 1;
            sig_a >>= diff;
            sig_a |= sticky;
        }
    }

    uint32_t sig_res;
    uint16_t sign_res;

    if (sign_a == sign_b) {
        // 同號加法
        sig_res = sig_a + sig_b;
        sign_res = sign_a;
        // 可能產生進位 (位長超過 11+3 bits)，檢查最高有效位 (原 leading 1 在 bit (10+3)=13)
        if (sig_res & (1u << (11 + 3))) { // overflow: 2.x 形式 (因為我們有 11 bits 有效 + 3 GRS)
            sig_res >>= 1;
            exp_res++;
            if (exp_res >= 0x1F) {
                return (sign_res << 15) | 0x7C00; // Inf
            }
        }
    } else {
        // 異號 => 做減法 (大 - 小)
        // 比較 magnitudes (使用未對齊前 exponent + mantissa? 需用對齊後 sig)
        if (sig_a > sig_b || (sig_a == sig_b && exp_a >= exp_b)) {
            sig_res = sig_a - sig_b;
            sign_res = sign_a;
        } else {
            sig_res = sig_b - sig_a;
            sign_res = sign_b;
        }
        if (sig_res == 0) {
            return 0; // 完全抵銷 => +0
        }
        // 規格化: 把 leading 1 移回到 0x400<<3 位置
        while ((sig_res & (0x400u << 3)) == 0 && exp_res > 0) {
            sig_res <<= 1;
            exp_res--;
        }
        if (exp_res <= 0) { // 變成 subnormal/0 -> flush 為 0
            return (sign_res << 15); // flush-to-zero
        }
    }

    // Rounding: sig_res 現在格式: [ 1 | 10 fraction | G | R | S ] (共 11+3 bits)
    uint32_t sig_main = sig_res >> 3; // 11 bits (含 leading 1)
    uint32_t guard = (sig_res >> 2) & 1;
    uint32_t round = (sig_res >> 1) & 1;
    uint32_t sticky = sig_res & 1;

    bool increment = false;
    if (guard) {
        if (round || sticky || (sig_main & 1)) increment = true; // round to nearest even
    }
    if (increment) {
        sig_main++;
        if (sig_main == (1u << 11)) { // overflow 2.0 => 1.0 *2
            sig_main >>= 1;
            exp_res++;
            if (exp_res >= 0x1F) {
                return (sign_res << 15) | 0x7C00; // Inf
            }
        }
    }

    // 去掉 leading 1 -> fraction
    uint16_t frac = sig_main & 0x3FF; // 低 10 bits

    // exponent 下溢 (結果為 subnormal) -> flush 0
    if (exp_res <= 0) {
        return (sign_res << 15); // flush-to-zero
    }

    return (sign_res << 15) | ((exp_res & 0x1F) << 10) | frac;
}

void InstructionMemory::load(const std::vector<uint16_t>& prog){
    if(prog.size() > mem.size()) throw std::runtime_error("Program too large for IM");
    std::copy(prog.begin(), prog.end(), mem.begin());
    program_size = size();
}

uint64_t DataMemory::readWord(int idx) const {
    if (idx < 0 || idx >= size()) {
        throw std::out_of_range("[readWord] Index out of range: " + std::to_string(idx));
    }
    const auto& bank = banks[active_bank];
    uint64_t word = 0;
    for (int i = 0; i < 8; ++i) {
        word |= static_cast<uint64_t>(bank[idx + i]) << (i * 8);
    }
    return word;
}

void DataMemory::writeBack(int idx, uint64_t v, uint8_t mask) {
    // Write to BACK (Non-active) bank - used by SDMA
    if (mask == 0) return;
    if (idx < 0 || idx >= size()) {
        throw std::out_of_range("[writeBack] Index out of range: " + std::to_string(idx));
    }
    int target_bank = 1 - active_bank;
    auto& bank = banks[target_bank];
    for (int i = 0; i < 8; ++i) {
        if (mask & (1 << i)) bank[idx + i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
    }
}

void TransformRegFile::shift(int maskBits){
    for(int i=0;i<11;++i){
        bool shiftEn = (maskBits >> i) & 1;
        if(shiftEn){
            reg[i] = reg[i+1]; // 往左移，T[i] <- T[i+1]
        } else {
            reg[i] = 0; // 清零
        }
    }
    reg[11] = 0; // 最後一個寄存器清零
}

void LoopController::loopIn(uint16_t start_pc, uint16_t count){
    if(count <= 1) return; // trivial loop 不建立
    LoopFrame fr{start_pc, count};
    loopstack.push_back(fr);
}

void LoopController::loopBreak(){
    if(!loopstack.empty()) loopstack.pop_back();
}

bool LoopController::handleLoopEndFlag(uint16_t &pc_after_increment){
    if(loopstack.empty()) return false;
    auto &top = loopstack.back(); // peek the top frame
    if(top.remaining == 0){ // 防護
        loopstack.pop_back();
        return false;
    }
    top.remaining--;
    if(top.remaining > 0){
        pc_after_increment = top.start_pc; // 回到 loop 內部開始位置
        return true; // 有跳轉
    } else {
        loopstack.pop_back();
        return false; // loop 結束無跳轉
    }
}

void DMAController::activate() {
    assert(!dma_active);
    dma_active = true;
    handle();
}

void DMAController::next() {
    if (!dma_active) return;
    handle();
}

void DMAController::checkLoopReset() {
     if(dma_len == 0){
         // loop_count > 1 means we need more iterations (N means total N times)
         // loop_count == 1 means we are done (1 time total)
         // loop_count == 0 means default (1 time total)
         if(loop_count > 1){
             loop_count--;
             dma_base = init_base;
             dma_len = init_len;
             dma_offset = 0;
         } else {
             dma_active = false;
             // If we are SDMA, finalize the bank (mark valid)
             if(request_type == DMARequestType::STORE_DWORD && dm != nullptr){
                dm->setValid(dm->getBackBank(), true);
             }
         }
     }
}

void DMAController::handle(){
    uint64_t v = 0;
    uint64_t broadcast_v = 0;
    if (dm == nullptr || dmrv == nullptr) {
        throw std::runtime_error("DMAController not initialized with DataMemory or DMRW");
    }
    switch (request_type) {
        case DMARequestType::LOAD_BYTE:
            v = dm->readWord(dma_base + dma_offset);
            v = v & 0xFF;
            if(dma_broadcast) {
                for(int i = 0; i < 4; ++i) {
                    broadcast_v |= v << (i * 16);
                }
                v = broadcast_v;
            }
            dmrv->fromUint64(v);
            break;
        case DMARequestType::LOAD_HALF:
            v = dm->readWord(dma_base + dma_offset);
            v = v & 0xFFFF;
            if(dma_broadcast) {
                for(int i = 0; i < 4; ++i) {
                    broadcast_v |= v << (i * 16);
                }
                v = broadcast_v;
            }
            dmrv->fromUint64(v);
            break;
        case DMARequestType::LOAD_WORD:
            v = dm->readWord(dma_base + dma_offset);
            v = v & 0xFFFFFFFF;
            if(dma_broadcast) {
                for(int i = 0; i < 2; ++i) {
                    broadcast_v |= v << (i * 32);
                }
                v = broadcast_v;
            }
            dmrv->fromUint64(v);
            break;
        case DMARequestType::LOAD_DWORD:
            v = dm->readWord(dma_base + dma_offset);
            dmrv->fromUint64(v);
            break;
        case DMARequestType::STORE_DWORD:
            // SDMA uses writeBack to fill the back buffer
            dm->writeBack(dma_base + dma_offset, dmwv->toUint64(), 0xFF);
            break;
        default:
            throw std::runtime_error("Unsupported DMA request type");
    }

    // step
    dma_offset += dma_stride * sizeof(uint16_t); // 16-bit word aligned
    if (dma_len > 0) dma_len--;

    // Check loop or completion
    checkLoopReset();
}

} // namespace hybridacc
