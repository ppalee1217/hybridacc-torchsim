#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <sstream>
#include <variant>
#include <memory>
#include <array>
#include <unordered_map>
#include <iomanip>
#include <limits>

// -----------------------------------------------------------------------------
// Simple JSON Parser (C++17)
// -----------------------------------------------------------------------------

struct JsonValue;

using JsonObject = std::map<std::string, std::shared_ptr<JsonValue>>;
using JsonArray = std::vector<std::shared_ptr<JsonValue>>;

enum class JsonType { Null, Bool, Number, String, Array, Object };

struct JsonValue {
    JsonType type = JsonType::Null;
    bool bool_val = false;
    double num_val = 0.0;
    std::string str_val;
    JsonArray arr_val;
    JsonObject obj_val;

    bool is_null() const { return type == JsonType::Null; }
    bool is_bool() const { return type == JsonType::Bool; }
    bool is_number() const { return type == JsonType::Number; }
    bool is_string() const { return type == JsonType::String; }
    bool is_array() const { return type == JsonType::Array; }
    bool is_object() const { return type == JsonType::Object; }

    bool as_bool() const { return bool_val; }
    double as_double() const { return num_val; }
    int as_int() const { return static_cast<int>(num_val); }
    int64_t as_int64() const { return static_cast<int64_t>(num_val); }
    const std::string& as_string() const { return str_val; }
    const JsonArray& as_array() const { return arr_val; }
    const JsonObject& as_object() const { return obj_val; }

    std::shared_ptr<JsonValue> operator[](const std::string& key) const {
        if (type != JsonType::Object) return nullptr;
        auto it = obj_val.find(key);
        return (it != obj_val.end()) ? it->second : nullptr;
    }

    std::shared_ptr<JsonValue> operator[](size_t index) const {
        if (type != JsonType::Array || index >= arr_val.size()) return nullptr;
        return arr_val[index];
    }
};

class JsonParser {
public:
    static std::shared_ptr<JsonValue> parse(const std::string& json) {
        size_t pos = 0;
        return parse_value(json, pos);
    }

    static std::shared_ptr<JsonValue> parse_file(const std::string& filepath) {
        std::ifstream f(filepath);
        if (!f.is_open()) return nullptr;
        std::stringstream buffer;
        buffer << f.rdbuf();
        return parse(buffer.str());
    }

private:
    static void skip_whitespace(const std::string& json, size_t& pos) {
        while (pos < json.size() && std::isspace(json[pos])) pos++;
    }

    static std::shared_ptr<JsonValue> parse_value(const std::string& json, size_t& pos) {
        skip_whitespace(json, pos);
        if (pos >= json.size()) return nullptr;

        char c = json[pos];
        if (c == '{') return parse_object(json, pos);
        if (c == '[') return parse_array(json, pos);
        if (c == '"') return parse_string(json, pos);
        if (c == 't' || c == 'f') return parse_bool(json, pos);
        if (c == 'n') return parse_null(json, pos);
        if (c == '-' || std::isdigit(c)) return parse_number(json, pos);

        return nullptr;
    }

    static std::shared_ptr<JsonValue> parse_object(const std::string& json, size_t& pos) {
        auto val = std::make_shared<JsonValue>();
        val->type = JsonType::Object;
        pos++; // skip '{'

        while (pos < json.size()) {
            skip_whitespace(json, pos);
            if (json[pos] == '}') {
                pos++;
                return val;
            }

            auto key_val = parse_string(json, pos);
            if (!key_val) return nullptr;
            std::string key = key_val->as_string();

            skip_whitespace(json, pos);
            if (pos >= json.size() || json[pos] != ':') return nullptr;
            pos++; // skip ':'

            auto member_val = parse_value(json, pos);
            if (!member_val) return nullptr;
            val->obj_val[key] = member_val;

            skip_whitespace(json, pos);
            if (json[pos] == ',') pos++;
        }
        return nullptr;
    }

    static std::shared_ptr<JsonValue> parse_array(const std::string& json, size_t& pos) {
        auto val = std::make_shared<JsonValue>();
        val->type = JsonType::Array;
        pos++; // skip '['

        while (pos < json.size()) {
            skip_whitespace(json, pos);
            if (json[pos] == ']') {
                pos++;
                return val;
            }

            auto elem = parse_value(json, pos);
            if (!elem) return nullptr;
            val->arr_val.push_back(elem);

            skip_whitespace(json, pos);
            if (json[pos] == ',') pos++;
        }
        return nullptr;
    }

