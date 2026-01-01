import torch
import numpy as np
from typing import Dict, Any
from ..utils.config import ScanChainConfig, PERouterMode
from ..utils.data import TestData
from ..model.conv import golden_conv2d
from ..model.gemm import golden_gemm

def generate_conv2d_test(num_pes: int = 64, num_bus: int = 4, stride: int = 1) -> TestData:
    """
    Generate Conv2d test case.
    Spec:
    1. conv2d 3x3, stride 1, padding 0
    input activation (NHWC): (1,18,200,4)
    weight (OC, H, W, C): (16,3,3,4)
    input partial sum (NHWC): (1,16,198,16)
    output partial sum (NHWC): (1,16,198,16)
    """
    print("Generating Conv2d test data...")

    # Shapes from spec
    N, H, W, C = 1, 18, 200, 4
    OC, KH, KW, _ = 16, 3, 3, 4

    # Generate random data
    input_act = torch.randn(N, H, W, C).numpy()
    weight = torch.randn(OC, KH, KW, C).numpy()
    input_ps = torch.randn(1, 16, 198, 16).numpy() # NHWC

    # Calculate Golden
    output_final_nhwc = golden_conv2d(input_act, weight, input_ps, stride=stride, padding=0)

    # Prepare numpy arrays
    inputs = {
        "activation": input_act,
        "weight": weight,
        "partial_sum": input_ps
    }
    outputs = {
        "partial_sum": output_final_nhwc
    }

    scan_chain = []

    def get_route_mode(row_idx: int, kh: int) -> int:
        if row_idx == 0: # First row
            return PERouterMode.PLI_FROM_BUS_PLO_TO_LN
        elif row_idx == kh-1: # Last row
            return PERouterMode.PLI_FROM_LN_PLO_TO_BUS
        else: # Middle rows
            return PERouterMode.PLI_FROM_LN_PLO_TO_LN

    num_pes_per_bus = num_pes // num_bus
    for i in range(num_bus):
        for j in range(num_pes_per_bus):
            cfg = ScanChainConfig(
                ps_id=i,
                pd_id=(i+j)*stride,
                pli_id=j if i==0 else 63,
                plo_id=j if i==KH-1 else 63,
                route_mode=get_route_mode(i, KH),
                enable=True if i<KH else False
            )
            scan_chain.append(cfg.pack())

    config = {
        "mode": "conv2d",
        "kernel_size": KH,
        "in_ch": C,
        "stride": 1,
        "out_ch": OC,
        "in_height": H,
        "in_width": W,
        "out_height": output_final_nhwc.shape[1],
        "out_width": output_final_nhwc.shape[2],
        "partial_sum_zero": False,
        "seed": 123
    }

    return TestData(
        name="conv2d_3x3",
        description="Conv2d 3x3, stride 1, padding 0",
        inputs=inputs,
        outputs=outputs,
        scan_chain=scan_chain,
        config=config
    )

def generate_gemm_test(num_pes: int = 64) -> TestData:
    """
    Generate GEMM test case.
    Size suggestion: M=32, N=32, K=32
    C = A * B + D (Input PS)
    """
    print("Generating GEMM test data...")

    M, N, K = 32, 32, 32

    # Generate random data
    A = torch.randn(M, K).numpy()
    B = torch.randn(K, N).numpy()
    D = torch.randn(M, N).numpy() # Input PS

    # Calculate GEMM
    C = golden_gemm(A, B, D)

    inputs = {
        "A": A,
        "B": B,
        "D": D
    }
    outputs = {
        "C": C
    }

    scan_chain = []
    for i in range(num_pes):
        cfg = ScanChainConfig(
            ps_id=i, pd_id=i, pli_id=i, plo_id=i,
            route_mode=PERouterMode.PLI_FROM_BUS_PLO_TO_BUS, enable=True
        )
        scan_chain.append(cfg.pack())

    config = {
        "mode": "gemm",
        "M": M,
        "N": N,
        "K": K,
        "partial_sum_zero": False,
        "seed": 123
    }

    return TestData(
        name="gemm_32x32x32",
        description="GEMM M=32, N=32, K=32",
        inputs=inputs,
        outputs=outputs,
        scan_chain=scan_chain,
        config=config
    )
