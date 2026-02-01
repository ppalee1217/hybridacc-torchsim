#pragma once

#include <cstdlib>
#include <cstdint>
#include <cstring>  // 添加 cstring 標頭檔用於 memcmp
#include <array>
#include <iostream>
#include <iomanip>
#include <type_traits>
#include <systemc>

// DEBUG Level 控制
enum DebugLevel {
    DEBUG_LEVEL_NONE = 0,
    DEBUG_LEVEL_PE_COMPONENTS,
    DEBUG_LEVEL_PE_STAGE,
    DEBUG_LEVEL_PE_TOP,
    DEBUG_LEVEL_NOC_COMPONENTS,
    DEBUG_LEVEL_NOC_TOP,
    DEBUG_LEVEL_ALL,
};

#ifdef DEBUG_UTILS
    #ifndef DEBUG_LEVEL_MAX
        #define DEBUG_LEVEL_MAX DEBUG_LEVEL_ALL
    #endif

    #ifndef DEBUG_LEVEL_MIN
        #define DEBUG_LEVEL_MIN DEBUG_LEVEL_NONE
    #endif

    #define DEBUG_MSG(msg, level)                                     \
        do {                                                          \
            if ((level) <= DEBUG_LEVEL_MAX &&                         \
                (level) >= DEBUG_LEVEL_MIN) {                         \
                std::cout << "[Debug] Time: " << sc_time_stamp()      \
                          << " [" << this->name() << "] "          \
                          << msg << std::endl;                        \
            }                                                         \
        } while (0)
#else
    #define DEBUG_MSG(msg, level) do {} while (0)
#endif

// -----------------------------------------------------------------------------
typedef uint16_t fp16_t; // 元素類型 (16-bit half precision)
typedef uint16_t pe_inst_t; // 指令類型 (16-bit instruction)

// -----------------------------------------------------------------------------
// 向量類型 (4-lane vector of fp16_t)
class v_fp16_t {
    public:
        std::array<fp16_t, 4> lanes; // 4-lane vector of fp16_t

        v_fp16_t() { lanes.fill(0); }
        v_fp16_t(const std::array<fp16_t, 4>& l) : lanes(l) {}
        fp16_t& operator[](size_t idx) { return lanes[idx]; }
        const fp16_t& operator[](size_t idx) const { return lanes[idx]; }
        uint64_t toUint64() const {
            uint64_t result = 0;
            for (size_t i = 0; i < lanes.size(); ++i) {
                result |= static_cast<uint64_t>(lanes[i]) << (i * 16);
            }
            return result;
        }
        void fromUint64(uint64_t value) {
            for (size_t i = 0; i < lanes.size(); ++i) {
                lanes[i] = static_cast<fp16_t>((value >> (i * 16)) & 0xFFFF);
            }
        }
        bool operator==(const v_fp16_t& other) const {
            return lanes == other.lanes;
        }
        friend std::ostream& operator<<(std::ostream& os, const v_fp16_t& v) {
            os << "v_fp16_t[";
            for (size_t i = 0; i < v.lanes.size(); ++i) {
                os << "0x" << std::hex << std::setw(4) << std::setfill('0') << v.lanes[i] << std::dec;
                if (i != v.lanes.size() - 1) os << ", ";
            }
            os << "]";
            return os;
        }
        friend void sc_trace(sc_core::sc_trace_file* tf, const v_fp16_t& v, const std::string& name) {
            for (size_t i = 0; i < v.lanes.size(); ++i) {
                sc_core::sc_trace(tf, v.lanes[i], name + ".lane" + std::to_string(i));
            }
        }
};

// -----------------------------------------------------------------------------
fp16_t fp16_add(fp16_t a, fp16_t b);
fp16_t fp16_mul(fp16_t a, fp16_t b);
// -----------------------------------------------------------------------------

template<typename DATA_TYPE, typename ADDR_TYPE>
struct request_t {
    DATA_TYPE data; // 64 bits
    ADDR_TYPE addr; // 9 bits
    size_t mask; // for async FIFO

    // 相等運算符，SystemC 需要
    bool operator==(const request_t& other) const {
        return data == other.data && addr == other.addr;
    }

