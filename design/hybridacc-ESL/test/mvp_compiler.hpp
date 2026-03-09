#pragma once

#include <vector>
#include <map>
#include <string>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <utility>
#include <systemc.h>

// --------------------------------------------------------------------------------
// Parameters (aligned with test_noc_sim.cpp)
// --------------------------------------------------------------------------------
struct ConvParams {
    int kernel_size = 0;
    int in_ch = 0;
    int out_ch = 0;
    int in_height = 0;
    int in_width = 0;
    int out_height = 0;
    int out_width = 0;
    int stride = 0;
    bool partial_sum_zero = false;
    bool ultra_mode = false;

    int temporal_wave_count = 0;
    int temporal_wave_out_h = 0;
    int temporal_wave_out_ch = 0;
    int temporal_wave_in_ch = 0;
};

struct GEMMParams {
    int M = 0;
    int N = 0;
    int K = 0;
    int grid_m = 1;
    int grid_n = 1;
    int grid_k = 1;
    int pe_m = 12;
    int pe_n = 8;
    int pe_k = 32;
    bool partial_sum_zero = false;
    bool ultra_mode = false;

    int wave_m = 1;
    int wave_n = 1;
    int wave_k = 1;

    std::vector<int> grid_m_per_wave;
    std::vector<int> grid_n_per_wave;
    std::vector<int> grid_k_per_wave;
};

// --------------------------------------------------------------------------------
// AGU Configuration Structure (Hardware Register View)
// --------------------------------------------------------------------------------
struct AguCfg {
    uint32_t base_addr = 0;
    uint32_t base_addr_h = 0;
    uint16_t iter0 = 1;
    uint16_t iter1 = 1;
    uint16_t iter2 = 1;
    uint16_t iter3 = 1;
    int32_t stride0 = 0;
    int32_t stride1 = 0;
    int32_t stride2 = 0;
    int32_t stride3 = 0;
    uint32_t lane_cfg = 0;
    uint32_t tag_base = 0;
    uint32_t tag_stride0 = 0;
    uint32_t tag_stride1 = 0;
    uint32_t tag_ctrl = 0;
    uint32_t mask_cfg = 0xF;
    bool ultra = false;
    bool enable = false;
};

// print AGU config for debugging
std::ostream& operator<<(std::ostream& os, const AguCfg& cfg) {
    os << "base_addr=0x" << std::hex << cfg.base_addr << std::dec
       << " iter=[" << cfg.iter0 << "," << cfg.iter1 << "," << cfg.iter2 << "," << cfg.iter3 << "]"
       << " stride=[" << cfg.stride0 << "," << cfg.stride1 << "," << cfg.stride2 << "," << cfg.stride3 << "]"
       << " lane_cfg=0x" << std::hex << cfg.lane_cfg << std::dec
       << " tag_base=0x" << std::hex << cfg.tag_base << std::dec
       << " tag_stride=[" << cfg.tag_stride0 << "," << cfg.tag_stride1 << "]"
       << " tag_ctrl=0x" << std::hex << cfg.tag_ctrl << std::dec
       << " mask_cfg=0x" << std::hex << cfg.mask_cfg << std::dec
       << " ultra=" << cfg.ultra;
    return os;
}

struct ClusterPlan {
    AguCfg agu_ps;
    AguCfg agu_pd;
    AguCfg agu_pli;
    AguCfg agu_plo;

    uint32_t global_mask = 0xF;
    bool ultra_mode = false;
    std::string name;
};

// --------------------------------------------------------------------------------
// MvpCompiler Class
// --------------------------------------------------------------------------------
class MvpCompiler {
public:
    static constexpr uint32_t kSpmGroupSpanWords = 8192u * 4u;
    static constexpr uint32_t kSpmWordBytes = 8u;
    static constexpr uint32_t kSpmGroupSpanBytes = kSpmGroupSpanWords * kSpmWordBytes;
    static constexpr int kNumPorts = 3;

    static uint32_t to_group_local_word_addr(uint32_t addr_bytes) {
        return (addr_bytes / kSpmWordBytes) % kSpmGroupSpanWords;
    }

    static int ceil_div_int(int a, int b) {
        if (b <= 0) return 0;
        return (a + b - 1) / b;
    }

