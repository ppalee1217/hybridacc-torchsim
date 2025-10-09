#ifndef HYBRIDACC_ARRAY_ARRAY_HPP
#define HYBRIDACC_ARRAY_ARRAY_HPP
// ============================================================================
//  File        : array.hpp
//  Module      : PEArray
//  Description : 包含多個 PE 之集合抽象 (單一 2D Grid)。
// ============================================================================
#include <vector>
#include <memory>
#include <systemc.h>
#include "hybridacc/pe/pe.hpp"

namespace hybridacc {

struct PEArrayDesc {
    unsigned rows{1};
    unsigned cols{1};
};

class PEArray : public sc_core::sc_module {
public:
    SC_HAS_PROCESS(PEArray);
    PEArray(sc_core::sc_module_name name, const PEArrayDesc& d);

    unsigned rows() const { return m_desc.rows; }
    unsigned cols() const { return m_desc.cols; }
    PE* at(unsigned r, unsigned c) const; // 簡單索引 (未檢查越界)

private:
    PEArrayDesc m_desc{};
    std::vector<std::unique_ptr<PE>> m_pes; // size = rows*cols
};

} // namespace hybridacc
#endif // HYBRIDACC_ARRAY_ARRAY_HPP