    // 輸出運算符，SystemC 訊號需要
    friend std::ostream& operator<<(std::ostream& os, const request_t& req) {
        os << "request_t{data=" << std::hex << req.data
           << ", addr=0x" << req.addr
           << ", mask=" << req.mask << std::dec << "}";
        return os;
    }

    // sc_trace for request_t
    friend void sc_trace(sc_core::sc_trace_file* tf, const request_t& req, const std::string& name) {
        sc_core::sc_trace(tf, req.data, name + ".data");
        sc_core::sc_trace(tf, req.addr, name + ".addr");
    }
};

typedef request_t<uint64_t, uint16_t> noc_request_t;

// struct for NoC Read Request (only address)
struct noc_addr_req_t {
    uint16_t addr;

    noc_addr_req_t(uint16_t a = 0) : addr(a) {}

    bool operator==(const noc_addr_req_t& other) const {
        return addr == other.addr;
    }

    friend std::ostream& operator<<(std::ostream& os, const noc_addr_req_t& req) {
        os << "noc_addr_req_t{addr=0x" << std::hex << req.addr << std::dec << "}";
        return os;
    }

    friend void sc_trace(sc_core::sc_trace_file* tf, const noc_addr_req_t& req, const std::string& name) {
        sc_core::sc_trace(tf, req.addr, name + ".addr");
    }
};

// -----------------------------------------------------------------------------
enum NOC_CHANNELS {
    NOC_CHANNEL_PS = 0,
    NOC_CHANNEL_PD = 1,
    NOC_CHANNEL_PLI = 2,
    NOC_CHANNEL_PLO = 3
};

inline std::ostream& operator<<(std::ostream& os, NOC_CHANNELS channel) {
    switch (channel) {
        case NOC_CHANNEL_PS: os << "NOC_CHANNEL_PS"; break;
        case NOC_CHANNEL_PD: os << "NOC_CHANNEL_PD"; break;
        case NOC_CHANNEL_PLI: os << "NOC_CHANNEL_PLI"; break;
        case NOC_CHANNEL_PLO: os << "NOC_CHANNEL_PLO"; break;
        default: os << "UNKNOWN_CHANNEL"; break;
    }
    return os;
}

inline void sc_trace(sc_core::sc_trace_file* tf, const NOC_CHANNELS& channel, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(channel), name);
}

// -----------------------------------------------------------------------------
enum class NOC_RESPONSE_STATUS {
    NOC_OK = 0,
    NOC_ERROR = 1,
    NOC_NOP = 2
};

inline std::ostream& operator<<(std::ostream& os, NOC_RESPONSE_STATUS status) {
    switch (status) {
        case NOC_RESPONSE_STATUS::NOC_OK: return os << "NOC_OK";
        case NOC_RESPONSE_STATUS::NOC_ERROR: return os << "NOC_ERROR";
        case NOC_RESPONSE_STATUS::NOC_NOP: return os << "NOC_NOP";
        default: return os << "UNKNOWN";
    }
}

inline void sc_trace(sc_core::sc_trace_file* tf, const NOC_RESPONSE_STATUS& status, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(status), name);
}

// -----------------------------------------------------------------------------

template<typename DATA_TYPE>
struct response_t {
    DATA_TYPE data; // 64 bits
    NOC_RESPONSE_STATUS status; // response status

    // 相等運算符，SystemC 需要
    bool operator==(const response_t& other) const {
        return data == other.data && status == other.status;
    }

    // 輸出運算符，SystemC 訊號需要
    friend std::ostream& operator<<(std::ostream& os, const response_t& resp) {
        os << "response_t{data=" << std::hex << resp.data << std::dec
           << ", status=" << static_cast<int>(resp.status) << "}";
        return os;
    }

    // sc_trace for response_t
    friend void sc_trace(sc_core::sc_trace_file* tf, const response_t& resp, const std::string& name) {
        sc_core::sc_trace(tf, resp.data, name + ".data");
        sc_core::sc_trace(tf, static_cast<int>(resp.status), name + ".status");
    }
};

typedef response_t<uint64_t> noc_response_t;

// -----------------------------------------------------------------------------
struct pe_decode_signals_t {
    uint16_t inst;
    // --- Stage IF/ID --- //
    bool halt;
    bool nop;
    int func3;
    uint16_t imm; // stride/addr/len/kernel immediate
    // Loop control signals
    bool loop_in;
    bool loop_break;
    bool loop_end;
    bool jump_en;
    // Swap Control
    bool is_swap;