    static std::pair<size_t, size_t> get_wave_range(size_t total, int waves, int wave_idx) {
        if (waves <= 0) return {0, total};
        size_t per_wave = (total + waves - 1) / waves;
        size_t start = wave_idx * per_wave;
        size_t end = std::min(start + per_wave, total);
        return {start, end};
    }

    static std::pair<size_t, size_t> get_wave_tile_range(const std::vector<int>& tiles_per_wave, int wave_idx, size_t total_tiles, int fallback_waves) {
        if (!tiles_per_wave.empty()) {
            size_t start = 0;
            for(int i=0; i<wave_idx && i < (int)tiles_per_wave.size(); ++i) start += tiles_per_wave[i];
            size_t count = (wave_idx < (int)tiles_per_wave.size()) ? tiles_per_wave[wave_idx] : 0;
            if (start >= total_tiles) start = total_tiles;
            size_t end = std::min(start + count, total_tiles);
            return {start, end};
        }
        return get_wave_range(total_tiles, fallback_waves, wave_idx);
    }

    // ----------------------------------------------------------------------------
    // Compile GEMM
    // ----------------------------------------------------------------------------
    static std::vector<ClusterPlan> compile_gemm(const GEMMParams& p,
                                                 uint32_t ps_base,
                                                 uint32_t pd_base,
                                                 uint32_t pli_base,
                                                 uint32_t plo_base)
    {
        std::vector<ClusterPlan> plans;
        const uint32_t ps_local_base = to_group_local_word_addr(ps_base);
        const uint32_t pd_local_base = to_group_local_word_addr(pd_base);
        const uint32_t pli_local_base = to_group_local_word_addr(pli_base);
        const uint32_t plo_local_base = to_group_local_word_addr(plo_base);

        int waves_k = p.grid_k_per_wave.empty() ? p.wave_k : p.grid_k_per_wave.size();
        int waves_n = p.grid_n_per_wave.empty() ? p.wave_n : p.grid_n_per_wave.size();
        int waves_m = p.grid_m_per_wave.empty() ? p.wave_m : p.grid_m_per_wave.size();
        if (waves_k < 1) waves_k = 1;
        if (waves_n < 1) waves_n = 1;
        if (waves_m < 1) waves_m = 1;

        for (int wk = 0; wk < waves_k; ++wk) {
            for (int wn = 0; wn < waves_n; ++wn) {
                for (int wm = 0; wm < waves_m; ++wm) {
                    auto k_r = get_wave_tile_range(p.grid_k_per_wave, wk, p.grid_k, waves_k);
                    auto n_r = get_wave_tile_range(p.grid_n_per_wave, wn, p.grid_n, waves_n);
                    auto m_r = get_wave_tile_range(p.grid_m_per_wave, wm, p.grid_m, waves_m);

                    if (k_r.first >= k_r.second || n_r.first >= n_r.second || m_r.first >= m_r.second) continue;

                    int n_tiles = n_r.second - n_r.first;
                    int row_w = ceil_div_int(p.N, 4);
                    int col_d = ceil_div_int(p.M, 4);
                    int tile_w = std::max(1, ceil_div_int(p.pe_n, 4));
                    int tile_d = std::max(1, ceil_div_int(p.pe_m, 4));

                    for (size_t kt = k_r.first; kt < k_r.second; ++kt) {
                        for (size_t mt = m_r.first; mt < m_r.second; ++mt) {
                            ClusterPlan plan;
                            plan.name = "GEMM_K" + std::to_string(wk)
                                        + "_N" + std::to_string(wn)
                                        + "_M" + std::to_string(wm)
                                        + "_KT" + std::to_string(kt)
                                        + "_MT" + std::to_string(mt);
                            plan.ultra_mode = p.ultra_mode;

                            uint32_t k_off = static_cast<uint32_t>(kt) * p.pe_k;
                            uint32_t n_off = static_cast<uint32_t>(n_r.first) * p.pe_n;
                            uint32_t m_off = static_cast<uint32_t>(mt) * p.pe_m;

                            plan.agu_ps.enable = true;
                            plan.agu_ps.ultra = p.ultra_mode;
                            uint32_t ps_base_words = (k_off * row_w) + (n_off / 4);
                            plan.agu_ps.base_addr = ps_local_base + ps_base_words;
                            plan.agu_ps.iter0 = static_cast<uint16_t>(tile_w);
                            plan.agu_ps.iter1 = static_cast<uint16_t>(p.pe_k);
                            plan.agu_ps.iter2 = static_cast<uint16_t>(n_tiles);
                            plan.agu_ps.iter3 = 1;
                            plan.agu_ps.stride0 = 1;
                            plan.agu_ps.stride1 = static_cast<int32_t>(row_w);
                            plan.agu_ps.stride2 = static_cast<int32_t>(tile_w);
                            plan.agu_ps.stride3 = static_cast<int32_t>(p.pe_k * row_w);
                            if (p.ultra_mode) {
                                plan.agu_ps.tag_base = 0;
                            } else {
                                plan.agu_ps.tag_base = static_cast<uint32_t>(kt) * p.grid_n + static_cast<uint32_t>(n_r.first);
                            }
                            plan.agu_ps.tag_stride0 = 1;
                            plan.agu_ps.tag_stride1 = 1;
                            plan.agu_ps.tag_ctrl = 2;

                            plan.agu_pd.enable = true;
                            plan.agu_pd.ultra = p.ultra_mode;
                            uint32_t pd_base_words = (k_off * col_d) + (m_off / 4);
                            plan.agu_pd.base_addr = pd_local_base + pd_base_words;
                            plan.agu_pd.iter0 = static_cast<uint16_t>(tile_d);
                            plan.agu_pd.iter1 = static_cast<uint16_t>(p.pe_k);
                            plan.agu_pd.iter2 = 1;
                            plan.agu_pd.iter3 = 1;
                            plan.agu_pd.stride0 = 1;
                            plan.agu_pd.stride1 = static_cast<int32_t>(col_d);
                            plan.agu_pd.stride2 = static_cast<int32_t>(p.pe_k * col_d);
                            plan.agu_pd.stride3 = static_cast<int32_t>(tile_d);
                            if (p.ultra_mode) {
                                plan.agu_pd.tag_base = 0;
                            } else {
                                plan.agu_pd.tag_base = static_cast<uint32_t>(kt) * p.grid_m + static_cast<uint32_t>(mt);
                            }
                            plan.agu_pd.tag_stride0 = 0;
                            plan.agu_pd.tag_stride1 = 1;
                            plan.agu_pd.tag_ctrl = 0;

                            if (wk == 0) {
                                plan.agu_pli.enable = true;
                                plan.agu_pli.ultra = p.ultra_mode;
                                uint32_t pli_base_words = (n_off * col_d) + (m_off / 4);
                                plan.agu_pli.base_addr = pli_local_base + pli_base_words;
                                plan.agu_pli.iter0 = static_cast<uint16_t>(tile_d);
                                plan.agu_pli.iter1 = static_cast<uint16_t>(p.pe_n);
                                plan.agu_pli.iter2 = static_cast<uint16_t>(n_tiles);
                                plan.agu_pli.iter3 = 1;
                                plan.agu_pli.stride0 = 1;
                                plan.agu_pli.stride1 = static_cast<int32_t>(col_d);
                                plan.agu_pli.stride2 = static_cast<int32_t>(p.pe_n * col_d);
                                plan.agu_pli.stride3 = static_cast<int32_t>(tile_d);
                                if (p.ultra_mode) {
                                    plan.agu_pli.tag_base = 0;
                                } else {
                                    plan.agu_pli.tag_base = static_cast<uint32_t>(mt) * p.grid_n + static_cast<uint32_t>(n_r.first);
                                }
                                plan.agu_pli.tag_stride0 = 1;
                                plan.agu_pli.tag_stride1 = 1;
                                plan.agu_pli.tag_ctrl = 2;
                            }
                            if (wk == waves_k - 1) {
                                plan.agu_plo.enable = true;
                                plan.agu_plo.ultra = p.ultra_mode;
                                uint32_t plo_base_words = (n_off * col_d) + (m_off / 4);
                                plan.agu_plo.base_addr = plo_local_base + plo_base_words;
                                plan.agu_plo.iter0 = static_cast<uint16_t>(tile_d);
                                plan.agu_plo.iter1 = static_cast<uint16_t>(p.pe_n);
                                plan.agu_plo.iter2 = static_cast<uint16_t>(n_tiles);
                                plan.agu_plo.iter3 = 1;
                                plan.agu_plo.stride0 = 1;
                                plan.agu_plo.stride1 = static_cast<int32_t>(col_d);
                                plan.agu_plo.stride2 = static_cast<int32_t>(p.pe_n * col_d);
                                plan.agu_plo.stride3 = static_cast<int32_t>(tile_d);
                                if (p.ultra_mode) {
                                    plan.agu_plo.tag_base = 0;
                                } else {
                                    plan.agu_plo.tag_base = static_cast<uint32_t>(mt) * p.grid_n + static_cast<uint32_t>(n_r.first);
                                }
                                plan.agu_plo.tag_stride0 = 1;
                                plan.agu_plo.tag_stride1 = 1;
                                plan.agu_plo.tag_ctrl = 2;
                            }

                            plans.push_back(plan);
                        }
                    }
                }
            }
        }
        return plans;
    }