    static std::shared_ptr<JsonValue> parse_string(const std::string& json, size_t& pos) {
        auto val = std::make_shared<JsonValue>();
        val->type = JsonType::String;
        pos++; // skip '"'

        std::string s;
        while (pos < json.size()) {
            char c = json[pos++];
            if (c == '"') {
                val->str_val = s;
                return val;
            }
            if (c == '\\') {
                if (pos >= json.size()) return nullptr;
                char next = json[pos++];
                if (next == '"') s += '"';
                else if (next == '\\') s += '\\';
                else if (next == '/') s += '/';
                else if (next == 'b') s += '\b';
                else if (next == 'f') s += '\f';
                else if (next == 'n') s += '\n';
                else if (next == 'r') s += '\r';
                else if (next == 't') s += '\t';
                else s += next; // Not handling unicode \uXXXX
            } else {
                s += c;
            }
        }
        return nullptr;
    }

    static std::shared_ptr<JsonValue> parse_number(const std::string& json, size_t& pos) {
        auto val = std::make_shared<JsonValue>();
        val->type = JsonType::Number;
        size_t start = pos;
        if (json[pos] == '-') pos++;
        while (pos < json.size() && std::isdigit(json[pos])) pos++;
        if (pos < json.size() && json[pos] == '.') {
            pos++;
            while (pos < json.size() && std::isdigit(json[pos])) pos++;
        }
        if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E')) {
            pos++;
            if (pos < json.size() && (json[pos] == '+' || json[pos] == '-')) pos++;
            while (pos < json.size() && std::isdigit(json[pos])) pos++;
        }
        try {
            val->num_val = std::stod(json.substr(start, pos - start));
        } catch (...) { return nullptr; }
        return val;
    }

    static std::shared_ptr<JsonValue> parse_bool(const std::string& json, size_t& pos) {
        auto val = std::make_shared<JsonValue>();
        val->type = JsonType::Bool;
        if (json.compare(pos, 4, "true") == 0) {
            val->bool_val = true;
            pos += 4;
        } else if (json.compare(pos, 5, "false") == 0) {
            val->bool_val = false;
            pos += 5;
        } else {
            return nullptr;
        }
        return val;
    }

    static std::shared_ptr<JsonValue> parse_null(const std::string& json, size_t& pos) {
        auto val = std::make_shared<JsonValue>();
        val->type = JsonType::Null;
        if (json.compare(pos, 4, "null") == 0) {
            pos += 4;
            return val;
        }
        return nullptr;
    }
};

// Verbose logging control
static bool verbose_logging_enabled = false;
inline void enable_verbose_logging(bool enable) {
    verbose_logging_enabled = enable;
}
#define VERBOSE_LOG(msg) \
    do { \
        if (verbose_logging_enabled) { \
            std::cout << msg << std::endl; \
        } \
    } while (0)

// Helper: Read binary file into vector
template<typename T>
std::vector<T> read_binary_file(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[Error] Cannot open file: " << filepath << std::endl;
        std::exit(1);
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<T> buffer(size / sizeof(T));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "[Error] Cannot read file: " << filepath << std::endl;
        std::exit(1);
    }
    return buffer;
}

inline std::string trim_copy(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [&](unsigned char c) { return !is_ws(c); }));
    s.erase(std::find_if(s.rbegin(), s.rend(), [&](unsigned char c) { return !is_ws(c); }).base(), s.end());
    return s;
}

// Helper: Parse config.txt
inline std::map<std::string, std::string> read_config_file(const std::string& filepath) {
    std::map<std::string, std::string> config;
    std::ifstream file(filepath);
    if (!file) return config;

    std::string line;
    while (std::getline(file, line)) {
        size_t delimiter_pos = line.find(':');
        if (delimiter_pos != std::string::npos) {
            std::string key = trim_copy(line.substr(0, delimiter_pos));
            std::string value = trim_copy(line.substr(delimiter_pos + 1));
            if (!key.empty()) {
                config[key] = value;
            }
        }
    }
    return config;
}

inline std::vector<int> parse_int_list(const std::string& str) {
    std::vector<int> result;
    std::string s = str;
    // Remove brackets if present
    if (!s.empty() && s.front() == '[') s.erase(0, 1);
    if (!s.empty() && s.back() == ']') s.erase(s.size() - 1);

    std::stringstream ss(s);
    std::string segment;
    while(std::getline(ss, segment, ',')) {
        segment = trim_copy(segment);
        if(!segment.empty()) {
            try {
                result.push_back(std::stoi(segment));
            } catch (...) {}
        }
    }
    return result;
}

