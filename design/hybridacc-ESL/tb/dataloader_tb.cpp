#include <systemc>
#include <iostream>
#include <vector>
#include <iomanip>
#include "hybridacc/pe/DataLoader.hpp"

using namespace sc_core;
using namespace hybridacc::pe;

struct DMModel : sc_module {
    sc_in<bool> clk{"clk"};
    sc_in<bool> en_i{"en_i"};
    sc_in<bool> wr_i{"wr_i"};
    sc_in<sc_uint<10>> addr_i{"addr_i"};
    sc_in<sc_uint<8>> mask_i{"mask_i"};
    sc_in<sc_uint<64>> wdata_i{"wdata_i"};
    sc_out<sc_uint<64>> rdata_o{"rdata_o"};

    sc_uint<10> last_addr{0};
    bool pending_read{false};

    static constexpr unsigned DEPTH = 1024; // 10-bit address space
    sc_uint<64> mem[DEPTH];

    uint64_t pattern_for_addr(uint16_t a){
        uint64_t v=0; for(int b=0;b<8;b++){ uint8_t byte = (a + b) & 0xFF; v |= (uint64_t)byte << (8*b); } return v;
    }

    void init_mem(){
        for(unsigned i=0;i<DEPTH;i++) mem[i]=pattern_for_addr(i);
    }

    SC_CTOR(DMModel) {
        init_mem();
        SC_METHOD(seq_proc);
        sensitive << clk.pos();
        SC_METHOD(comb_proc);
        sensitive << en_i << wr_i << addr_i << wdata_i << mask_i;
        dont_initialize();
    }

    void seq_proc(){
        if(pending_read){
            if(last_addr < DEPTH) rdata_o.write(mem[last_addr]); else rdata_o.write(0);
            pending_read = false;
        }
        if(en_i.read() && wr_i.read()) {
            // Write operation
            uint64_t wdata = wdata_i.read();
            uint8_t mask = mask_i.read();
            uint16_t addr = addr_i.read();
            for(int b=0;b<8;b++){
                if(mask & (1 << b)){
                    if(addr + b < DEPTH){
                        mem[addr + b] = (mem[addr + b] & ~(0xFFULL << (8*b))) | ((wdata & (0xFFULL << (8*b))));
                    }
                }
            }
        }
    }

    void comb_proc(){
        if(en_i.read() && !wr_i.read()) {
            last_addr = addr_i.read();
            pending_read = true;
        }
    }
};

struct DLTestBench : sc_module {
    sc_clock clk{"clk", 10, SC_NS};
    sc_signal<bool> rst_n{"rst_n"};

    // DataLoader I/O signals
    sc_signal<sc_uint<10>> addr_len;
    sc_signal<bool> set_addr, set_len;
    sc_signal<sc_uint<3>> mode;
    sc_signal<sc_uint<3>> stride;
    sc_signal<bool> wen;
    sc_signal<bool> activate;
    sc_signal<bool> next_sig;
    sc_signal<bool> done, busy;

    sc_signal<bool> dm_en, dm_wr;
    sc_signal<sc_uint<10>> dm_addr;
    sc_signal<sc_uint<8>> dm_mask;
    sc_signal<sc_uint<64>> dm_wdata;
    sc_signal<sc_uint<64>> dm_rdata;
    sc_signal<sc_uint<64>> data_out;
    sc_signal<bool> data_valid;

    // 新增 ps port signals
    sc_signal<bool> ps_valid; sc_signal<bool> ps_ready; sc_signal<sc_uint<PORT_STATIC_WIDTH>> ps_data;

    DataLoader dl{"dataloader"};
    DMModel dm{"dm_model"};

    unsigned error_count = 0;

    // 新增：DM 讀取位址 logger（在時脈正緣統一取樣）
    std::vector<uint16_t> rd_addr_log;
    void log_dm_tx(){
        if(dm_en.read() && !dm_wr.read()){
            //std::cout << "[DM] Read Addr: 0x" << std::hex << std::setw(3) << std::setfill('0') << dm_addr.read() << std::dec << "\n";
            rd_addr_log.push_back(dm_addr.read());
        }
    }

