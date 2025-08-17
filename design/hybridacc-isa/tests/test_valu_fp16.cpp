#include "../src/simulator/component.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cassert>
#include <cstring> // for std::memcpy
#include <limits> // 加入以使用 infinity / NaN

using namespace hybridacc;

// Simple conversion between float and fp16 (IEEE 754 half) for test reference
// 實作 round-to-nearest-even，處理 normal / subnormal / overflow / underflow / Inf / NaN
static uint16_t float_to_fp16(float f){
    uint32_t w; std::memcpy(&w,&f,4);
    uint32_t sign = (w >> 31) & 0x1;
    uint32_t exp  = (w >> 23) & 0xFF;     // float exponent
    uint32_t mant = w & 0x7FFFFF;         // float mantissa (23 bits)

    // 特殊值: Inf / NaN
    if(exp == 0xFF){
        if(mant == 0){ // Inf
            return (sign << 15) | 0x7C00;
        }
        // NaN: 轉成 quiet, 保留高 payload bits (截斷)
        uint16_t payload = (mant >> 13) & 0x01FF; // 9 bits payload
        return (sign << 15) | 0x7C00 | 0x0200 | payload; // 1xxx xxxx xxxx: quiet NaN
    }

    // 處理非規格化 (float) -> 轉成規格化方便後續 (將 exp 調到 1 並左移 mant)
    if(exp == 0){
        if(mant == 0){ // ±0
            return (sign << 15);
        }
        // 將 subnormal float 正規化: 找到 leading 1
        exp = 1; // 在 IEEE 754 binary32 subnormal 隱含 exponent = 1 - bias (但這裡暫時設成1, 之後調整)
        while((mant & 0x00800000) == 0){ // 直到 bit23 成為 1
            mant <<= 1;
            exp--;                        // 相當於 exponent 減 1 (往更小)
        }
        mant &= 0x007FFFFF;              // 移除隱含 1 位 (bit23)
    }

    // 將 float exponent 轉成 half exponent: bias 差異 127 -> 15
    int32_t half_exp = (int32_t)exp - 127 + 15;

    // 溢出: 轉 Inf (不做 saturate)
    if(half_exp >= 0x1F){
        return (sign << 15) | 0x7C00; // Inf
    }

    // 可能成為 subnormal 或 0
    if(half_exp <= 0){
        // 若小到無法成為 subnormal (超過 10 bits 與 implicit 1 也移光) -> 直接為 0
        if(half_exp < -10){
            return (sign << 15);
        }
        // 形成帶有隱含 1 的 24-bit mantissa: 1.x * 2^(exp) -> mant 部分加上隱含位
        uint32_t m = mant | 0x00800000; // 加入 leading 1 (bit23)
        // shift = 1 - half_exp: 我們需要右移 (1 - half_exp) 次使 exponent = 0 並下放為 subnormal
        int shift = 1 - half_exp; // 1..10
        // 目標是得到 10-bit mantissa (無隱含 1) 以及 RNE 捨入
        // 先把需要保留的位數 (10) 左邊的 bits 取出
        // 我們想最後得到: m_sub = (m >> (shift + 13)) (因 float mant 有 23 bits, half mant 10 bits, 差 13)
        uint32_t shifted = m >> (shift + 13); // 初步 10-bit (含可能進位前)
        uint32_t rem_mask = (1u << (shift + 13)) - 1; // 被捨去的所有 bits
        uint32_t rem = m & rem_mask;                  // remainder bits
        uint32_t halfway = 1u << (shift + 12);        // halfway bit (guard 位置)
        uint32_t lsb = shifted & 1u;                  // 最低保留位，用於 ties-to-even
        if(rem > halfway || (rem == halfway && lsb)){
            shifted++;
        }
        return (sign << 15) | (uint16_t)shifted; // exp=0 已包含在格式中
    }

    // Normal case
    // 取得 10-bit mantissa 與保留捨去區 (13 bits) -> 使用 RNE
    uint32_t base = mant >> 13;              // 10 bits (可能進位前)
    uint32_t rem  = mant & 0x1FFF;           // 13 bits remainder
    uint32_t halfway = 0x1000;               // 中點 (bit12)
    uint32_t lsb = base & 1u;
    if(rem > halfway || (rem == halfway && lsb)){
        base++;
        if(base == 0x400){ // mantissa overflow (從 0x3FF + 1 -> 0x400)
            base = 0;      // mantissa rollover
            half_exp++;
            if(half_exp >= 0x1F){ // 進位造成 exponent 溢出 -> Inf
                return (sign << 15) | 0x7C00;
            }
        }
    }

    return (sign << 15) | ((half_exp & 0x1F) << 10) | (uint16_t)(base & 0x3FF);
}

