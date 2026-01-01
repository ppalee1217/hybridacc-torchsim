import torch
import torch.nn.functional as F
import numpy as np
from typing import Tuple, Dict

def golden_conv2d(
    input_act: np.ndarray,
    weight: np.ndarray,
    input_ps: np.ndarray,
    stride: int = 1,
    padding: int = 0
) -> np.ndarray:
    """
    Calculate Golden Conv2d result.
    input_act: (N, H, W, C)
    weight: (OC, KH, KW, C)
    input_ps: (N, OC, H_out, W_out) or compatible
    """
    # Convert to torch tensors
    t_input = torch.from_numpy(input_act)
    t_weight = torch.from_numpy(weight)
    t_ps = torch.from_numpy(input_ps)

    # PyTorch expects NCHW for input and (OC, C, KH, KW) for weight
    # Input: (N, H, W, C) -> (N, C, H, W)
    input_nchw = t_input.permute(0, 3, 1, 2)
    # Weight: (OC, KH, KW, C) -> (OC, C, KH, KW)
    weight_nchw = t_weight.permute(0, 3, 1, 2)

    # Input PS: (N, OC, H_out, W_out) -> (N, OC, H_out, W_out)
    # Assuming input_ps is already in correct shape or needs permutation?
    # In original code: input_ps was (1, 16, 198, 16) -> NHWC?
    # Original code: input_ps = torch.randn(1, 16, 198, 16) -> (N, H_out, W_out, OC) ?
    # Wait, original code: input_ps = torch.randn(1, 16, 198, 16)
    # input_ps_nchw = input_ps.permute(0, 3, 1, 2)
    # If input_ps is (N, H, W, C), then permute(0, 3, 1, 2) makes it (N, C, H, W).
    # So input_ps is NHWC.

    input_ps_nchw = t_ps.permute(0, 3, 1, 2)

    # Calculate Conv2d
    output_conv = F.conv2d(input_nchw, weight_nchw, stride=stride, padding=padding)

    # Add input partial sum
    output_final = output_conv + input_ps_nchw

    # Convert back to NHWC
    output_final_nhwc = output_final.permute(0, 2, 3, 1)

    return output_final_nhwc.numpy()

def golden_conv1d(act: np.ndarray, weight: np.ndarray, stride: int) -> np.ndarray:
    """
    Calculate Golden Conv1D result.
    act: (C_in, W_in)
    weight: (C_out, C_in, K)
    """
    act_torch = torch.tensor(act[None, :, :], dtype=torch.float32)
    weight_torch = torch.tensor(weight, dtype=torch.float32)
    out_torch = F.conv1d(act_torch, weight_torch, stride=stride)
    return out_torch.numpy()[0]
