#include "component.hpp"
#include "../assambler/utils.hpp"
#include <algorithm>
#include <stdexcept>
#include <iostream>
#include <cassert>

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
    // Extract sign, exponent, and mantissa
    uint16_t sign_a = (a >> 15) & 0x1;
    uint16_t sign_b = (b >> 15) & 0x1;
    uint16_t exp_a = (a >> 10) & 0x1F;
    uint16_t exp_b = (b >> 10) & 0x1F;
    uint16_t mant_a = a & 0x3FF;
    uint16_t mant_b = b & 0x3FF;

    // Handle special cases (zero, infinity, NaN)
    if (exp_a == 0x1F || exp_b == 0x1F) {
        return FP16_INFINITY; // Return infinity for simplicity
    }
    if (exp_a == 0 || exp_b == 0) {
        return FP16_ZERO; // Return zero if any operand is zero
    }

    // Compute sign, exponent, and mantissa
    uint16_t sign_res = sign_a ^ sign_b;
    int exp_res = exp_a + exp_b - 15; // Adjust bias
    uint32_t mant_res = (uint32_t)(mant_a | 0x400) * (mant_b | 0x400); // Include implicit 1

    // Normalize result
    if (mant_res & 0x800000) {
        mant_res >>= 11;
        exp_res += 1;
    } else {
        mant_res >>= 10;
    }

    // Handle overflow and underflow
    if (exp_res >= 0x1F) {
        return (sign_res << 15) | 0x7C00; // Overflow to infinity
    }
    if (exp_res <= 0) {
        return 0; // Underflow to zero
    }

    // Assemble result
    return (sign_res << 15) | ((exp_res & 0x1F) << 10) | (mant_res & 0x3FF);
}

uint16_t VALU::fp16_add(uint16_t a, uint16_t b) {
    // Extract sign, exponent, and mantissa
    uint16_t sign_a = (a >> 15) & 0x1;
    uint16_t sign_b = (b >> 15) & 0x1;
    uint16_t exp_a = (a >> 10) & 0x1F;
    uint16_t exp_b = (b >> 10) & 0x1F;
    uint16_t mant_a = a & 0x3FF;
    uint16_t mant_b = b & 0x3FF;

    // Handle special cases (zero, infinity, NaN)
    if (exp_a == 0x1F || exp_b == 0x1F) {
        if ((mant_a != 0 && exp_a == 0x1F) || (mant_b != 0 && exp_b == 0x1F)) {
            return FP16_INFINITY; // NaN
        }
        return (exp_a == 0x1F) ? a : b; // Infinity
    }
    if (exp_a == 0) {
        if (mant_a == 0) return b; // a is zero
        exp_a = 1; // Denormalized to normalized
    }
    if (exp_b == 0) {
        if (mant_b == 0) return a; // b is zero
        exp_b = 1; // Denormalized to normalized
    }

    // Align mantissas
    mant_a |= 0x400; // Add implicit leading 1
    mant_b |= 0x400; // Add implicit leading 1
    if (exp_a > exp_b) {
        mant_b >>= (exp_a - exp_b);
        exp_b = exp_a;
    } else if (exp_b > exp_a) {
        mant_a >>= (exp_b - exp_a);
        exp_a = exp_b;
    }

    // Perform addition or subtraction
    uint16_t result_sign = sign_a;
    int32_t mant_res;
    if (sign_a == sign_b) {
        mant_res = mant_a + mant_b;
    } else {
        if (mant_a >= mant_b) {
            mant_res = mant_a - mant_b;
        } else {
            mant_res = mant_b - mant_a;
            result_sign = sign_b;
        }
    }

    // Normalize result
    int result_exp = exp_a;
    while (mant_res >= 0x800) {
        mant_res >>= 1;
        result_exp++;
    }
    while (mant_res > 0 && mant_res < 0x400) {
        mant_res <<= 1;
        result_exp--;
    }

    // Handle overflow and underflow
    if (result_exp >= 0x1F) {
        return (result_sign << 15) | FP16_INFINITY; // Overflow to infinity
    }
    if (result_exp <= 0) {
        return FP16_ZERO; // Underflow to zero
    }

    // Assemble result
    return (result_sign << 15) | ((result_exp & 0x1F) << 10) | (mant_res & 0x3FF);
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
    uint64_t word = 0;
    for (int i = 0; i < 8; ++i) {
        word |= static_cast<uint64_t>(mem[idx * 8 + i]) << (i * 8);
    }
    return word;
}

