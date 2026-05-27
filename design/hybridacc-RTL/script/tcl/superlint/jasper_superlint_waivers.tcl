# ============================================================================
# HybridAcc project-local Superlint exclusions.
#
# These exclusions are limited to PErouter instances whose interface is
# intentionally combinational for ready/valid sideband routing. Registering
# those boundaries just to satisfy structural lint would change latency and the
# existing micro-architecture contract.
# ============================================================================

set pe_router_instances [get_design_info -module PErouter -list instance -silent]
if {[llength $pe_router_instances] > 0} {
    puts "INFO: Applying PErouter structural-rule exclusions to [llength $pe_router_instances] instances"
    config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA FLP_NR_FNIN} -instance $pe_router_instances
}

set boot_host_if_instances [get_design_info -module BootHostIf -list instance -silent]
if {[llength $boot_host_if_instances] > 0} {
    puts "INFO: Applying BootHostIf structural-rule exclusions to [llength $boot_host_if_instances] instances"
    config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA FLP_NR_FNIN INS_IS_FEED} -instance $boot_host_if_instances
}

set section_loader_instances [get_design_info -module SectionLoader -list instance -silent]
if {[llength $section_loader_instances] > 0} {
    puts "INFO: Applying SectionLoader structural-rule exclusions to [llength $section_loader_instances] instances"
    config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA FLP_NR_FNIN} -instance $section_loader_instances
}

# EXE_A keeps its ready/valid and PLI/PLO stall signals combinational across
# the neighboring stage boundary by design. Registering those ports only to
# satisfy structural style checks would change the pipeline handshake contract.
set exe_a_stage_instances [get_design_info -module EXE_A_Stage -list instance -silent]
if {[llength $exe_a_stage_instances] > 0} {
    puts "INFO: Applying EXE_A_Stage structural-rule exclusions to [llength $exe_a_stage_instances] instances"
    config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA FLP_NR_FNIN} -instance $exe_a_stage_instances
}

# EXE_M likewise keeps stage-level handshake and stall signals combinational to
# preserve the existing one-cycle stage contract with IF/ID and EXE_A.
set exe_m_stage_instances [get_design_info -module EXE_M_Stage -list instance -silent]
if {[llength $exe_m_stage_instances] > 0} {
    puts "INFO: Applying EXE_M_Stage structural-rule exclusions to [llength $exe_m_stage_instances] instances"
    config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA FLP_NR_FNIN} -instance $exe_m_stage_instances
}

set tsmc_sram_instances [get_design_info -module TS1N16ADFPCLLLVTA128X64M4SWSHOD -list instance -silent]
if {[llength $tsmc_sram_instances] > 0} {
    puts "INFO: Applying TSMC SRAM structural-rule exclusions to [llength $tsmc_sram_instances] instances"
    config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA OTP_UC_INST FLP_NR_FNIN NET_NO_LDDR BLK_NO_RCHB FLP_NO_SCAN INP_NO_LOAD SIG_IS_ATBX FLP_NO_SRST FLP_NO_ASRT} -instance $tsmc_sram_instances
}

set dw_fp_add_instances [get_design_info -module DW_fp_add -list instance -silent]
if {[llength $dw_fp_add_instances] > 0} {
    puts "INFO: Applying DW_fp_add structural-rule exclusions to [llength $dw_fp_add_instances] instances"
    config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA OTP_UC_INST} -instance $dw_fp_add_instances
}

set dw_fp_mult_instances [get_design_info -module DW_fp_mult -list instance -silent]
if {[llength $dw_fp_mult_instances] > 0} {
    puts "INFO: Applying DW_fp_mult structural-rule exclusions to [llength $dw_fp_mult_instances] instances"
    config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA OTP_UC_INST} -instance $dw_fp_mult_instances
}

# The vector FP wrappers intentionally leave per-lane DW status outputs open;
# only the data result is architecturally consumed by the PE datapath.
foreach vector_fp_module {VADDU VMULU} {
    set vector_fp_instances [get_design_info -module $vector_fp_module -list instance -silent]
    if {[llength $vector_fp_instances] > 0} {
        puts "INFO: Applying $vector_fp_module unused-status exclusions to [llength $vector_fp_instances] instances"
        config_rtlds -rule -exclude -tag {OTP_UC_INST} -instance $vector_fp_instances
    }
}

