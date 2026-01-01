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