    SC_CTOR(DLTestBench) {
        // Bind
        dl.clk(clk); dl.rst_n(rst_n);
        dl.addr_len_i(addr_len); dl.set_addr_i(set_addr); dl.set_len_i(set_len);
        dl.mode_i(mode); dl.stride_i(stride); dl.wen_i(wen);
        dl.activate_i(activate); dl.next_i(next_sig);
        dl.done_o(done); dl.busy_o(busy);
        dl.dm_en_o(dm_en); dl.dm_wr_o(dm_wr); dl.dm_addr_o(dm_addr); dl.dm_mask_o(dm_mask); dl.dm_wdata_o(dm_wdata); dl.dm_rdata_i(dm_rdata);
        dl.data_o(data_out); dl.data_valid_o(data_valid);
        dl.ps_valid_i(ps_valid); dl.ps_ready_o(ps_ready); dl.ps_data_i(ps_data);

        dm.clk(clk); dm.en_i(dm_en); dm.wr_i(dm_wr); dm.addr_i(dm_addr); dm.mask_i(dm_mask); dm.wdata_i(dm_wdata); dm.rdata_o(dm_rdata);

        // 新增：於時脈正緣記錄 dm 讀取位址
        SC_METHOD(log_dm_tx);
        sensitive << clk.posedge_event();
        dont_initialize();

        SC_THREAD(run);
    }

    void step(unsigned n=1){ for(unsigned i=0;i<n;i++) wait(clk.posedge_event()); } // run for n cycles

    void clear_ctrl(){ set_addr=0; set_len=0; activate=0; next_sig=0; wen=0; }

    void run(){
        // Reset
        rst_n = false; clear_ctrl(); mode=0; stride=1; addr_len=0; ps_valid=0; ps_data=0; step(3); rst_n = true; step(1);

        test_normal_load(); std::cout<<"[DL] normal load test done\n";
        test_broadcast(); std::cout<<"[DL] broadcast test done\n";
        test_stride_modes(); std::cout<<"[DL] stride modes test done\n"; // 新增：跨模式 stride/element_bytes 測試
        test_broadcast_lhb_lwb(); std::cout<<"[DL] LHB/LWB broadcast test done\n"; // 新增：LHB/LWB broadcast 測試
        test_store(); std::cout<<"[DL] store test done\n";
        test_multi_store(); std::cout<<"[DL] multi-store test done\n";
        test_zero_len(); std::cout<<"[DL] zero-length test done\n";
        test_counters_reset_and_reload(); std::cout<<"[DL] counters reset/reload test done\n";

        std::cout << (error_count?"[DL][FAIL] errors=" : "[DL][PASS] errors=") << error_count << "\n";
        sc_stop();
    }

    void wait_prefetch_ready(){
        step(1);
    }

    void pulse_next(){ next_sig=1; step(1); next_sig=0; }

    // Test cases 1: normal load
    void test_normal_load(){
        std::vector<uint16_t> exp_addrs{0x10,0x12,0x14};
        // 以 logger 的起始索引作為切片基準
        size_t rd_base = rd_addr_log.size();

        addr_len = exp_addrs.front(); set_addr=1; step(1); set_addr=0;
        addr_len = exp_addrs.size(); set_len=1; step(1); set_len=0;
        mode = 0; // LB
        stride = 2; // bytes per element increment

        activate=1; step(1); activate=0; // issue first read (dm_en pulse)
        wait_prefetch_ready();
        for(size_t i=0;i<exp_addrs.size();++i){
            pulse_next(); // consume prefetched -> may trigger next read if remaining >1
            uint64_t v = data_out.read().to_uint64();
            uint16_t expect_addr = exp_addrs[i];
            uint64_t expect_data = dm.pattern_for_addr(expect_addr);
            if(v != expect_data){ std::cout << "[ERR] normal load elem "<<i<<" data mismatch got=0x"<<std::hex<<v<<" exp=0x"<<expect_data<<std::dec<<"\n"; error_count++; }
            wait_prefetch_ready();
        }
        // 再多等一拍，確保最後一筆發出已被 logger 捕捉
        step(1);

        // 以 logger 結果驗證位址序列
        size_t got_cnt = rd_addr_log.size() - rd_base;
        if(got_cnt!=exp_addrs.size()) {
            std::cout<<"[ERR] normal load addr count got="<<got_cnt<<" exp="<<exp_addrs.size()<<"\n"; error_count++;
        }
        for(size_t i=0;i<std::min(got_cnt, exp_addrs.size()); ++i){
            uint16_t got = rd_addr_log[rd_base + i];
            if(got != exp_addrs[i]) {
                std::cout<<"[ERR] normal load addr["<<i<<"] got=0x"<<std::hex<<got<<" exp=0x"<<exp_addrs[i]<<std::dec<<"\n"; error_count++;
            }
        }
        if(dl.get_load_outputs() != exp_addrs.size()) {
            std::cout << "[ERR] load_outputs_cnt="<<dl.get_load_outputs()<<" exp="<<exp_addrs.size()<<"\n";
            error_count++;
        }
    }