inline bool parse_bool(const std::string& s) {
    std::string val = s;
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    return (val == "true" || val == "1" || val == "yes");
}

// Helper function to convert fp16 to float
float fp16_to_float(uint16_t fp16_val) {
    uint32_t sign = (fp16_val >> 15) & 0x1;
    uint32_t exponent = (fp16_val >> 10) & 0x1F;
    uint32_t fraction = fp16_val & 0x3FF;

    if (exponent == 0 && fraction == 0) {
        return sign ? -0.0f : 0.0f; // Zero
    } else if (exponent == 0x1F) {
        if (fraction == 0) {
            return sign ? -INFINITY : INFINITY; // Infinity
        } else {
            return NAN; // NaN
        }
    }

    // Normalize exponent
    int32_t exp_unbiased = static_cast<int32_t>(exponent) - 15 + 127; // Adjust bias from 15 to 127

    // Normalize fraction
    uint32_t mantissa = fraction << 13; // Shift to align with float mantissa

    uint32_t float_bits = (sign << 31) | (exp_unbiased << 23) | mantissa;
    float result;
    std::memcpy(&result, &float_bits, sizeof(float));
    return result;
}

struct VerifyStats {
    size_t total_elements = 0;
    size_t mismatches = 0;
    double cosine_similarity = 0.0;
    double max_diff = 0.0;
    double mse = 0.0;
};

std::ostream& operator<<(std::ostream& os, const VerifyStats& stats) {
    const int n = 30;
    os << "\n";
    for(int i = 0; i < n*2 + 2; ++i) os << "=";
    os << "\n";
    os << std::setw(n) << "|   _   _       _          _     _    _" << std::endl;
    os << std::setw(n) << "|  | | | |_   _| |__  _ __(_) __| |  / \\   ___ ___" << std::endl;
    os << std::setw(n) << "|  | |_| | | | | '_ \\| '__| |/ _` | / _ \\ / __/ __|" << std::endl;
    os << std::setw(n) << "|  |  _  | |_| | |_) | |  | | (_| |/ ___ \\ (_| (__" << std::endl;
    os << std::setw(n) << "|  |_| |_|\\__, |_.__/|_|  |_|\\__,_/_/   \\_\\___\\___|" << std::endl;
    os << std::setw(n) << "|  __     |___/     _  __       ____  _        _" << std::endl;
    os << std::setw(n) << "|  \\ \\   / /__ _ __(_)/ _|_   _/ ___|| |_ __ _| |_ ___" << std::endl;
    os << std::setw(n) << "|   \\ \\ / / _ \\ '__| | |_| | | \\___ \\| __/ _` | __/ __|" << std::endl;
    os << std::setw(n) << "|    \\ V /  __/ |  | |  _| |_| |___) | || (_| | |_\\__ \\" << std::endl;
    os << std::setw(n) << "|     \\_/ \\___|_|  |_|_|  \\__, |____/ \\__\\__,_|\\__|___/" << std::endl;
    os << "|\n";
    for(int i = 0; i < n*2 + 2; ++i) os << "=";
    os << "\n";
    os << "|  total_elements    │ " << stats.total_elements << "\n";
    os << "|  mismatches        │ " << stats.mismatches << "\n";
    os << "|  cosine_similarity │ " << stats.cosine_similarity << "\n";
    os << "|  max_diff          │ " << stats.max_diff << "\n";
    os << "|  mse               │ " << stats.mse << "\n";
    for(int i = 0; i < n*2 + 2; ++i) os << "=";
    os << "\n";
    return os;
}

