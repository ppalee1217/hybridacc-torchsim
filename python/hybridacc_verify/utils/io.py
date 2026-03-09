import numpy as np
from pathlib import Path

def save_array(arr: np.ndarray, path: Path, fmt: str):
    """儲存陣列為 bin 或 hex 格式"""
    arr16 = arr.astype(np.float16)
    if fmt == 'bin':
        path.write_bytes(arr16.tobytes())
    else:
        u16 = arr16.view(np.uint16).reshape(-1)
        with path.open('w') as f:
            for v in u16:
                f.write(f"{v:04x}\n")

def compile_pe_program(input_asm, assambler_exe, output_bin, output_json):
    """Use External assembler to compile PE program from assembly to binary format."""
    import subprocess
    result = subprocess.run([assambler_exe, input_asm, '-o', output_bin, "--json", output_json], capture_output=True, text=True)
    if result.returncode != 0:
        print(f"Error assembling {input_asm}:\n{result.stderr}")
        raise RuntimeError(f"Assembly failed for {input_asm}")