static float fp16_to_float(uint16_t h){
    uint32_t sign = (h>>15)&1;
    uint32_t exp = (h>>10)&0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t out_sign = sign<<31;
    if(exp==0){
        if(mant==0){ uint32_t z=out_sign; float f; std::memcpy(&f,&z,4); return f; }
        // subnormal
        float m = mant / 1024.0f;
        float v = std::ldexp(m, -14); // 2^-14 * m
        return sign? -v : v;
    } else if(exp==0x1F){
        if(mant==0){ uint32_t inf = out_sign | 0x7F800000; float f; std::memcpy(&f,&inf,4); return f; }
        uint32_t nan = out_sign | 0x7FC00000; float f; std::memcpy(&f,&nan,4); return f;
    }
    int32_t full_exp = (int32_t)exp - 15 + 127;
    uint32_t full_mant = mant << 13;
    uint32_t bits = out_sign | ((full_exp & 0xFF)<<23) | full_mant;
    float f; std::memcpy(&f,&bits,4); return f;
}

float fp16_trunc(float fp32) {
    // 將 fp32 量化到 fp16 再轉回 (模擬硬體半精度來源運算數)
    uint16_t h = float_to_fp16(fp32);
    float f = fp16_to_float(h);
    return f;
}

void print_fp16(uint16_t h) {
    float f = fp16_to_float(h);
    uint16_t sign = (h >> 15) & 0x1;
    uint16_t exp = (h >> 10) & 0x1F;
    uint16_t mantissa = h & 0x3FF;
    std::cout << "fp16: " << std::fixed << f << " (0x" << std::hex << h << ")"
              << " [sign: " << sign << ", exp: " << exp << ", mantissa: " << mantissa << "]"
              << std::dec << "\n";
}

static bool fp16_is_subnormal(uint16_t h){
    return ((h & 0x7C00) == 0) && (h & 0x03FF); // exp=0 & mantissa!=0
}
static uint16_t fp16_flush_subnormal_to_zero(uint16_t h){
    if(!fp16_is_subnormal(h)) return h;
    return (h & 0x8000); // 保留符號,  mant=0 -> ±0
}

struct BinOpCase { float a; float b; };