    // --- Stage EXE/M --- //
    // DL control signals
    bool DL_setaddr;
    bool DL_setlen;
    // DL_mode =  func3
    bool DL_active;
    bool DL_next;
    bool DL_setloop;
    bool DL_is_sdma;
    // TR control signals
    int rid3;
    int rid5;
    bool pd_load;
    bool tr_en;
    bool tr_write;
    bool tr_shift;
    // tr_shift_mask = func3
    bool tr_clear_regs;
    bool tr_use_vcounter;
    bool tr_set_vcounter;
    bool tr_clear_vcounter;
    bool tr_incr_vcounter;

    // --- Stage EXE/A --- //
    // port
    bool pli_plo_operation;
    // PR control signals
    bool pr_en;
    bool pr_write;
    bool pr_mode; // 0: scalar, 1: vector 64-bit
    bool pr_clear_regs;
    bool pr_use_vcounter;
    bool pr_set_vcounter;
    bool pr_clear_vcounter;
    bool pr_incr_vcounter;
    // VADDU control signals
    bool vaddu_en;
    int vaddu_mode;
};


// << for pe_decode_signals_t
inline std::ostream& operator<<(std::ostream& os, const pe_decode_signals_t& sig) {
    os << "pe_decode_signals_t{"
       << "halt=" << sig.halt << ", "
       << "nop=" << sig.nop << ", "
       << "func3=" << sig.func3 << ", "
       << "imm=0x" << std::hex << sig.imm << std::dec << ", "
       << "loop_in=" << sig.loop_in << ", "
       << "loop_break=" << sig.loop_break << ", "
       << "loop_end=" << sig.loop_end << ", "
       << "jump_en=" << sig.jump_en << ", "
       << "DL_setaddr=" << sig.DL_setaddr << ", "
       << "DL_setlen=" << sig.DL_setlen << ", "
       << "DL_active=" << sig.DL_active << ", "
       << "DL_next=" << sig.DL_next << ", "
       << "rid3=" << sig.rid3 << ", "
       << "rid5=" << sig.rid5 << ", "
       << "pd_load=" << sig.pd_load << ", "
       << "tr_en=" << sig.tr_en << ", "
       << "tr_write=" << sig.tr_write << ", "
       << "tr_shift=" << sig.tr_shift << ", "
       << "tr_clear_regs=" << sig.tr_clear_regs << ", "
       << "tr_use_vcounter=" << sig.tr_use_vcounter << ", "
       << "tr_set_vcounter=" << sig.tr_set_vcounter << ", "
       << "tr_clear_vcounter=" << sig.tr_clear_vcounter << ", "
       << "tr_incr_vcounter=" << sig.tr_incr_vcounter << ", "
       << "pli_plo_operation=" << sig.pli_plo_operation << ", "
       << "pr_en=" << sig.pr_en << ", "
       << "pr_write=" << sig.pr_write << ", "
       << "pr_mode=" << sig.pr_mode << ", "
       << "pr_clear_regs=" << sig.pr_clear_regs << ", "
       << "pr_use_vcounter=" << sig.pr_use_vcounter << ", "
       << "pr_set_vcounter=" << sig.pr_set_vcounter << ", "
       << "pr_clear_vcounter=" << sig.pr_clear_vcounter << ", "
       << "pr_incr_vcounter=" << sig.pr_incr_vcounter << ", "
       << "vaddu_en=" << sig.vaddu_en << ", "
       << "vaddu_mode=" << sig.vaddu_mode
       << "}";
    return os;
}

// == for pe_decode_signals_t
inline bool operator==(const pe_decode_signals_t& a, const pe_decode_signals_t& b) {
    return std::memcmp(&a, &b, sizeof(pe_decode_signals_t)) == 0;
}

