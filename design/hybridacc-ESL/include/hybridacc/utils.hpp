#ifndef HYBRIDACC_UTILS_HPP
#define HYBRIDACC_UTILS_HPP
#include <systemc.h>

namespace hybridacc {

#define INST_MEM_SIZE 256
#define DATA_MEM_SIZE 256
#define ADDR_BITS     8   // log2(INST_MEM_SIZE) or log2
#define DATA_WIDTH    16  // 16 bits
#define VECTOR_SIZE  4   // 4 lanes
#define INST_WIDTH   16  // 16 bits
#define PORT_STATIC_WIDTH 64   // 64 bits
#define PORT_DYNAMIC_WIDTH 16  // 16 bits
#define PORT_LOCAL_WIDTH 64  // 64 bits
#define BUS_DATA_WIDTH   64 // 64 bits
#define BUS_ADDR_WIDTH  9 // 9 bits
#define LOOP_COUNT_BITS 10  // 新增: 迴圈計數位寬 (需與 LoopStack template 預設一致)

#define TCOUNTER_BITS 2 // T counter 位寬 (0~3)
#define PCOUNTER_BITS 5 // P counter 位寬 (0~31)

#define PE_ID_BITS 6 // PE ID 位寬 (支援最多 64 個 PE)
#define NOC_CHANNELS 4 // NoC 通道數量

typedef enum {VADD, REDUCTION} VADDMODE;

// ---------------- Interface Width Configuration (可由外部覆寫) ----------------

template <typename T>
struct ReadyValidIf : public sc_interface {
    virtual void send(const T& v) = 0;
    virtual bool receive(T& v) = 0;
};

template <typename T>
struct ReadyValidChannel : public sc_channel, public ReadyValidIf<T> {
    sc_in<bool> clk{"clk"};
    sc_signal<T>    data{"data"};
    sc_signal<bool> ready{"ready"};
    sc_signal<bool> valid{"valid"};

    SC_CTOR(ReadyValidChannel) {}

    void send(const T& v) override {
        data.write(v);
        valid.write(true);
        do {
            wait(clk.posedge_event()); // wait for clock's posedge
        } while (!ready.read());
        valid.write(false);
    }

    bool receive(T& v) override {
        do {
            wait(clk.posedge_event()); // wait for clock's posedge
        } while (!valid.read());
        v = data.read();
        ready.write(true);
        wait(clk.posedge_event()); // handshake
        ready.write(false);
        return true;
    }
};

template <unsigned int DEPTH, unsigned int PORT_WIDTH>
SC_MODULE(ReadyValidFIFO) {
    // Ports
    sc_in<bool> clk;      // Clock signal
    sc_in<bool> rst_n;    // Active-low reset signal

    sc_in<sc_uint<PORT_WIDTH>> data_in;  // Input data
    sc_in<bool> valid_in;                // Input valid signal
    sc_out<bool> ready_out;              // Output ready signal

    sc_out<sc_uint<PORT_WIDTH>> data_out; // Output data
    sc_out<bool> valid_out;               // Output valid signal
    sc_in<bool> ready_in;                 // Input ready signal

    // Internal signals
    sc_signal<sc_uint<PORT_WIDTH>> fifo[DEPTH];
    sc_signal<unsigned int> read_ptr;
    sc_signal<unsigned int> write_ptr;
    sc_signal<unsigned int> count;  // 當前 FIFO 佔用 (public 可直接 sc_trace / 讀取)

    // Constructor
    SC_CTOR(ReadyValidFIFO) {
        SC_METHOD(seq_proc);
        sensitive << clk.pos();
        dont_initialize();

        SC_METHOD(comb_proc);
        sensitive << rst_n << valid_in << ready_in << count;
    }

    // === 公開介面: 佔用查詢 (避免在外部自行統計) ===
    inline unsigned int occupancy() const { return count.read(); }
    inline unsigned int capacity() const { return DEPTH; }

private:
    // Sequential process
    void seq_proc() {
        if (!rst_n.read()) {
            read_ptr.write(0);
            write_ptr.write(0);
            count.write(0);
        } else {
            if (valid_in.read() && ready_out.read()) {
                fifo[write_ptr.read()] = data_in.read();
                write_ptr.write((write_ptr.read() + 1) % DEPTH);
                count.write(count.read() + 1);
            }
            if (valid_out.read() && ready_in.read()) {
                read_ptr.write((read_ptr.read() + 1) % DEPTH);
                count.write(count.read() - 1);
            }
        }
    }

    // Combinational process
    void comb_proc() {
        if (!rst_n.read()) {
            valid_out.write(false);
            ready_out.write(false);
        } else {
            valid_out.write(count.read() > 0);
            ready_out.write(count.read() < DEPTH);
            if (count.read() > 0) {
                data_out.write(fifo[read_ptr.read()]);
            }
        }
    }
};

}
#endif // HYBRIDACC_UTILS_HPP