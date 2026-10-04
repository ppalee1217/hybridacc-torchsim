// NoC requests travel on sc_signal, which notifies readers only when
// operator== reports a different value. The lane mask decides how many
// elements a PE FIFO accepts (Utils/async_FIFO.hpp), so a request that
// differs only in its mask must still be observable: an all-zero payload
// with valid lanes is not the idle value.
#include <systemc>
#include <array>
#include <iostream>

#include "Utils/utils.hpp"

using namespace sc_core;

static int failures = 0;

static void check(bool ok, const char* what) {
    std::cout << (ok ? "PASS: " : "FAIL: ") << what << "\n";
    failures += ok ? 0 : 1;
}

int sc_main(int, char**) {
    using wide_request_t = request_t<sc_dt::sc_biguint<192>, uint16_t>;
    sc_signal<noc_request_t> narrow("narrow");
    sc_signal<wide_request_t> wide("wide");
    auto settle = [] { sc_start(1, SC_NS); };

    const size_t full = 0xF;
    noc_request_t idle;
    noc_request_t zero_full = idle;
    zero_full.mask = full;
    check(!(idle == zero_full), "idle and an all-zero request with valid lanes compare unequal");

    settle();
    narrow.write(zero_full);
    settle();
    check(narrow.read().mask == full, "narrow: a mask-only change reaches the reader");

    noc_request_t zero_partial = idle;
    zero_partial.mask = 0x3;
    narrow.write(zero_partial);
    settle();
    check(narrow.read().mask == 0x3, "narrow: a partial mask reaches the reader");

    narrow.write(idle);
    settle();
    check(narrow.read().mask == 0, "narrow: returning to idle reaches the reader");

    wide_request_t wide_zero_full;
    wide_zero_full.mask = full;
    wide.write(wide_zero_full);
    settle();
    check(wide.read().mask == full, "wide: a mask-only change reaches the reader");

    // The D-31 conv1x1 second wave: 12 windows x tags 0..3 x 3 all-zero
    // vectors per tag, each with every lane valid. Every vector must arrive.
    std::array<unsigned, 4> full_vectors{};
    for (unsigned window = 0; window < 12; ++window) {
        for (unsigned tag = 0; tag < 4; ++tag) {
            for (unsigned vector = 0; vector < 3; ++vector) {
                noc_request_t req;
                req.addr = tag;
                req.mask = full;
                narrow.write(req);
                settle();
                if (narrow.read().addr == tag && narrow.read().mask == full) ++full_vectors[tag];
            }
        }
    }
    check(full_vectors == std::array<unsigned, 4>{36, 36, 36, 36},
          "all 36 zero-data vectors of each tag are observed");

    std::cout << (failures ? "FAILED" : "ALL PASSED") << "\n";
    return failures ? 1 : 0;
}