inline VerifyStats verify_fp16_vectors(const std::vector<uint16_t>& expected_fp16,
                                      const std::vector<uint16_t>& received_fp16,
                                      double tolerance = 1e-2, bool verbose = false) {
    VerifyStats stats;
    stats.total_elements = expected_fp16.size();

    if (expected_fp16.size() != received_fp16.size() || expected_fp16.empty()) {
        stats.mismatches = std::max(expected_fp16.size(), received_fp16.size());
        stats.cosine_similarity = 0.0;
        stats.max_diff = std::numeric_limits<double>::infinity();
        stats.mse = std::numeric_limits<double>::infinity();
        return stats;
    }

    double dot_product = 0.0;
    double mag_recv = 0.0;
    double mag_exp = 0.0;
    double max_diff = 0.0;
    double total_sq_err = 0.0;
    size_t mismatches = 0;

    for (size_t i = 0; i < expected_fp16.size(); i++) {
        const float recv_val = fp16_to_float(received_fp16[i]);
        const float exp_val = fp16_to_float(expected_fp16[i]);
        dot_product += static_cast<double>(recv_val) * static_cast<double>(exp_val);
        mag_recv += static_cast<double>(recv_val) * static_cast<double>(recv_val);
        mag_exp += static_cast<double>(exp_val) * static_cast<double>(exp_val);
        const double diff = std::abs(static_cast<double>(recv_val) - static_cast<double>(exp_val));
        total_sq_err += diff * diff;
        if (diff > max_diff) {
            max_diff = diff;
        }
        if (diff > tolerance) {
            mismatches++;
            if (verbose) {
                std::cout << "[Mismatch] Index " << i
                          << ": Expected " << exp_val << std::hex << "(" << expected_fp16[i] << ")" << std::dec
                          << ", Received " << recv_val << std::hex << "(" << received_fp16[i] << ")" << std::dec
                          << ", Diff " << diff << std::endl;
            }
        }
    }

    mag_recv = std::sqrt(mag_recv);
    mag_exp = std::sqrt(mag_exp);

    stats.mismatches = mismatches;
    stats.max_diff = max_diff;
    stats.mse = total_sq_err / static_cast<double>(expected_fp16.size());
    stats.cosine_similarity = (mag_recv > 0.0 && mag_exp > 0.0) ? (dot_product / (mag_recv * mag_exp)) : 0.0;
    return stats;
}


// number to (K/M/G/T) string
inline std::string num_to_str(uint64_t num, uint64_t base=1024) {
    const char* suffixes[] = {"", "K", "M", "G", "T"};
    size_t suffix_index = 0;
    double value = static_cast<double>(num);

    while (value >= static_cast<double>(base) && suffix_index < 4) {
        value /= static_cast<double>(base);
        ++suffix_index;
    }

    char buffer[50];
    std::snprintf(buffer, sizeof(buffer), "%.3f %s", value, suffixes[suffix_index]);
    return std::string(buffer);
}

// -----------------------------------------------------------------------------
// Helper: Tensor index utilities (Index2D / Index3D / Index4D)
// Provide simple, constexpr-style small classes to compute flattened indices
// with row-major ordering. Improves readability over manual arithmetic.

struct Index2D {
    size_t D0, D1;
    Index2D(size_t d0, size_t d1) : D0(d0), D1(d1) {}
    inline size_t operator()(size_t i, size_t j) const { return i * D1 + j; }
};

struct Index3D {
    size_t D0, D1, D2;
    Index3D(size_t d0, size_t d1, size_t d2) : D0(d0), D1(d1), D2(d2) {}
    inline size_t operator()(size_t i, size_t j, size_t k) const { return (i * D1 + j) * D2 + k; }
};

struct Index4D {
    size_t D0, D1, D2, D3;
    Index4D(size_t d0, size_t d1, size_t d2, size_t d3) : D0(d0), D1(d1), D2(d2), D3(d3) {}
    inline size_t operator()(size_t a, size_t b, size_t c, size_t d) const { return (((a * D1 + b) * D2 + c) * D3 + d); }
};

// -----------------------------------------------------------------------------
// Cluster Simulation Testbench Utilities
// -----------------------------------------------------------------------------

