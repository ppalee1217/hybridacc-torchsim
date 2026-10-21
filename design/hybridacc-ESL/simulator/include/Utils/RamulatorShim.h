// C interface to Ramulator2 for FakeDram's optional DRAM timing (M22, ESL Ramulator adapter; RDR M22).
// Ramulator2's headers need C++20 and the simulator is C++17, so only this C interface crosses; the
// implementation is ramulator_shim/RamulatorShim.cc, built when CMake is given HACC_RAMULATOR_DIR.
#pragma once
#include <cstddef>
#include <cstdint>

extern "C" {
struct hacc_ram;
// Builds the memory system from a Ramulator2 YAML with the External frontend (clock ratio 1, as TOGSim does).
// Returns nullptr and writes a message to err on failure.
hacc_ram* hacc_ram_create(const char* config_path, char* err, std::size_t err_len);
float hacc_ram_tck_ns(hacc_ram* r);
int hacc_ram_tx_bytes(hacc_ram* r);
// Offers one access; returns 1 when the memory system accepted it. id comes back from hacc_ram_pop_done.
int hacc_ram_send(hacc_ram* r, int is_write, std::uint64_t addr, std::uint64_t id);
// One DRAM clock.
void hacc_ram_tick(hacc_ram* r);
// Completed accesses in completion order; returns 0 when none is left.
int hacc_ram_pop_done(hacc_ram* r, std::uint64_t* id);
// Finalizes and writes Ramulator2's statistics (YAML) to stats_path when it is not null.
void hacc_ram_finish(hacc_ram* r, const char* stats_path);
}
