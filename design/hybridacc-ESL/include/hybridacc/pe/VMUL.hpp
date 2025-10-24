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

template <unsigned T_DATA_WIDTH = 16, unsigned T_VECTOR_SIZE = 4>
SC_MODULE(VALU) {

    sc_in<sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE>> va_i; // Input vector A
    sc_in<sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE>> vb_i; // Input vector B
    sc_out<sc_uint<T_DATA_WIDTH*T_VECTOR_SIZE>> vout_o;           // Output result

    SC_CTOR(VALU)
    {
        SC_METHOD(comb_proc);
        sensitive << va_i << vb_i;
        dont_initialize();
    }

    void comb_proc() {
        // Convert input sc_uint to std::vector for easier manipulation
        std::vector<DATA_TYPE> va = sc_uint_to_vector(va.read());
        std::vector<DATA_TYPE> vb = sc_uint_to_vector(vb.read());
        // Element-wise multiplication
        std::vector<DATA_TYPE> mul_result = vector_mul(vec_mul_a, vec_mul_b);
        // Convert back to sc_uint
        vout_o.write(vector_to_sc_uint(mul_result));
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
};

} // namespace pe
} // namespace hybridacc

#endif // HYBRIDACC_PE_VALU_HPP