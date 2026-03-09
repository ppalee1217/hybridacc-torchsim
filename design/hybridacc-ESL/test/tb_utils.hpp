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
// Minimal JSON Parser for configuration