void DataMemory::writeWord(int idx, uint64_t v, uint8_t mask) {
    if (mask == 0) return; // 如果 mask 為 0，則
    if (idx < 0 || idx >= size()) {
        throw std::out_of_range("[writeWord] Index out of range: " + std::to_string(idx));
    }
    for (int i = 0; i < 8; ++i) {
        if (mask & (1 << i)) mem[idx * 8 + i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
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
    stack.push_back(fr);
}

void LoopController::loopBreak(){
    if(!stack.empty()) stack.pop_back();
}

bool LoopController::handleLoopEndFlag(uint16_t &pc_after_increment){
    if(stack.empty()) return false;
    auto &top = stack.back(); // peek the top frame
    if(top.remaining == 0){ // 防護
        stack.pop_back();
        return false;
    }
    top.remaining--;
    if(top.remaining > 0){
        pc_after_increment = top.start_pc; // 回到 loop 內部開始位置
        return true; // 有跳轉
    } else {
        stack.pop_back();
        return false; // loop 結束無跳轉
    }
}

void DMAController::activate() {
    assert(!dma_active); // 確保沒有活動的 DMA
    dma_active = true;
    handle(); // 立即處理下一步
}

void DMAController::next() {
    if (!dma_active) return; // 如果沒有活動則不處理
    handle(); // 處理當前請求
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
            v = v & 0xFF; // 假設只讀取低位
            if(dma_broadcast) {
                for(int i = 0; i < 4; ++i) {
                    broadcast_v |= v << (i * 16);
                }
                v = broadcast_v; // 將所有 lane 設為相同值
            }
            dmrv->fromUint64(v); // 假設將其存儲為 64 位
            break;
        case DMARequestType::LOAD_HALF:
            v = dm->readWord(dma_base + dma_offset);
            v = v & 0xFFFF; // 假設只讀取低位
            if(dma_broadcast) {
                for(int i = 0; i < 4; ++i) {
                    broadcast_v |= v << (i * 16);
                }
                v = broadcast_v; // 將所有 lane 設為相同值
            }
            dmrv->fromUint64(v); // 假設將其存儲為 64 位
            break;
        case DMARequestType::LOAD_WORD:
            v = dm->readWord(dma_base + dma_offset);
            v = v & 0xFFFFFFFF; // 假設只讀取低位
            if(dma_broadcast) {
                for(int i = 0; i < 2; ++i) {
                    broadcast_v |= v << (i * 32);
                }
                v = broadcast_v; // 將所有 lane 設為相同值
            }
            dmrv->fromUint64(v); // 假設將其存儲為 64 位
            break;
        case DMARequestType::LOAD_DWORD:
            v = dm->readWord(dma_base + dma_offset);
            dmrv->fromUint64(v); // 假設將其存儲為 64 位
            break;
        case DMARequestType::STORE_DWORD:
            dm->writeWord(dma_base + dma_offset, dmwv->toUint64(), 0xFF); // 假設寫入整個字
            break;
        default:
            throw std::runtime_error("Unsupported DMA request type");
    }

    // step
    dma_offset += dma_stride * sizeof(uint16_t); // 假設 stride 是 16-bit 對齊
    dma_len--;
    if (dma_len == 0) {
        dma_active = false; // 完成後關閉 DMA
    }
}

} // namespace hybridacc
