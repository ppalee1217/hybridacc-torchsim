#!/usr/bin/env python3
"""Batch-modify RTL testbenches to add GATE_SIM ifdef support.

Wraps RTL-specific includes in `ifndef GATE_SIM and adds SDF annotation block.
"""
import re
import sys
from pathlib import Path

# Map: TB file -> (DUT module name, list of RTL includes to wrap, SDF path relative to TB dir)
TB_MAP = {
    # PE unit tests
    "tb/PE/tb_vaddu.sv":            ("VADDU",            ["../../src/PE/VADDU.sv"],                                      "../../syn/VADDU/VADDU.sdf"),
    "tb/PE/tb_vmulu.sv":            ("VMULU",            ["../../src/PE/VMULU.sv"],                                      "../../syn/VMULU/VMULU.sdf"),
    "tb/PE/tb_decoder.sv":          ("Decoder",          ["../../src/PE/Decoder.sv"],                                    "../../syn/Decoder/Decoder.sdf"),
    "tb/PE/tb_instructionmemory.sv":("InstructionMemory",["../../src/PE/InstructionMemory.sv"],                          "../../syn/InstructionMemory/InstructionMemory.sdf"),
    "tb/PE/tb_loopcontroller.sv":   ("LoopController",   ["../../src/PE/LoopController.sv"],                             "../../syn/LoopController/LoopController.sdf"),
    "tb/PE/tb_datamemory.sv":       ("DataMemory",       ["../../src/PE/DataMemory.sv"],                                 "../../syn/DataMemory/DataMemory.sdf"),
    "tb/PE/tb_psumregfile.sv":      ("PsumRegFile",      ["../../src/PE/PsumRegFile.sv"],                                "../../syn/PsumRegFile/PsumRegFile.sdf"),
    "tb/PE/tb_transformregfile.sv":  ("TransformRegFile", ["../../src/PE/TransformRegFile.sv"],                           "../../syn/TransformRegFile/TransformRegFile.sdf"),
    "tb/PE/tb_ldma.sv":             ("LDMA",             ["../../src/PE/LDMA.sv"],                                       "../../syn/LDMA/LDMA.sdf"),
    "tb/PE/tb_sdma.sv":             ("SDMA",             ["../../src/PE/SDMA.sv"],                                       "../../syn/SDMA/SDMA.sdf"),
    "tb/PE/tb_if_id_stage.sv":      ("IF_ID_Stage",      ["../../src/PE/InstructionMemory.sv","../../src/PE/Decoder.sv","../../src/PE/LoopController.sv","../../src/PE/IF_ID_Stage.sv"],
                                                                                                                          "../../syn/IF_ID_Stage/IF_ID_Stage.sdf"),
    "tb/PE/tb_exe_m_stage.sv":      ("EXE_M_Stage",      ["../../src/PE/TransformRegFile.sv","../../src/PE/VMULU.sv","../../src/PE/LDMA.sv","../../src/PE/SDMA.sv","../../src/PE/DataMemory.sv","../../src/PE/EXE_M_Stage.sv"],
                                                                                                                          "../../syn/EXE_M_Stage/EXE_M_Stage.sdf"),
    "tb/PE/tb_exe_a_stage.sv":      ("EXE_A_Stage",      ["../../src/PE/VADDU.sv","../../src/PE/PsumRegFile.sv","../../src/PE/EXE_A_Stage.sv"],
                                                                                                                          "../../syn/EXE_A_Stage/EXE_A_Stage.sdf"),
    "tb/PE/tb_perouter.sv":         ("PErouter",         ["../../src/FIFO.sv","../../src/asyncFIFO.sv","../../src/PE/PErouter.sv"],
                                                                                                                          "../../syn/PErouter/PErouter.sdf"),
    "tb/PE/tb_processelement.sv":   ("ProcessElement",   ["../../src/FIFO.sv","../../src/asyncFIFO.sv","../../src/PE/InstructionMemory.sv","../../src/PE/LoopController.sv","../../src/PE/Decoder.sv","../../src/PE/VADDU.sv","../../src/PE/VMULU.sv","../../src/PE/TransformRegFile.sv","../../src/PE/PsumRegFile.sv","../../src/PE/DataMemory.sv","../../src/PE/LDMA.sv","../../src/PE/SDMA.sv","../../src/PE/IF_ID_Stage.sv","../../src/PE/EXE_M_Stage.sv","../../src/PE/EXE_A_Stage.sv","../../src/PE/PErouter.sv","../../src/PE/ProcessElement.sv"],
                                                                                                                          "../../syn/ProcessElement/ProcessElement.sdf"),
    "tb/PE/tb_pe_sim.sv":           ("ProcessElement",   ["../../src/FIFO.sv","../../src/asyncFIFO.sv","../../src/PE/InstructionMemory.sv","../../src/PE/LoopController.sv","../../src/PE/Decoder.sv","../../src/PE/VADDU.sv","../../src/PE/VMULU.sv","../../src/PE/TransformRegFile.sv","../../src/PE/PsumRegFile.sv","../../src/PE/DataMemory.sv","../../src/PE/LDMA.sv","../../src/PE/SDMA.sv","../../src/PE/IF_ID_Stage.sv","../../src/PE/EXE_M_Stage.sv","../../src/PE/EXE_A_Stage.sv","../../src/PE/PErouter.sv","../../src/PE/ProcessElement.sv"],
                                                                                                                          "../../syn/ProcessElement/ProcessElement.sdf"),
    # Base module tests
    "tb/tb_fifo.sv":                ("FIFO",             ["../src/FIFO.sv"],                                             "../syn/FIFO/FIFO.sdf"),
    "tb/tb_asyncfifo.sv":           ("asyncFIFO",        ["../src/asyncFIFO.sv"],                                        "../syn/asyncFIFO/asyncFIFO.sdf"),
}