    void test_broadcast(){
        addr_len=0x30; set_addr=1; step(1); set_addr=0;
        addr_len=2; set_len=1; step(1); set_len=0;
        mode=4; stride=1; // LBB broadcast of a single byte
        std::vector<uint16_t> exp_addrs{0x30,0x31};

        // 使用 logger: 記錄起始索引
        size_t rd_base = rd_addr_log.size();

        activate=1; step(1); activate=0;
        wait_prefetch_ready();
        for(size_t i=0;i<exp_addrs.size(); ++i){
            pulse_next();
            uint16_t a = exp_addrs[i];
            uint64_t raw = dm.pattern_for_addr(a);
            uint8_t first = raw & 0xFF;
            uint64_t expected=0; for(int b=0;b<8;b++) expected |= (uint64_t)first << (8*b);
            uint64_t got = data_out.read().to_uint64();
            if(got != expected){ std::cout<<"[ERR] broadcast elem="<<i<<" got=0x"<<std::hex<<got<<" exp=0x"<<expected<<std::dec<<"\n"; error_count++; }
            if(i+1 < exp_addrs.size()) { wait_prefetch_ready(); }
        }
        // 末尾多等一拍，確保最後一次讀取位址被 logger 捕捉
        step(1);

        // 用 logger 驗證位址序列
        size_t got_cnt = rd_addr_log.size() - rd_base;
        if(got_cnt!=exp_addrs.size()) { std::cout<<"[ERR] broadcast addr count mismatch got="<<got_cnt<<" exp="<<exp_addrs.size()<<"\n"; error_count++; }
        for(size_t i=0;i<std::min(got_cnt, exp_addrs.size()); ++i){
            uint16_t got = rd_addr_log[rd_base + i];
            if(got!=exp_addrs[i]) { std::cout<<"[ERR] broadcast addr["<<i<<"] got=0x"<<std::hex<<got<<" exp=0x"<<exp_addrs[i]<<std::dec<<"\n"; error_count++; }
        }
    }

    void test_store(){
        addr_len=0x55; set_addr=1; step(1); set_addr=0;
        addr_len=1; set_len=1; step(1); set_len=0; // length for symmetry (1 傳輸)
        mode=3; stride=1; wen=1; activate=1; step(1); activate=0; wen=0; // store start
        ps_data = 0xAABBCCDDEEFF0011ULL; ps_valid=1; step(1); ps_valid=0;
        if(!busy.read()) { std::cout<<"[ERR] store busy not asserted\n"; error_count++; }
        step(1);
        if(!done.read()) { std::cout<<"[ERR] store done not pulsed\n"; error_count++; }
        if(dl.get_store_transfers()!=1){ std::cout<<"[ERR] store transfers cnt="<<dl.get_store_transfers()<<" exp=1\n"; error_count++; }
        step(1);
    }

    void test_multi_store(){
        // 清除計數器
        dl.clr_counters();

        // 設定起始位址和傳輸長度
        addr_len=0x90; set_addr=1; step(1); set_addr=0;
        addr_len=4; set_len=1; step(1); set_len=0; // 4 筆傳輸

        // 設定模式、步進值、啟用寫入
        mode=3; stride=2; wen=1; activate=1; step(1); activate=0; wen=0;

        // 初始化變數
        unsigned handshakes=0; unsigned cycle=0; uint16_t baseA=0x90; bool busy_seen=false;

        // 模擬多次傳輸
        while(true){
            // 模擬特定週期發送資料
            bool send = (cycle==1 || cycle==2 || cycle==5 || cycle==6);
            if(send && ps_ready.read()){
                ps_valid=1; ps_data = (sc_uint<64>)(0x1000 + handshakes); // 傳送資料
            } else {
                ps_valid=0; // 無資料傳送
            }

            step(1); // 時脈步進

            // 計算握手次數
            if(ps_valid.read() && ps_ready.read()) handshakes++;

            // 檢查 busy 狀態
            if(busy.read()) busy_seen=true;

            // 檢查是否完成
            if(done.read()) break;

            // 超時檢查
            if(cycle++ > 40){
                std::cout<<"[ERR] multi-store timeout\n";
                error_count++;
                break;
            }
        }

        ps_valid=0; // 停止傳送資料

        // 驗證 busy 狀態是否曾經啟用
        if(!busy_seen){
            std::cout<<"[ERR] multi-store never busy\n";
            error_count++;
        }

        // 驗證傳輸次數是否正確
        if(dl.get_store_transfers()!=4){
            std::cout<<"[ERR] multi-store cnt="<<dl.get_store_transfers()<<" exp=4\n";
            error_count++;
        }
    }