namespace cluster_json {

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

inline std::ostream& operator<<(std::ostream& os, const AguCfg& cfg) {
    os << "base_addr=0x" << std::hex << cfg.base_addr << std::dec
       << " iter=[" << cfg.iter0 << "," << cfg.iter1 << "," << cfg.iter2 << "," << cfg.iter3 << "]"
       << " stride=[" << cfg.stride0 << "," << cfg.stride1 << "," << cfg.stride2 << "," << cfg.stride3 << "]"
       << " lane_cfg=0x" << std::hex << cfg.lane_cfg << std::dec
       << " tag_base=0x" << std::hex << cfg.tag_base << std::dec
       << " tag_stride=[" << cfg.tag_stride0 << "," << cfg.tag_stride1 << "]"
       << " tag_ctrl=0x" << std::hex << cfg.tag_ctrl << std::dec
       << " mask_cfg=0x" << std::hex << cfg.mask_cfg << std::dec
       << " ultra=" << cfg.ultra
       << " enable=" << cfg.enable;
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

struct SpmSectionAddr {
    uint32_t linear_addr = 0;
    uint32_t parallel_addr = 0;
};

struct DmaAddrGen4D {
    bool enabled = false;
    uint32_t base_addr = 0;
    std::array<uint32_t, 4> iter = {1, 1, 1, 1};
    std::array<int32_t, 4> stride = {0, 0, 0, 8};
};

inline std::ostream& operator<<(std::ostream& os, const DmaAddrGen4D& gen) {
    os << "AddrGen4D(enabled=" << gen.enabled
       << ", base_addr=0x" << std::hex << gen.base_addr << std::dec
       << ", iter=[" << gen.iter[0] << "," << gen.iter[1] << "," << gen.iter[2] << "," << gen.iter[3] << "]"
       << ", stride=[" << gen.stride[0] << "," << gen.stride[1] << "," << gen.stride[2] << "," << gen.stride[3] << "]"
       << ")";
    return os;
}

struct DmaTransferCfg {
    enum class Direction {
        DramToSpm,
        SpmToDram,
    };

    std::string tensor;
    int group_id = -1;
    std::string section;
    Direction direction = Direction::DramToSpm;
    uint32_t src_dram_addr = 0;
    uint32_t dst_spm_addr = 0;
    uint32_t src_spm_addr = 0;
    uint32_t src_parallel_spm_addr = 0;
    uint32_t dst_dram_addr = 0;
    uint32_t dst_parallel_spm_addr = 0;
    uint32_t size_words64 = 0;
    DmaAddrGen4D src_addr_gen;
    DmaAddrGen4D dst_addr_gen;
};

struct DmaWaveCfg {
    uint32_t wave_id = 0;
    int compute_plan_idx = -1;
    uint32_t buf_sel = 0;
    uint8_t spm_map_val = 0xE4;
    bool has_spm_map = false;
    std::string spm_map_reason;
    std::vector<DmaTransferCfg> transfers;
};

inline std::string format_json(const std::shared_ptr<JsonValue>& v) {
    if (!v || v->is_null()) return "null";
    if (v->is_string()) return v->as_string();
    if (v->is_number()) return std::to_string(v->as_int());
    if (v->is_bool()) return v->as_bool() ? "true" : "false";
    if (v->is_array()) {
        std::stringstream ss;
        ss << "[";
        const auto& arr = v->as_array();
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i > 0) ss << ",";
            ss << format_json(arr[i]);
        }
        ss << "]";
        return ss.str();
    }
    if (v->is_object()) {
        std::stringstream ss;
        ss << "{";
        size_t count = 0;
        for (const auto& [key, val] : v->as_object()) {
            if (count++ > 0) ss << ",";
            ss << key << ":" << format_json(val);
        }
        ss << "}";
        return ss.str();
    }
    return "?";
}

inline bool json_to_bool(const std::shared_ptr<JsonValue>& node, bool default_value = false) {
    if (!node) return default_value;
    if (node->is_bool()) return node->as_bool();
    return node->as_int64() != 0;
}

inline bool parse_u32_array4(const std::shared_ptr<JsonValue>& node, std::array<uint32_t, 4>& out) {
    if (!node || !node->is_array()) return false;
    const auto& arr = node->as_array();
    if (arr.size() != 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        if (!arr[i]) return false;
        out[i] = static_cast<uint32_t>(arr[i]->as_int64());
    }
    return true;
}

inline bool parse_i32_array4(const std::shared_ptr<JsonValue>& node, std::array<int32_t, 4>& out) {
    if (!node || !node->is_array()) return false;
    const auto& arr = node->as_array();
    if (arr.size() != 4) return false;
    for (size_t i = 0; i < 4; ++i) {
        if (!arr[i]) return false;
        out[i] = static_cast<int32_t>(arr[i]->as_int64());
    }
    return true;
}

