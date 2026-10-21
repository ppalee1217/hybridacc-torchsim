// Ramulator2 behind the C interface of include/Utils/RamulatorShim.h (built as C++20; see that header).
#include "Utils/RamulatorShim.h"

#include <algorithm>
#include <deque>
#include <exception>
#include <fstream>
#include <string>

#include "ramulator/base/base.h"
#include "ramulator/base/config.h"
#include "ramulator/base/request.h"
#include "ramulator/frontend/i_frontend.h"
#include "ramulator/memory_system/i_memory_system.h"

struct hacc_ram {
    Ramulator::IFrontEnd* fe = nullptr;
    Ramulator::IMemorySystem* mem = nullptr;
    std::deque<std::uint64_t> done;
    int tx_bytes = 0;
    bool finished = false;
};

extern "C" {

hacc_ram* hacc_ram_create(const char* config_path, char* err, std::size_t err_len) {
    try {
        Ramulator::ConfigNode config = Ramulator::Config::parse_config_file(config_path);
        Ramulator::ConfigNode frontend;
        frontend.set("impl", std::string("External"));
        frontend.set("clock_ratio", 1);
        config.set("frontend", frontend);
        auto* r = new hacc_ram;
        r->fe = Ramulator::Factory::create_frontend(config);
        r->mem = Ramulator::Factory::create_memory_system(config);
        r->fe->connect_memory_system(r->mem);
        r->mem->connect_frontend(r->fe);
        r->tx_bytes = r->mem->get_tx_bytes();
        return r;
    } catch (const std::exception& e) {
        if (err && err_len) {
            std::string m = e.what();
            m.copy(err, err_len - 1);
            err[std::min(m.size(), err_len - 1)] = '\0';
        }
        return nullptr;
    }
}

float hacc_ram_tck_ns(hacc_ram* r) { return r->mem->get_tCK(); }

int hacc_ram_tx_bytes(hacc_ram* r) { return r->tx_bytes; }

int hacc_ram_send(hacc_ram* r, int is_write, std::uint64_t addr, std::uint64_t id) {
    auto cb = [r, id](Ramulator::Request&) { r->done.push_back(id); };
    return r->fe->receive_external_requests(is_write ? 1 : 0,  // 0 read, 1 write, as TOGSim's wrapper
                                            static_cast<Ramulator::Addr_t>(addr), 0, cb, r->tx_bytes) ? 1 : 0;
}

void hacc_ram_tick(hacc_ram* r) { r->mem->tick(); }

int hacc_ram_pop_done(hacc_ram* r, std::uint64_t* id) {
    if (r->done.empty()) return 0;
    *id = r->done.front();
    r->done.pop_front();
    return 1;
}

void hacc_ram_finish(hacc_ram* r, const char* stats_path) {
    if (r->finished) return;
    r->finished = true;
    r->fe->finalize();
    r->mem->finalize();
    if (stats_path) {
        std::ofstream os(stats_path);
        r->fe->print_stats(os);
        r->mem->print_stats(os);
    }
}

}  // extern "C"
