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