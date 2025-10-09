#ifndef HYBRIDACC_PE_VALU_HPP
#define HYBRIDACC_PE_VALU_HPP

#include <systemc.h>
#include <vector>
#include <queue>
#include <cstdint>
#include "hybridacc/utils.hpp"

using namespace sc_core;

namespace hybridacc {
namespace pe {

typedef uint16_t DATA_TYPE; // Assuming 16-bit data width for simplicity
typedef enum { VALU_NOP, VMAC, VMUL, VADD } VALUMODE;

template <unsigned T_DATA_WIDTH = 16, unsigned T_VECTOR_SIZE = 4>
SC_MODULE(VALU) {
    // Ports
    sc_in<VALUMODE> valumode_i; // Operation mode

    sc_in<sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE>> vec_mul_a_i; // Input vector A
    sc_in<sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE>> vec_mul_b_i; // Input vector B
    sc_in<sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE>> vec_p_i;     // Input vector partial sums
    sc_in<sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE>> vec_pli_i;   // Input vector port local inputs

    sc_out<sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE>> out_vec_o;           // Output result

    sc_in<sc_uint<T_DATA_WIDTH>> p_i; // Input scalar partial sum
    sc_out<sc_uint<T_DATA_WIDTH>> out_p_o; // Output scalar result

    SC_CTOR(VALU)
    {
        SC_METHOD(comb_proc);
        sensitive << valumode_i
                   << vec_mul_a_i << vec_mul_b_i
                   << vec_p_i << vec_pli_i
                   << p_i;
        dont_initialize();
    }

    void comb_proc() {
        // Convert input sc_uint to std::vector for easier manipulation
        std::vector<DATA_TYPE> vec_mul_a = sc_uint_to_vector(vec_mul_a_i.read());
        std::vector<DATA_TYPE> vec_mul_b = sc_uint_to_vector(vec_mul_b_i.read());
        std::vector<DATA_TYPE> vec_p     = sc_uint_to_vector(vec_p_i.read());
        std::vector<DATA_TYPE> vec_pli   = sc_uint_to_vector(vec_pli_i.read());

        // Element-wise multiplication
        std::vector<DATA_TYPE> mul_result = vector_mul(vec_mul_a, vec_mul_b);

        // Element-wise addition with vec_p (partial sums)
        std::vector<DATA_TYPE> add_result;
        if (valumode_i.read() == VADD) {
            add_result = vector_add(vec_pli, vec_p);
        } else if (valumode_i.read() == VMUL) {
            // Pure multiply accumulate into zero (or treat vec_p as zero) => here: just mul_result
            add_result = mul_result; // ignoring vec_p accumulation
        } else if (valumode_i.read() == VMAC) {
            // mul + accumulate
            add_result = vector_add(mul_result, vec_p);
        } else { // VALU_NOP
            add_result = vec_p; // pass-through
        }

        // Reduction (adder tree) over add_result + scalar p_i
        DATA_TYPE reduced_result = adder_tree(add_result, p_i.read());

        // Convert back to sc_uint
        out_vec_o.write(vector_to_sc_uint(add_result));
        out_p_o.write(reduced_result);
    }

private:
    std::vector<DATA_TYPE> sc_uint_to_vector(const sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE>& data) {
        std::vector<DATA_TYPE> vec(T_VECTOR_SIZE);
        for (unsigned i = 0; i < T_VECTOR_SIZE; ++i) {
            vec[i] = (data >> (i * T_DATA_WIDTH)) & ((1 << T_DATA_WIDTH) - 1);
        }
        return vec;
    }

    sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE> vector_to_sc_uint(const std::vector<DATA_TYPE>& vec) {
        sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE> data = 0;
        for (unsigned i = 0; i < T_VECTOR_SIZE; ++i) {
            data |= (sc_uint<T_DATA_WIDTH>(vec[i]) << (i * T_DATA_WIDTH));
        }
        return data;
    }

    // parallel vector operations (in one cycle)
    std::vector<DATA_TYPE> vector_mul(const std::vector<DATA_TYPE>& a, const std::vector<DATA_TYPE>& b) {
        assert(a.size() == b.size());
        std::vector<DATA_TYPE> result(a.size());
        for (size_t i = 0; i < a.size(); ++i) {
            result[i] = fp16_mul(a[i], b[i]);
        }
        return result;
    }

    // parallel vector addition (in one cycle)
    std::vector<DATA_TYPE> vector_add(const std::vector<DATA_TYPE>& a, const std::vector<DATA_TYPE>& b) {
        assert(a.size() == b.size());
        std::vector<DATA_TYPE> result(a.size());
        for (size_t i = 0; i < a.size(); ++i) {
            result[i] = fp16_add(a[i], b[i]);
        }
        return result;
    }

    // reduction using adder tree (log2(N) cycles)
    DATA_TYPE adder_tree(const std::vector<DATA_TYPE>& vec, DATA_TYPE initial_value) { // reduction
        DATA_TYPE sum = initial_value;
        for (size_t i = 0; i < vec.size(); ++i) {
            sum = fp16_add(sum, vec[i]);
        }
        return sum;
    }

    // FP16 multiplication

    uint16_t fp16_mul(uint16_t a, uint16_t b) {
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

        // Inf * 0 => qNaN
        if( (a_is_inf && b_is_zero) || (b_is_inf && a_is_zero) ) return 0x7E00;
        // ±Inf * ±Inf => ±Inf
        if(a_is_inf || b_is_inf){
            uint16_t sign_res = sign_a ^ sign_b;
            return (sign_res<<15)|0x7C00;
        }
        // num * ±0 => ±0
        if(a_is_zero || b_is_zero){
            uint16_t sign_res = sign_a ^ sign_b;
            return (sign_res<<15); // 帶符號 0
        }

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

        // 取出主要 11 bits (leading1 + 10 fraction)
        uint32_t sig_main = (uint32_t)(prod >> 10); // prod [20:10]
        uint32_t guard = (prod >> 9) & 1; // prod [9]
        uint32_t round = (prod >> 8) & 1; // prod [8]
        uint32_t sticky = (prod & ((1u<<8)-1)) ? 1 : 0; // |prod [7:0]

        // Round to nearest even
        bool increment = false;
        if(guard){
            if(round || sticky || (sig_main & 1)) increment = true;
        }
        if(increment){
            sig_main++;
            if(sig_main == (1u<<11)){ // mantissa overflow 2.0 -> 1.0 x 2
                sig_main >>=1;
                exp_res++;
            }
        }

        uint16_t sign_res = sign_a ^ sign_b;

        // 溢位 -> Inf
        if(exp_res == 0x1F) return (sign_res<<15)|0x7C00;

        // 下溢或 subnormal -> flush 0 (符合測試 FTZ 行為)
        if(exp_res <= 0) return (sign_res<<15);

        uint16_t frac = sig_main & 0x3FF; // 去除 leading1
        return (sign_res<<15) | ((exp_res & 0x1F)<<10) | frac;
    }

    uint16_t fp16_add(uint16_t a, uint16_t b) {
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
        if (a_is_nan) return a | 0x200; // qNaN
        if (b_is_nan) return b | 0x200;

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

        // 對齊：先判斷 exponent 大小，標記大數/小數，diff = 大 - 小
        uint16_t big_exp, small_exp;
        uint32_t small_sig;

        bool a_is_big = (exp_a >= exp_b); // exp 相等時視 a 為大數 (不影響對齊)
        if (a_is_big) {
            big_exp = exp_a; small_exp = exp_b;
            small_sig = sig_b;
        } else {
            big_exp = exp_b; small_exp = exp_a;
            small_sig = sig_a;
        }

        int exp_res = big_exp; // 結果 exponent 先取較大的
        int diff = (int)big_exp - (int)small_exp; // >= 0
        if (diff > 14) {
            // 差距過大，小數全被右移掉 => 只留下 sticky = 1
            small_sig = 1; // 000...001 (S=1)
        } else if (diff > 0) {
            uint32_t sticky = 0;
            uint32_t shifted_out_mask = (1u << diff) - 1u;
            uint32_t shifted_out = small_sig & shifted_out_mask;
            if (shifted_out) sticky = 1;
            small_sig >>= diff; // 右移對齊
            small_sig |= sticky; // 放入 sticky 位
        }

        // 對齊後再放回 small_sig 到 sig_a / sig_b，保持後續加減邏輯不變
        if (a_is_big) {
            sig_b = small_sig;
        } else {
            sig_a = small_sig;
        }

        uint32_t sig_res;
        uint16_t sign_res;

        if (sign_a == sign_b) {
            // 同號加法
            sig_res = sig_a + sig_b;
            sign_res = sign_a;
            // sig相加可能產生進位 (位長超過 11+3 bits)，檢查最高有效位 bit 14 (第15個bit)
            if (sig_res & (1u << (11 + 3))) { // overflow: 2.x 形式 (因為我們有 11 bits 有效 + 3 GRS)
                sig_res >>= 1;
                exp_res++;
                if (exp_res == 0x1F) { // ==可以取代>=，因原本exp_res已經被排除為0x1F的可能
                    return (sign_res << 15) | 0x7C00; // Inf, exponent 全為 1
                }
            }
        } else {
            // 異號 => 做減法 (大 - 小)
            if (sig_a > sig_b || (sig_a == sig_b && exp_a >= exp_b)) { // a >= b
                sig_res = sig_a - sig_b;
                sign_res = sign_a;
            } else { // b > a
                sig_res = sig_b - sig_a;
                sign_res = sign_b;
            }
            if (sig_res == 0) {
                return 0; // 完全抵銷 => +0
            }
            // 規格化: 把 leading 1 移回到原本最高位 (bit 13 = 第14個bit) 的位置
            // ver.3
            int msb3 = ((sig_res >> 8) & 0xFF) ? 1 : 0; // 判斷msb[3]值，msb[3] = |sig_res[15:8];
            int data8 = msb3 ? ((sig_res >> 8) & 0xFF) : (sig_res & 0xFF); // 剩下要判斷的8個bit, data8 = msb[3] ? (sig_res[15:8]) : (sig_res[7:0])
            int msb2 = ((data8 >> 4) & 0xF) ? 1 : 0; // 判斷msb[2]值
            int data4 = msb2 ? ((data8 >> 4) & 0xF) : (data8 & 0xF); // 剩下要判斷的4個bit
            int msb1 = ((data4 >> 2) & 0x3) ? 1 : 0; // 判斷msb[1]值
            int data2 = msb1 ? ((data4 >> 2) & 0x3) : (data4 & 0x3); // 剩下要判斷的2個bit
            int msb0 = (data2 >> 1) & 0x1; // 判斷msb[0]值
            int msb = (msb3 << 3) | (msb2 << 2) | (msb1 << 1) | msb0; // 組合回msb

            int shift_cnt = 13 - msb; // 需要左移的位數
            sig_res <<= shift_cnt;
            exp_res  -= shift_cnt;

            // 不考慮後續subnormal rounding 回 normal 的 case，在這邊就FTZ
            if (exp_res <= 0) { // 變成 subnormal/0 -> flush 為 0
                return (sign_res << 15); // flush-to-zero
            }
        }

        // Rounding: sig_res 現在格式: [ 1 | 10 fraction | G | R | S ] (共 11+3 bits)
        uint32_t sig_main = sig_res >> 3; // 11 bits (含 leading 1)
        uint32_t guard_res = (sig_res >> 2) & 1;
        uint32_t round_res = (sig_res >> 1) & 1;
        uint32_t sticky_res = sig_res & 1;

        bool increment = false;
        if (guard_res) {
            if (round_res || sticky_res || (sig_main & 1)) increment = true; // round to nearest even
        }
        if (increment) {
            sig_main++;
            if (sig_main == (1u << 11)) { // 檢查進位，2.0 => 1.0 *2
                sig_main >>= 1;
                exp_res++;
                if (exp_res >= 0x1F) {
                    return (sign_res << 15) | 0x7C00; // Inf
                }
            }
        }

        // 去掉 leading 1 -> fraction
        uint16_t frac = sig_main & 0x3FF; // 低 10 bits

        return (sign_res << 15) | ((exp_res & 0x1F) << 10) | frac;
    }
};

} // namespace pe
} // namespace hybridacc

#endif // HYBRIDACC_PE_VALU_HPP