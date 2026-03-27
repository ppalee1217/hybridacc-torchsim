`include "../tb_common.svh"
`include "../tb_networkonchip.sv"

module tb_noc_system;
    initial begin
        $display("tb_noc_system delegates to tb_networkonchip");
        #1;
    end
endmodule