inline bool parse_addr_gen4d(const std::shared_ptr<JsonValue>& node, DmaAddrGen4D& out) {
    if (!node || !node->is_object()) return false;
    auto iter_v = (*node)["iter"];
    auto stride_v = (*node)["stride"];
    if (!iter_v || !stride_v) return false;

    std::array<uint32_t, 4> iter = {1, 1, 1, 1};
    std::array<int32_t, 4> stride = {0, 0, 0, 8};
    if (!parse_u32_array4(iter_v, iter) || !parse_i32_array4(stride_v, stride)) return false;

    out.enabled = true;
    out.base_addr = (*node)["base_addr"] ? static_cast<uint32_t>((*node)["base_addr"]->as_int64()) : 0;
    out.iter = iter;
    out.stride = stride;
    return true;
}

inline std::vector<uint32_t> build_dma_addr_list(const DmaAddrGen4D& gen,
                                                 uint32_t fallback_base,
                                                 uint32_t word_count) {
    constexpr uint32_t kWordBytes = 8;
    std::vector<uint32_t> out;
    out.reserve(word_count);

    if (!gen.enabled) {
        for (uint32_t i = 0; i < word_count; ++i) {
            out.push_back(fallback_base + i * kWordBytes);
        }
        return out;
    }

    for (uint32_t i0 = 0; i0 < gen.iter[0] && out.size() < word_count; ++i0) {
        for (uint32_t i1 = 0; i1 < gen.iter[1] && out.size() < word_count; ++i1) {
            for (uint32_t i2 = 0; i2 < gen.iter[2] && out.size() < word_count; ++i2) {
                for (uint32_t i3 = 0; i3 < gen.iter[3] && out.size() < word_count; ++i3) {
                    const int64_t addr_i64 = static_cast<int64_t>(gen.base_addr)
                        + static_cast<int64_t>(i0) * static_cast<int64_t>(gen.stride[0])
                        + static_cast<int64_t>(i1) * static_cast<int64_t>(gen.stride[1])
                        + static_cast<int64_t>(i2) * static_cast<int64_t>(gen.stride[2])
                        + static_cast<int64_t>(i3) * static_cast<int64_t>(gen.stride[3]);
                    out.push_back(static_cast<uint32_t>(addr_i64));
                }
            }
        }
    }

    if (out.size() < word_count) {
        for (uint32_t i = static_cast<uint32_t>(out.size()); i < word_count; ++i) {
            out.push_back(fallback_base + i * kWordBytes);
        }
    }
    return out;
}

inline bool parse_agu_cfg(const std::shared_ptr<JsonValue>& node, AguCfg& out) {
    if (!node || !node->is_object()) return false;
    auto read_u32 = [&](const std::string& key, uint32_t& dst) {
        auto v = (*node)[key];
        if (v) dst = static_cast<uint32_t>(v->as_int64());
    };
    auto read_u16 = [&](const std::string& key, uint16_t& dst) {
        auto v = (*node)[key];
        if (v) dst = static_cast<uint16_t>(v->as_int64());
    };
    auto read_i32 = [&](const std::string& key, int32_t& dst) {
        auto v = (*node)[key];
        if (v) dst = static_cast<int32_t>(v->as_int64());
    };

    read_u32("base_addr", out.base_addr);
    read_u32("base_addr_h", out.base_addr_h);
    read_u16("iter0", out.iter0);
    read_u16("iter1", out.iter1);
    read_u16("iter2", out.iter2);
    read_u16("iter3", out.iter3);
    read_i32("stride0", out.stride0);
    read_i32("stride1", out.stride1);
    read_i32("stride2", out.stride2);
    read_i32("stride3", out.stride3);
    read_u32("lane_cfg", out.lane_cfg);
    read_u32("tag_base", out.tag_base);
    read_u32("tag_stride0", out.tag_stride0);
    read_u32("tag_stride1", out.tag_stride1);
    read_u32("tag_ctrl", out.tag_ctrl);
    read_u32("mask_cfg", out.mask_cfg);
    out.ultra = json_to_bool((*node)["ultra"], out.ultra);
    out.enable = json_to_bool((*node)["enable"], out.enable);
    return true;
}

inline std::vector<ClusterPlan> parse_cluster_plans(const std::shared_ptr<JsonValue>& software) {
    std::vector<ClusterPlan> plans;
    auto plan_arr = (*software)["cluster_plans"];
    if (!plan_arr || !plan_arr->is_array()) return plans;

    for (const auto& p : plan_arr->as_array()) {
        if (!p || !p->is_object()) continue;
        ClusterPlan plan;
        auto name = (*p)["name"];
        if (name && name->is_string()) plan.name = name->as_string();
        auto global_mask = (*p)["global_mask"];
        if (global_mask) plan.global_mask = static_cast<uint32_t>(global_mask->as_int64());
        plan.ultra_mode = json_to_bool((*p)["ultra_mode"], false);
        parse_agu_cfg((*p)["agu_ps"], plan.agu_ps);
        parse_agu_cfg((*p)["agu_pd"], plan.agu_pd);
        parse_agu_cfg((*p)["agu_pli"], plan.agu_pli);
        parse_agu_cfg((*p)["agu_plo"], plan.agu_plo);
        plans.push_back(std::move(plan));
    }
    return plans;
}