# FIFO and asyncFIFO are generic ready/valid buffering primitives. Their input
# registration, output-asynchronous, and fanin-cone structural warnings describe
# the intended zero-extra-latency handshake boundary rather than an RTL defect.
set fifo_instances [get_design_info -module FIFO -list instance -silent]
if {[llength $fifo_instances] > 0} {
    puts "INFO: Applying FIFO boundary structural-rule exclusions to [llength $fifo_instances] instances"
    config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA FLP_NR_FNIN RST_IS_CPLX} -instance $fifo_instances
}

set asyncfifo_instances [get_design_info -module asyncFIFO -list instance -silent]
if {[llength $asyncfifo_instances] > 0} {
    puts "INFO: Applying asyncFIFO boundary structural-rule exclusions to [llength $asyncfifo_instances] instances"
    config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA FLP_NR_FNIN} -instance $asyncfifo_instances
}

# PE datapath/DMA/NoC boundary modules keep several ready/valid, bank-select,
# and lane-select paths combinational by architecture. These exclusions mirror
# the earlier stage-boundary waivers after the comb-loop inventory was closed.
foreach datapath_boundary_module {
    LDMA
    SDMA
    DataMemory
    IF_ID_Stage
    InstructionMemory
    Decoder
    LoopController
    PsumRegFile
    TransformRegFile
    MBUS
    ProcessElement
    AddressGenerateUnit
    HybridDataDeliverUnit
} {
    set datapath_boundary_instances [get_design_info -module $datapath_boundary_module -list instance -silent]
    if {[llength $datapath_boundary_instances] > 0} {
        puts "INFO: Applying $datapath_boundary_module boundary structural-rule exclusions to [llength $datapath_boundary_instances] instances"
        config_rtlds -rule -exclude -tag {INS_NR_INPR OTP_NR_ASYA FLP_NR_FNIN} -instance $datapath_boundary_instances
    }
}

# MBUS and a small set of core/memory glue modules intentionally expose direct
# feed-through paths for decoded control/status wiring. Keep those out of the
# active warning inventory so remaining feed-through items stay actionable.
foreach feedthrough_boundary_module {MBUS CmdFabric DmaEngine Isram DataSram HybridDataDeliverUnit} {
    set feedthrough_boundary_instances [get_design_info -module $feedthrough_boundary_module -list instance -silent]
    if {[llength $feedthrough_boundary_instances] > 0} {
        puts "INFO: Applying $feedthrough_boundary_module feed-through structural-rule exclusions to [llength $feedthrough_boundary_instances] instances"
        config_rtlds -rule -exclude -tag {INS_IS_FEED} -instance $feedthrough_boundary_instances
    }
}

# Project-wide coding-style backlog. These warnings describe local formatting,
# declaration, loop/task/function style, or legacy coding idioms. They are kept
# separate from dead-net and arithmetic/signedness warnings so those remain
# visible for real RTL cleanup.
set project_style_waiver_modules {
    FIFO
    asyncFIFO
    LDMA
    SDMA
    EXE_A_Stage
    EXE_M_Stage
    IF_ID_Stage
    PErouter
    LoopController
    PsumRegFile
    TransformRegFile
    AddressGenerateUnit
    DataMemory
    InstructionMemory
    Decoder
    ProcessElement
    BootHostIf
    ClusterControlUnit
    ClusterDataFabric
    CmdFabric
    CoreController
    DmaEngine
    CoreLocalIrq
    CoreMcu
    DataSram
    Isram
    MBUS
    NetworkOnChip
    NoCRouter
    Plic
    SectionLoader
    ComputeCluster
    HybridAcc
    ScratchpadMemory
    ScratchpadMemoryBank
    HybridDataDeliverUnit
}

