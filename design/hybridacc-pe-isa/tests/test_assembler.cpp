#include "../src/assambler/instruction.hpp"
#include <cassert>
#include <iostream>
using namespace hybridacc;

int main(){
    Assembler a; Disassembler d;

    // Test 1: basic size
    auto w1 = a.assemble("NOP\nHALT\n", false);
    assert(w1.size()==2);
    assert(d.disasmWord(w1[0]).find("NOP")!=std::string::npos);
    assert(d.disasmWord(w1[1]).find("HALT")!=std::string::npos);

    // Test 2: label + jump
    auto w2 = a.assemble("start:\nNOP\nJ start\nHALT\n", false);
    assert(w2.size()==3);
    std::string dj = d.disasmWord(w2[1]);
    assert(dj.find("J 0")!=std::string::npos); // jump to index 0

    // Test 3: LOOPEND pseudo
    auto w3 = a.assemble("VMAC P1, VT2\nLOOPEND\n", false);
    assert(w3.size()==1);
    assert((w3[0] & 1)==1);

    // Test 4: TSHIFT K5
    auto w4 = a.assemble("TSHIFT K5\n", false);
    assert(w4.size()==1);
    // kernel size code 1 at bits [12:10]
    assert( ((w4[0]>>10)&0x7) == 1 );

    // Test 5: VMACN func1 bit set
    auto w5 = a.assemble("VMACN P3, VT1\n", false);
    assert(w5.size()==1);
    assert( ((w5[0]>>12)&1) == 1 );

    // Test 6: SWAPDM
    auto w6 = a.assemble("SWAPDM\n", false);
    assert(w6.size()==1);
    // Opcode=3 (bits 1-2), Funct2=3 (bits 3-4), Func3=4 (bits 13-15)
    // 0x801E or similar check
    uint16_t sw = w6[0];
    assert( ((sw>>1)&0x3) == 3 ); // opcode
    assert( ((sw>>3)&0x3) == 3 ); // funct2
    assert( ((sw>>13)&0x7) == 4); // func3

    // Negative tests (error handling)
    auto expectError = [&](const char* name, const std::string &src, const char* mustContain){
        bool thrown=false;
        try {
            a.assemble(src, false);
        } catch(const std::exception &e){
            thrown=true;
            std::cerr << "[DEBUG] " << name << " got error: " << e.what() << "\n";
            if(mustContain && std::string(mustContain).size()) {
                auto msg = std::string(e.what());
                if(msg.find(mustContain)==std::string::npos){
                    std::cerr << "[DEBUG] Expect substring: '"<<mustContain<<"' NOT FOUND in: '"<<msg<<"'\n";
                }
                assert(msg.find(mustContain)!=std::string::npos);
            }
        }
        if(!thrown){
            std::cerr<<"Expected error not thrown: "<<name<<"\n"; assert(false);
        }
    };

    expectError("J unaligned","J 3\n", "even");                // J immediate must be even
    expectError("Undefined label","J target\nNOP\n", "Undefined label");
    expectError("vtstride overflow","VMACR 0, 4\n", "vtstride out of range");
    expectError("LDMA.LEN range","LDMA.LEN 3000\n", "len out of range");
    expectError("Duplicate label","L1:\nL1:\nNOP\n", "Duplicate label");

    std::cout<<"All error tests passed.\n";

    std::cout<<"All tests passed.\n";
    return 0;
}