// sc_trace for pe_decode_signals_t
inline void sc_trace(sc_core::sc_trace_file* tf, const pe_decode_signals_t& sig, const std::string& name) {
    sc_core::sc_trace(tf, sig.halt, name + ".halt");
    sc_core::sc_trace(tf, sig.nop, name + ".nop");
    sc_core::sc_trace(tf, sig.func3, name + ".func3");
    sc_core::sc_trace(tf, sig.imm, name + ".imm");
    sc_core::sc_trace(tf, sig.loop_in, name + ".loop_in");
    sc_core::sc_trace(tf, sig.loop_break, name + ".loop_break");
    sc_core::sc_trace(tf, sig.loop_end, name + ".loop_end");
    sc_core::sc_trace(tf, sig.jump_en, name + ".jump_en");
    sc_core::sc_trace(tf, sig.DL_setaddr, name + ".DL_setaddr");
    sc_core::sc_trace(tf, sig.DL_setlen, name + ".DL_setlen");
    sc_core::sc_trace(tf, sig.DL_active, name + ".DL_active");
    sc_core::sc_trace(tf, sig.DL_next, name + ".DL_next");
    sc_core::sc_trace(tf, sig.rid3, name + ".rid3");
    sc_core::sc_trace(tf, sig.rid5, name + ".rid5");
    sc_core::sc_trace(tf, sig.pd_load, name + ".pd_load");
    sc_core::sc_trace(tf, sig.tr_en, name + ".tr_en");
    sc_core::sc_trace(tf, sig.tr_write, name + ".tr_write");
    sc_core::sc_trace(tf, sig.tr_shift, name + ".tr_shift");
    sc_core::sc_trace(tf, sig.tr_clear_regs, name + ".tr_clear_regs");
    sc_core::sc_trace(tf, sig.tr_use_vcounter, name + ".tr_use_vcounter");
    sc_core::sc_trace(tf, sig.tr_set_vcounter, name + ".tr_set_vcounter");
    sc_core::sc_trace(tf, sig.tr_clear_vcounter, name + ".tr_clear_vcounter");
    sc_core::sc_trace(tf, sig.tr_incr_vcounter, name + ".tr_incr_vcounter");
    sc_core::sc_trace(tf, sig.pli_plo_operation, name + ".pli_plo_operation");
    sc_core::sc_trace(tf, sig.pr_en, name + ".pr_en");
    sc_core::sc_trace(tf, sig.pr_write, name + ".pr_write");
    sc_core::sc_trace(tf, sig.pr_mode, name + ".pr_mode");
    sc_core::sc_trace(tf, sig.pr_clear_regs, name + ".pr_clear_regs");
    sc_core::sc_trace(tf, sig.pr_use_vcounter, name + ".pr_use_vcounter");
    sc_core::sc_trace(tf, sig.pr_set_vcounter, name + ".pr_set_vcounter");
    sc_core::sc_trace(tf, sig.pr_clear_vcounter, name + ".pr_clear_vcounter");
    sc_core::sc_trace(tf, sig.pr_incr_vcounter, name + ".pr_incr_vcounter");
    sc_core::sc_trace(tf, sig.vaddu_en, name + ".vaddu_en");
    sc_core::sc_trace(tf, sig.vaddu_mode, name + ".vaddu_mode");
}


// -----------------------------------------------------------------------------
// valid-ready IF template

// valid-ready data in interface
template <typename T>
struct VRDIF : public sc_core::sc_module {
    sc_core::sc_in<T> data_in;
    sc_core::sc_in<bool> valid_in;
    sc_core::sc_out<bool> ready_out;

    SC_CTOR(VRDIF)
        : data_in("data_in"),
          valid_in("valid_in"),
          ready_out("ready_out") {}
};

// valid-ready data out interface
template <typename T>
struct VRDOF : public sc_core::sc_module {
    sc_core::sc_out<T> data_out;
    sc_core::sc_out<bool> valid_out;
    sc_core::sc_in<bool> ready_in;

    SC_CTOR(VRDOF)
        : data_out("data_out"),
          valid_out("valid_out"),
          ready_in("ready_in") {}
};

// valid-ready data connect signals
template <typename T>
struct VRDSIG : public sc_core::sc_module {
    sc_core::sc_signal<T> data_sig;
    sc_core::sc_signal<bool> valid_sig;
    sc_core::sc_signal<bool> ready_sig;

    SC_CTOR(VRDSIG)
        : data_sig("data_sig"),
          valid_sig("valid_sig"),
          ready_sig("ready_sig") {}
};

// binding function for VRDIF inner to VRDIF outer
template <typename T>
void bind_vr_interface(VRDIF<T>& vrdif_inner, VRDIF<T>& vrdif_outer) {
    vrdif_inner.ready_out(vrdif_outer.ready_out);
    vrdif_inner.valid_in(vrdif_outer.valid_in);
    vrdif_inner.data_in(vrdif_outer.data_in);
}