    // ----------------------------------------------------------------------------
    // Compile Conv2D
    // ----------------------------------------------------------------------------
    static std::vector<ClusterPlan> compile_conv2d(const ConvParams& p,
                                                   uint32_t ps_base,
                                                   uint32_t pd_base,
                                                   uint32_t pli_base,
                                                   uint32_t plo_base)
    {
        std::vector<ClusterPlan> plans;
        const uint32_t ps_local_base = to_group_local_word_addr(ps_base);
        const uint32_t pd_local_base = to_group_local_word_addr(pd_base);
        const uint32_t pli_local_base = to_group_local_word_addr(pli_base);
        const uint32_t plo_local_base = to_group_local_word_addr(plo_base);
        int waves_h = std::max(1, p.temporal_wave_out_h);
        int waves_oc = std::max(1, p.temporal_wave_out_ch);
        int waves_ic = std::max(1, p.temporal_wave_in_ch);
        int K = p.kernel_size;
        int in_ch_pack = std::max(1, ceil_div_int(p.in_ch, 4));
        int out_ch_pack = std::max(1, ceil_div_int(p.out_ch, 4));

        int loop_in_height = p.in_height;
        int loop_out_height = p.out_height;
        if (p.ultra_mode) {
            loop_in_height = std::max(1, p.in_height / kNumPorts);
            loop_out_height = std::max(1, p.out_height / kNumPorts);
        }

        for(int wh=0; wh<waves_h; ++wh) {
            for(int woc=0; woc<waves_oc; ++woc) {
                for(int wic=0; wic<waves_ic; ++wic) {
                     ClusterPlan plan;
                     plan.name = "CONV_H" + std::to_string(wh) + "_OC" + std::to_string(woc) + "_IC" + std::to_string(wic);
                     plan.ultra_mode = p.ultra_mode;

                     auto oh_r = get_wave_range(loop_out_height, waves_h, wh);
                     auto oc_r = get_wave_range(p.out_ch, waves_oc, woc);
                     auto ic_r = get_wave_range(p.in_ch, waves_ic, wic);
                     if (oh_r.first >= oh_r.second || oc_r.first >= oc_r.second || ic_r.first >= ic_r.second) continue;

                     int ih_start = static_cast<int>(oh_r.first) * p.stride;
                     int ih_end = std::min(loop_in_height, static_cast<int>(oh_r.second) * p.stride + K - 1);
                     if (ih_start >= ih_end) continue;

                     int count_oc = oc_r.second - oc_r.first;
                     int count_oc_pack = std::max(1, ceil_div_int(count_oc, 4));
                     int count_ic_pack = std::max(1, ceil_div_int(static_cast<int>(ic_r.second - ic_r.first), 4));
                     int ic_pack_start = static_cast<int>(ic_r.first) / 4;
                     int oc_pack_start = static_cast<int>(oc_r.first) / 4;

                     plan.agu_ps.enable = true;
                     plan.agu_ps.ultra = p.ultra_mode;
                     plan.agu_ps.iter0 = static_cast<uint16_t>(count_ic_pack);
                     plan.agu_ps.iter1 = static_cast<uint16_t>(K);
                     plan.agu_ps.iter2 = static_cast<uint16_t>(K);
                     plan.agu_ps.iter3 = static_cast<uint16_t>(count_oc);

                     plan.agu_ps.stride0 = 1;
                     plan.agu_ps.stride1 = static_cast<int32_t>(in_ch_pack);
                     plan.agu_ps.stride2 = static_cast<int32_t>(K * in_ch_pack);
                     plan.agu_ps.stride3 = static_cast<int32_t>(K * K * in_ch_pack);

                     uint32_t ps_offset = static_cast<uint32_t>(ic_pack_start);
                     plan.agu_ps.base_addr = ps_local_base + ps_offset;

                     plan.agu_ps.tag_base = 0;
                     plan.agu_ps.tag_stride0 = 1;
                     plan.agu_ps.tag_stride1 = 1;
                     plan.agu_ps.tag_ctrl = 2;

                     plan.agu_pd.enable = true;
                     plan.agu_pd.ultra = p.ultra_mode;
                     plan.agu_pd.iter0 = static_cast<uint16_t>(count_ic_pack);
                     plan.agu_pd.iter1 = static_cast<uint16_t>(ih_end - ih_start);
                     plan.agu_pd.iter2 = static_cast<uint16_t>(p.in_width);
                     plan.agu_pd.iter3 = 1;
                     plan.agu_pd.stride0 = 1;
                     plan.agu_pd.stride1 = static_cast<int32_t>(p.in_width * in_ch_pack);
                     plan.agu_pd.stride2 = static_cast<int32_t>(in_ch_pack);
                     plan.agu_pd.stride3 = 0;
                     uint32_t pd_offset = static_cast<uint32_t>(ih_start) * p.in_width * in_ch_pack + static_cast<uint32_t>(ic_pack_start);
                     plan.agu_pd.base_addr = pd_local_base + pd_offset;
                     plan.agu_pd.tag_base = 0;
                     plan.agu_pd.tag_stride0 = 1;
                     plan.agu_pd.tag_stride1 = 1;
                     plan.agu_pd.tag_ctrl = 1;

                     plan.agu_pli.ultra = p.ultra_mode;
                     plan.agu_pli.base_addr = pli_local_base;
                     plan.agu_pli.iter0 = static_cast<uint16_t>(count_oc_pack);
                     plan.agu_pli.iter1 = static_cast<uint16_t>(oh_r.second - oh_r.first);
                     plan.agu_pli.iter2 = static_cast<uint16_t>(p.out_width);
                     plan.agu_pli.iter3 = 1;
                     plan.agu_pli.stride0 = 1;
                     plan.agu_pli.stride1 = static_cast<int32_t>(p.out_width * count_oc_pack);
                     plan.agu_pli.stride2 = static_cast<int32_t>(count_oc_pack);
                     plan.agu_pli.stride3 = 0;
                     plan.agu_pli.tag_base = 0;
                     plan.agu_pli.tag_stride0 = 1;
                     plan.agu_pli.tag_stride1 = 1;
                     plan.agu_pli.tag_ctrl = 1;

                     plan.agu_plo.ultra = p.ultra_mode;
                     plan.agu_plo.base_addr = plo_local_base;
                     plan.agu_plo.iter0 = static_cast<uint16_t>(count_oc_pack);
                     plan.agu_plo.iter1 = static_cast<uint16_t>(oh_r.second - oh_r.first);
                     plan.agu_plo.iter2 = static_cast<uint16_t>(p.out_width);
                     plan.agu_plo.iter3 = 1;
                     plan.agu_plo.stride0 = 1;
                     plan.agu_plo.stride1 = static_cast<int32_t>(p.out_width * count_oc_pack);
                     plan.agu_plo.stride2 = static_cast<int32_t>(count_oc_pack);
                     plan.agu_plo.stride3 = 0;
                     plan.agu_plo.tag_base = 0;
                     plan.agu_plo.tag_stride0 = 1;
                     plan.agu_plo.tag_stride1 = 1;
                     plan.agu_plo.tag_ctrl = 1;

                     plan.agu_pli.enable = (wic == 0);
                     plan.agu_plo.enable = (wic == waves_ic - 1);

                     plans.push_back(plan);
                }
            }
        }
        return plans;
    }
};