    void test_zero_len(){
        addr_len=0x80; set_addr=1; step(1); set_addr=0;
        addr_len=0; set_len=1; step(1); set_len=0;
        mode=0; stride=1; activate=1; step(1); activate=0;
        bool saw_data=false;
        for(int i=0;i<3;i++){
            if(done.read()) break;
            if(data_valid.read()) saw_data=true;
            step(1);
        }
        if(saw_data){ std::cout<<"[ERR] zero-len produced data\n"; error_count++; }
        if(!done.read()) { std::cout<<"[ERR] zero-len no done pulse\n"; error_count++; }
    }

    void test_counters_reset_and_reload(){
        dl.clr_counters(); step(1);
        addr_len=0x40; set_addr=1; step(1); set_addr=0; addr_len=1; set_len=1; step(1); set_len=0; mode=0; stride=1; activate=1; step(1); activate=0; wait_prefetch_ready(); pulse_next(); step(1);
        if(dl.get_load_outputs()!=1){ std::cout<<"[ERR] after reset load cnt="<<dl.get_load_outputs()<<" exp=1\n"; error_count++; }
        addr_len=0xA0; set_addr=1; step(1); set_addr=0; addr_len=2; set_len=1; step(1); set_len=0; mode=3; stride=1; wen=1; activate=1; step(1); activate=0; wen=0;
        unsigned sent=0; while(!done.read()){ if(ps_ready.read()){ ps_valid=1; ps_data = (sc_uint<64>)(0xAB00 + sent); sent++; } step(1); }
        ps_valid=0; if(dl.get_store_transfers()!=2){ std::cout<<"[ERR] after reset store cnt="<<dl.get_store_transfers()<<" exp=2\n"; error_count++; }
    }

    void test_stride_modes(){
        // 測試 mode 0..3 (LB/LH/LW/LD) 每種 element_bytes 是否正確推進地址
        struct ModeInfo { uint8_t mode; unsigned elem_bytes; const char* name; } modes[] = {
            {0,1,"LB"},{1,2,"LH"},{2,4,"LW"},{3,8,"LD"}
        };
        for(auto &m: modes){
            // 兩種 stride: 1 與 2，用 len=3
            for(unsigned stride_val : {1u,2u}){
                dl.clr_counters();
                uint16_t baseA = (uint16_t)(0x120 + m.mode*0x20 + (stride_val*3)); // 避免重疊
                addr_len = baseA; set_addr=1; step(1); set_addr=0;
                addr_len = 3; set_len=1; step(1); set_len=0;
                mode = m.mode; stride = stride_val;
                std::vector<uint16_t> exp_addrs; exp_addrs.reserve(3);
                uint16_t cur = baseA;
                for(int i=0;i<3;i++){ exp_addrs.push_back(cur); cur = (uint16_t)(cur + stride_val * m.elem_bytes); }

                // 使用 logger: 記錄起始索引
                size_t rd_base = rd_addr_log.size();

                activate=1; step(1); activate=0;
                wait_prefetch_ready();
                for(int i=0;i<3;i++){
                    pulse_next();
                    uint64_t v = data_out.read().to_uint64();
                    uint64_t expect_data = dm.pattern_for_addr(exp_addrs[i]);
                    if(v != expect_data){
                        std::cout << "[ERR] stride mode="<<m.name<<" stride="<<stride_val
                                  <<" elem="<<i<<" data mismatch got=0x"<<std::hex<<v
                                  <<" exp=0x"<<expect_data<<std::dec<<"\n"; error_count++; }
                    if(i<2) { wait_prefetch_ready(); }
                }
                // 末尾多等一拍，確保最後一次讀取位址被 logger 捕捉
                step(1);

                // 用 logger 驗證位址序列
                size_t got_cnt = rd_addr_log.size() - rd_base;
                if(got_cnt!=exp_addrs.size()){
                    std::cout<<"[ERR] stride mode="<<m.name<<" stride="<<stride_val
                             <<" addr count got="<<got_cnt<<" exp="<<exp_addrs.size()<<"\n"; error_count++; }
                for(size_t i=0;i<exp_addrs.size() && i<got_cnt; ++i){
                    uint16_t got = rd_addr_log[rd_base + i];
                    if(got!=exp_addrs[i]){
                        std::cout<<"[ERR] stride mode="<<m.name<<" stride="<<stride_val
                                 <<" addr["<<i<<"] got=0x"<<std::hex<<got
                                 <<" exp=0x"<<exp_addrs[i]<<std::dec<<"\n"; error_count++; }
                }
                if(dl.get_load_outputs()!=3){
                    std::cout<<"[ERR] stride mode="<<m.name<<" stride="<<stride_val
                             <<" load_outputs_cnt="<<dl.get_load_outputs()<<" exp=3\n"; error_count++; }
            }
        }
    }