inline std::unordered_map<std::string, SpmSectionAddr> parse_spm_sections(const std::shared_ptr<JsonValue>& software) {
    std::unordered_map<std::string, SpmSectionAddr> out;
    auto spm = (*software)["spm"];
    if (!spm) return out;

    auto groups = (*spm)["groups"];
    if (groups && groups->is_array()) {
        for (const auto& g : groups->as_array()) {
            if (!g || !g->is_object()) continue;
            auto sections = (*g)["sections"];
            if (!sections || !sections->is_array()) continue;
            for (const auto& s : sections->as_array()) {
                if (!s || !s->is_object() || !(*s)["name"]) continue;
                const std::string name = (*s)["name"]->as_string();
                SpmSectionAddr addr;
                if ((*s)["global_linear_addr"]) addr.linear_addr = static_cast<uint32_t>((*s)["global_linear_addr"]->as_int64());
                if ((*s)["global_parallel_addr"]) addr.parallel_addr = static_cast<uint32_t>((*s)["global_parallel_addr"]->as_int64());
                if ((*s)["linear_spm_addr"]) addr.linear_addr = static_cast<uint32_t>((*s)["linear_spm_addr"]->as_int64());
                if ((*s)["parallel_spm_addr"]) addr.parallel_addr = static_cast<uint32_t>((*s)["parallel_spm_addr"]->as_int64());
                if ((*s)["spm_addr"]) addr.linear_addr = static_cast<uint32_t>((*s)["spm_addr"]->as_int64());
                out[name] = addr;
            }
        }
    }
    return out;
}

inline uint32_t get_spm_tensor_addr_or_default(const std::shared_ptr<JsonValue>& software,
                                               const std::string& tensor_name,
                                               uint32_t default_addr,
                                               bool prefer_parallel) {
    auto spm = (*software)["spm"];
    if (!spm) return default_addr;
    auto tensor_mapping = (*spm)["tensor_mapping"];
    if (!tensor_mapping) return default_addr;
    auto t = (*tensor_mapping)[tensor_name];
    if (!t) return default_addr;

    bool has_linear = false;
    bool has_parallel = false;
    uint32_t linear_addr = 0;
    uint32_t parallel_addr = 0;

    if ((*t)["linear_spm_addr"]) {
        has_linear = true;
        linear_addr = static_cast<uint32_t>((*t)["linear_spm_addr"]->as_int64());
    }
    if ((*t)["parallel_spm_addr"]) {
        has_parallel = true;
        parallel_addr = static_cast<uint32_t>((*t)["parallel_spm_addr"]->as_int64());
    }

    if ((*t)["spm_addr"]) {
        const uint32_t v = static_cast<uint32_t>((*t)["spm_addr"]->as_int64());
        auto mode_v = (*t)["spm_mode"];
        const std::string mode = (mode_v && mode_v->is_string()) ? mode_v->as_string() : "";
        if (mode == "parallel") {
            has_parallel = true;
            parallel_addr = v;
        } else {
            has_linear = true;
            linear_addr = v;
        }
    }

    if (prefer_parallel) {
        if (has_parallel) return parallel_addr;
        if (has_linear) return linear_addr;
    } else {
        if (has_linear) return linear_addr;
        if (has_parallel) return parallel_addr;
    }
    return default_addr;
}