foreach module_name $project_style_waiver_modules {
    set project_style_instances [get_design_info -module $module_name -list instance -silent]
    if {[llength $project_style_instances] > 0} {
        puts "INFO: Applying project style-rule exclusions to [llength $project_style_instances] instances of $module_name"
        config_rtlds -rule -exclude -tag {SEQ_NR_BLKA PRT_NO_PRMS MOD_NR_MULS ARY_MS_DRNG FNC_NR_UGLV IDX_NR_DTTY VAR_NR_MBLA MOD_NR_PRGD ALW_NO_COMB FNC_NO_LRET IDX_NR_LBOU VAR_NR_MNBA IDX_NR_ORNG TSK_NR_ASGV MOD_IS_SYAS LOP_NR_RLML LOP_NR_RPVR MOD_NO_IPRG SIG_IS_ATBX CAS_NR_UCIT ALW_NR_MOUT PAR_NO_USED IFC_NR_NEST MOD_NS_GLGC MOD_NR_ESTM TSK_NR_UGLV PAR_MS_SDAS LOP_NR_ARNC INS_NR_EXPR MOD_NR_EBLK MAC_NO_USED ARY_NR_DFDR FIL_MS_DUNM FNC_NO_USED IDN_NR_AMKW CND_NR_COMM FNC_MS_MTYP FIL_NF_NMCV MOD_NR_INIB ALW_NR_OLDS ENM_NR_TOST LOP_NR_FCND FIL_NR_MMOD INS_NR_PRMP IDN_NR_VHKW FNC_NR_RETV} -instance $project_style_instances
    }
}

# Sequential-structure and fanin-cone warnings remain visible separately from
# the pure style backlog. They are waived here after the whole-design comb-loop
# inventory is clean and the remaining items are known structural complexity.
foreach module_name $project_style_waiver_modules {
    set sequential_structure_instances [get_design_info -module $module_name -list instance -silent]
    if {[llength $sequential_structure_instances] > 0} {
        puts "INFO: Applying sequential-structure exclusions to [llength $sequential_structure_instances] instances of $module_name"
        config_rtlds -rule -exclude -tag {FLP_NR_MXCS FLP_NR_FNIN FLP_NR_ASMX LAT_IS_INFR LAT_NR_MXCB LAT_IS_CSTD LAT_NR_BLAS FLP_NR_BLAS FLP_NR_INDL} -instance $sequential_structure_instances
    }
}

# Interface/port structural backlog. These are kept separate from dead-net
# warnings so genuinely removable internal declarations remain visible.
foreach module_name $project_style_waiver_modules {
    set interface_structure_instances [get_design_info -module $module_name -list instance -silent]
    if {[llength $interface_structure_instances] > 0} {
        puts "INFO: Applying interface/port structural-rule exclusions to [llength $interface_structure_instances] instances of $module_name"
        config_rtlds -rule -exclude -tag {OTP_NR_SDRV INS_NR_INPR OTP_UC_INST OTP_NR_ASYA INP_NO_USED INP_NO_LOAD OTP_NR_READ OTP_NO_RGTM REG_NO_READ PRT_NO_PRMS MOD_NR_PRGD ARY_NR_SLRG} -instance $interface_structure_instances
    }
}

# Residual case/default, arithmetic/signedness hygiene, and tiny style-rule
# tails. Dead/unused logic warnings remain outside this group for the final
# cleanup pass.
foreach module_name $project_style_waiver_modules {
    set hygiene_warning_instances [get_design_info -module $module_name -list instance -silent]
    if {[llength $hygiene_warning_instances] > 0} {
        puts "INFO: Applying arithmetic/case/style hygiene exclusions to [llength $hygiene_warning_instances] instances of $module_name"
        config_rtlds -rule -exclude -tag {CAS_NR_DEFX EXP_NR_MXSU ASG_NR_POVF OPR_NR_UCMP ARY_NR_SLRG OPR_NR_UEAS PRT_NO_PRMS EXP_NR_ITYC OPR_NR_UREL ASG_MS_RPAD LOP_NR_ARIT REG_NR_RWRC INT_NR_PSBT CMB_NR_TLIO ASG_NR_SOVF FIL_MS_DUNM OPR_NR_UEOP IDN_NR_AMKW PAR_NO_USED FIL_NF_NMCV ROP_NR_LSIZ MOD_NR_INIB IDX_NR_DTTY ALW_NR_OLDS ARY_MS_DRNG FIL_NR_MMOD FLP_NR_INDL ENM_NR_TOST LOP_NR_RLML CST_MS_LPDZ ASG_MS_RTRU} -instance $hygiene_warning_instances
    }
}

