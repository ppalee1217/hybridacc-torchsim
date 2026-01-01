import torch
import numpy as np

def golden_gemm(A: np.ndarray, B: np.ndarray, D: np.ndarray) -> np.ndarray:
    """
    Calculate Golden GEMM result.
    C = A * B + D
    """
    t_A = torch.from_numpy(A)
    t_B = torch.from_numpy(B)
    t_D = torch.from_numpy(D)

    C = torch.matmul(t_A, t_B) + t_D
    return C.numpy()
