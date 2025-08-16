#include "../assambler/instruction.hpp"
#include <iostream>

namespace hybridacc { int run_simulator_cli(int argc, char **argv); }

int main(int argc, char **argv){
    return hybridacc::run_simulator_cli(argc, argv);
}