int main(){
    VALU valu;

    // 預備特殊常數 (只測 normal / inf / nan / zero，不含 subnormal 操作數)
    const float INF = std::numeric_limits<float>::infinity();
    const float QNAN = std::numeric_limits<float>::quiet_NaN();
    const float MIN_NORMAL_HALF = std::ldexp(1.0f, -14); // 2^-14 = 6.10352e-5

    // create diverse test cases ( >= 32 ) ，過濾掉 subnormal (< 6.10352e-5) 的操作數
    std::vector<BinOpCase> mul_cases = {
        // normal * normal / zero
        {0.0f,0.0f},{1.0f,1.0f},{-1.0f,1.0f},{1.0f,-2.0f},{3.5f,-4.0f},{65504.f,1.0f},
        {-3.14159f,2.71828f},{0.3333f,0.6666f},{10.f,0.125f},{-0.5f,-0.5f},{4.f,0.25f},
        {8.f,0.125f},{15.f,-0.75f},{1.5f,1.5f},{2.f,2.f},{-8.f,-4.f},{16.f,-0.25f},
        {123.5f,-0.03125f},{1.0f,65504.f},{0.0001f,0.0002f},{1000.f,0.001f},{-100.f,0.01f},
        {65504.f,2.0f},{65504.f,-2.0f},{42.f,-1.f},{-7.f,-9.f},{3.25f,3.25f},{2.5f,-3.5f},{9.f,9.f},{1.0f,-65504.f},
        // infinity / NaN cases
        {INF,1.0f},{-INF,-1.0f},{INF,-INF},{INF,65504.f},{-INF,65504.f},
        {QNAN,1.0f},{1.0f,QNAN},{QNAN,QNAN}
    };

    std::vector<BinOpCase> add_cases = {
        {0.0f,0.0f},{1.0f,1.0f},{-1.0f,1.0f},{1.0f,-2.0f},{3.5f,-4.0f},{65504.f,1.0f},
        {-3.14159f,2.71828f},{0.3333f,0.6666f},{10.f,0.125f},{-0.5f,-0.5f},{4.f,0.25f},
        {8.f,0.125f},{15.f,-0.75f},{1.5f,1.5f},{2.f,2.f},{-8.f,-4.f},{16.f,-0.25f},
        {123.5f,-0.03125f},{1.0f,65504.f},{0.0001f,0.0002f},{1000.f,0.001f},{-100.f,0.01f},
        {65504.f,2.0f},{-65504.f,-2.0f},{42.f,-1.f},{-7.f,-9.f},{3.25f,3.25f},{2.5f,-3.5f},{9.f,9.f},{1.0f,-65504.f},
        // infinity / NaN cases
        {INF,1.0f},{-INF,-1.0f},{INF,-INF},{INF,65504.f},{-INF,65504.f},
        {QNAN,1.0f},{1.0f,QNAN},{QNAN,QNAN}
    };

    int mul_pass=0, add_pass=0;

    // test multiply
    for(auto &c: mul_cases){
        uint16_t ha = float_to_fp16(c.a);
        uint16_t hb = float_to_fp16(c.b);
        uint16_t hres_raw = valu.fp16_mul(ha, hb);
        uint16_t hres = fp16_flush_subnormal_to_zero(hres_raw); // FTZ: subnormal -> ±0
        float ref = c.a * c.b; // 高精度
        float ref_eff = ref;
        uint16_t href = float_to_fp16(ref_eff);
        if(fp16_is_subnormal(href)) ref_eff = std::copysign(0.0f, ref_eff); // 參考值同步 FTZ

        print_fp16(ha); // 印出操作數
        print_fp16(hb); // 印出操作數
        print_fp16(hres); // 印出結果
        print_fp16(href); // 印出參考值
        std::cout << "---------------------- MUL \n";

        float got = fp16_to_float(hres);
        if(hres != href) { // compare hres and href directly
            std::cerr << "MUL mismatch a="<<c.a<<" b="<<c.b<<" ref="<<ref<<" got="<<got<<"\n";
            assert(false);
        }
        mul_pass++;
    }

    // test add
    for(auto &c: add_cases){
        uint16_t ha = float_to_fp16(c.a);
        uint16_t hb = float_to_fp16(c.b);
        uint16_t hres_raw = valu.fp16_add(ha, hb);
        uint16_t hres = fp16_flush_subnormal_to_zero(hres_raw);
        // 參考：先量化操作數 -> 還原成 float32 -> 在此精度下相加 -> 再量化 (模擬硬體 half 加法)
        float ref = fp16_trunc(c.a) + fp16_trunc(c.b);
        float ref_eff = ref;
        uint16_t href = float_to_fp16(ref_eff);
        print_fp16(ha); // 印出操作數
        print_fp16(hb); // 印出操作數
        print_fp16(hres); // 印出結果
        print_fp16(href); // 印出參考值
        std::cout << "---------------------- ADD \n";
        float got = fp16_to_float(hres);
        if(hres != href) { // compare hres and href directly
            std::cerr << "ADD mismatch a="<<c.a<<" b="<<c.b<<" ref="<<ref<<" got="<<got<<"\n";
            assert(false);
        }
        add_pass++;
    }

    std::cout << "fp16 mul cases passed: "<<mul_pass<<" add cases passed: "<<add_pass<<"\n";
    return 0;
}