# Includes to KEEP outside the ifdef (needed by both RTL and gate-level)
KEEP_INCLUDES = {"tb_common.svh", "hybridacc_utils_pkg.sv"}


def should_keep(include_path: str) -> bool:
    """Check if an include should stay outside the ifdef."""
    return any(k in include_path for k in KEEP_INCLUDES)


def modify_tb(root: Path, tb_rel: str, dut_mod: str, rtl_includes: list, sdf_path: str):
    """Modify a single testbench file."""
    tb_path = root / tb_rel
    if not tb_path.exists():
        print(f"  SKIP (not found): {tb_rel}")
        return

    text = tb_path.read_text()

    # Check if already modified
    if "GATE_SIM" in text:
        print(f"  SKIP (already modified): {tb_rel}")
        return

    lines = text.split("\n")
    new_lines = []
    in_rtl_block = False
    rtl_block_started = False
    sdf_inserted = False

    i = 0
    while i < len(lines):
        line = lines[i]

        # Check if this is an include line
        m = re.match(r'^(\s*)`include\s+"([^"]+)"', line)
        if m:
            indent = m.group(1)
            inc_path = m.group(2)

            if should_keep(inc_path):
                # Keep this include outside the ifdef
                if in_rtl_block:
                    new_lines.append("`endif")
                    in_rtl_block = False
                new_lines.append(line)
            else:
                # This is an RTL include - wrap in ifndef GATE_SIM
                if not in_rtl_block:
                    new_lines.append("`ifndef GATE_SIM")
                    in_rtl_block = True
                    rtl_block_started = True
                new_lines.append(line)
        else:
            # Non-include line
            if in_rtl_block:
                new_lines.append("`endif")
                in_rtl_block = False

            # Insert SDF annotation after module declaration line
            if not sdf_inserted and re.match(r'^module\s+\w+', line):
                new_lines.append(line)
                # Find the end of module declaration (semicolon)
                # Then insert after the DUT instantiation - we'll do it differently
                sdf_inserted = True  # Will insert later
                i += 1
                continue

            new_lines.append(line)
        i += 1

    # Close any open RTL block
    if in_rtl_block:
        new_lines.append("`endif")

    # Now insert SDF annotation block
    # Find the DUT instantiation and insert SDF annotation after it
    result_text = "\n".join(new_lines)

    # Insert SDF annotation block before the first `initial begin` or after module body start
    # Strategy: find a good insertion point (after DUT instantiation)
    sdf_block = f"""
`ifdef GATE_SIM
initial begin
    $sdf_annotate("{sdf_path}", dut);
end
`endif
"""

    # Find the DUT instantiation line and insert after its semicolon
    # Look for pattern: ModuleName ... dut( or ModuleName #(...) dut(
    dut_pattern = re.compile(
        rf'(?:^|\n)([ \t]*{re.escape(dut_mod)}\s+(?:#\([^)]*\)\s+)?dut\s*\()',
        re.MULTILINE
    )
    m = dut_pattern.search(result_text)
    if m:
        # Find the matching closing );
        start = m.start(1)
        # Find the ); after the instantiation
        depth = 0
        pos = m.end(1) - 1  # position of '('
        for j in range(pos, len(result_text)):
            if result_text[j] == '(':
                depth += 1
            elif result_text[j] == ')':
                depth -= 1
                if depth == 0:
                    # Find the ; after )
                    semi_pos = result_text.index(';', j)
                    result_text = result_text[:semi_pos+1] + "\n" + sdf_block + result_text[semi_pos+1:]
                    break
    else:
        # Fallback: insert after module declaration
        m2 = re.search(r'^(module\s+\w+\s*;)', result_text, re.MULTILINE)
        if m2:
            result_text = result_text[:m2.end()] + "\n" + sdf_block + result_text[m2.end():]
        else:
            print(f"  WARNING: Could not find DUT or module declaration in {tb_rel}")

    tb_path.write_text(result_text)
    print(f"  MODIFIED: {tb_rel}")


def main():
    root = Path(__file__).resolve().parent.parent  # hybridacc-RTL/
    print(f"Root: {root}")

    for tb_rel, (dut_mod, rtl_includes, sdf_path) in TB_MAP.items():
        modify_tb(root, tb_rel, dut_mod, rtl_includes, sdf_path)

    print("\nDone! All testbenches modified for GATE_SIM support.")


if __name__ == "__main__":
    main()
