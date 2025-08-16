#pragma once
#include <string>
#include <vector>

#define FP16_ZERO 0x0000
#define FP16_ONE  0x3C00
#define FP16_INFINITY 0x7C00

namespace hybridacc {
// I/O 公用函式：讀寫 16-bit 指令字序列 (host endian)
// 發生錯誤時拋出 AsmError (定義於 instruction.hpp)
class AsmError; // 前向宣告 (實作在 instruction.hpp)

void writeBin(const std::string &path, const std::vector<uint16_t> &words);
void writeHex(const std::string &path, const std::vector<uint16_t> &words);
std::vector<uint16_t> readBin(const std::string &path);
std::vector<uint16_t> readHex(const std::string &path);
// 新增：讀取整份組合語言原始碼為字串
std::string readAsm(const std::string &path);
}