    void test_broadcast_lhb_lwb(){
        // mode 5: LHB (broadcast half), mode 6: LWB (broadcast word)
        struct BInfo { uint8_t mode; unsigned eb; const char* name; } bs[] = {{5,2,"LHB"},{6,4,"LWB"}};
        for(auto &b: bs){
            dl.clr_counters();
            uint16_t baseA = (uint16_t)(0x300 + b.mode*0x10);
            addr_len = baseA; set_addr=1; step(1); set_addr=0;
            addr_len = 2; set_len=1; step(1); set_len=0; // len=2 elements
            mode = b.mode; stride = 1;
            std::vector<uint16_t> exp_addrs{ (uint16_t)(baseA), (uint16_t)(baseA + 1* b.eb) }; // stride=1 => addr increment by eb bytes

            // 使用 logger: 記錄起始索引
            size_t rd_base = rd_addr_log.size();

            activate=1; step(1); activate=0;
            wait_prefetch_ready();
            for(int i=0;i<2;i++){
                pulse_next();
                uint16_t a = exp_addrs[i];
                uint64_t raw = dm.pattern_for_addr(a);
                // 取前 eb bytes 作為 pattern
                uint64_t expected=0;
                if(b.eb==2){
                    uint16_t hw = raw & 0xFFFF;
                    for(int r=0;r<4;r++) expected |= (uint64_t)hw << (16*r);
                } else if(b.eb==4){
                    uint32_t w = raw & 0xFFFFFFFFu;
                    expected = (uint64_t)w | ((uint64_t)w << 32);
                }
                uint64_t got = data_out.read().to_uint64();
                if(got!=expected){
                    std::cout<<"[ERR] broadcast "<<b.name<<" elem="<<i<<" got=0x"<<std::hex<<got
                             <<" exp=0x"<<expected<<std::dec<<"\n"; error_count++; }
                if(i==0){ wait_prefetch_ready(); }
            }
            // 末尾多等一拍，確保最後一次讀取位址被 logger 捕捉
            step(1);

            // 用 logger 驗證位址序列
            size_t got_cnt = rd_addr_log.size() - rd_base;
            if(got_cnt!=exp_addrs.size()){
                std::cout<<"[ERR] broadcast "<<b.name<<" addr count got="<<got_cnt<<" exp="<<exp_addrs.size()<<"\n"; error_count++; }
            for(size_t i=0;i<exp_addrs.size() && i<got_cnt; ++i){
                uint16_t got = rd_addr_log[rd_base + i];
                if(got!=exp_addrs[i]){
                    std::cout<<"[ERR] broadcast "<<b.name<<" addr["<<i<<"] got=0x"<<std::hex<<got
                             <<" exp=0x"<<exp_addrs[i]<<std::dec<<"\n"; error_count++; }
            }
        }
    }
};

int sc_main(int argc, char* argv[]){
    (void)argc; (void)argv;
    DLTestBench tb{"tb"};
    const char* dump = std::getenv("DL_WAVE");
    sc_trace_file* tf=nullptr;
    if(dump){ tf = sc_create_vcd_trace_file("dataloader_tb"); tb.dl.add_traces(tf);}
    sc_start();
    if(tf) sc_close_vcd_trace_file(tf);
    return tb.error_count==0 ? 0 : 1;
}