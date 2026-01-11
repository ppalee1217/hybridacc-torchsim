#pragma once

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <map>

// Helper: Read binary file into vector
template<typename T>
std::vector<T> read_binary_file(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << "[Error] Cannot open file: " << filepath << std::endl;
        exit(1);
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<T> buffer(size / sizeof(T));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "[Error] Cannot read file: " << filepath << std::endl;
        exit(1);
    }
    return buffer;
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
            std::string key = line.substr(0, delimiter_pos);
            std::string value = line.substr(delimiter_pos + 1);
            config[key] = value;
        }
    }
    return config;
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
