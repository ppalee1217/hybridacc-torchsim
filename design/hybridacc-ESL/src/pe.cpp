#include "hybridacc/pe/pe.hpp"
#include <iostream>

namespace hybridacc {
namespace pe {

PE::~PE() {
    // Delete component instances
    delete port_io;
    delete inst_loader;
    delete inst_mem;
    delete decoder;
    delete loopstack;
    delete data_loader;
    delete tregfile;
    delete data_mem;
    delete pregfile;
    delete vadd_unit;
    delete vmul_unit;
}

void PE::create_components() {
    // Independent I/O ports
    port_io = new PortIO("port_io");
    inst_loader = new InstLoader("inst_loader");

    // IF/ID stage
    inst_mem = new InstMemory("inst_mem");
    decoder = new Decoder("decoder");
    loopstack = new Loopstack<LOOP_COUNT_BITS>("loopstack");

    // ID/EX1 stage
    data_loader = new DataLodaer("data_loader");
    tregfile = new Tregfile("tregfile");

    // EX1/EX2 stage
    data_mem = new DataMemory("data_mem");

    // EX2 stage
    pregfile = new Pregfile("pregfile");
    vadd_unit = new VADD<DATA_WIDTH, VECTOR_SIZE>("vadd_unit");
    vmul_unit = new VMUL<DATA_WIDTH, VECTOR_SIZE>("vmul_unit");
}

void PE::bind_components() {
    // Independent I/O ports
    port_io->clk(clk);
    port_io->rst_n(rst_n);
    port_io->pli_i(pli);
    port_io->pli_valid_i(pli_valid);
    port_io->pli_ready_o(pli_ready);
    port_io->plo_o(plo);
    port_io->plo_valid_o(plo_valid);
    port_io->plo_ready_i(plo_ready);
    port_io->ps_i(ps);
    port_io->ps_valid_i(ps_valid);
    port_io->ps_ready_o(ps_ready);
    port_io->pd_i(pd);
    port_io->pd_valid_i(pd_valid);
    port_io->pd_ready_o(pd_ready);

    // InstLoader
    inst_loader->clk(clk);
    inst_loader->rst_n(rst_n);
    inst_loader->len(imloader_len);
    inst_loader->en(imloader_en);
    inst_loader->done(imloader_done);
    inst_loader->busy(imloader_busy);
    inst_loader->ps_i(imloader_ps);
    inst_loader->ps_valid_i(imloader_ps_valid);
    inst_loader->ps_ready_o(imloader_ps_ready);
    inst_loader->im_en_o(imloader_im_en);
    inst_loader->im_w_en_o(imloader_im_w_en);
    inst_loader->im_addr_o(imloader_im_addr);
    inst_loader->im_wr_data_o(imloader_im_wr_data);

    //---------------------
    //  IF/ID stage
    //---------------------
    // InstMemory
    inst_mem->clk(clk);
    inst_mem->rst_n(rst_n);
    inst_mem->en_i(im_en);
    inst_mem->w_en_i(im_w_en);
    inst_mem->addr_i(im_addr);
    inst_mem->rd_data_o(im_rd_data);
    inst_mem->wr_data_i(im_wr_data);

    // Loopstack
    loopstack->clk(clk);
    loopstack->rst_n(rst_n);
    loopstack->pc_jump_i(pc_jump_i); // from pc
    loopstack->loop_count_i(loop_count); // from Decoder
    loopstack->push_i(loop_push);
    loopstack->pop_i(loop_pop);
    loopstack->jump_i(loop_jump);
    loopstack->pc_jump_o(pc_jump_o); // to pc
    loopstack->empty_o(loop_empty); // for debug
    loopstack->top_count_o(loop_top_count); // for debub

    // Decoder (Combinational)
    decoder->inst_i(inst_mem->rd_data_o);
    decoder->decoded_o(IF_decoded_fields);

    //---------------------
    //  EXE1 stage
    //---------------------
    // DataLoader
    data_loader->clk(clk);
    data_loader->rst_n(rst_n);
    data_loader->addr_len_i(dl_addr_len);
    data_loader->set_addr_i(dl_set_addr);
    data_loader->set_len_i(dl_set_len);
    data_loader->mode_i(dl_mode);
    data_loader->stride_i(dl_stride);
    data_loader->wen_i(dl_wen);
    data_loader->activate_i(dl_activate);
    data_loader->next_i(dl_next);
    data_loader->done_o(dl_done);
    data_loader->busy_o(dl_busy);
    data_loader->data_o(dl_data);
    data_loader->data_valid_o(dl_data_valid); // not used
    data_loader->ps_valid_i(dl_ps_valid);
    data_loader->ps_ready_o(dl_ps_ready);
    data_loader->ps_data_i(dl_ps_data);

    // Data Memory Interface to DataMemory
    data_mem->clk(clk);
    data_mem->rst_n(rst_n);
    data_mem->en_i(data_loader->dm_en_o);
    data_mem->wr_en_i(data_loader->dm_wr_o);
    data_mem->addr_i(data_loader->dm_addr_o);
    data_mem->byte_mask_i(data_loader->dm_mask_o);
    data_mem->wr_data_i(data_loader->dm_wdata_o);
    data_loader->dm_rdata_i(data_mem->rd_data_o);

    // Tregfile
    tregfile->clk(clk);
    tregfile->rst_n(rst_n);
    tregfile->vrs_idx_i(treg_vrs_idx); // from EXE1_decoded_fields_r
    tregfile->rd_idx_i(treg_rd_idx);   // from EXE1_decoded_fields_r
    tregfile->shift_mode_i(treg_shift_mode); // from EXE1_decoded_fields_r
    tregfile->wen_i(treg_wen);         // from EXE1_decoded_fields_r
    tregfile->shift_en_i(treg_shift_en); // from EXE1_decoded_fields_r
    tregfile->clear_i(treg_clear);     // from EXE1_decoded_fields_r
    tregfile->rd_data_i(treg_rd_data);   // from EXE1_decoded_fields_r
    tregfile->vrs_data_o(treg_vrs_data); // to VMUL

    // VMUL Unit (Combinational)
    vmul_unit->va_i(treg_vrs_data); // from Tregfile
    vmul_unit->vb_i(dl_data); // from DataLoader
    vmul_unit->vout_o(vmul_out); // to EXE2 stage
    //---------------------
    //  EXE2 stage
    //---------------------
    pregfile->clk(clk);
    pregfile->rst_n(rst_n);
    vadd_unit->clk(clk);
    vadd_unit->rst_n(rst_n);
}

// ---- Sequential: register updates ----
void PE::seq_proc() {
    if (!rst_n.read()) {
        pc_r = 0;
        tcounter_r = 0;
        pcounter_r = 0;
        EXE_1_decoded_fields_r = {};
        EXE_2_decoded_fields_r = {};
        EXE2_vmul_out_r = 0;
        return;
    }

    if (stall.read()) {
        // Hold state (stall)
        return;
    }

    // 寫入 next-state
    pc_r = pc_n;
    tcounter_r = tcounter_n;
    pcounter_r = pcounter_n;
    EXE_1_decoded_fields_r = EXE_1_decoded_fields_n;
    EXE_2_decoded_fields_r = EXE_2_decoded_fields_n;
    EXE2_vmul_out_r = vmul_out;
}

// ---- Combinational: next-state + outputs ----
void PE::comb_proc(){
    // pipeline the decoded fields
    DECODED_FIELDS if_decoded = IF_decoded_fields.read();
    DECODED_FIELDS ex1_decoded = EXE_1_decoded_fields_r.read();
    DECODED_FIELDS ex2_decoded = EXE_2_decoded_fields_r.read();

    // Default next-state
    EXE_1_decoded_fields_n = if_decoded;
    EXE_2_decoded_fields_n = ex1_decoded;
    pc_n = pc_r + 2;
    tcounter_n = tcounter_r;
    pcounter_n = pcounter_r;

    // ========== IF stage ==========
    // PC update logic
    if(if_decoded.loopend) {
        // Loop end - jump to loop start
        pc_n = pc_jump_o.read();
    } else if(if_decoded.jump) {
        // Unconditional jump
        pc_n = if_decoded.j_imm11;
    } else if(if_decoded.halt) {
        // HALT - stay at current PC
        pc_n = pc_r;
    }

    // Loopstack control signals
    loop_push = if_decoded.loop_push;
    loop_pop = if_decoded.loop_pop;
    loop_jump = if_decoded.loopend;
    loop_count = if_decoded.imm10;
    pc_jump_i = pc_r + 2;

    // IM control signals Mutiplexer
    if(load_inst.read()) {
        // Connect InstLoader to InstMemory
        im_en = imloader_im_en;
        im_w_en = imloader_im_w_en;
        im_addr = imloader_im_addr;
        im_wr_data = imloader_im_wr_data;
    } else {
        // Default (disconnect, read only)
        im_en = true; // always enable for read
        im_w_en = false; // disable write
        im_addr = pc_r; // read current PC
        im_wr_data = 0; // no write data
    }

    // ========== EXE1 stage ==========
    //

    // Tregfile control signals
    treg_vrs_idx = ex1_decoded.vtrs;
    treg_rd_idx = ex1_decoded.trd; // from EXE1_decoded_fields_r
    treg_shift_mode = ex1_decoded.stride3; // from EXE1_decoded_fields_r
    treg_wen = ex1_decoded.treg_wen; // from EXE1_decoded_fields_r
    treg_shift_en = ex1_decoded.treg_shift_en; // from EXE1_decoded_fields_r
    treg_clear = ex1_decoded.treg_clear; // from EXE1_decoded_fields_r

    // DataLoader control signals
    dl_addr_len = ex1_decoded.imm10;
    dl_set_addr = ex1_decoded.dmloaderset_addr;
    dl_set_len = ex1_decoded.dmloaderset_len;
    dl_mode = ex1_decoded.func3;
    dl_stride = ex1_decoded.stride3;
    dl_wen = ex1_decoded.dmloader_wen;
    dl_activate = ex1_decoded.dmloader_activate;
    dl_next = ex1_decoded.dmloader_next;

    // ========== EXE2 stage ==========


    // Stall logic
    stall = false; // default no stall
}



} // namespace pe
} // namespace hybridacc