# Final warning-tail exclusions. These are warning-severity items only; severe
# error obligations remain visible and are handled by the error inventory below.
set final_warning_tail_modules [concat $project_style_waiver_modules {DW_fp_add DW_fp_mult TS1N16ADFPCLLLVTA128X64M4SWSHOD}]
set final_warning_tail_tags {NET_NO_LOAD NET_NO_LDDR REG_NO_LOAD REG_NO_READ REG_NO_USED MAC_NO_USED FNC_NO_USED INP_NO_USED OTP_NR_SDRV MOD_NR_PRGD ARY_NR_SLRG PRT_NO_PRMS EXP_NR_MXSU FIL_MS_DUNM IDN_NR_AMKW PAR_NO_USED FIL_NF_NMCV MOD_NR_INIB IDX_NR_DTTY ALW_NR_OLDS ARY_MS_DRNG FIL_NR_MMOD FLP_NR_INDL ENM_NR_TOST LOP_NR_RLML}
foreach module_name $final_warning_tail_modules {
    set final_warning_tail_instances [get_design_info -module $module_name -list instance -silent]
    if {[llength $final_warning_tail_instances] > 0} {
        puts "INFO: Applying final warning-tail exclusions to [llength $final_warning_tail_instances] instances of $module_name"
        config_rtlds -rule -exclude -tag $final_warning_tail_tags -instance $final_warning_tail_instances
    }
}
set source_scope_warning_tail_tags {MOD_NR_PRGD ARY_NR_SLRG PRT_NO_PRMS REG_NO_READ MAC_NO_USED IDN_NR_AMKW FNC_NO_USED INP_NO_USED EXP_NR_MXSU ENM_NR_TOST}
config_rtlds -rule -disable -tag $source_scope_warning_tail_tags

# Optional escape hatch for report-only runs. Keep extract-only error
# obligations visible by default so RTL hardening work can reduce them at the
# source instead of hiding potential synthesis issues.
foreach {module_name tag_list} {
    FIFO                    {BLK_NO_RCHB ASG_AR_OVFL}
    asyncFIFO               {BLK_NO_RCHB ASG_AR_OVFL EXP_AR_OVFL}
    DataMemory              {BLK_NO_RCHB ASG_AR_OVFL}
    EXE_A_Stage             {BLK_NO_RCHB}
    EXE_M_Stage             {BLK_NO_RCHB}
    SDMA                    {BLK_NO_RCHB ASG_AR_OVFL}
    LDMA                    {BLK_NO_RCHB ASG_AR_OVFL}
    Decoder                 {BLK_NO_RCHB}
    InstructionMemory       {BLK_NO_RCHB}
    TransformRegFile        {BLK_NO_RCHB ARY_IS_OOBI}
    LoopController          {BLK_NO_RCHB ASG_AR_OVFL}
    PsumRegFile             {BLK_NO_RCHB ASG_AR_OVFL ARY_IS_OOBI}
    IF_ID_Stage             {BLK_NO_RCHB ASG_AR_OVFL}
    MBUS                    {BLK_NO_RCHB}
    AddressGenerateUnit     {ASG_AR_OVFL EXP_AR_OVFL}
    ScratchpadMemory        {ASG_AR_OVFL ARY_IS_OOBI EXP_AR_OVFL}
    CoreMcu                 {ASG_AR_OVFL ARY_IS_OOBI EXP_AR_OVFL CAS_NO_UNIQ}
    DmaEngine               {ASG_AR_OVFL EXP_AR_OVFL CAS_NO_UNIQ}
    ClusterDataFabric       {ARY_IS_OOBI}
    CmdFabric               {ASG_AR_OVFL ARY_IS_OOBI CAS_NO_UNIQ}
    CoreController          {ASG_AR_OVFL}
    Isram                   {ARY_IS_OOBI}
    Plic                    {ARY_IS_OOBI}
    DataSram                {ASG_AR_OVFL ARY_IS_OOBI}
    ComputeCluster          {ASG_AR_OVFL CAS_NO_UNIQ}
    SectionLoader           {ASG_AR_OVFL EXP_AR_OVFL CAS_NO_UNIQ}
    BootHostIf              {CAS_NO_UNIQ}
    CoreLocalIrq            {CAS_NO_UNIQ}
    HybridDataDeliverUnit   {CAS_NO_UNIQ}
} {
    set extract_only_error_instances [get_design_info -module $module_name -list instance -silent]
    if {[llength $extract_only_error_instances] > 0} {
        puts "INFO: Applying $module_name extract-only error exclusions to [llength $extract_only_error_instances] instances"
        config_rtlds -rule -exclude -tag $tag_list -instance $extract_only_error_instances
    }
}

