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

    // Test 2: hardware loop (ISA v3 removed J; control flow is LOOPIN/LOOPEND)
    auto w2 = a.assemble("LOOPIN 4\nNOP\nLOOPEND\nHALT\n", false);
    assert(w2.size()==3);
    assert( ((w2[0]>>1)&0x3) == 2 );      // opcode
    assert( ((w2[0]>>3)&0x3) == 0 );      // func2 = LOOPIN
    assert( ((w2[0]>>6)&0x3FF) == 3 );    // payload = count-1
    assert( (w2[1]&1) == 1 );              // LOOPEND marks the NOP
    assert(d.disasmWord(w2[0]).find("LOOPIN 4")!=std::string::npos);

    // Test 3: LOOPEND pseudo
    auto w3 = a.assemble("VMAC P1, VT2\nLOOPEND\n", false);
    assert(w3.size()==1);
    assert((w3[0] & 1)==1);

    // Test 4: TSHIFT K5
    auto w4 = a.assemble("TSHIFT K5\n", false);
    assert(w4.size()==1);
    // ISA v3: kernel_size (K3/K5/K7 -> 0/1/2) at bits [10:9]
    assert( ((w4[0]>>9)&0x3) == 1 );

    // Test 5: VMACN func1 bit set (ISA v3: func1 is bit 5)
    auto w5 = a.assemble("VMACN P3, VT1\n", false);
    assert(w5.size()==1);
    assert( ((w5[0]>>5)&1) == 1 );
    assert( ((w3[0]>>5)&1) == 0 );         // VMAC leaves func1 clear

    // Test 6: SWAPDM (ISA v3: a SYS.SYNC flag, no standalone instruction)
    auto w6 = a.assemble("SYS.SYNC (SWAPDM)\n", false);
    assert(w6.size()==1);
    uint16_t sw = w6[0];
    assert( ((sw>>1)&0x3) == 2 ); // opcode
    assert( ((sw>>3)&0x3) == 1 ); // func2
    assert( ((sw>>5)&0x1) == 1 ); // func1 = SYS.SYNC
    assert( ((sw>>6)&0x1) == 1 ); // payload[0] = SWAPDM

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

    // ISA v3 removed J and the standalone SWAPDM; no v3 instruction takes a label operand.
    expectError("J removed","J 0\n", "Unknown mnemonic");
    expectError("SWAPDM removed","SWAPDM\n", "Unknown mnemonic");
    expectError("vtstride overflow","VMACR 0, 4\n", "vtstride out of range");
    expectError("LDMA.LEN range","LDMA.LEN 3000\n", "len out of range");
    expectError("Duplicate label","L1:\nL1:\nNOP\n", "Duplicate label");

    std::cout<<"All error tests passed.\n";

    std::cout<<"All tests passed.\n";
    return 0;
}
