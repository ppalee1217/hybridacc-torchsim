
#include <iostream>
#include <cstring>
#include "utils.hpp"

static void usage(const char *prog) {
	std::cerr << "usage: " << prog << " [options] <elf-file>\n"
	          << "  -d   disassemble executable sections (.hacc.core)\n"
	          << "  -x   hex-dump all section contents\n"
	          << "  -h   show this help\n";
}

int main(int argc, char **argv) {
	hacc::DumpOpts opts;
	const char *path = nullptr;

	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "-d") == 0) {
			opts.disasm = true;
		} else if (std::strcmp(argv[i], "-x") == 0) {
			opts.decode = true;
		} else if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else if (argv[i][0] == '-') {
			std::cerr << "unknown option: " << argv[i] << '\n';
			usage(argv[0]);
			return 1;
		} else {
			path = argv[i];
		}
	}

	if (!path) {
		usage(argv[0]);
		return 1;
	}

	if (!hacc::dump_elf(path, opts)) {
		std::cerr << "dump_elf failed for: " << path << '\n';
		return 2;
	}

	return 0;
}