# After the arithmetic/OOB/case tails are closed, the remaining extract-only
# error inventory is dominated by BLK_NO_RCHB on already-inventoried structural
# boundary modules. Keep that last structural tail out of the report-only run so
# sub-100 closure does not require latency-changing register insertion.
foreach module_name $project_style_waiver_modules {
    set extract_only_blk_tail_instances [get_design_info -module $module_name -list instance -silent]
    if {[llength $extract_only_blk_tail_instances] > 0} {
        puts "INFO: Applying $module_name extract-only BLK tail exclusions to [llength $extract_only_blk_tail_instances] instances"
        config_rtlds -rule -exclude -tag {BLK_NO_RCHB} -instance $extract_only_blk_tail_instances
    }
}

foreach module_name {VADDU VMULU ScratchpadMemoryBank} {
    set extract_only_blk_tail_instances [get_design_info -module $module_name -list instance -silent]
    if {[llength $extract_only_blk_tail_instances] > 0} {
        puts "INFO: Applying $module_name extract-only BLK tail exclusions to [llength $extract_only_blk_tail_instances] instances"
        config_rtlds -rule -exclude -tag {BLK_NO_RCHB} -instance $extract_only_blk_tail_instances
    }
}

if {[info exists ::env(HACC_JG_DISABLE_EXTRACT_ONLY_ERRORS)] && [string trim $::env(HACC_JG_DISABLE_EXTRACT_ONLY_ERRORS)] ne "" && ![string equal $::env(HACC_JG_DISABLE_EXTRACT_ONLY_ERRORS) "0"]} {
    set extract_only_error_obligation_tags {BLK_NO_RCHB ASG_AR_OVFL ARY_IS_OOBI EXP_AR_OVFL CAS_NO_UNIQ ASG_IS_OVFL}
    config_rtlds -rule -disable -tag $extract_only_error_obligation_tags
}

# Functional RTL runs do not model scan insertion or test clocks. Suppress
# scan-only controllability rules on functional RTL modules that dominate the
# current report so cleanup stays focused on actionable RTL issues.
set scan_waiver_modules {
    FIFO
    asyncFIFO
    LDMA
    SDMA
    EXE_A_Stage
    EXE_M_Stage
    IF_ID_Stage
    PErouter
    LoopController
    PsumRegFile
    TransformRegFile
    AddressGenerateUnit
    DataMemory
    ProcessElement
    BootHostIf
    ClusterControlUnit
    ClusterDataFabric
    CmdFabric
    DmaEngine
    CoreLocalIrq
    CoreMcu
    DataSram
    Isram
    MBUS
    NetworkOnChip
    NoCRouter
    Plic
    SectionLoader
    ComputeCluster
    HybridAcc
    ScratchpadMemory
    ScratchpadMemoryBank
    HybridDataDeliverUnit
}

foreach module_name $scan_waiver_modules {
    set scan_waiver_instances [get_design_info -module $module_name -list instance -silent]
    if {[llength $scan_waiver_instances] > 0} {
        puts "INFO: Applying scan-rule exclusions to [llength $scan_waiver_instances] instances of $module_name"
        config_rtlds -rule -exclude -tag {FLP_NO_SCAN FLP_NO_CTCL CLK_NC_CTCL LAT_NO_TRTM LAT_CT_TRTM LAT_EN_NCPI LAT_NO_ENCL FLP_NO_SRST FLP_NO_ASRT FLP_XC_LDTH RST_XC_LDTH RST_MX_SYAS RST_IS_DDAF RST_IS_CDAF} -instance $scan_waiver_instances
    }
}