inline std::vector<DmaWaveCfg> parse_dma_waves(
    const std::shared_ptr<JsonValue>& software,
    const std::unordered_map<std::string, SpmSectionAddr>& section_addr_map) {
    std::vector<DmaWaveCfg> waves;
    auto dma = (*software)["dma"];
    if (!dma) return waves;

    auto wave_arr = (*dma)["waves"];
    if (!wave_arr || !wave_arr->is_array()) return waves;

    for (const auto& w : wave_arr->as_array()) {
        if (!w || !w->is_object()) continue;
        DmaWaveCfg wave;
        if ((*w)["wave_id"]) wave.wave_id = static_cast<uint32_t>((*w)["wave_id"]->as_int());
        if ((*w)["buf_sel"]) wave.buf_sel = static_cast<uint32_t>((*w)["buf_sel"]->as_int());

        auto spm_map = (*w)["spm_map"];
        if (spm_map && spm_map->is_object()) {
            if ((*spm_map)["map_val"]) {
                wave.spm_map_val = static_cast<uint8_t>((*spm_map)["map_val"]->as_int64() & 0xFF);
                wave.has_spm_map = true;
            }
            if ((*spm_map)["reason"] && (*spm_map)["reason"]->is_string()) {
                wave.spm_map_reason = (*spm_map)["reason"]->as_string();
            }
        }

        if ((*w)["spm_map_val"]) {
            wave.spm_map_val = static_cast<uint8_t>((*w)["spm_map_val"]->as_int64() & 0xFF);
            wave.has_spm_map = true;
        }

        auto sync = (*w)["sync"];
        if (sync && (*sync)["compute_plan_idx"]) wave.compute_plan_idx = (*sync)["compute_plan_idx"]->as_int();

        auto transfers = (*w)["transfers"];
        if (transfers && transfers->is_array()) {
            for (const auto& t : transfers->as_array()) {
                if (!t || !t->is_object()) continue;
                DmaTransferCfg cfg;
                if ((*t)["tensor"]) cfg.tensor = (*t)["tensor"]->as_string();
                if ((*t)["group_id"]) cfg.group_id = (*t)["group_id"]->as_int();
                if ((*t)["section"]) cfg.section = (*t)["section"]->as_string();

                auto dir = (*t)["direction"];
                const std::string dir_s = (dir && dir->is_string()) ? dir->as_string() : "dram_to_spm";
                cfg.direction = (dir_s == "spm_to_dram")
                    ? DmaTransferCfg::Direction::SpmToDram
                    : DmaTransferCfg::Direction::DramToSpm;

                if ((*t)["src_dram_addr"]) cfg.src_dram_addr = static_cast<uint32_t>((*t)["src_dram_addr"]->as_int64());
                if ((*t)["dst_spm_addr"]) cfg.dst_spm_addr = static_cast<uint32_t>((*t)["dst_spm_addr"]->as_int64());
                if ((*t)["src_spm_addr"]) cfg.src_spm_addr = static_cast<uint32_t>((*t)["src_spm_addr"]->as_int64());
                if ((*t)["dst_dram_addr"]) cfg.dst_dram_addr = static_cast<uint32_t>((*t)["dst_dram_addr"]->as_int64());
                if ((*t)["src_parallel_spm_addr"]) cfg.src_parallel_spm_addr = static_cast<uint32_t>((*t)["src_parallel_spm_addr"]->as_int64());
                if ((*t)["dst_parallel_spm_addr"]) cfg.dst_parallel_spm_addr = static_cast<uint32_t>((*t)["dst_parallel_spm_addr"]->as_int64());

                if (cfg.dst_spm_addr == 0 && !cfg.section.empty()) {
                    auto it = section_addr_map.find(cfg.section);
                    if (it != section_addr_map.end()) {
                        cfg.dst_spm_addr = it->second.linear_addr;
                        cfg.dst_parallel_spm_addr = it->second.parallel_addr;
                    }
                }
                if (cfg.src_spm_addr == 0 && !cfg.section.empty()) {
                    auto it = section_addr_map.find(cfg.section);
                    if (it != section_addr_map.end()) {
                        cfg.src_spm_addr = it->second.linear_addr;
                        cfg.src_parallel_spm_addr = it->second.parallel_addr;
                    }
                }

                if ((*t)["size_words64"]) cfg.size_words64 = static_cast<uint32_t>((*t)["size_words64"]->as_int64());
                if ((*t)["word_count"]) cfg.size_words64 = static_cast<uint32_t>((*t)["word_count"]->as_int64());

                auto src_gen = (*t)["src_addr_gen"];
                if (src_gen) parse_addr_gen4d(src_gen, cfg.src_addr_gen);
                auto dst_gen = (*t)["dst_addr_gen"];
                if (dst_gen) parse_addr_gen4d(dst_gen, cfg.dst_addr_gen);

                wave.transfers.push_back(std::move(cfg));
            }
        }
        waves.push_back(std::move(wave));
    }
    return waves;
}

} // namespace cluster_json