// binding function for VRDOF outer to VRDOF inner
template <typename T>
void bind_vr_interface(VRDOF<T>& vrdof_outer, VRDOF<T>& vrdof_inner) {
    vrdof_inner.ready_in(vrdof_outer.ready_in);
    vrdof_inner.valid_out(vrdof_outer.valid_out);
    vrdof_inner.data_out(vrdof_outer.data_out);
}

// connect VRDIF to signals
template <typename T>
void connect_vr_signals(VRDIF<T>& vrdif, VRDSIG<T>& sig) {
    vrdif.data_in(sig.data_sig);
    vrdif.valid_in(sig.valid_sig);
    vrdif.ready_out(sig.ready_sig);
}

// connect signals to VRDOF
template <typename T>
void connect_vr_signals(VRDOF<T>& vrdof, VRDSIG<T>& sig) {
    vrdof.data_out(sig.data_sig);
    vrdof.valid_out(sig.valid_sig);
    vrdof.ready_in(sig.ready_sig);
}

// -----------------------------------------------------------------------------
enum class PERouterMode {
    PLI_FROM_LN_PLO_TO_LN = 0b00,  // PLI from LN, PLO to LN
    PLI_FROM_BUS_PLO_TO_LN = 0b01, // PLI from bus, PLO to LN
    PLI_FROM_LN_PLO_TO_BUS = 0b10, // PLI from LN, PLO to Bus
    PLI_FROM_BUS_PLO_TO_BUS = 0b11  // PLI from bus, PLO to Bus
};

// Add operator<< support for PERouterMode
inline std::ostream& operator<<(std::ostream& os, PERouterMode mode) {
    switch (mode) {
        case PERouterMode::PLI_FROM_LN_PLO_TO_LN: return os << "PLI_FROM_LN_PLO_TO_LN";
        case PERouterMode::PLI_FROM_BUS_PLO_TO_LN: return os << "PLI_FROM_BUS_PLO_TO_LN";
        case PERouterMode::PLI_FROM_LN_PLO_TO_BUS: return os << "PLI_FROM_LN_PLO_TO_BUS";
        case PERouterMode::PLI_FROM_BUS_PLO_TO_BUS: return os << "PLI_FROM_BUS_PLO_TO_BUS";
        default: return os << "UNKNOWN";
    }
}

// sc_trace support for PERouterMode
inline void sc_trace(sc_core::sc_trace_file* tf, const PERouterMode& mode, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(mode), name);
}

// -----------------------------------------------------------------------------
enum class message_command_t {
    CMD_RESET = 0, // clear reg
    CMD_INIT = 1, // setting ids, mode, enable
    CMD_LOAD_PROGRAM = 2, // load program to IM
    CMD_STOP_PE = 3, // stop PE operation
    CMD_START_PE = 4, // start PE operation

    CMD_NOC_SCAN_CHAIN = 8 // scan-chain operation
};

// Add operator<< support for message_command_t
inline std::ostream& operator<<(std::ostream& os, message_command_t command) {
    switch (command) {
        case message_command_t::CMD_RESET: return os << "CMD_RESET";
        case message_command_t::CMD_INIT: return os << "CMD_INIT";
        case message_command_t::CMD_LOAD_PROGRAM: return os << "CMD_LOAD_PROGRAM";
        case message_command_t::CMD_STOP_PE: return os << "CMD_STOP_PE";
        case message_command_t::CMD_START_PE: return os << "CMD_START_PE";
        case message_command_t::CMD_NOC_SCAN_CHAIN: return os << "CMD_NOC_SCAN_CHAIN";
        default: return os << "UNKNOWN";
    }
}

// sc_trace support for message_command_t
inline void sc_trace(sc_core::sc_trace_file* tf, const message_command_t& command, const std::string& name) {
    sc_core::sc_trace(tf, static_cast<int>(command), name);
}

// -----------------------------------------------------------------------------

// scan-chain utilities
struct ScanChainFormat{
    uint8_t ps_id;
    uint8_t pd_id;
    uint8_t pli_id;
    uint8_t plo_id;
    PERouterMode route_mode;
    bool enable;

