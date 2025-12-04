// HybridAcc PE-ISA Package Tool
// Creates package binaries containing multiple templates
#include "package.hpp"
#include "../assambler/instruction.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <cstring>

using namespace hybridacc;

static void printUsage(const char *progName) {
    std::cout << "Usage: " << progName << " [options] <input1> <input2> ...\n\n";
    std::cout << "Creates a package binary containing multiple templates.\n\n";
    std::cout << "Input files can be:\n";
    std::cout << "  *.asm   - Template assembly files (will be assembled first)\n";
    std::cout << "  *.json  - Pre-assembled template JSON files\n\n";
    std::cout << "Options:\n";
    std::cout << "  -o <file.pkg>       Output package binary file\n";
    std::cout << "  --json <file.json>  Output package JSON information\n";
    std::cout << "  --header <file.h>   Output C header file with package definitions\n";
    std::cout << "  --verbose           Show detailed packaging process\n";
    std::cout << "  -h, --help          Show this help message\n\n";
    std::cout << "Example:\n";
    std::cout << "  " << progName << " -o kernels.pkg --json kernels.json --header kernels.h \\\n";
    std::cout << "    template/conv1d_k3s1.asm template/conv1d_k5s1.asm template/gemv.json\n\n";
    std::cout << "Package Binary Format:\n";
    std::cout << "  Header:\n";
    std::cout << "    [package version,  8bits]\n";
    std::cout << "    [num of templates, 8bits]\n";
    std::cout << "    [offset vector 0,  16bits]\n";
    std::cout << "    [offset vector 1,  16bits]\n";
    std::cout << "    ...\n";
    std::cout << "  Body:\n";
    std::cout << "    [template binary 0]\n";
    std::cout << "    [template binary 1]\n";
    std::cout << "    ...\n";
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::vector<std::string> inputFiles;
    std::string outputBinary;
    std::string outputJson;
    std::string outputHeader;
    bool verbose = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "Error: -o requires an argument\n";
                return 1;
            }
            outputBinary = argv[++i];
        } else if (arg == "--json") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --json requires an argument\n";
                return 1;
            }
            outputJson = argv[++i];
        } else if (arg == "--header") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --header requires an argument\n";
                return 1;
            }
            outputHeader = argv[++i];
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg[0] == '-') {
            std::cerr << "Error: Unknown option: " << arg << "\n";
            return 1;
        } else {
            inputFiles.push_back(arg);
        }
    }

    // Validate inputs
    if (inputFiles.empty()) {
        std::cerr << "Error: No input files specified\n";
        printUsage(argv[0]);
        return 1;
    }

    if (outputBinary.empty() && outputJson.empty() && outputHeader.empty()) {
        std::cerr << "Error: No output file specified. Use -o, --json, or --header\n";
        return 1;
    }

    try {
        Packager packager;

        if (verbose) {
            std::cout << "HybridAcc Package Tool\n";
            std::cout << "======================\n\n";
        }

        // Load templates
        for (const auto &inputFile : inputFiles) {
            if (verbose) {
                std::cout << "Loading: " << inputFile << " ... ";
            }

            // Check file extension
            if (inputFile.size() >= 4 && inputFile.substr(inputFile.size() - 4) == ".asm") {
                packager.addTemplateFromAsm(inputFile);
                if (verbose) std::cout << "OK (assembled)\n";
            } else if (inputFile.size() >= 5 && inputFile.substr(inputFile.size() - 5) == ".json") {
                packager.addTemplateFromJson(inputFile);
                if (verbose) std::cout << "OK (from JSON)\n";
            } else {
                std::cerr << "\nWarning: Unknown file extension, assuming .json: " << inputFile << "\n";
                packager.addTemplateFromJson(inputFile);
                if (verbose) std::cout << "OK (from JSON)\n";
            }
        }

        // Create package
        if (verbose) {
            std::cout << "\nCreating package...\n";
        }

        Package pkg = packager.createPackage();

        if (verbose) {
            std::cout << "  Package version: " << (int)pkg.version << "\n";
            std::cout << "  Number of templates: " << pkg.templates.size() << "\n";
            std::cout << "  Total binary size: " << pkg.calculateBinarySize() << " bytes\n\n";

            std::vector<uint16_t> offsets = pkg.calculateOffsets();
            for (size_t i = 0; i < pkg.templates.size(); ++i) {
                const auto &tpl = pkg.templates[i];
                std::cout << "  Template " << i << ": " << tpl.name << "\n";
                std::cout << "    Offset: " << offsets[i] << " bytes\n";
                std::cout << "    Parameters: " << tpl.params.size() << "\n";
                std::cout << "    Patches: " << tpl.patches.size() << "\n";
                std::cout << "    Instructions: " << tpl.instructions.size() << "\n";
            }
            std::cout << "\n";
        }

        // Write outputs
        if (!outputBinary.empty()) {
            if (verbose) {
                std::cout << "Writing binary: " << outputBinary << " ... ";
            }
            packager.writePackageBinary(outputBinary, pkg);
            if (verbose) std::cout << "OK\n";
        }

        if (!outputJson.empty()) {
            if (verbose) {
                std::cout << "Writing JSON: " << outputJson << " ... ";
            }
            packager.writePackageJson(outputJson, pkg);
            if (verbose) std::cout << "OK\n";
        }

        if (!outputHeader.empty()) {
            if (verbose) {
                std::cout << "Writing header: " << outputHeader << " ... ";
            }
            packager.writePackageHeader(outputHeader, pkg);
            if (verbose) std::cout << "OK\n";
        }

        if (verbose) {
            std::cout << "\nPackage created successfully!\n";
        }

        return 0;

    } catch (const AsmError &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