    // Add equality operator for SystemC signals
    bool operator==(const ScanChainFormat& other) const {
        return ps_id == other.ps_id &&
               pd_id == other.pd_id &&
               pli_id == other.pli_id &&
               plo_id == other.plo_id &&
               route_mode == other.route_mode &&
               enable == other.enable;
    }

    // Add output operator for debugging
    friend std::ostream& operator<<(std::ostream& os, const ScanChainFormat& fmt) {
        os << "ScanChainFormat{ps_id=" << (int)fmt.ps_id
           << ", pd_id=" << (int)fmt.pd_id
           << ", pli_id=" << (int)fmt.pli_id
           << ", plo_id=" << (int)fmt.plo_id
           << ", route_mode=" << fmt.route_mode
           << ", enable=" << fmt.enable << "}";
        return os;
    }

    // Add sc_trace for SystemC tracing
    friend void sc_trace(sc_core::sc_trace_file* tf, const ScanChainFormat& fmt, const std::string& name) {
        sc_core::sc_trace(tf, fmt.ps_id, name + ".ps_id");
        sc_core::sc_trace(tf, fmt.pd_id, name + ".pd_id");
        sc_core::sc_trace(tf, fmt.pli_id, name + ".pli_id");
        sc_core::sc_trace(tf, fmt.plo_id, name + ".plo_id");
        sc_core::sc_trace(tf, static_cast<int>(fmt.route_mode), name + ".route_mode");
        sc_core::sc_trace(tf, fmt.enable, name + ".enable");
    }
};

// parse scan-chain data from uint32_t
inline ScanChainFormat parse_scan_chain_data(uint32_t data) {
    ScanChainFormat format;
    format.ps_id = (data >> 4) & 0x3F;
    format.pd_id = (data >> 10) & 0x3F;
    format.pli_id = (data >> 16) & 0x3F;
    format.plo_id = (data >> 22) & 0x3F;
    format.route_mode = static_cast<PERouterMode>((data >> 28) & 0x03);
    format.enable = ((data >> 30) & 0x01) != 0;
    return format;
}

#include <mutex>
#include <fstream>

// Perfetto Trace Manager
class PerfettoTrace {
public:
    static PerfettoTrace& getInstance() {
        static PerfettoTrace instance;
        return instance;
    }

    void open(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mutex);
        if (file.is_open()) file.close();
        file.open(filename);
        if (file.is_open()) {
            file << "[\n";
            is_first = true;
        }
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        if (file.is_open()) {
            file << "\n]";
            file.close();
        }
    }

    void logEvent(const std::string& name, const std::string& cat, const std::string& ph,
                  uint32_t pid, uint32_t tid, const std::string& args = "{}") {
        if (!file.is_open()) return;

        double ts = sc_core::sc_time_stamp().to_seconds() * 1000000.0; // microseconds

        std::lock_guard<std::mutex> lock(mutex);
        if (!is_first) {
            file << ",\n";
        }
        is_first = false;

        file << "{\"name\": \"" << name << "\", \"cat\": \"" << cat << "\", \"ph\": \"" << ph
             << "\", \"ts\": " << std::fixed << std::setprecision(3) << ts
             << ", \"pid\": " << pid << ", \"tid\": " << tid
             << ", \"args\": " << args << "}";
    }

    void setThreadName(uint32_t pid, uint32_t tid, const std::string& thread_name) {
        logEvent("thread_name", "__metadata", "M", pid, tid, "{\"name\": \"" + thread_name + "\"}");
    }

private:
    PerfettoTrace() {}
    ~PerfettoTrace() {
        if (file.is_open()) {
            file << "\n]";
            file.close();
        }
    }
    std::ofstream file;
    std::mutex mutex;
    bool is_first = true;
};

#define TRACE_EVENT(name, cat, ph, pid, tid, args) \
    PerfettoTrace::getInstance().logEvent(name, cat, ph, static_cast<uint32_t>(pid), tid, args)

#define TRACE_THREAD_NAME(pid, tid, name) \
    PerfettoTrace::getInstance().setThreadName(static_cast<uint32_t>(pid), tid, name)

#define TRACE_BEGIN "B"
#define TRACE_END "E"

enum class TRACE_PID {
    PE,
    MBUS,
    NOC_ROUTER,
};
